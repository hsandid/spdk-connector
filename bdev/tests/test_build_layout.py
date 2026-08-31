# SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Regression checks for the out-of-tree source boundary."""

from __future__ import annotations

from pathlib import Path


def test_build_uses_vendored_spdk_implementation_only() -> None:
    """The extension may consume generic LMCache headers, never SPDK sources."""
    root = Path(__file__).parent.parent
    setup_source = (root / "setup.py").read_text(encoding="utf-8")

    assert "storage_backends/spdk" not in setup_source
    assert 'LMCACHE_TAG = "v0.5.4"' in setup_source
    assert 'SPDK_TAG = "v26.05"' in setup_source
    assert '"csrc/spdk/spdk_runtime.cpp"' in setup_source
    assert '"csrc/spdk/spdk_buffer_pool.cpp"' in setup_source
    assert '"csrc/spdk/spdk_bdev_context.cpp"' in setup_source
    assert '"csrc/spdk_connector.cpp"' in setup_source
    assert '"csrc/spdk_pybind_common.cpp"' in setup_source
    assert '"csrc/page_extent_allocator.cpp"' in setup_source
    assert (root / "csrc" / "page_extent_allocator.h").is_file()
    assert (root / "csrc" / "spdk_pybind_common.h").is_file()
    assert "PYBIND11_MODULE(_native" in (
        root / "csrc" / "native_engine.cpp"
    ).read_text(encoding="utf-8")
    assert "PYBIND11_MODULE" not in (
        root / "csrc" / "spdk_connector.cpp"
    ).read_text(encoding="utf-8")
    for name in (
        "spdk_config.h",
        "spdk_runtime.h",
        "spdk_runtime.cpp",
        "spdk_buffer_pool.h",
        "spdk_buffer_pool.cpp",
        "spdk_bdev_context.h",
        "spdk_bdev_context.cpp",
        "storage_geometry.h",
    ):
        assert (root / "csrc" / "spdk" / name).is_file()
