# SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
from pathlib import Path

import pytest

from lmcache_spdk_connector.config import SpdkConnectorConfig


def _config(tmp_path: Path, **overrides):
    spdk_json = tmp_path / "bdev.json"
    spdk_json.write_text("{}", encoding="utf-8")
    value = {
        "spdk_json_config": str(spdk_json),
        "bdev_name": "Malloc0",
        "spdk_reactor_cpus": [1],
        "connector_worker_cpus": [2],
        "spdk_io_thread_count": 1,
    }
    value.update(overrides)
    return value


def test_rejects_overlapping_cpu_sets(tmp_path, monkeypatch):
    monkeypatch.setattr("os.sched_getaffinity", lambda _: {1, 2})
    with pytest.raises(ValueError, match="overlap"):
        SpdkConnectorConfig.from_dict(_config(tmp_path, connector_worker_cpus=[1]))


def test_uses_spdk_defaults(tmp_path, monkeypatch):
    monkeypatch.setattr("os.sched_getaffinity", lambda _: {1, 2})
    config = SpdkConnectorConfig.from_dict(_config(tmp_path))
    assert config.spdk_memory_size_mb == 1024
    assert config.spdk_io_segment_bytes == 1024 * 1024
