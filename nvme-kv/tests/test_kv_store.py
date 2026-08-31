# SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Semantic tests for the direct NVMe-KV connector's object format."""

from __future__ import annotations

import pytest

from lmcache_spdk_connector_kv.kv_format import (
    KV_KEY_BYTES,
    KvFormatError,
    decode_manifest,
    make_object_plan,
)
from lmcache_spdk_connector_kv.kv_store import KvObjectConflict, KvObjectStore


class MemoryKvEngine:
    """A small conditional-write KV namespace used without SPDK hardware."""

    def __init__(self, max_value_bytes: int, fail_on_store: int | None = None) -> None:
        self.max_value_bytes = max_value_bytes
        self.records: dict[bytes, bytes] = {}
        self._fail_on_store = fail_on_store
        self._store_count = 0

    def store_if_absent(self, key: bytes, value: bytes) -> bool:
        self._store_count += 1
        if self._store_count == self._fail_on_store:
            raise OSError("injected KV STORE failure")
        if len(key) != KV_KEY_BYTES or len(value) > self.max_value_bytes:
            raise ValueError("device request exceeds KV namespace limit")
        if key in self.records:
            return False
        self.records[key] = value
        return True

    def retrieve(self, key: bytes) -> bytes | None:
        return self.records.get(key)

    def delete(self, key: bytes) -> bool:
        return self.records.pop(key, None) is not None

    def list_keys(self) -> tuple[bytes, ...]:
        return tuple(self.records)


def test_plan_uses_device_sized_keys_and_values() -> None:
    payload = b"a" * 600
    plan = make_object_plan("model@00000000@0@hash@tenant", payload, 256)

    assert len(plan.chunks) == 3
    assert len(plan.manifest_key) == KV_KEY_BYTES
    assert all(len(chunk.key) == KV_KEY_BYTES for chunk in plan.chunks)
    assert [len(chunk.value) for chunk in plan.chunks] == [256, 256, 88]
    manifest = decode_manifest(plan.manifest_value)
    assert manifest.logical_bytes == len(payload)
    assert manifest.chunk_count == 3


def test_store_load_delete_and_recover_chunked_object() -> None:
    engine = MemoryKvEngine(max_value_bytes=256)
    store = KvObjectStore(engine)
    identity = "model@00000000@0@hash@tenant"
    payload = b"a" * 600

    assert store.store(identity, payload)
    assert not store.store(identity, payload)
    assert store.load(identity) == payload
    assert KvObjectStore(engine).recover_identities() == (identity.encode(),)
    assert store.delete(identity)
    assert store.load(identity) is None
    assert not store.delete(identity)


def test_failed_chunk_store_never_publishes_a_manifest() -> None:
    engine = MemoryKvEngine(max_value_bytes=256, fail_on_store=2)
    store = KvObjectStore(engine)
    identity = "model@00000000@0@hash@tenant"

    with pytest.raises(OSError, match="injected"):
        store.store(identity, b"a" * 600)

    assert store.load(identity) is None
    assert len(engine.records) == 1


def test_rejects_conflicting_orphan_chunk() -> None:
    engine = MemoryKvEngine(max_value_bytes=256)
    identity = "model@00000000@0@hash@tenant"
    plan = make_object_plan(identity, b"a" * 600, engine.max_value_bytes)
    engine.records[plan.chunks[0].key] = b"nope"

    with pytest.raises(KvObjectConflict, match="chunk"):
        KvObjectStore(engine).store(identity, b"a" * 600)


def test_rejects_manifest_that_cannot_fit_the_device() -> None:
    with pytest.raises(KvFormatError, match="manifest exceeds"):
        make_object_plan("identity", b"x", 32)
