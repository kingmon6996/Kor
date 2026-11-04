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
import math
import os
import platform
import statistics
import sys
from collections.abc import Sequence
from typing import Any


def physical_memory_bytes() -> int | None:
    if sys.platform == "win32":
        class MemoryStatus(ctypes.Structure):
            _fields_ = [
                ("length", ctypes.c_ulong),
                ("memory_load", ctypes.c_ulong),
                ("total_physical", ctypes.c_ulonglong),
                ("available_physical", ctypes.c_ulonglong),
                ("total_page_file", ctypes.c_ulonglong),
                ("available_page_file", ctypes.c_ulonglong),
                ("total_virtual", ctypes.c_ulonglong),
                ("available_virtual", ctypes.c_ulonglong),
                ("available_extended_virtual", ctypes.c_ulonglong),
            ]

        status = MemoryStatus()
        status.length = ctypes.sizeof(status)
        if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
            return None
        return int(status.total_physical)

    try:
        page_size = os.sysconf("SC_PAGE_SIZE")
        page_count = os.sysconf("SC_PHYS_PAGES")
    except (AttributeError, OSError, ValueError):
        return None
    return int(page_size * page_count)


def environment_metadata(**workload: Any) -> dict[str, Any]:
    return {
        "python": sys.version.split()[0],
        "os": platform.platform(),
        "machine": platform.machine(),
        "processor": platform.processor(),
        "logical_cpu_count": os.cpu_count(),
        "physical_memory_bytes": physical_memory_bytes(),
        "python_build_compiler": platform.python_compiler(),
        "native_compiler": os.environ.get("CXX", "not exposed by the extension"),
        "workload": workload,
    }


def summarize_latency(samples_seconds: Sequence[float], operations_per_sample: int = 1) -> dict[str, float | int]:
    if not samples_seconds or operations_per_sample <= 0:
        raise ValueError("Latency samples and operations_per_sample must be positive.")
    ordered = sorted(samples_seconds)

    def percentile(fraction: float) -> float:
        index = max(0, math.ceil(fraction * len(ordered)) - 1)
        return ordered[index]

    elapsed = sum(samples_seconds)
    return {
        "sample_count": len(samples_seconds),
        "median_ms": round(statistics.median(ordered) * 1000.0, 3),
        "p95_ms": round(percentile(0.95) * 1000.0, 3),
        "p99_ms": round(percentile(0.99) * 1000.0, 3),
        "operations_per_second": round(
            (len(samples_seconds) * operations_per_sample) / elapsed,
            2,
        ),
    }
