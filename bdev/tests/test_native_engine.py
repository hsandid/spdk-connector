# SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
from __future__ import annotations

from pathlib import Path
import os
import subprocess
import sys

import pytest


def _run_smoke_script(name: str) -> None:
    """Run one SPDK smoke script in a fresh process."""
    environment = os.environ.copy()
    source_root = str(Path(__file__).parent.parent / "src")
    environment["PYTHONPATH"] = (
        source_root + os.pathsep + environment.get("PYTHONPATH", "")
    )
    script = Path(__file__).parent / "smoke" / name
    subprocess.run([sys.executable, str(script)], check=True, env=environment)


@pytest.mark.skipif(
    os.environ.get("RUN_SPDK_SMOKE") != "1",
    reason="requires an SPDK-capable host and 1 GiB SPDK memory reservation",
)
def test_native_connector_extents_complete_through_generic_contract() -> None:
    """Verify native ephemeral extent placement through the generic contract."""
    _run_smoke_script("native_connector.py")


@pytest.mark.skipif(
    os.environ.get("RUN_SPDK_SMOKE") != "1",
    reason="requires an SPDK-capable host and 1 GiB SPDK memory reservation",
)
def test_native_connectors_share_one_runtime_across_bdevs() -> None:
    """Verify distinct bdev contexts coexist in SPDK's one process runtime."""
    _run_smoke_script("multiple_bdevs.py")


@pytest.mark.skipif(
    os.environ.get("RUN_SPDK_SMOKE") != "1",
    reason="requires an SPDK-capable host and 1 GiB SPDK memory reservation",
)
def test_native_plugin_bridge_stores_and_loads_multiple_keys() -> None:
    """Verify LMCache's native-plugin bridge over one multi-key native batch."""
    _run_smoke_script("native_plugin.py")
