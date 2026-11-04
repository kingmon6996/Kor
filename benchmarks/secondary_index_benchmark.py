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

import json
import sys
import tempfile
import time
from pathlib import Path

from _common import environment_metadata, summarize_latency
from kor import Database


RECORDS = 2500
REPETITIONS = 7
QUERY_LOOPS = 2000


def latency_summary(
    operation,
    repetitions: int = REPETITIONS,
    operations_per_sample: int = 1,
) -> dict[str, float | int]:
    samples = []
    for _ in range(repetitions):
        started = time.perf_counter()
        operation()
        samples.append(time.perf_counter() - started)
    return summarize_latency(samples, operations_per_sample)


def make_document(number: int) -> dict[str, object]:
    label = f"group-{number % 50}"
    score = number % 1000
    return {
        "label": label,
        "label_scan": label,
        "score": score,
        "score_scan": score,
        "payload": number,
    }


def populate(db: Database, count: int) -> None:
    with db.transaction() as transaction:
        for number in range(count):
            transaction.set(f"key-{number:05}", make_document(number))


def run_index_query_benchmark(root: Path) -> dict[str, object]:
    db = Database(path=root, name="queries", max_memory=128, snapshot_interval=60)
    populate(db, RECORDS)

    key_reads = [f"key-{number % RECORDS:05}" for number in range(QUERY_LOOPS)]
    [db.get(key) for key in key_reads]
    primary_before = latency_summary(lambda: [db.get(key) for key in key_reads], operations_per_sample=QUERY_LOOPS)
    equality_scan = latency_summary(lambda: db.find("label_scan", "group-7"))
    range_scan = latency_summary(lambda: db.find_range("score_scan", 490, 510))

    started = time.perf_counter()
    db.create_index("label_hash", "label", "hash")
    db.create_index("score_order", "score", "ordered")
    index_build = time.perf_counter() - started

    expected_equal = db.find("label_scan", "group-7")
    indexed_equal = db.find("label", "group-7")
    expected_range = db.find_range("score_scan", 490, 510)
    indexed_range = db.find_range("score", 490, 510)
    assert indexed_equal == expected_equal
    assert indexed_range == expected_range

    primary_after = latency_summary(lambda: [db.get(key) for key in key_reads], operations_per_sample=QUERY_LOOPS)
    equality_indexed = latency_summary(lambda: db.find("label", "group-7"))
    range_indexed = latency_summary(lambda: db.find_range("score", 490, 510))
    memory = db._storage.memory_stats()

    db.close()
    started = time.perf_counter()
    reopened = Database(path=root, name="queries", max_memory=128, snapshot_interval=60)
    assert len(reopened.list_indexes()) == 2
    reopen_and_rebuild = time.perf_counter() - started
    reopened.close()

    return {
        "records": RECORDS,
        "query_loops": QUERY_LOOPS,
        f"primary_get_{QUERY_LOOPS}_before_indexes": primary_before,
        f"primary_get_{QUERY_LOOPS}_after_indexes": primary_after,
        "equality_scan_latency": equality_scan,
        "equality_indexed_latency": equality_indexed,
        "range_scan_latency": range_scan,
        "range_indexed_latency": range_indexed,
        "two_index_build_seconds": index_build,
        "reopen_and_rebuild_seconds": reopen_and_rebuild,
        "retained_index_bytes_estimate": memory.index_bytes,
        "retained_total_bytes_estimate": memory.total_bytes,
    }


def run_write_benchmark(root: Path, index_count: int) -> dict[str, float | int]:
    db = Database(path=root, name=f"writes-{index_count}", max_memory=128, snapshot_interval=60)
    populate(db, 1000)
    if index_count >= 1:
        db.create_index("label_hash", "label", "hash")
    if index_count >= 2:
        db.create_index("score_order", "score", "ordered")

    sequence = 0

    def write_one() -> None:
        nonlocal sequence
        db.set("write-probe", make_document(sequence))
        sequence += 1

    def delete_one() -> None:
        db.delete("write-probe")

    def commit_batch() -> None:
        nonlocal sequence
        with db.transaction() as transaction:
            for offset in range(25):
                transaction.set(f"key-{offset:05}", make_document(sequence + offset))
        sequence += 25

    set_latency = latency_summary(write_one)
    delete_latency = latency_summary(delete_one)
    transaction_commit = latency_summary(commit_batch, repetitions=3, operations_per_sample=25)
    db.close()
    return {
        "secondary_indexes": index_count,
        "set_latency": set_latency,
        "delete_latency": delete_latency,
        "25_record_transaction_commit_latency": transaction_commit,
    }


def run_memory_scaling_benchmark(root: Path) -> list[dict[str, int]]:
    measurements = []
    for count in (500, 1250, RECORDS):
        db = Database(path=root, name=f"memory-{count}", max_memory=128, snapshot_interval=60)
        populate(db, count)
        db.create_index("label_hash", "label", "hash")
        stats = db._storage.memory_stats()
        measurements.append(
            {
                "records": count,
                "estimated_index_bytes": stats.index_bytes,
                "estimated_total_bytes": stats.total_bytes,
            }
        )
        db.close()
    return measurements


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="kor-index-bench-") as directory:
        root = Path(directory)
        report = {
            "environment": {
                **environment_metadata(
                    records_for_query_benchmark=RECORDS,
                    repetitions=REPETITIONS,
                    query_loop_size=QUERY_LOOPS,
                    max_memory_mib=128,
                ),
                "platform_id": sys.platform,
            },
            "queries": run_index_query_benchmark(root),
            "writes": [run_write_benchmark(root, count) for count in range(3)],
            "memory_scaling": run_memory_scaling_benchmark(root),
        }
        print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
