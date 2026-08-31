# SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Tests for the native-plugin Python factory contract."""

from __future__ import annotations

import sys
from types import SimpleNamespace

import pytest

from lmcache_spdk_connector_kv.connector import SpdkNvmeKvConnector


@pytest.mark.parametrize(
    ("kwargs", "message"),
    [
        ({"pci_bdf": ""}, "pci_bdf"),
        ({"pci_bdf": "0000:00:05.0", "num_workers": 0}, "num_workers"),
        (
            {"pci_bdf": "0000:00:05.0", "hugepage_memory_mb": 0},
            "hugepage_memory_mb",
        ),
    ],
)
def test_rejects_invalid_native_connector_options(
    kwargs: dict[str, object], message: str
) -> None:
    with pytest.raises(ValueError, match=message):
        SpdkNvmeKvConnector(**kwargs)


def test_forwards_native_plugin_options(monkeypatch: pytest.MonkeyPatch) -> None:
    received: list[tuple[object, ...]] = []

    class FakeNativeConnector:
        def __init__(self, *args: object) -> None:
            received.append(args)

    monkeypatch.setitem(
        sys.modules,
        "lmcache_spdk_connector_kv._native",
        SimpleNamespace(SpdkNvmeKvConnector=FakeNativeConnector),
    )

    result = SpdkNvmeKvConnector(
        "0000:00:05.0",
        num_workers=2,
        hugepage_memory_mb=128,
        trace_events=True,
    )

    assert isinstance(result, FakeNativeConnector)
    assert received == [("0000:00:05.0", 2, 128, True)]
