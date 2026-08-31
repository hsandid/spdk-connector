# SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
"""LMCache native-plugin factory for direct NVMe Key-Value I/O."""

from __future__ import annotations


class SpdkNvmeKvConnector:
    """Create the package-owned direct SPDK NVMe-KV native connector.

    Args:
        pci_bdf: PCIe BDF of the NVMe-KV controller visible to the process.
        num_workers: Number of independent SPDK qpair workers.
        hugepage_memory_mb: DPDK hugepage allocation for the SPDK environment.
        trace_events: Emit ``LMCACHE_KV_EVENT`` JSON lines to stderr.

    The POC maps one LMCache L2 object to one KV value. Its configured object
    size must not exceed the namespace's advertised maximum value length.
    """

    def __new__(
        cls,
        pci_bdf: str,
        num_workers: int = 1,
        hugepage_memory_mb: int = 64,
        trace_events: bool = False,
    ):
        """Return the pybind connector instance expected by LMCache."""
        if not isinstance(pci_bdf, str) or not pci_bdf:
            raise ValueError("pci_bdf must be a non-empty string")
        if num_workers <= 0:
            raise ValueError("num_workers must be positive")
        if hugepage_memory_mb <= 0:
            raise ValueError("hugepage_memory_mb must be positive")

        from ._native import SpdkNvmeKvConnector as NativeConnector

        return NativeConnector(
            pci_bdf,
            num_workers,
            hugepage_memory_mb,
            trace_events,
        )
