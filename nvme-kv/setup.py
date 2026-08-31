# SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Build the package-owned native NVMe-KV connector against external sources."""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

import pybind11
from setuptools import Extension, find_packages, setup

LMCACHE_TAG = "v0.5.4"
SPDK_TAG = "v26.05"


def _required_root(name: str) -> Path:
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"{name} must name a source checkout")
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
    environment = {**os.environ, "PKG_CONFIG_PATH": str(root / "build/lib/pkgconfig")}
    output = subprocess.check_output(
        [
            "pkg-config",
            "--libs-only-l",
            "spdk_nvme",
            "spdk_env_dpdk",
            "spdk_syslibs",
        ],
        text=True,
        env=environment,
    )
    libraries = [item[2:] for item in output.split() if item.startswith("-l")]
    if not libraries:
        raise RuntimeError("SPDK pkg-config returned no link libraries")
    return libraries


lmcache_root = _required_root("LMCACHE_SOURCE_ROOT")
spdk_root = _required_root("SPDK_ROOT")
_validate_tag("LMCache", lmcache_root, LMCACHE_TAG)
_validate_tag("SPDK", spdk_root, SPDK_TAG)
lmcache_headers = lmcache_root / "csrc/storage_backends"
required_headers = (
    "connector_base.h",
    "connector_interface.h",
    "connector_pybind_utils.h",
    "connector_types.h",
    "event_notifier.h",
)
missing_headers = [
    name for name in required_headers if not (lmcache_headers / name).is_file()
]
if missing_headers:
    raise RuntimeError(
        "LMCache checkout is missing connector ABI headers: "
        + ", ".join(missing_headers)
    )

spdk_library_dirs = [
    spdk_root / "build/lib",
    spdk_root / "dpdk/build/lib",
    spdk_root / "isa-l/.libs",
    spdk_root / "isa-l-crypto/.libs",
]
if any(not path.is_dir() for path in spdk_library_dirs):
    raise RuntimeError("SPDK build is missing shared libraries or pkg-config metadata")

extension = Extension(
    "lmcache_spdk_connector_kv._native",
    sources=["csrc/native_kv_connector.cpp", "csrc/pybind.cpp"],
    include_dirs=[
        "csrc",
        str(lmcache_headers),
        str(spdk_root / "include"),
        str(spdk_root / "build/include"),
        pybind11.get_include(),
    ],
    library_dirs=[str(path) for path in spdk_library_dirs],
    extra_link_args=[
        "-Wl,--no-as-needed",
        *[f"-l{library}" for library in _spdk_link_libraries(spdk_root)],
    ],
    language="c++",
    extra_compile_args=[
        "-O3",
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wno-error=missing-field-initializers",
    ],
)

setup(
    packages=find_packages(where="src"),
    package_dir={"": "src"},
    ext_modules=[extension],
)
