# SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Synchronous semantic layer for the future direct NVMe-KV engine.

The native SPDK implementation will adapt asynchronous KV commands to this
small interface. Keeping publication and recovery semantics here makes them
testable without a PCIe device or SPDK runtime.
"""

from __future__ import annotations

from collections.abc import Iterable
from typing import Protocol

from .kv_format import (
    Identity,
    KvFormatError,
    KvManifest,
    chunk_key,
    decode_manifest,
    make_object_plan,
    manifest_key,
    validate_payload,
)


class KvEngine(Protocol):
    """The minimum device operations needed by the KV object layer."""

    @property
    def max_value_bytes(self) -> int:
        """Return the namespace's maximum permitted KV value length."""

    def store_if_absent(self, key: bytes, value: bytes) -> bool:
        """Store a record only when its key is absent."""

    def retrieve(self, key: bytes) -> bytes | None:
        """Return the value for a key, or ``None`` when it is absent."""

    def delete(self, key: bytes) -> bool:
        """Delete a key and return whether it existed."""

    def list_keys(self) -> Iterable[bytes]:
        """Return a snapshot of all device keys."""


class KvObjectConflict(RuntimeError):
    """A visible or orphaned device key does not match the requested object."""


class KvObjectStore:
    """Publish, load, delete, and recover immutable chunked KV objects."""

    def __init__(self, engine: KvEngine) -> None:
        self._engine = engine

    def store(self, identity: Identity, payload: bytes | memoryview) -> bool:
        """Store an object and return whether this call published it.

        Payload chunks are inserted before the manifest. Thus a failed write
        cannot become visible, although it can leave chunks for later garbage
        collection. Existing visible objects are immutable.
        """
        plan = make_object_plan(identity, payload, self._engine.max_value_bytes)
        requested = decode_manifest(plan.manifest_value)
        existing_manifest = self._engine.retrieve(plan.manifest_key)
        if existing_manifest is not None:
            self._check_existing_manifest(existing_manifest, requested)
            return False

        for chunk in plan.chunks:
            if not self._engine.store_if_absent(chunk.key, chunk.value):
                existing_chunk = self._engine.retrieve(chunk.key)
                if existing_chunk != chunk.value:
                    raise KvObjectConflict(
                        "existing chunk does not match the requested object"
                    )

        if self._engine.store_if_absent(plan.manifest_key, plan.manifest_value):
            return True
        existing_manifest = self._engine.retrieve(plan.manifest_key)
        if existing_manifest is None:
            raise KvObjectConflict("manifest disappeared while publishing object")
        self._check_existing_manifest(existing_manifest, requested)
        return False

    def load(self, identity: Identity) -> bytes | None:
        """Return a verified object, or ``None`` for any cache miss or corruption."""
        key = manifest_key(identity)
        encoded_manifest = self._engine.retrieve(key)
        if encoded_manifest is None:
            return None
        try:
            manifest = decode_manifest(encoded_manifest)
            if manifest.identity != _identity_bytes(identity):
                return None
            chunks = tuple(
                self._engine.retrieve(chunk_key(identity, index))
                for index in range(manifest.chunk_count)
            )
            if any(chunk is None for chunk in chunks):
                return None
            return validate_payload(
                manifest, tuple(chunk for chunk in chunks if chunk is not None)
            )
        except KvFormatError:
            return None

    def delete(self, identity: Identity) -> bool:
        """Hide one object, then remove its chunks when its manifest is valid."""
        key = manifest_key(identity)
        encoded_manifest = self._engine.retrieve(key)
        if encoded_manifest is None:
            return False
        try:
            manifest = decode_manifest(encoded_manifest)
        except KvFormatError:
            return self._engine.delete(key)
        if manifest.identity != _identity_bytes(identity):
            return False
        self._engine.delete(key)
        for index in range(manifest.chunk_count):
            self._engine.delete(chunk_key(identity, index))
        return True

    def recover_identities(self) -> tuple[bytes, ...]:
        """Return valid visible identities by scanning device records.

        KV LIST returns keys rather than record types, so recovery reads each
        candidate and recognizes a manifest only when it hashes to that key.
        """
        identities: list[bytes] = []
        for key in self._engine.list_keys():
            encoded_manifest = self._engine.retrieve(key)
            if encoded_manifest is None:
                continue
            try:
                manifest = decode_manifest(encoded_manifest)
            except KvFormatError:
                continue
            if manifest_key(manifest.identity) != key:
                continue
            if self.load(manifest.identity) is not None:
                identities.append(manifest.identity)
        return tuple(sorted(identities))

    @staticmethod
    def _check_existing_manifest(value: bytes, requested: KvManifest) -> None:
        try:
            existing = decode_manifest(value)
        except KvFormatError as error:
            raise KvObjectConflict("existing manifest is malformed") from error
        if existing != requested:
            raise KvObjectConflict(
                "existing manifest does not match the requested object"
            )


def _identity_bytes(identity: Identity) -> bytes:
    if isinstance(identity, str):
        return identity.encode("utf-8")
    if isinstance(identity, bytes):
        return identity
    raise KvFormatError("object identity must be str or bytes")
