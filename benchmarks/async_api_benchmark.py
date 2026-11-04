# Copyright 2026 Pravanjan Roy (kingmon6996)
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from __future__ import annotations

import asyncio
import concurrent.futures
import json
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

from _common import environment_metadata, summarize_latency
from kor import Database, async_connect


REPETITIONS = 24
CONCURRENT_OPERATIONS = 24
RECORD_COUNT = 100
CHURN_CYCLES = 12


def benchmark_sync(root: Path) -> dict[str, Any]:
    database = Database(path=root, name="sync", max_memory=64, snapshot_interval=60)
    database.start()

    set_latencies = []
    get_latencies = []
    for index in range(REPETITIONS):
        started = time.perf_counter()
        database.set(f"single:{index}", index)
        set_latencies.append(time.perf_counter() - started)
        started = time.perf_counter()
        database.get(f"single:{index}")
        get_latencies.append(time.perf_counter() - started)

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as workers:
        started = time.perf_counter()
        list(
            workers.map(
                database.get,
                (f"single:{index % REPETITIONS}" for index in range(CONCURRENT_OPERATIONS)),
            )
        )
        concurrent_reads_seconds = time.perf_counter() - started

        started = time.perf_counter()
        list(
            workers.map(
                lambda index: database.set(f"concurrent:{index}", index),
                range(CONCURRENT_OPERATIONS),
            )
        )
        concurrent_writes_seconds = time.perf_counter() - started

    transaction_latencies = []
    for index in range(REPETITIONS):
        started = time.perf_counter()
        with database.transaction() as transaction:
            transaction.set(f"transaction:{index}", index)
        transaction_latencies.append(time.perf_counter() - started)

    for index in range(RECORD_COUNT):
        database.set(f"record:{index:04}", {"score": index, "group": index % 10})
    database.create_index("score_index", "score", index_type="ordered")
    query_latencies = []
    for index in range(REPETITIONS):
        started = time.perf_counter()
        database.find("score", index % RECORD_COUNT)
        query_latencies.append(time.perf_counter() - started)
    database.find_range("score", 10, 30)

    for _ in range(CHURN_CYCLES):
        database.set("churn", "x" * 4096)
        database.delete("churn")
    started = time.perf_counter()
    compact_result = database.compact()
    compaction_seconds = time.perf_counter() - started
    database.close()

    return {
        "set_latency": summarize_latency(set_latencies),
        "get_latency": summarize_latency(get_latencies),
        "concurrent_reads_seconds_4_workers": round(concurrent_reads_seconds, 6),
        "concurrent_writes_seconds_4_workers": round(concurrent_writes_seconds, 6),
        "transaction_commit_latency": summarize_latency(transaction_latencies),
        "indexed_query_latency": summarize_latency(query_latencies),
        "compaction_ms": round(compaction_seconds * 1000.0, 3),
        "compaction_file_length_reduction_bytes": compact_result["file_length_reduction_bytes"],
        "checkpoint_api": "not available in the synchronous SDK",
    }


async def benchmark_async(root: Path) -> dict[str, Any]:
    database = await async_connect(path=root, name="async")
    set_latencies = []
    get_latencies = []
    for index in range(REPETITIONS):
        started = time.perf_counter()
        await database.set(f"single:{index}", index)
        set_latencies.append(time.perf_counter() - started)
        started = time.perf_counter()
        await database.get(f"single:{index}")
        get_latencies.append(time.perf_counter() - started)

    for index in range(CONCURRENT_OPERATIONS):
        await database.set(f"single:{index}", index)
    started = time.perf_counter()
    await asyncio.gather(
        *(database.get(f"single:{index % REPETITIONS}") for index in range(CONCURRENT_OPERATIONS))
    )
    concurrent_reads_seconds = time.perf_counter() - started

    started = time.perf_counter()
    await asyncio.gather(
        *(database.set(f"concurrent:{index}", index) for index in range(CONCURRENT_OPERATIONS))
    )
    concurrent_writes_seconds = time.perf_counter() - started

    transaction_latencies = []
    for index in range(REPETITIONS):
        started = time.perf_counter()
        async with database.transaction() as transaction:
            await transaction.set(f"transaction:{index}", index)
        transaction_latencies.append(time.perf_counter() - started)

    for index in range(RECORD_COUNT):
        await database.set(f"record:{index:04}", {"score": index, "group": index % 10})
    await database.create_index("score_index", "score", index_type="ordered")
    query_latencies = []
    for index in range(REPETITIONS):
        started = time.perf_counter()
        await database.find("score", index % RECORD_COUNT)
        query_latencies.append(time.perf_counter() - started)
    await database.find_range("score", 10, 30)

    for _ in range(CHURN_CYCLES):
        await database.set("churn", "x" * 4096)
        await database.delete("churn")
    started = time.perf_counter()
    compact_result = await database.compact()
    compaction_seconds = time.perf_counter() - started

    heartbeat_intervals: list[float] = []
    finished = asyncio.Event()

    async def load_storage() -> None:
        for index in range(60):
            await database.set(f"responsiveness:{index}", index)
        finished.set()

    async def heartbeat() -> None:
        previous = time.perf_counter()
        while not finished.is_set():
            await asyncio.sleep(0.002)
            now = time.perf_counter()
            heartbeat_intervals.append(now - previous)
            previous = now

    load_task = asyncio.create_task(load_storage())
    heartbeat_task = asyncio.create_task(heartbeat())
    await load_task
    await heartbeat_task
    max_heartbeat_delay_ms = (
        round(max(heartbeat_intervals) * 1000.0, 3) if heartbeat_intervals else None
    )
    await database.close()

    return {
        "set_latency": summarize_latency(set_latencies),
        "get_latency": summarize_latency(get_latencies),
        "concurrent_reads_seconds": round(concurrent_reads_seconds, 6),
        "concurrent_writes_seconds": round(concurrent_writes_seconds, 6),
        "transaction_commit_latency": summarize_latency(transaction_latencies),
        "indexed_query_latency": summarize_latency(query_latencies),
        "compaction_ms": round(compaction_seconds * 1000.0, 3),
        "compaction_file_length_reduction_bytes": compact_result["file_length_reduction_bytes"],
        "max_event_loop_heartbeat_delay_ms_under_60_writes": max_heartbeat_delay_ms,
        "checkpoint_api": "not available in the async SDK",
    }


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="kor-async-benchmark-") as temporary:
        root = Path(temporary)
        sync_results = benchmark_sync(root)
        async_results = asyncio.run(benchmark_async(root))
    print(
        json.dumps(
            {
                "environment": {
                    **environment_metadata(
                        repetitions=REPETITIONS,
                        concurrent_operations=CONCURRENT_OPERATIONS,
                        indexed_records=RECORD_COUNT,
                        churn_cycles=CHURN_CYCLES,
                        max_memory_mib=64,
                        async_executor="one worker per connection; bounded admission of 16 operations",
                    ),
                    "platform_id": sys.platform,
                },
                "sync": sync_results,
                "async": async_results,
                "interpretation": (
                    "Async scheduling is for event-loop responsiveness. Each connection serializes "
                    "native operations, so it is not expected to increase per-database throughput."
                ),
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
