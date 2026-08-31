# SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Build the native SPDK bdev connector against external LMCache and SPDK."""

from __future__ import annotations

from pathlib import Path
import os
import subprocess

from setuptools import Extension, find_packages, setup

import pybind11

LMCACHE_TAG = "v0.5.4"
SPDK_TAG = "v26.05"


def _required_root(name: str) -> Path:
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"{name} must name the pinned source checkout")
    root = Path(value).expanduser().resolve()
    if not root.is_dir():
        raise RuntimeError(f"{name} is not a directory: {root}")
    return root


def _git_revision(root: Path, revision: str = "HEAD") -> str:
    return subprocess.check_output(
        ["git", "-C", str(root), "rev-parse", f"{revision}^{{commit}}"], text=True
    ).strip()


def _validate_tag(name: str, root: Path, tag: str) -> None:
    actual = _git_revision(root)
    try:
        expected = _git_revision(root, tag)
    except subprocess.CalledProcessError as error:
        raise RuntimeError(f"{name} checkout does not contain required tag {tag}") from error
    if actual != expected:
        raise RuntimeError(f"{name} revision is {actual}; check out {tag}")


def _spdk_link_libraries(root: Path) -> list[str]:
    """Return SPDK's generated complete shared-library dependency closure."""
    package_config = root / "build" / "lib" / "pkgconfig"
    command = [
        "pkg-config",
        "--libs-only-l",
        "spdk_bdev",
        "spdk_event",
        "spdk_event_bdev",
        "spdk_env_dpdk",
        "spdk_syslibs",
    ]
    environment = {**os.environ, "PKG_CONFIG_PATH": str(package_config)}
    output = subprocess.check_output(command, text=True, env=environment)
    libraries = [argument[2:] for argument in output.split() if argument.startswith("-l")]
    if not libraries:
        raise RuntimeError("SPDK pkg-config returned no link libraries")
    return libraries


lmcache_root = _required_root("LMCACHE_SOURCE_ROOT")
spdk_root = _required_root("SPDK_ROOT")
_validate_tag("LMCache", lmcache_root, LMCACHE_TAG)
_validate_tag("SPDK", spdk_root, SPDK_TAG)

spdk_lib_dir = spdk_root / "build" / "lib"
if not (spdk_lib_dir / "pkgconfig").is_dir():
    raise RuntimeError(f"SPDK pkg-config metadata is missing: {spdk_lib_dir / 'pkgconfig'}")

lmcache_connector_headers = lmcache_root / "csrc" / "storage_backends"
required_generic_headers = (
    "connector_base.h",
    "connector_interface.h",
    "connector_types.h",
    "connector_pybind_utils.h",
    "event_notifier.h",
)
missing_headers = [
    name for name in required_generic_headers if not (lmcache_connector_headers / name).is_file()
]
if missing_headers:
    raise RuntimeError(
        "LMCache source checkout is missing required generic connector headers: "
        + ", ".join(missing_headers)
    )
spdk_library_dirs = [
    spdk_lib_dir,
    spdk_root / "dpdk" / "build" / "lib",
    spdk_root / "isa-l" / ".libs",
    spdk_root / "isa-l-crypto" / ".libs",
]
if any(not directory.is_dir() for directory in spdk_library_dirs):
    raise RuntimeError("SPDK build is missing required generated dependency libraries")
spdk_libraries = _spdk_link_libraries(spdk_root)
native_sources = [
    "csrc/native_engine.cpp",
    "csrc/spdk_connector.cpp",
    "csrc/spdk_pybind_common.cpp",
    "csrc/page_extent_allocator.cpp",
    "csrc/storage_geometry.cpp",
    "csrc/spdk/spdk_runtime.cpp",
    "csrc/spdk/spdk_buffer_pool.cpp",
    "csrc/spdk/spdk_bdev_context.cpp",
]

extension = Extension(
    "lmcache_spdk_connector._native",
    sources=native_sources,
    include_dirs=[
        "csrc",
        "csrc/spdk",
        str(lmcache_connector_headers),
        str(spdk_root / "include"),
        str(spdk_root / "build" / "include"),
        pybind11.get_include(),
    ],
    library_dirs=[str(directory) for directory in spdk_library_dirs],
    extra_link_args=[
        "-Wl,--no-as-needed",
        *[f"-l{library}" for library in spdk_libraries],
    ],
    language="c++",
    extra_compile_args=["-O3", "-std=c++17", "-Wall", "-Wextra"],
)

setup(
    packages=find_packages(where="src"),
    package_dir={"": "src"},
    ext_modules=[extension],
)
