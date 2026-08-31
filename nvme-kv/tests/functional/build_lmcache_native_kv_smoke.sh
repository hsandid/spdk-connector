#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
# Build the LMCache native-connector smoke executable against SPDK.

set -euo pipefail

: "${LMCACHE_SOURCE_ROOT:?Set LMCACHE_SOURCE_ROOT to the LMCache v0.5.4 source checkout}"
: "${SPDK_ROOT:?Set SPDK_ROOT to the pinned SPDK checkout}"

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
package_root="$(cd "${script_dir}/../.." && pwd)"
lmcache_headers="${LMCACHE_SOURCE_ROOT}/csrc/storage_backends"
spdk_pkgconfig="${SPDK_ROOT}/build/lib/pkgconfig"
library_dirs=(
    "${SPDK_ROOT}/build/lib"
    "${SPDK_ROOT}/dpdk/build/lib"
    "${SPDK_ROOT}/isa-l/.libs"
    "${SPDK_ROOT}/isa-l-crypto/.libs"
)

for path in "${lmcache_headers}" "${spdk_pkgconfig}" "${library_dirs[@]}"; do
    if [[ ! -d "${path}" ]]; then
        printf 'Required build directory does not exist: %s\n' "${path}" >&2
        exit 2
    fi
done

read -r -a spdk_libraries <<< "$(PKG_CONFIG_PATH="${spdk_pkgconfig}" \
    pkg-config --libs-only-l spdk_nvme spdk_env_dpdk spdk_syslibs)"
if (( ${#spdk_libraries[@]} == 0 )); then
    printf 'SPDK pkg-config returned no linker libraries\n' >&2
    exit 2
fi

link_args=( -Wl,--disable-new-dtags -Wl,--no-as-needed )
for path in "${library_dirs[@]}"; do
    link_args+=( "-L${path}" "-Wl,-rpath,${path}" )
done

"${CXX:-g++}" -std=c++17 -O2 -Wall -Wextra -Werror \
    "${package_root}/csrc/native_kv_connector.cpp" \
    "${script_dir}/lmcache_native_kv_smoke.cpp" \
    -I "${package_root}/csrc" \
    -I "${lmcache_headers}" \
    -I "${SPDK_ROOT}/include" \
    -I "${SPDK_ROOT}/build/include" \
    "${link_args[@]}" \
    "${spdk_libraries[@]}" \
    -o "${script_dir}/lmcache_native_kv_smoke"
