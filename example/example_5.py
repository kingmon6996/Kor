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

"""Example 5: Duplicate Key Protection & Explicit Overwrites

Demonstrates:
- set() raises DuplicateKeyError when a key already exists (default behaviour).
- set_many() raises DuplicateKeyError listing all duplicate keys found.
- Explicit overwrite=True allows replacing an existing record.
- Verifying that only overwrite=True mutates an existing record.
"""

from pathlib import Path
from tempfile import TemporaryDirectory
from kor import Database, db, DuplicateKeyError


def main() -> None:
    with TemporaryDirectory(prefix="kor-ex5-") as tmp:
        db_path = Path(tmp)

        hex_token = Database(db_path, "orders_db", 3)
        client = db.connect(db_path, "orders_db", hex_token)

        # Initial insert
        client.set(key="order:5001", c1="Customer-A", c2="Pending", c3=150.0)
        initial = client.get("order:5001")
        print(f"[Initial] order:5001 -> {initial}")
        assert initial == ["Customer-A", "Pending", 150.0]

        # 1. Default behaviour: set() on an existing key raises DuplicateKeyError
        try:
            client.set(key="order:5001", c1="Customer-A", c2="Shipped", c3=175.5)
            raise AssertionError("Expected DuplicateKeyError")
        except DuplicateKeyError as exc:
            print(f"[DuplicateKeyError] set() blocked duplicate: {exc}")

        # Confirm record is unchanged
        assert client.get("order:5001") == ["Customer-A", "Pending", 150.0]
        print("[Verified] Record unchanged after blocked set().")

        # 2. Explicit overwrite=True allows the update
        client.set(key="order:5001", c1="Customer-A", c2="Shipped", c3=175.5, overwrite=True)
        updated = client.get("order:5001")
        print(f"[Overwrite] order:5001 -> {updated}")
        assert updated == ["Customer-A", "Shipped", 175.5]

        # 3. set_many() also raises DuplicateKeyError for any existing key
        client.set(key="order:5002", c1="Customer-B", c2="Pending", c3=80.0)
        try:
            client.set_many({
                "order:5001": ["Customer-A", "Delivered", 175.5],  # already exists
                "order:5002": ["Customer-B", "Shipped",   80.0],   # already exists
                "order:5003": ["Customer-C", "Pending",   60.0],   # new — fine
            })
            raise AssertionError("Expected DuplicateKeyError from set_many()")
        except DuplicateKeyError as exc:
            print(f"[DuplicateKeyError] set_many() blocked duplicates: {exc}")

        # 5003 was not inserted because the whole batch was rejected
        assert client.contains("order:5003") is False
        print("[Verified] New key was not inserted — batch rejected atomically.")

        # 4. set_many() with overwrite=True accepts the whole batch
        client.set_many({
            "order:5001": ["Customer-A", "Delivered", 175.5],
            "order:5002": ["Customer-B", "Shipped",   80.0],
            "order:5003": ["Customer-C", "Pending",   60.0],
        }, overwrite=True)
        assert client.get("order:5001") == ["Customer-A", "Delivered", 175.5]
        assert client.contains("order:5003") is True
        print("[Overwrite Many] All three records written with overwrite=True.")

        client.close()
        print("[Done] Example 5 completed successfully.")


if __name__ == "__main__":
    main()
