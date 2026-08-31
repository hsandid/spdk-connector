# SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Build and run the ephemeral C++ page allocator without an SPDK install."""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess

import pytest


def test_page_extent_allocator_reuses_fragmented_capacity(tmp_path: Path) -> None:
    compiler = shutil.which("g++")
    if compiler is None:
        pytest.skip("g++ is required for the C++ allocator test")

    root = Path(__file__).parent.parent
    executable = tmp_path / "page_extent_allocator_test"
    subprocess.run(
        [
            compiler,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(root / "csrc"),
            str(root / "csrc" / "page_extent_allocator.cpp"),
            str(root / "tests" / "cpp" / "page_extent_allocator_test.cpp"),
            "-o",
            str(executable),
        ],
        check=True,
    )
    subprocess.run([str(executable)], check=True)
