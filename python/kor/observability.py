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

import hashlib
import logging
import math
import time
from pathlib import Path
from typing import Any

_LOGGER = logging.getLogger("kor")
if not any(isinstance(handler, logging.NullHandler) for handler in _LOGGER.handlers):
    _LOGGER.addHandler(logging.NullHandler())

_PERCENTILES = (("p50", 0.50), ("p95", 0.95), ("p99", 0.99))


def database_identifier(path: Path, name: str) -> str:
    identity = f"{path.resolve(strict=False)}\\{name}.kr".encode("utf-8", errors="surrogatepass")
    return hashlib.sha256(identity).hexdigest()[:16]


def log_event(
    logger: logging.Logger,
    *,
    event: str,
    database_id: str,
    operation: str,
    success: bool,
    duration_ns: int,
    error: BaseException | None = None,
    level: int = logging.INFO,
) -> None:
    if not success:
        level = logging.ERROR
    if not logger.isEnabledFor(level):
        return
    fields: dict[str, Any] = {
        "timestamp_ns": time.time_ns(),
        "event": event,
        "db_id": database_id,
        "operation": operation,
        "success": success,
        "duration_ns": duration_ns,
    }
    if error is not None:
        fields["error_category"] = type(error).__name__
    try:
        logger.log(level, event, extra=fields)
    except Exception:
        # Application handlers must not change an already completed database operation.
        return


def add_latency_percentiles(latencies: dict[str, Any]) -> dict[str, Any]:
    result = {name: dict(summary) for name, summary in latencies.items()}
    for summary in result.values():
        count = summary["count"]
        percentiles: dict[str, int | None] = {}
        for name, fraction in _PERCENTILES:
            if count == 0:
                percentiles[name] = None
                continue
            target = math.ceil(count * fraction)
            cumulative = 0
            estimate: int | None = None
            for upper_bound_ns, bucket_count in summary["buckets"]:
                cumulative += bucket_count
                if cumulative >= target:
                    estimate = min(upper_bound_ns, summary["maximum_ns"])
                    break
            percentiles[name] = estimate
        summary["approximate_percentiles_ns"] = percentiles
        summary["buckets"] = tuple(
            {"upper_bound_ns": upper, "count": bucket}
            for upper, bucket in summary["buckets"]
        )
    return result
