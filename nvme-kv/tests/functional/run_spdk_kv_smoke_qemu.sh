#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
# Run the package-owned SPDK NVMe-KV smoke test inside a Nosi QEMU guest.

set -euo pipefail

: "${GUEST_IMAGE:?Set GUEST_IMAGE to a Nosi raw guest image}"
: "${VFU_KVSSD:?Set VFU_KVSSD to the released vfu_kvssd static binary}"

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
workspace_root="$(cd "${script_dir}/../../.." && pwd)"
launcher="${script_dir}/run_vfio_user_qemu.sh"
smoke_binary="${script_dir}/spdk_kv_smoke"
smoke_binary_name="${SMOKE_BINARY_NAME:-spdk_kv_smoke}"
smoke_success_marker="${SMOKE_SUCCESS_MARKER:-SPDK NVMe-KV smoke test passed}"
system_library_dir="${SYSTEM_LIBRARY_DIR:-/lib/x86_64-linux-gnu}"
serial_socket="$(mktemp -u)"
serial_log="$(mktemp)"
qemu_log="$(mktemp)"
login_delay="${SERIAL_LOGIN_DELAY:-25}"
guest_user="${NOSI_USER:-odus}"
guest_password="${NOSI_PASSWORD:-odus.321}"
trace_log="${TRACE_LOG:-}"
status=0

cleanup() {
    if [[ -n "${trace_log}" && -f "${serial_log}" ]]; then
        cp "${serial_log}" "${trace_log}"
    fi
    rm -f "${serial_socket}" "${serial_log}" "${qemu_log}"
}
trap cleanup EXIT INT TERM

if [[ ! "${smoke_binary_name}" =~ ^[A-Za-z0-9._-]+$ ]]; then
    printf 'SMOKE_BINARY_NAME contains unsupported characters: %s\n' "${smoke_binary_name}" >&2
    exit 2
fi
smoke_binary="${script_dir}/${smoke_binary_name}"
if [[ ! -x "${smoke_binary}" ]]; then
    printf 'Build the host smoke executable first: %s\n' "${smoke_binary}" >&2
    exit 2
fi
if [[ ! -f "${system_library_dir}/libfuse3.so.3" ]]; then
    printf 'SYSTEM_LIBRARY_DIR must contain libfuse3.so.3: %s\n' "${system_library_dir}" >&2
    exit 2
fi
if [[ -n "${trace_log}" && ! -d "$(dirname "${trace_log}")" ]]; then
    printf 'TRACE_LOG parent directory does not exist: %s\n' "${trace_log}" >&2
    exit 2
fi

guest_command="mkdir -p /mnt/lmcache /mnt/lmcache-artifacts && mount -t 9p -o trans=virtio,version=9p2000.L lmcache /mnt/lmcache && mount -t 9p -o trans=virtio,version=9p2000.L lmcache-artifacts /mnt/lmcache-artifacts && sysctl -w vm.nr_hugepages=32 && BDF=\$(lspci -d 1b36:0010 -D | awk '{print \$1}') && test -n \"\${BDF}\" && modprobe uio_pci_generic && printf '%s\\n' \"\${BDF}\" > /sys/bus/pci/devices/\${BDF}/driver/unbind && printf 'uio_pci_generic\\n' > /sys/bus/pci/devices/\${BDF}/driver_override && printf '%s\\n' \"\${BDF}\" > /sys/bus/pci/drivers/uio_pci_generic/bind && setpci -s \"\${BDF}\" COMMAND=0x06 && readlink -f /sys/bus/pci/devices/\${BDF}/driver && test -d /sys/bus/pci/devices/\${BDF}/uio && LD_PRELOAD=/mnt/lmcache-artifacts/libfuse3.so.3 LD_LIBRARY_PATH=/mnt/lmcache/spdk/build/lib:/mnt/lmcache/spdk/dpdk/build/lib:/mnt/lmcache/spdk/isa-l/.libs:/mnt/lmcache/spdk/isa-l-crypto/.libs /mnt/lmcache/lmcache-spdk-connector-kv/tests/functional/${smoke_binary_name} \"\${BDF}\"; status=\$?; printf 'LMCache SPDK KV smoke exit: %s\\n' \"\${status}\"; poweroff; exit \"\${status}\""

SERIAL_SOCKET="${serial_socket}" MOUNT_SOURCE="${workspace_root}" ARTIFACT_SOURCE="${system_library_dir}" \
    timeout 90s "${launcher}" >"${qemu_log}" 2>&1 &
qemu_pid=$!

for _ in $(seq 1 100); do
    [[ -S "${serial_socket}" ]] && break
    sleep 0.1
done
if [[ ! -S "${serial_socket}" ]]; then
    printf 'QEMU did not create the serial socket\n' >&2
    status=1
else
    { sleep "${login_delay}"; printf '%s\r' "${guest_user}"; sleep 1; printf '%s\r' "${guest_password}"; sleep 3; printf 'sudo -i\r'; sleep 1; printf '%s\r' "${guest_command}"; sleep 55; } |
        timeout 90s socat - "UNIX-CONNECT:${serial_socket}" | tee "${serial_log}" || status=1
fi

wait "${qemu_pid}" || status=1
if ! rg -F -q "${smoke_success_marker}" "${serial_log}"; then
    printf 'Guest did not report a successful smoke test: %s\n' "${smoke_success_marker}" >&2
    status=1
fi
if (( status != 0 )); then
    printf 'QEMU and vfio-user log:\n' >&2
    readarray -t qemu_output < "${qemu_log}"
    printf '%s\n' "${qemu_output[@]}" >&2
fi
exit "${status}"
