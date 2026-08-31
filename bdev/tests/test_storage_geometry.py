# SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Build and run storage geometry checks without an SPDK install."""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess

import pytest


def test_storage_geometry_applies_the_bdev_maximum_io_limit(tmp_path: Path) -> None:
    """Verify configured segments are capped and aligned to bdev geometry."""
    compiler = shutil.which("g++")
    if compiler is None:
        pytest.skip("g++ is required for the C++ geometry test")

    root = Path(__file__).parent.parent
    executable = tmp_path / "storage_geometry_test"
    subprocess.run(
        [
            compiler,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(root / "csrc" / "spdk"),
            str(root / "csrc" / "storage_geometry.cpp"),
            str(root / "tests" / "cpp" / "storage_geometry_test.cpp"),
            "-o",
            str(executable),
        ],
        check=True,
    )
    subprocess.run([str(executable)], check=True)
