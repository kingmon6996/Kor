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

"""Example 13: High-Throughput In-Memory Operations

Demonstrates:
- Fast bulk writes into RAM backed by ROM persistence.
- Rapid multi-column retrieval of 5,000 records.
- Verifying latency and list format consistency at scale.
"""

import time
from pathlib import Path
from tempfile import TemporaryDirectory
from kor import Database, db, NULL


def main() -> None:
    with TemporaryDirectory(prefix="kor-ex13-") as tmp:
        db_path = Path(tmp)

        num_records = 3000
        hex_token = Database(db_path, "perf_db", 3)
        client = db.connect(db_path, "perf_db", hex_token)

        # 1. Bulk insertion
        records = {
            f"metric:{i:05d}": [f"sensor_{i % 50}", i * 1.5, None if i % 2 == 0 else f"tag_{i}"]
            for i in range(num_records)
        }

        start_write = time.perf_counter()
        client.set_many(records)
        write_time = time.perf_counter() - start_write
        print(f"[Bulk Write] Inserted {num_records} multi-column records in {write_time:.4f}s "
              f"({num_records / write_time:,.0f} ops/sec)")

        assert client.count() == num_records

        # 2. Bulk retrieval
        all_keys = [f"metric:{i:05d}" for i in range(num_records)]
        start_read = time.perf_counter()
        results = client.get_many(all_keys)
        read_time = time.perf_counter() - start_read
        print(f"[Bulk Read] Retrieved {num_records} records in {read_time:.4f}s "
              f"({num_records / read_time:,.0f} ops/sec)")

        # Verify samples
        assert results[0] == ["sensor_0", 0.0, None]
        assert results[1] == ["sensor_1", 1.5, "tag_1"]

        client.close()
        print("[Done] Example 13 completed successfully.")


if __name__ == "__main__":
    main()

