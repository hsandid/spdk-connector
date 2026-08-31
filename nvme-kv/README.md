# Quickstart

- `lmcache-spdk-connector-kv` is an out-of-tree LMCache L2 plugin for an NVMe
Key-Value namespace.
- It manages the SPDK NVMe controller lifecycle itself and
issues `spdk_nvme_kv_*` commands directly; it does not use the SPDK bdev API.

Build instructions:

```sh
export LMCACHE_SOURCE_ROOT=/path/to/LMCache
export SPDK_ROOT=/path/to/spdk
git -C "${LMCACHE_SOURCE_ROOT}" checkout v0.5.4
git -C "${SPDK_ROOT}" checkout v26.05
make venv
make build
make test-all

# Required whenever Python loads the native extension outside `make test`
export LD_LIBRARY_PATH="${SPDK_ROOT}/build/lib:${SPDK_ROOT}/dpdk/build/lib:${SPDK_ROOT}/isa-l/.libs:${SPDK_ROOT}/isa-l-crypto/.libs${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
```

- `make build` installs LMCache with GPU support when its build dependencies are
available, builds the native extension, and installs this package into `.venv`.
- Use `make build NO_GPU_EXT=1` for a CPU-only LMCache build.

Configure LMCache's `native_plugin` adapter as follows:

```json
{
  "type": "native_plugin",
  "module_path": "lmcache_spdk_connector_kv",
  "class_name": "SpdkNvmeKvConnector",
  "adapter_params": {
    "pci_bdf": "0000:00:05.0",
    "num_workers": 1,
    "hugepage_memory_mb": 64,
    "trace_events": false
  }
}
```

| Option | Required/default | Description |
| --- | --- | --- |
| `adapter_params.pci_bdf` | Required | PCIe BDF of the NVMe-KV controller visible to the process. |
| `adapter_params.num_workers` | `1` | Number of ConnectorBase workers and independently owned SPDK I/O qpairs. |
| `adapter_params.hugepage_memory_mb` | `64` | Hugepage memory reserved by the SPDK environment. |
| `adapter_params.trace_events` | `false` | Emits payload-free `LMCACHE_KV_EVENT` JSON records to stderr. |

## Main Architecture & Design Aspects

- **Direct NVMe-KV commands.** The native connector initializes `spdk_env`,
  attaches the PCIe controller, selects an NVMe-KV namespace, and submits
  `STORE`, `RETRIEVE`, `EXIST`, and `DELETE` commands through SPDK I/O qpairs.
  No bdev JSON or bdev interface is involved.
- **One environment per process.** The connector owns process-global SPDK
  environment state and permits only one NVMe-KV connector per process.
- **Worker-owned qpairs and DMA buffers.** Each ConnectorBase worker owns one
  qpair, submits and polls its own commands, and uses a 4 KiB-aligned
  `spdk_dma_zmalloc` buffer to stage each store or load.
- **Device key and value limits.** LMCache keys are SHA-256 domain-separated
   and truncated to the 16-byte NVMe-KV device-key limit. The current native POC
   maps one LMCache object to one device value, so an object must not exceed the
   namespace-reported `kvvml` maximum value length.

For a detailed description of controller setup, key mapping, worker ownership,
and command flows, see the [architecture guide](docs/architecture.md).

# Tests

Tests cover the Python key and manifest format, native-plugin option validation,
and QEMU KVSSD functional smoke coverage for native SET, EXISTS, GET, DELETE,
and final missing EXISTS. See [the QEMU KVSSD guide](docs/qemu-kvssd.md)
to run the functional harness.

No benchmarks or performance tests were conducted against the NVMe-KV connector PoC,
only functional tests.

# Current Limitations / Future Work

- **Single-value native POC.** The native connector currently stores one
  LMCache object in one device value and rejects objects larger than `kvvml`.
- **PCIe-only attachment.** The current configuration accepts only `pci_bdf`;
  NVMe-oF transport configuration is future work.
- **No zero-copy path.** LMCache buffers are copied through per-operation DMA
  staging buffers because they are not assumed to meet SPDK DMA requirements.
