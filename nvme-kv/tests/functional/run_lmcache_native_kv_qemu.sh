#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
# Run the LMCache native-connector SPDK KV smoke test in QEMU.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SMOKE_BINARY_NAME=lmcache_native_kv_smoke \
    SMOKE_SUCCESS_MARKER='LMCache native NVMe-KV smoke test passed' \
    exec bash "${script_dir}/run_spdk_kv_smoke_qemu.sh"
