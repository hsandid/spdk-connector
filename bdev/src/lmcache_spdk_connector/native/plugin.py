# SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Factory for LMCache's generic ``native_plugin`` L2 adapter."""

from __future__ import annotations

import os
import threading

from ..config import SpdkConnectorConfig


_runtime_lock = threading.Lock()
_runtime_settings: tuple[object, ...] | None = None


def _guard_runtime(config: SpdkConnectorConfig) -> None:
    """Freeze one compatible external SPDK runtime identity for this process.

    The environment sentinel prevents a declared in-tree runtime from sharing
    this package's process-wide SPDK application. Settings intentionally remain
    claimed after a connector closes because SPDK application initialization is
    process-scoped and cannot be recreated safely.
    """
    owner = os.environ.get("LMCACHE_SPDK_RUNTIME_OWNER")
    if owner not in (None, "external"):
        raise RuntimeError(
            "this process declares an in-tree SPDK runtime; external and in-tree "
            "SPDK adapters cannot share a process"
        )
    settings = (
        config.spdk_json_config,
        config.spdk_reactor_cpus,
        config.spdk_memory_size_mb,
        config.spdk_no_huge,
        config.spdk_no_pci,
        config.spdk_io_thread_count,
    )
    global _runtime_settings
    with _runtime_lock:
        if _runtime_settings is not None and _runtime_settings != settings:
            raise RuntimeError(
                "all external SPDK connectors in one process must use identical "
                "runtime settings"
            )
        _runtime_settings = settings
        os.environ["LMCACHE_SPDK_RUNTIME_OWNER"] = "external"


class SpdkNativeConnector:
    """Create the package-owned C++ ephemeral extent SPDK connector.

    All arguments are the ``adapter_params`` documented by
    :class:`SpdkConnectorConfig`. ``connector_worker_cpus`` pins each generic
    connector worker through the package-owned connection setup hook.
    """

    def __new__(cls, **adapter_params: object):
        """Validate native-plugin parameters and return the pybind connector.

        Raises:
            ValueError: If the configuration is invalid.
            RuntimeError: If the package extension has not been built.
        """
        config = SpdkConnectorConfig.from_dict(adapter_params)
        _guard_runtime(config)
        try:
            from .._native import SpdkConnector
        except ImportError as error:
            raise RuntimeError(
                "lmcache-spdk-connector native extension is unavailable. Build "
                "this package against LMCache v0.5.4 and the supported SPDK source."
            ) from error
        return SpdkConnector(
            spdk_json_config=config.spdk_json_config,
            bdev_name=config.bdev_name,
            reactor_cpus=list(config.spdk_reactor_cpus),
            worker_cpus=list(config.connector_worker_cpus),
            spdk_io_thread_count=config.spdk_io_thread_count,
            spdk_buffer_count=config.spdk_buffer_count,
            spdk_memory_size_mb=config.spdk_memory_size_mb,
            spdk_no_huge=config.spdk_no_huge,
            spdk_no_pci=config.spdk_no_pci,
            spdk_io_segment_bytes=config.spdk_io_segment_bytes,
        )
