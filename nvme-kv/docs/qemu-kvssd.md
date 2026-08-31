# QEMU KVSSD Functional Test

This operational guide runs the native NVMe-KV connector's functional smoke
test against
[vfio-user-kvssd](https://github.com/safl/vfio-user-kvssd) in QEMU. QEMU does
not provide a native NVMe Key-Value SSD device, so this custom vfio-user
project presents a virtual PCIe NVMe controller with a Key-Value namespace to
the guest. It is functional coverage only, not a performance or persistence
test.

Complete the [package Quickstart](../README.md#quickstart) first. It builds the
connector and exports `LMCACHE_SOURCE_ROOT` and `SPDK_ROOT` for the commands
below.

## Prerequisites

The host needs Linux, a working KVM setup, QEMU 10.1 or newer with the
`vfio-user-pci` device, a C++17 compiler, `pkg-config`, `socat`, Python 3.10+
and `pybind11`. The SPDK checkout must have been configured and built with
shared libraries.

Verify QEMU support before continuing:

```sh
qemu-system-x86_64 --version
qemu-system-x86_64 -device vfio-user-pci,help
```

The native POC stores one LMCache L2 object in one NVMe-KV value. Configure
the LMCache object size at or below the namespace `kvvml` value.

## Prepare QEMU

Create an artifact directory and fetch the pinned guest image

```sh
export ARTIFACT_DIR=/tmp/lmcache-kv-artifacts
mkdir -p "${ARTIFACT_DIR}"

oras pull ghcr.io/safl/nosi/ubuntu-2604-headless:2026.W32 \
  -o "${ARTIFACT_DIR}"

cd "${ARTIFACT_DIR}"
read -r expected_digest _ < nosi-ubuntu-2604-headless-x86_64.img.gz.sha256
actual_digest="$(sha256sum nosi-ubuntu-2604-headless-x86_64.img.gz)"
test "${actual_digest%% *}" = "${expected_digest}"
gzip -dk nosi-ubuntu-2604-headless-x86_64.img.gz
```

Download `vfu_kvssd` release `v0.1.11` and verify its published SHA-256:

```sh
export VFU_KVSSD="${ARTIFACT_DIR}/vfu_kvssd-0.1.11-x86_64-linux"
curl -fL \
  -o "${VFU_KVSSD}" \
  https://github.com/safl/vfio-user-kvssd/releases/download/v0.1.11/vfu_kvssd-0.1.11-x86_64-linux
test "$(sha256sum "${VFU_KVSSD}" | cut -d ' ' -f 1)" = \
  45bf83758b2afc0b08dd097cb079ec3b00f4fcd0c1c10181510e679ecb202cdd
chmod +x "${VFU_KVSSD}"
```

Build the native LMCache-contract smoke executable on the host.

```sh
cd /path/to/work/lmcache-spdk-connector-kv
LMCACHE_SOURCE_ROOT="${LMCACHE_SOURCE_ROOT}" SPDK_ROOT="${SPDK_ROOT}" \
  bash tests/functional/build_lmcache_native_kv_smoke.sh
```

Run the smoke test. It starts the virtual device, boots Nosi without guest
networking, reserves 64 MiB hugepages, transfers the controller from `nvme`
to `uio_pci_generic`, and powers off after completion.

```sh
export TRACE_LOG="${ARTIFACT_DIR}/lmcache-native-kv-smoke.log"

GUEST_IMAGE_FORMAT=raw \
GUEST_IMAGE="${ARTIFACT_DIR}/nosi-ubuntu-2604-headless-x86_64.img" \
VFU_KVSSD="${VFU_KVSSD}" \
NETWORK_MODE=none \
TRACE_LOG="${TRACE_LOG}" \
bash tests/functional/run_lmcache_native_kv_qemu.sh
```

The test performs a 4 KiB native batch SET, EXISTS, GET with byte-for-byte
comparison, DELETE, and final missing EXISTS. Successful output ends with:

```text
LMCache native NVMe-KV smoke test passed (4096 bytes)
```

## Inspect The Trace

`TRACE_LOG` retains the guest serial transcript.
`LMCACHE_KV_EVENT` records contain a native future ID, worker ID, byte count,
success state, operation, and a four-byte digest prefix of the key.

```sh
rg 'LMCACHE_KV_EVENT|LMCache native NVMe-KV smoke test passed' "${TRACE_LOG}"
```

The expected causal sequence is:

```text
future 1: submit set -> io_begin store -> io_end store -> complete
future 2: submit exists -> io_end exist present -> complete
future 3: submit get -> io_begin retrieve -> io_end retrieve -> complete
future 4: submit delete -> io_end delete -> complete
future 5: submit exists -> io_end exist absent -> complete
```
