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

"""Example 7: Batch Ingestion and Retrieval (set_many & get_many)

Demonstrates:
- Using client.set_many() with column dictionaries (e.g. {"c1": ..., "c2": ...}).
- Using client.set_many() with value lists (e.g. ["val1", "val2"]).
- Using client.get_many() to fetch multiple keys in a single round-trip.
- Ensuring missing keys in get_many return null column lists.
"""

from pathlib import Path
from tempfile import TemporaryDirectory
from kor import Database, db, NULL


def main() -> None:
    with TemporaryDirectory(prefix="kor-ex7-") as tmp:
        db_path = Path(tmp)

        hex_token = Database(db_path, "batch_db", 3)
        client = db.connect(db_path, "batch_db", hex_token)

        # 1. Batch insertion using column-name mappings
        records_dict = {
            "prod:101": {"c1": "Keyboard", "c2": "Electronics", "c3": 79.99},
            "prod:102": {"c1": "Mouse", "c2": "Electronics", "c3": 29.99},
            "prod:103": {"c1": "Desk Mat", "c2": "Accessories", "c3": NULL},
        }
        client.set_many(records_dict)
        print(f"[set_many dict] Inserted {len(records_dict)} records.")

        # 2. Batch insertion using list of values
        records_list = {
            "prod:104": ["Monitor", "Electronics", 299.99],
            "prod:105": ["Webcam", "Electronics", 49.99],
        }
        client.set_many(records_list)
        print(f"[set_many list] Inserted {len(records_list)} records.")

        assert client.count() == 5

        # 3. Batch retrieval with get_many
        keys_to_fetch = ["prod:101", "prod:103", "prod:104", "prod:missing"]
        results = client.get_many(keys_to_fetch)
        print(f"[get_many] Fetched {len(results)} items:")
        for k, v in zip(keys_to_fetch, results):
            print(f"  {k} -> {v}")

        assert results[0] == ["Keyboard", "Electronics", 79.99]
        assert results[1] == ["Desk Mat", "Accessories", None]
        assert results[2] == ["Monitor", "Electronics", 299.99]
        assert results[3] == [None, None, None]  # Missing key returns null list

        client.close()
        print("[Done] Example 7 completed successfully.")


if __name__ == "__main__":
    main()

