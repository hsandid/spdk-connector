# SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Device-independent object format for a future NVMe Key-Value adapter.

NVMe KV namespaces permit keys of at most 16 bytes and advertise a maximum
value length per namespace. LMCache object identities and objects exceed those
limits, so this module defines a deterministic manifest-plus-chunks mapping.
"""

from __future__ import annotations

import base64
import json
from dataclasses import dataclass
from hashlib import sha256

KV_KEY_BYTES = 16
"""The maximum NVMe Key-Value Command Set key size."""

_FORMAT_VERSION = 1
_MANIFEST_CHUNK_INDEX = (1 << 32) - 1
_DOMAIN_SEPARATOR = b"lmcache-spdk-nvme-kv/v1\0"


class KvFormatError(ValueError):
    """Raised when a KV object manifest or its limits are invalid."""


@dataclass(frozen=True)
class KvManifest:
    """The committed record that makes all chunks of one object visible."""

    identity: bytes
    logical_bytes: int
    chunk_count: int
    payload_checksum: str


@dataclass(frozen=True)
class KvChunk:
    """One raw payload chunk addressed by a device-sized key."""

    key: bytes
    value: bytes


@dataclass(frozen=True)
class KvObjectPlan:
    """All device records required to store one immutable LMCache object."""

    manifest_key: bytes
    manifest_value: bytes
    chunks: tuple[KvChunk, ...]


Identity = str | bytes


def make_object_plan(
    identity: Identity, payload: bytes | memoryview, max_value_bytes: int
) -> KvObjectPlan:
    """Return deterministic manifest and chunks for a non-empty object.

    The manifest is written last by the storage engine. A manifest is therefore
    the only visibility marker for readers; interrupted writes leave only safe,
    unreachable chunk garbage.
    """
    identity_bytes = _identity_bytes(identity)
    payload_bytes = bytes(payload)
    if not payload_bytes:
        raise KvFormatError("NVMe KV objects must not be empty")
    if max_value_bytes <= 0:
        raise KvFormatError("max_value_bytes must be positive")

    chunks = tuple(
        KvChunk(
            _derive_key(identity_bytes, index),
            payload_bytes[offset : offset + max_value_bytes],
        )
        for index, offset in enumerate(range(0, len(payload_bytes), max_value_bytes))
    )
    manifest = KvManifest(
        identity=identity_bytes,
        logical_bytes=len(payload_bytes),
        chunk_count=len(chunks),
        payload_checksum=sha256(payload_bytes).hexdigest(),
    )
    manifest_value = encode_manifest(manifest)
    if len(manifest_value) > max_value_bytes:
        raise KvFormatError("manifest exceeds the namespace maximum value length")
    return KvObjectPlan(
        manifest_key=_derive_key(identity_bytes, _MANIFEST_CHUNK_INDEX),
        manifest_value=manifest_value,
        chunks=chunks,
    )


def encode_manifest(manifest: KvManifest) -> bytes:
    """Serialize one manifest into a stable, self-validating device value."""
    _validate_manifest(manifest)
    document = {
        "chunks": manifest.chunk_count,
        "identity": base64.b64encode(manifest.identity).decode("ascii"),
        "length": manifest.logical_bytes,
        "sha256": manifest.payload_checksum,
        "version": _FORMAT_VERSION,
    }
    return json.dumps(document, sort_keys=True, separators=(",", ":")).encode("utf-8")


def decode_manifest(value: bytes) -> KvManifest:
    """Deserialize a manifest value and reject malformed or unknown records."""
    try:
        document = json.loads(value.decode("utf-8"))
        if set(document) != {"chunks", "identity", "length", "sha256", "version"}:
            raise KvFormatError("manifest fields are invalid")
        if document["version"] != _FORMAT_VERSION:
            raise KvFormatError("manifest version is unsupported")
        identity = base64.b64decode(document["identity"], validate=True)
        manifest = KvManifest(
            identity=identity,
            logical_bytes=document["length"],
            chunk_count=document["chunks"],
            payload_checksum=document["sha256"],
        )
    except (
        KeyError,
        TypeError,
        UnicodeDecodeError,
        ValueError,
        json.JSONDecodeError,
    ) as error:
        if isinstance(error, KvFormatError):
            raise
        raise KvFormatError("manifest is malformed") from error
    _validate_manifest(manifest)
    return manifest


def manifest_key(identity: Identity) -> bytes:
    """Return the deterministic 16-byte key of an object's manifest."""
    return _derive_key(_identity_bytes(identity), _MANIFEST_CHUNK_INDEX)


def chunk_key(identity: Identity, chunk_index: int) -> bytes:
    """Return the deterministic 16-byte key of one payload chunk."""
    return _derive_key(_identity_bytes(identity), chunk_index)


def validate_payload(manifest: KvManifest, chunks: tuple[bytes, ...]) -> bytes:
    """Join and verify retrieved chunks, returning the original payload.

    A validation failure must be handled by a caller as a cache miss rather
    than exposing partial or wrongly addressed data to LMCache.
    """
    _validate_manifest(manifest)
    if len(chunks) != manifest.chunk_count:
        raise KvFormatError("retrieved chunk count differs from manifest")
    payload = b"".join(chunks)
    if len(payload) != manifest.logical_bytes:
        raise KvFormatError("retrieved payload length differs from manifest")
    if sha256(payload).hexdigest() != manifest.payload_checksum:
        raise KvFormatError("retrieved payload checksum differs from manifest")
    return payload


def _identity_bytes(identity: Identity) -> bytes:
    if isinstance(identity, str):
        result = identity.encode("utf-8")
    elif isinstance(identity, bytes):
        result = identity
    else:
        raise KvFormatError("object identity must be str or bytes")
    if not result:
        raise KvFormatError("object identity must not be empty")
    return result


def _derive_key(identity: bytes, chunk_index: int) -> bytes:
    if not 0 <= chunk_index <= _MANIFEST_CHUNK_INDEX:
        raise KvFormatError("chunk index is outside the format range")
    return sha256(
        _DOMAIN_SEPARATOR + identity + chunk_index.to_bytes(4, "big")
    ).digest()[:KV_KEY_BYTES]


def _validate_manifest(manifest: KvManifest) -> None:
    if not manifest.identity:
        raise KvFormatError("manifest identity must not be empty")
    if not isinstance(manifest.logical_bytes, int) or manifest.logical_bytes <= 0:
        raise KvFormatError("manifest length must be a positive integer")
    if (
        not isinstance(manifest.chunk_count, int)
        or not 0 < manifest.chunk_count < _MANIFEST_CHUNK_INDEX
    ):
        raise KvFormatError("manifest chunk count is invalid")
    if (
        not isinstance(manifest.payload_checksum, str)
        or len(manifest.payload_checksum) != 64
    ):
        raise KvFormatError("manifest checksum is invalid")
    try:
        int(manifest.payload_checksum, 16)
    except ValueError as error:
        raise KvFormatError("manifest checksum is invalid") from error
