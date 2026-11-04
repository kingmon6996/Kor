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

"""Example 6: Deletion, Membership & Count Operations

Demonstrates:
- Checking key existence with client.contains() and client.contains_many().
- Tracking record count with client.count().
- Deleting a single key with client.delete().
- Deleting multiple keys with client.delete_many().
- Verifying that deleted keys revert to returning null lists on get().
"""

from pathlib import Path
from tempfile import TemporaryDirectory
from kor import Database, db


def main() -> None:
    with TemporaryDirectory(prefix="kor-ex6-") as tmp:
        db_path = Path(tmp)

        hex_token = Database(db_path, "ops_db", 2)
        client = db.connect(db_path, "ops_db", hex_token)

        # Insert 3 records
        client.set(key="item:1", c1="Apple", c2=10)
        client.set(key="item:2", c1="Banana", c2=20)
        client.set(key="item:3", c1="Cherry", c2=30)

        # Count check
        assert client.count() == 3
        print(f"[Count] Total items: {client.count()}")

        # Contains checks
        assert client.contains("item:1") is True
        assert client.contains("item:99") is False
        assert client.contains_many(["item:1", "item:2", "item:99"]) == [True, True, False]
        print("[Membership] contains() and contains_many() verified.")

        # Single deletion
        client.delete("item:1")
        assert client.contains("item:1") is False
        assert client.get("item:1") == [None, None]  # Reverts to null list
        assert client.count() == 2
        print("[Delete] item:1 deleted successfully.")

        # Batch deletion
        client.delete_many(["item:2", "item:3"])
        assert client.count() == 0
        assert client.contains("item:2") is False
        print("[Delete Many] item:2 and item:3 deleted; count is 0.")

        client.close()
        print("[Done] Example 6 completed successfully.")


if __name__ == "__main__":
    main()

