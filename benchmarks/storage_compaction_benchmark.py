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

import ctypes
import json
import sys
import tempfile
import time
from pathlib import Path

from _common import environment_metadata, summarize_latency
from kor import Database


RECORD_COUNT = 250
CHURN_CYCLES = 40
CHURN_VALUE_BYTES = 4096
MUTATION_REPETITIONS = 7


def peak_working_set_bytes() -> int | None:
    if sys.platform != "win32":
        return None

    class ProcessMemoryCounters(ctypes.Structure):
        _fields_ = [
            ("cb", ctypes.c_ulong),
            ("PageFaultCount", ctypes.c_ulong),
            ("PeakWorkingSetSize", ctypes.c_size_t),
            ("WorkingSetSize", ctypes.c_size_t),
            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
            ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
            ("PagefileUsage", ctypes.c_size_t),
            ("PeakPagefileUsage", ctypes.c_size_t),
        ]

    counters = ProcessMemoryCounters()
    counters.cb = ctypes.sizeof(counters)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    kernel32.GetCurrentProcess.restype = ctypes.c_void_p
    psapi.GetProcessMemoryInfo.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ProcessMemoryCounters),
        ctypes.c_ulong,
    ]
    psapi.GetProcessMemoryInfo.restype = ctypes.c_int
    process = kernel32.GetCurrentProcess()
    if not psapi.GetProcessMemoryInfo(
        process,
        ctypes.byref(counters),
        counters.cb,
    ):
        return None
    return int(counters.PeakWorkingSetSize)


def time_reopen(root: Path, expected_records: int) -> float:
    started = time.perf_counter()
    db = Database(path=root, name="compaction-benchmark", max_memory=128, snapshot_interval=60)
    try:
        assert db.storage_stats()["record_count"] == expected_records
        assert len(db.list_indexes()) == 1
        return time.perf_counter() - started
    finally:
        db.close()


def run_benchmark(root: Path) -> dict[str, object]:
    db = Database(path=root, name="compaction-benchmark", max_memory=128, snapshot_interval=60)
    with db.transaction() as transaction:
        for number in range(RECORD_COUNT):
            transaction.set(
                f"record-{number:04}",
                {
                    "group": f"group-{number % 25}",
                    "score": number,
                    "payload": f"record-data-{number:04}",
                },
            )
    db.create_index("group_lookup", "group", index_type="hash")

    churn_value = "x" * CHURN_VALUE_BYTES
    churn_start = db.storage_stats()
    started = time.perf_counter()
    for _ in range(CHURN_CYCLES):
        db.set("churn", churn_value)
        db.delete("churn")
    churn_seconds = time.perf_counter() - started
    churn_end = db.storage_stats()

    db.close()
    recovery_before_seconds = time_reopen(root, RECORD_COUNT)
    db = Database(path=root, name="compaction-benchmark", max_memory=128, snapshot_interval=60)
    db.storage_stats()

    mutation_samples = []
    for number in range(MUTATION_REPETITIONS):
        started = time.perf_counter()
        db.set("latency-probe", number)
        mutation_samples.append(time.perf_counter() - started)
    peak_before_compaction = peak_working_set_bytes()
    started = time.perf_counter()
    compact_result = db.compact()
    compaction_seconds = time.perf_counter() - started
    peak_after_compaction = peak_working_set_bytes()
    stats_after = db.storage_stats()
    db.close()
    recovery_after_seconds = time_reopen(root, RECORD_COUNT + 1)

    peak_delta = None
    if peak_before_compaction is not None and peak_after_compaction is not None:
        peak_delta = max(0, peak_after_compaction - peak_before_compaction)

    return {
        "records": RECORD_COUNT,
        "set_delete_cycles": CHURN_CYCLES,
        "churn_value_bytes": CHURN_VALUE_BYTES,
        "wal_file_size_before_churn_bytes": churn_start["file_size_bytes"],
        "wal_file_size_after_churn_bytes": churn_end["file_size_bytes"],
        "wal_bytes_required_before_compaction": churn_end["wal_bytes_required"],
        "set_delete_churn_seconds": churn_seconds,
        "normal_set_latency": summarize_latency(mutation_samples),
        "recovery_before_compaction_seconds": recovery_before_seconds,
        "compaction_seconds": compaction_seconds,
        "compaction": compact_result,
        "recovery_after_compaction_seconds": recovery_after_seconds,
        "reusable_free_space_after_compaction_bytes": stats_after["reusable_free_space_bytes"],
        "working_set_peak_delta_during_compaction_bytes": peak_delta,
        "note": "Working-set peak delta is a process high-water-mark observation, not isolated native allocation accounting.",
    }


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="kor-storage-bench-") as directory:
        report = {
            "environment": {
                **environment_metadata(
                    records=RECORD_COUNT,
                    set_delete_cycles=CHURN_CYCLES,
                    churn_value_bytes=CHURN_VALUE_BYTES,
                    mutation_repetitions=MUTATION_REPETITIONS,
                    max_memory_mib=128,
                ),
                "platform_id": sys.platform,
            },
            "measurements": run_benchmark(Path(directory)),
        }
        print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
