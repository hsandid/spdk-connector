# SPDK bdev Connector Benchmark Results

## Environment

| Components | Configuration |
| --- | --- |
| Host | AWS `c5d.metal` instance with 96 logical CPUs, Intel Xeon Platinum 8275CL, SMT enabled |
| Kernel | Ubuntu `6.17.0-1017-aws`, performance governor |
| NVMe controller | `0000:e7:00.0`, NUMA node 1 |
| Kernel path | ext4 with O_DIRECT on a dedicated 50 GiB partition |
| Native path | Separate 50 GiB partition, bound to `vfio-pci` during SPDK runs |
| CPU allocation | SPDK reactors 24-25, connector workers 26-29, submitter 30 |
| Hugepages | 1,024 2 MiB pages; selected 4 MiB buffer tests used more temporarily |

## Results

### (1) Fio Testing

- No LMCache running in these tests, only fio
- SPDK was built/used with the fio plugin.
- This results serve as baseline reference for LMCache testing
- Each point of data corresponds to one fio test running for 60 seconds

| Request | Kernel MiB/s | SPDK MiB/s | Kernel p99 ms | SPDK p99 ms |
| --- | ---: | ---: | ---: | ---: |
| 4 KiB sequential read Q1 | 146.6 | 181.5 | 0.030 | 0.024 |
| 4 KiB sequential write Q1 | 116.0 | 157.1 | 0.044 | 0.031 |
| 4 KiB sequential read Q32 | 625.4 | 635.7 | 0.449 | 0.440 |
| 4 KiB sequential write Q32 | 678.8 | 678.7 | 0.208 | 0.214 |
| 256 KiB sequential read Q32 | 1,597.6 | 1,617.2 | 5.145 | 5.145 |
| 256 KiB sequential write Q32 | 752.9 | 760.8 | 10.682 | 10.682 |
| 1 MiB sequential read Q32 | 1,597.6 | 1,617.3 | 20.054 | 25.297 |
| 1 MiB sequential write Q32 | 753.0 | 760.8 | 42.729 | 53.215 |


### (2) LMCache Testing

- LMCache tests mainly focus on comparing the SPDK bdev connector with LMCache's `fs_native_odirect` adapter.
- Each LMCache test case was executed with 3 repetitions.
- At 256 KiB/Q32, native polling improved measured load throughput and task tail
latency.
- At 10, 20, and 30 MiB/Q1, both paths reached the same device-limited
throughput; polling improved Store p99.9 modestly.
- SPDK Polling still used materially more CPU than the kernel path, so these results do not establish
a general CPU-efficiency advantage for the SPDK connector.

| Scenario | Kernel Load MiB/s | Poll Load MiB/s | Kernel Load p99.9 ms | Poll Load p99.9 ms | Kernel Store MiB/s | Poll Store MiB/s | Kernel Store p99.9 ms | Poll Store p99.9 ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 256 KiB Q1 | 723.7 | 782.4 | 0.571 | 0.506 | 666.1 | 678.8 | 0.719 | 0.509 |
| 256 KiB Q4 | 1,105.9 | 1,183.3 | 0.945 | 0.875 | 1,070.9 | 1,178.4 | 1.060 | 0.856 |
| 256 KiB Q32 | 1,722.1 | 2,506.1 | 4.717 | 3.258 | 1,236.6 | 1,320.2 | 15.307 | 8.095 |
| 10 MiB Q1 | 1,619.8 | 1,620.1 | 6.418 | 6.446 | 755.2 | 755.2 | 13.882 | 13.463 |
| 20 MiB Q1 | 1,603.5 | 1,603.6 | 12.829 | 12.716 | 751.7 | 751.6 | 27.937 | 26.790 |
| 30 MiB Q1 | 1,598.0 | 1,598.2 | 19.044 | 19.184 | 750.6 | 750.4 | 43.228 | 40.180 |

The polling SPDK connector is at least on par with the tested
`fs_native_odirect` adapter at every measured throughput point. At 256 KiB,
it is 2-10% faster at Q1 and Q4; at Q32 it improves Load throughput by 45%
and Store throughput by 7%.

The 10/20/30 MiB Q1 LMCache results are interesting as they simulate more closely the
I/O patterns expected from LMCache workloads. They converge on the same approximate 1.6
GiB/s read and 0.75 GiB/s write rates as the focused 256 KiB and 1 MiB
sequential fio controls above.

**Q32 throughput outlier.** The 2,506.1 MiB/s 256 KiB/Q32 Load result is an
outlier going beyond the reported fio device limits, and is most probably due to
an accounting issue in the logical LMCache task-throughput measurement.


## Test Setup

### Build

```sh
cd bdev
export LMCACHE_SOURCE_ROOT=/path/to/LMCache
export SPDK_ROOT=/path/to/spdk
make build
```

### Prepare The Device

- Reserve hugepages, select a dedicated namespace or bounded partition, and use
disjoint CPU sets for SPDK reactors, connector workers, the workload submitter,
and kernel IRQs.
- Use the kernel driver for the filesystem baseline; unmount the
native target and bind only its controller to an SPDK-compatible driver before
the SPDK tests.

```sh
export BDF=0000:00:00.0                  # Dedicated test controller
export KERNEL_FILE=/mnt/bench/fio.bin     # Filesystem baseline file
export BDEV_NAME=NativeNvme0n1p1          # Bounded bdev from SPDK JSON
export BDEV_JSON=/path/to/nvme-bdev.json
export CPUS=24,25,26,27,28,29
```

### Kernel Fio Baseline

```sh
taskset -c "${CPUS}" fio \
  --name=kernel-read-256k-q32 --filename="${KERNEL_FILE}" \
  --rw=read --bs=256k --iodepth=16 --numjobs=2 --direct=1 \
  --time_based --runtime=60 --group_reporting --output-format=json
```

- Repeat for different reads and writes, selected block sizes, and queue depths.

### SPDK Fio Baseline

- Use the `spdk_bdev` fio engine built with SPDK.

```sh
taskset -c "${CPUS}" fio \
  --name=spdk-read-256k-q32 --filename="${BDEV_NAME}" \
  --rw=read --bs=256k --iodepth=16 --numjobs=2 --size=64M \
  --offset_increment=64M --direct=1 --time_based --runtime=60 \
  --group_reporting --ioengine=spdk_bdev --spdk_json_conf="${BDEV_JSON}" \
  --output-format=json
```

### LMCache L2 Profiles

- Run the same parameters through the kernel adapter and the Bdev
`native_plugin` configuration. The example below uses the standalone
`lmcache bench l2` runner shipped by LMCache `v0.5.4`. It performs Store,
Lookup, and Load in one adapter lifetime, which is required by this connector's
in-memory placement map.

```sh
# Edit the SPDK JSON path, bdev name, and CPU lists in this configuration.
export L2_ADAPTER_JSON="$(tr -d '\n' < extras/config/native-plugin.json)"

# One 256 KiB object per submit and 32 submits per round reproduces 256 KiB/Q32.
taskset -c "${CPUS}" "${LMCACHE_BENCH:-.venv/bin/lmcache}" bench l2 \
  --l2-adapter "${L2_ADAPTER_JSON}" \
  --num-keys 1 --in-flight 32 --data-size-kb 256 \
  --rounds 3 --warmup-rounds 1 --no-skip-verify
```

For Store-only or Load-only measurements, use `--only store` or `--only load`.
The latter must run against an adapter instance that already stored the keys;
it cannot reuse the Bdev connector's mappings from a previous process.
