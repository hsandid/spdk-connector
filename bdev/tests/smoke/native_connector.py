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
    connector = SpdkConnector(
        str(config), "SmokeMalloc0", [cpus[0]], [cpus[1]], 1, 1, 1024, True,
        True, 1048576,
    )

    source = memoryview(bytearray(b"x" * 4096))
    second_source = memoryview(bytearray(b"y" * 4096))
    third_source = memoryview(bytearray(b"z" * 4096))
    assert complete(
        connector,
        connector.submit_batch_set(
            ["first", "second", "third"], [source, second_source, third_source]
        ),
    ) is None
    assert complete(
        connector, connector.submit_batch_exists(["first", "second", "third"])
    ) == [True, True, True]
    destination = memoryview(bytearray(4096))
    second_destination = memoryview(bytearray(4096))
    assert complete(
        connector,
        connector.submit_batch_get(
            ["first", "second"], [destination, second_destination]
        ),
    ) == [True, True]
    assert destination == source
    assert second_destination == second_source
    assert complete(
        connector, connector.submit_batch_delete(["first", "second"])
    ) == [True, True]
    assert complete(connector, connector.submit_batch_exists(["first", "second"])) == [
        False,
        False,
    ]
    connector.close()


if __name__ == "__main__":
    main()
