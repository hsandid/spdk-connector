#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
# Boot a QEMU guest with a vfio-user KV SSD for in-guest SPDK validation.

set -euo pipefail

: "${GUEST_IMAGE:?Set GUEST_IMAGE to a bootable qcow2 guest image}"
: "${VFU_KVSSD:?Set VFU_KVSSD to the released vfu_kvssd static binary}"

qemu_system="${QEMU_SYSTEM:-qemu-system-x86_64}"
guest_memory="${GUEST_MEMORY:-4G}"
guest_cpus="${GUEST_CPUS:-4}"
guest_ssh_port="${GUEST_SSH_PORT:-2222}"
guest_image_format="${GUEST_IMAGE_FORMAT:-qcow2}"
kv_capacity="${VFU_KVSSD_CAPACITY:-2G}"
mount_source="${MOUNT_SOURCE:-}"
artifact_source="${ARTIFACT_SOURCE:-}"
cloud_init_image="${CLOUD_INIT_IMAGE:-}"
cloud_init_dir="${CLOUD_INIT_DIR:-}"
cloud_init_port="${CLOUD_INIT_PORT:-8000}"
network_mode="${NETWORK_MODE:-none}"
passt_binary="${PASST_BINARY:-}"
serial_socket="${SERIAL_SOCKET:-}"
runtime_dir="$(mktemp -d)"
socket_path="${runtime_dir}/vfu_kvssd.sock"

cleanup() {
    if [[ -n "${kvssd_pid:-}" ]]; then
        kill "${kvssd_pid}" 2>/dev/null || true
        wait "${kvssd_pid}" 2>/dev/null || true
    fi
    if [[ -n "${cloud_init_pid:-}" ]]; then
        kill "${cloud_init_pid}" 2>/dev/null || true
        wait "${cloud_init_pid}" 2>/dev/null || true
    fi
    rm -rf "${runtime_dir}"
}
trap cleanup EXIT INT TERM

if [[ ! -x "${VFU_KVSSD}" ]]; then
    printf 'VFU_KVSSD is not executable: %s\n' "${VFU_KVSSD}" >&2
    exit 2
fi
if [[ ! -f "${GUEST_IMAGE}" ]]; then
    printf 'GUEST_IMAGE does not exist: %s\n' "${GUEST_IMAGE}" >&2
    exit 2
fi
if [[ -n "${cloud_init_dir}" && ! -f "${cloud_init_dir}/user-data" ]]; then
    printf 'CLOUD_INIT_DIR must contain user-data: %s\n' "${cloud_init_dir}" >&2
    exit 2
fi
if [[ -n "${cloud_init_image}" && ! -f "${cloud_init_image}" ]]; then
    printf 'CLOUD_INIT_IMAGE does not exist: %s\n' "${cloud_init_image}" >&2
    exit 2
fi
if [[ "${network_mode}" != "none" && "${network_mode}" != "user" && "${network_mode}" != "passt" ]]; then
    printf 'NETWORK_MODE must be user, passt, or none\n' >&2
    exit 2
fi
if [[ -n "${passt_binary}" && ! -x "${passt_binary}" ]]; then
    printf 'PASST_BINARY is not executable: %s\n' "${passt_binary}" >&2
    exit 2
fi
qemu_version="$(${qemu_system} --version | awk 'NR == 1 { print $4 }')"
if [[ ! "${qemu_version}" =~ ^([0-9]+)\.([0-9]+) ]]; then
    printf 'Could not determine QEMU version: %s\n' "${qemu_system}" >&2
    exit 2
fi
if (( BASH_REMATCH[1] < 10 || (BASH_REMATCH[1] == 10 && BASH_REMATCH[2] < 1) )); then
    printf 'QEMU 10.1 or newer is required; found %s\n' "${qemu_version}" >&2
    exit 2
fi
if ! "${qemu_system}" -device vfio-user-pci,help >/dev/null 2>&1; then
    printf 'QEMU lacks vfio-user-pci; use QEMU 10.1 or newer: %s\n' "${qemu_system}" >&2
    exit 2
fi

"${VFU_KVSSD}" --socket "${socket_path}" --capacity "${kv_capacity}" &
kvssd_pid=$!
for _ in $(seq 1 100); do
    [[ -S "${socket_path}" ]] && break
    sleep 0.05
done
if [[ ! -S "${socket_path}" ]]; then
    printf 'vfu_kvssd did not create its socket\n' >&2
    exit 1
fi

qemu_args=(
    -machine "q35,accel=kvm,memory-backend=mem"
    -object "memory-backend-memfd,id=mem,size=${guest_memory},share=on"
    -cpu host
    -smp "${guest_cpus}"
    -drive "file=${GUEST_IMAGE},format=${guest_image_format},if=virtio"
    -device "{\"driver\":\"vfio-user-pci\",\"socket\":{\"path\":\"${socket_path}\",\"type\":\"unix\"}}"
)
if [[ -n "${serial_socket}" ]]; then
    qemu_args+=(
        -display none
        -monitor none
        -chardev "socket,id=serial,path=${serial_socket},server=on,wait=off"
        -serial chardev:serial
    )
else
    qemu_args+=(
        -nographic
    )
fi
if [[ "${network_mode}" == "user" ]]; then
    qemu_args+=(
        -netdev "user,id=net0,hostfwd=tcp::${guest_ssh_port}-:22"
        -device virtio-net-pci,netdev=net0
    )
elif [[ "${network_mode}" == "passt" ]]; then
    if [[ -z "${passt_binary}" ]]; then
        printf 'Set PASST_BINARY when NETWORK_MODE=passt\n' >&2
        exit 2
    fi
    export PATH="$(dirname "${passt_binary}"):${PATH}"
    qemu_args+=(
        -netdev "passt,id=net0"
        -device virtio-net-pci,netdev=net0
    )
else
    qemu_args+=(
        -nic none
    )
fi
if [[ -n "${mount_source}" ]]; then
    qemu_args+=(
        -virtfs "local,path=${mount_source},mount_tag=lmcache,security_model=none,readonly=on"
    )
fi
if [[ -n "${artifact_source}" ]]; then
    qemu_args+=(
        -virtfs "local,path=${artifact_source},mount_tag=lmcache-artifacts,security_model=none,readonly=on"
    )
fi
if [[ -n "${cloud_init_image}" ]]; then
    qemu_args+=(
        -drive "file=${cloud_init_image},format=raw,if=virtio,readonly=on"
    )
fi
if [[ -n "${cloud_init_dir}" ]]; then
    python3 -m http.server "${cloud_init_port}" --bind 0.0.0.0 --directory "${cloud_init_dir}" &
    cloud_init_pid=$!
    qemu_args+=(
        -smbios "type=1,serial=ds=nocloud-net;s=http://10.0.2.2:${cloud_init_port}/"
    )
fi

if [[ "${network_mode}" == "user" ]]; then
    printf 'Guest SSH will be available on localhost:%s\n' "${guest_ssh_port}"
else
    printf 'Guest serial console is active; network mode: %s\n' "${network_mode}"
fi
"${qemu_system}" "${qemu_args[@]}"
