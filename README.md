# LMCache SPDK Connectors

This repository contains two independent out-of-tree native L2 connectors for
[LMCache](https://github.com/LMCache/LMCache):

| Package | SPDK interface | Use case |
| --- | --- | --- |
| [`bdev/`](bdev/) | SPDK bdev | Local NVMe, NVMe-oF, or another SPDK bdev selected in JSON. |
| [`nvme-kv/`](nvme-kv/) | SPDK NVMe-KV | An NVMe controller exposing a Key-Value namespace. |

There is currently no connector directly using SPDK's NVMe and NVMe-oF APIs, these
targets are only supported through the Bdev API.

## Compatibility

The connectors were tested with:
- LMCache `v0.5.4`
- SPDK `v26.05`

```sh
git clone --branch v0.5.4 https://github.com/LMCache/LMCache.git
git clone --branch v26.05 https://github.com/spdk/spdk.git
```


See [`bdev/README.md`](bdev/README.md) for more details on the bdev connector,
and [`nvme-kv/README.md`](nvme-kv/README.md) for more details on the nvme-kv connector.

## License

This repository is licensed under the [Apache License 2.0](LICENSE). The
connectors build against separately obtained LMCache and SPDK checkouts; see
[NOTICE](NOTICE) and [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES) for dependency
boundaries and distribution notices.
