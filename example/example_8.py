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

"""Example 8: Key Prefix Scanning & Cursor Pagination

Demonstrates:
- Scanning records matching a key prefix (client.scan).
- Traversing pages of multi-column results using cursors (next_cursor).
- Receiving values formatted as column lists in scan results.
"""

from pathlib import Path
from tempfile import TemporaryDirectory
from kor import Database, db


def main() -> None:
    with TemporaryDirectory(prefix="kor-ex8-") as tmp:
        db_path = Path(tmp)

        hex_token = Database(db_path, "scan_db", 2)
        client = db.connect(db_path, "scan_db", hex_token)

        # Insert 15 sensor records and 5 system records
        for i in range(15):
            client.set(key=f"sensor:{i:03d}", c1=f"Room-{i%3}", c2=20.0 + i * 0.5)
        for i in range(5):
            client.set(key=f"sys:{i:03d}", c1="OK", c2=100)

        # 1. Scan with prefix filter
        page1 = client.scan(prefix="sensor:", limit=6)
        records_page1 = page1["records"]
        print(f"[Scan Page 1] Found {len(records_page1)} records, has_more={page1['has_more']}")
        assert len(records_page1) == 6
        assert page1["has_more"] is True
        # Verify first record is in list format
        first_key, first_val = records_page1[0]
        print(f"  First item: {first_key} -> {first_val}")
        assert isinstance(first_val, list)
        assert len(first_val) == 2

        # 2. Page through remaining records
        all_sensor_keys = [k for k, _ in records_page1]
        cursor = page1["next_cursor"]

        while cursor is not None:
            page = client.scan(prefix="sensor:", after=cursor, limit=6)
            for k, _ in page["records"]:
                all_sensor_keys.append(k)
            if not page["has_more"]:
                break
            cursor = page["next_cursor"]

        print(f"[Scan Complete] Traversed all {len(all_sensor_keys)} sensor keys.")
        assert len(all_sensor_keys) == 15
        assert all(k.startswith("sensor:") for k in all_sensor_keys)

        client.close()
        print("[Done] Example 8 completed successfully.")


if __name__ == "__main__":
    main()

