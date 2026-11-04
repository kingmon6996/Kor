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
import os
import platform
from typing import Any


_MIB = 1024 * 1024


class InsufficientSystemMemoryError(MemoryError):
    """Raised when a Kor memory budget exceeds currently available system memory."""


class MemoryUsageWarning(UserWarning):
    """Warns when the Kor memory budget exceeds half of currently available memory."""


def available_memory_bytes() -> int:
    """Return currently available RAM, honoring common Linux container limits."""
    operating_system = platform.system()
    if operating_system == "Windows":
        class _MemoryStatus(ctypes.Structure):
            _fields_ = [
                ("dwLength", ctypes.c_ulong),
                ("dwMemoryLoad", ctypes.c_ulong),
                ("ullTotalPhys", ctypes.c_ulonglong),
                ("ullAvailPhys", ctypes.c_ulonglong),
                ("ullTotalPageFile", ctypes.c_ulonglong),
                ("ullAvailPageFile", ctypes.c_ulonglong),
                ("ullTotalVirtual", ctypes.c_ulonglong),
                ("ullAvailVirtual", ctypes.c_ulonglong),
                ("ullAvailExtendedVirtual", ctypes.c_ulonglong),
            ]

        status = _MemoryStatus()
        status.dwLength = ctypes.sizeof(_MemoryStatus)
        if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
            raise OSError("Windows could not report currently available physical memory.")
        return int(status.ullAvailPhys)

    if operating_system == "Linux":
        available: int | None = None
        try:
            with open("/proc/meminfo", encoding="ascii") as meminfo:
                for line in meminfo:
                    if line.startswith("MemAvailable:"):
                        available = int(line.split()[1]) * 1024
                        break
        except OSError:
            available = None

        if available is None:
            available_pages = os.sysconf("SC_AVPHYS_PAGES")
            page_size = os.sysconf("SC_PAGE_SIZE")
            available = int(available_pages) * int(page_size)

        for limit_path, current_path in (
            ("/sys/fs/cgroup/memory.max", "/sys/fs/cgroup/memory.current"),
            (
                "/sys/fs/cgroup/memory/memory.limit_in_bytes",
                "/sys/fs/cgroup/memory/memory.usage_in_bytes",
            ),
        ):
            try:
                with open(limit_path, encoding="ascii") as limit_file:
                    raw_limit = limit_file.read().strip()
                with open(current_path, encoding="ascii") as current_file:
                    usage = int(current_file.read().strip())
            except FileNotFoundError:
                continue
            except OSError as error:
                raise OSError("Unable to read the Linux container memory limit.") from error
            if raw_limit != "max":
                limit = int(raw_limit)
                if limit > 0:
                    available = min(available, max(0, limit - usage))
                break
        return available

    try:
        return int(os.sysconf("SC_AVPHYS_PAGES")) * int(os.sysconf("SC_PAGE_SIZE"))
    except (AttributeError, OSError, ValueError) as error:
        raise OSError("This platform cannot report currently available physical memory.") from error


def check_memory_budget(max_memory_mib: int) -> dict[str, Any]:
    """Validate a configured Kor budget against live available RAM and report its share."""
    if not isinstance(max_memory_mib, int) or isinstance(max_memory_mib, bool) or max_memory_mib <= 0:
        raise ValueError("max_memory_mib must be a positive integer.")

    configured_bytes = max_memory_mib * _MIB
    available_bytes = available_memory_bytes()
    if configured_bytes > available_bytes:
        raise InsufficientSystemMemoryError(
            f"Kor memory budget ({max_memory_mib} MiB) exceeds currently available RAM "
            f"({available_bytes // _MIB} MiB)."
        )

    return {
        "configured_bytes": configured_bytes,
        "available_bytes": available_bytes,
        "available_mib": available_bytes // _MIB,
        "available_fraction_used": configured_bytes / available_bytes if available_bytes else 1.0,
        "warning": configured_bytes > available_bytes // 2,
    }
