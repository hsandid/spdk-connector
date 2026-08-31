# SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
from pathlib import Path
import argparse
import json
import os
import time

from lmcache.v1.distributed.api import ObjectKey
from lmcache.v1.distributed.l2_adapters import create_l2_adapter
from lmcache.v1.distributed.l2_adapters.config import parse_args_to_l2_adapters_config


class ObjectBuffer:
    def __init__(self, data: bytearray) -> None:
        self.byte_array = data

    def get_size(self) -> int:
        return len(self.byte_array)


def main() -> None:
    config = Path(__file__).resolve().parent.parent / "data" / "spdk-malloc-smoke.json"
    cpus = sorted(os.sched_getaffinity(0))
    adapter_config = {
        "type": "native_plugin",
        "module_path": "lmcache_spdk_connector",
        "class_name": "SpdkNativeConnector",
        "max_capacity_gb": 0.00001,
        "adapter_params": {
            "spdk_json_config": str(config),
            "bdev_name": "SmokeMalloc0",
            "spdk_reactor_cpus": [cpus[0]],
            "connector_worker_cpus": [cpus[1]],
            "spdk_io_thread_count": 1,
            "spdk_buffer_count": 1,
            "spdk_memory_size_mb": 1024,
            "spdk_no_huge": True,
            "spdk_no_pci": True,
            "spdk_io_segment_bytes": 1048576,
        },
    }
    parsed = parse_args_to_l2_adapters_config(
        argparse.Namespace(l2_adapter=[json.dumps(adapter_config)])
    )
    adapter = create_l2_adapter(parsed.adapters[0])
    keys = [ObjectKey(bytes([value]) * 32, "native-smoke", 0) for value in (1, 2)]
    source = [ObjectBuffer(bytearray(bytes([value]) * 4096)) for value in (1, 2)]
    task = adapter.submit_store_task(keys, source)
    while True:
        result = adapter.pop_completed_store_tasks().get(task)
        if result is not None:
            assert result.is_successful()
            break
        time.sleep(0.01)
    lookup = adapter.submit_lookup_and_lock_task(keys, None)
    while True:
        result = adapter.query_lookup_and_lock_result(lookup)
        if result is not None:
            assert result.popcount() == 2
            break
        time.sleep(0.01)
    destination = [ObjectBuffer(bytearray(4096)) for _ in keys]
    load = adapter.submit_load_task(keys, destination)
    while True:
        result = adapter.query_load_result(load)
        if result is not None:
            assert result.popcount() == 2
            break
        time.sleep(0.01)
    assert [item.byte_array for item in destination] == [
        item.byte_array for item in source
    ]
    adapter.submit_unlock(keys)
    adapter.close()


if __name__ == "__main__":
    main()
