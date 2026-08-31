# SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Configuration validation for the native SPDK bdev L2 plugin."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any
import json
import os

from lmcache.v1.distributed.l2_adapters.config import L2AdapterConfigBase


def _positive_int(value: object, field: str) -> int:
    """Reject values that cannot safely size native pools or bdev commands."""
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise ValueError(f"{field} must be a positive integer")
    return value


def _cpu_list(value: object, field: str) -> tuple[int, ...]:
    """Validate a unique CPU partition available to the current process.

    Checking ``sched_getaffinity`` makes container or service cpusets an
    explicit deployment boundary before the connector starts worker threads.
    """
    if not isinstance(value, list) or not value:
        raise ValueError(f"{field} must be a non-empty list of CPU IDs")
    if any(
        isinstance(cpu, bool) or not isinstance(cpu, int) or cpu < 0 for cpu in value
    ):
        raise ValueError(f"{field} must contain non-negative integer CPU IDs")
    cpus = tuple(value)
    if len(set(cpus)) != len(cpus):
        raise ValueError(f"{field} must not contain duplicate CPU IDs")
    allowed = os.sched_getaffinity(0)
    unavailable = sorted(set(cpus) - allowed)
    if unavailable:
        raise ValueError(
            f"{field} contains CPUs unavailable to this process: {unavailable}"
        )
    return cpus


@dataclass(frozen=True)
class SpdkConnectorConfig(L2AdapterConfigBase):
    """Validated configuration for the external native bdev connector."""

    spdk_json_config: str
    bdev_name: str
    spdk_reactor_cpus: tuple[int, ...]
    connector_worker_cpus: tuple[int, ...]
    spdk_io_thread_count: int
    spdk_buffer_count: int
    spdk_memory_size_mb: int = 1024
    spdk_no_huge: bool = True
    spdk_no_pci: bool = False
    spdk_io_segment_bytes: int = 1024 * 1024

    @classmethod
    def help(cls) -> str:
        """Return the external package's plugin configuration help."""
        return (
            "LMCache SPDK connector plugin fields:\n"
            "- spdk_json_config (str): SPDK bdev JSON file\n"
            "- bdev_name (str): bdev name from the JSON file\n"
            "- spdk_reactor_cpus (list[int]): required SPDK reactor CPUs\n"
            "- connector_worker_cpus (list[int]): required, disjoint L2 worker CPUs\n"
            "- spdk_io_thread_count (int): bdev channel owners\n"
            "- spdk_buffer_count (int): DMA staging buffers per I/O worker\n"
            "- spdk_memory_size_mb (int, default 1024): SPDK memory reservation\n"
            "- spdk_no_huge (bool, default true): disable SPDK hugepages\n"
            "- spdk_no_pci (bool, default false): disable PCI probing\n"
            "- spdk_io_segment_bytes (int, default 1048576): maximum bdev command size\n"
        )

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> "SpdkConnectorConfig":
        """Parse native-plugin parameters before startup.

        This validates process-visible CPU partitions and SPDK geometry inputs;
        bdev block geometry and the effective I/O segment limit are discovered
        later by the native engine after opening the selected bdev.
        """
        raw_path = data.get("spdk_json_config")
        if not isinstance(raw_path, str) or not raw_path.strip():
            raise ValueError("spdk_json_config must be a non-empty string")
        config_path = Path(raw_path).expanduser().resolve()
        if not config_path.is_file():
            raise ValueError(f"SPDK JSON configuration does not exist: {config_path}")
        try:
            if not isinstance(
                json.loads(config_path.read_text(encoding="utf-8")), dict
            ):
                raise ValueError("SPDK JSON configuration must contain a JSON object")
        except json.JSONDecodeError as error:
            raise ValueError(
                f"cannot parse SPDK JSON configuration: {error}"
            ) from error

        bdev_name = data.get("bdev_name")
        if not isinstance(bdev_name, str) or not bdev_name.strip():
            raise ValueError("bdev_name must be a non-empty string")
        reactor_cpus = _cpu_list(data.get("spdk_reactor_cpus"), "spdk_reactor_cpus")
        worker_cpus = _cpu_list(
            data.get("connector_worker_cpus"), "connector_worker_cpus"
        )
        overlap = sorted(set(reactor_cpus) & set(worker_cpus))
        if overlap:
            raise ValueError(
                f"SPDK reactor and connector worker CPUs overlap: {overlap}"
            )
        io_threads = _positive_int(
            data.get("spdk_io_thread_count"), "spdk_io_thread_count"
        )
        if io_threads > len(reactor_cpus):
            raise ValueError("spdk_io_thread_count must not exceed spdk_reactor_cpus")
        no_huge = data.get("spdk_no_huge", True)
        no_pci = data.get("spdk_no_pci", False)
        if not isinstance(no_huge, bool) or not isinstance(no_pci, bool):
            raise ValueError("spdk_no_huge and spdk_no_pci must be booleans")
        return cls(
            spdk_json_config=str(config_path),
            bdev_name=bdev_name.strip(),
            spdk_reactor_cpus=reactor_cpus,
            connector_worker_cpus=worker_cpus,
            spdk_io_thread_count=io_threads,
            spdk_buffer_count=_positive_int(
                data.get("spdk_buffer_count", 16), "spdk_buffer_count"
            ),
            spdk_memory_size_mb=_positive_int(
                data.get("spdk_memory_size_mb", 1024), "spdk_memory_size_mb"
            ),
            spdk_no_huge=no_huge,
            spdk_no_pci=no_pci,
            spdk_io_segment_bytes=_positive_int(
                data.get("spdk_io_segment_bytes", 1024 * 1024), "spdk_io_segment_bytes"
            ),
        )
