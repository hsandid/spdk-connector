# Quickstart

`lmcache-spdk-connector` is an out-of-tree native LMCache L2 plugin that supports the
SPDK bdev APIs. 

An SPDK bdev JSON configuration can be provided to the connector to initialize
a local NVMe and/or NVMe-oF bdev configuration in SPDK.

This connector has been tested with LMCache `v0.5.4` and SPDK `v26.05`, and can be built as follow:


```sh
# Fetch dependencies
export LMCACHE_SOURCE_ROOT=/path/to/LMCache
export SPDK_ROOT=/path/to/spdk
git -C "${LMCACHE_SOURCE_ROOT}" checkout v0.5.4
git -C "${SPDK_ROOT}" checkout v26.05

# Build & Test
make venv # Setup venv for build
make build  # Use `make build NO_GPU_EXT=1` if you'd like to test with a CPU-only LMCache build
make test-all # Run all tests

# Required whenever Python loads the native extension outside `make test`
export LD_LIBRARY_PATH="${SPDK_ROOT}/build/lib:${SPDK_ROOT}/dpdk/build/lib:${SPDK_ROOT}/isa-l/.libs:${SPDK_ROOT}/isa-l-crypto/.libs${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

# Cleanup
make clean
```

An example LMCache configuration for the SPDK L2 connector is available in [`extras/config/native-plugin.json`](extras/config/native-plugin.json).

| Option | Required/default | Description |
| --- | --- | --- |
| `adapter_params.spdk_json_config` | Required | Path to SPDK bdev JSON configuration. |
| `adapter_params.bdev_name` | Required | SPDK bdev name declared by JSON configuration. |
| `adapter_params.spdk_reactor_cpus` | Required | Non-empty, unique list of process-available CPUs reserved for SPDK reactors. |
| `adapter_params.connector_worker_cpus` | Required | Non-empty, unique list of process-available CPUs reserved for LMCache connector workers. It must not overlap polling `spdk_reactor_cpus` to avoid contention issues. |
| `adapter_params.spdk_io_thread_count` | Required | Number of bdev channel owners. It must not exceed the number of reactor CPUs. |
| `adapter_params.spdk_buffer_count` | `16` | DMA staging buffers per I/O worker. |
| `adapter_params.spdk_memory_size_mb` | `1024` | SPDK memory reservation in MiB. |
| `adapter_params.spdk_no_huge` | `true` | Disables SPDK hugepages. |
| `adapter_params.spdk_no_pci` | `false` | Disables SPDK PCI probing. |
| `adapter_params.spdk_io_segment_bytes` | `1048576` | Maximum bdev command size in bytes. |

## Main Architecture & Design Aspects

- **CPU and thread ownership.** To avoid contention, ConnectorBase workers
  which handle metadata/request preparation are pinned to dedicated CPUs defined
  in `connector_worker_cpus`, and SPDK reactors and bdev I/O channels are pinned
  to dedicated CPUs defined in `spdk_reactor_cpus`.
- **One SPDK runtime for multiple bdev contexts.** SPDK connector instances
  running in the same LMCache process share a single `SpdkRuntime`, while each
  `SpdkBdevContext` exclusively claims one bdev and owns its descriptor,
  channels, I/O workers, and buffer pools.
- **Support for all LMCache operations**: 'Store'/'Load' operations are forwarded to the SPDK back-end, while 'Lookup' operations
    interact only with the SPDK Connector's in-memory extent allocator  
- **Connector-controlled page and block translation.** `SpdkConnector` rounds
  an LMCache chunk up to storage pages, records the allocated `PageExtent`
  ranges in its in-memory key map, and splits each extent into I/O segments.
  `SpdkBdevContext` converts each segment's connector-calculated byte offset
  and physical length to a block offset and block count using the bdev geometry,
  then calls `spdk_bdev_read_blocks` or `spdk_bdev_write_blocks`. SPDK owns the
  backend-specific mapping and execution after that call; it does not own this
  connector's key-to-page placement.
- **Segmented block I/O.** The effective segment size is the configured
  `spdk_io_segment_bytes`, capped by the bdev maximum read/write size and
  rounded to whole storage pages. Each extent is split into segments no larger
  than that limit. For example, with a 1 MiB effective segment size, a 10 MiB
  LMCache request becomes ten 1 MiB bdev operations when its extents permit
  that layout; fragmentation can produce additional, smaller operations. The
  final physical segment is page-aligned, so a partial final page is zero-padded
  on writes and only its logical bytes are returned on reads.
- **DMA staging buffers.** Since LMCache's source and destination buffers
    do not explicitly follow DMA allocation or alignment requirements, SPDK connector
    initializes per-I/O worker pools of `spdk_dma_zmalloc` bounce buffers to meet this
    requirement for certain bdevs. Each buffer is sized to one
    effective segment. There is currently no zero-copy approach.

For a detailed description of the connector's component ownership, object
placement, and I/O flows, see the [architecture guide](docs/architecture.md).


# Tests

Tests cover LMCache store/lookup/load integration, direct SPDK connector and
multi-bdev behavior, plus the C++ page allocator and storage-geometry logic.
SPDK Bdev benchmark results are available in
[`docs/benchmarks.md`](docs/benchmarks.md).

# Current Limtiations / Future Work

- **No Zero-copy support / LMCache MP mode Hugepages limitations** LMCache MP mode currently
  does not support L1 hugepages allocations, which would allow us to simply
  register LMCache buffers into SPDK and have a zero-copy data path, as opposed to the current
  approach using DMA buffers.
- **No persistent metadata or recovery.** When LMCache shuts down, we lose the SPDK connector's page map
  which is only present in memory and not persisted. Therefore after a restart, a bdev
  cannot recover the content/mapping of KVCache.
