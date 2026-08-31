# SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
from pathlib import Path
import os
import select

from lmcache_spdk_connector._native import SpdkConnector


def complete(connector: SpdkConnector, future_id: int):
    poller = select.poll()
    poller.register(connector.event_fd(), select.POLLIN)
    assert poller.poll(30000)
    result = next(
        item for item in connector.drain_completions() if item[0] == future_id
    )
    assert result[1], result[2]
    return result[3]


def main() -> None:
    config = Path(__file__).resolve().parent.parent / "data" / "spdk-malloc-smoke.json"
    cpus = sorted(os.sched_getaffinity(0))
    assert len(cpus) >= 2

    def connector(bdev_name: str) -> SpdkConnector:
        return SpdkConnector(
            str(config), bdev_name, [cpus[0]], [cpus[1]], 1, 1, 1024, True,
            True, 1048576,
        )

    first = connector("SmokeMalloc0")
    second = connector("SmokeMalloc1")
    assert first.runtime_status()["is_available"]
    assert second.runtime_status()["is_available"]

    first_value = memoryview(bytearray(b"a" * 4096))
    second_value = memoryview(bytearray(b"b" * 4096))
    assert complete(first, first.submit_batch_set(["key"], [first_value])) is None
    assert complete(second, second.submit_batch_set(["key"], [second_value])) is None

    first_destination = memoryview(bytearray(4096))
    second_destination = memoryview(bytearray(4096))
    assert complete(first, first.submit_batch_get(["key"], [first_destination])) == [
        True
    ]
    assert complete(second, second.submit_batch_get(["key"], [second_destination])) == [
        True
    ]
    assert first_destination == first_value
    assert second_destination == second_value

    first.close()
    second_destination = memoryview(bytearray(4096))
    assert complete(second, second.submit_batch_get(["key"], [second_destination])) == [
        True
    ]
    assert second_destination == second_value
    second.close()


if __name__ == "__main__":
    main()
