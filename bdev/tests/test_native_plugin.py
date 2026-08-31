# SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
from __future__ import annotations

from pathlib import Path
from types import ModuleType
import sys

import pytest

from lmcache_spdk_connector.native import SpdkNativeConnector


def _params(tmp_path: Path, **overrides: object) -> dict[str, object]:
    config = tmp_path / "bdev.json"
    config.write_text("{}", encoding="utf-8")
    params: dict[str, object] = {
        "spdk_json_config": str(config),
        "bdev_name": "Malloc0",
        "spdk_reactor_cpus": [1],
        "connector_worker_cpus": [2],
        "spdk_io_thread_count": 1,
        "spdk_buffer_count": 1,
    }
    params.update(overrides)
    return params


def test_native_factory_builds_extent_connector(tmp_path, monkeypatch) -> None:
    """The native-plugin factory forwards ephemeral extent parameters."""
    monkeypatch.setattr("os.sched_getaffinity", lambda _: {1, 2})
    captured: dict[str, object] = {}

    class FakeConnector:
        def __init__(self, **kwargs: object) -> None:
            captured.update(kwargs)

    native_module = ModuleType("lmcache_spdk_connector._native")
    native_module.SpdkConnector = FakeConnector
    monkeypatch.setitem(sys.modules, "lmcache_spdk_connector._native", native_module)

    connector = SpdkNativeConnector(**_params(tmp_path))

    assert isinstance(connector, FakeConnector)
    assert captured["worker_cpus"] == [2]


def test_native_factory_rejects_declared_in_tree_runtime(tmp_path, monkeypatch) -> None:
    """A deployment-declared in-tree runtime cannot be mixed with this package."""
    monkeypatch.setattr("os.sched_getaffinity", lambda _: {1, 2})
    monkeypatch.setenv("LMCACHE_SPDK_RUNTIME_OWNER", "in_tree")
    with pytest.raises(RuntimeError, match="cannot share"):
        SpdkNativeConnector(**_params(tmp_path))

