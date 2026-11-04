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

"""Example 2: Multi-Column Definition with Explicit NULL

Demonstrates:
- Setting column values using keyword arguments: c1, c2, c3, etc.
- Using explicit kor.NULL (or None) for null column values.
- Exactly matching the user requirement:
  (key="user:102", c1="Jane Smith", c2="Design", c3=NULL)
- Retrieving the stored multi-column row in list format.
"""

from pathlib import Path
from tempfile import TemporaryDirectory
from kor import Database, db, NULL


def main() -> None:
    with TemporaryDirectory(prefix="kor-ex2-") as tmp:
        db_path = Path(tmp)

        # Initialize a 3-column table
        hex_token = Database(db_path, "users_db", 3)
        client = db.connect(db_path, "users_db", hex_token)

        # Setting with key and explicit NULL for c3
        client.set(key="user:102", c1="Jane Smith", c2="Design", c3=NULL)
        print("[Set] Stored user:102 with c1='Jane Smith', c2='Design', c3=NULL")

        # Retrieve row in list format
        record = client.get("user:102")
        print(f"[Get] user:102 -> {record}")
        assert record == ["Jane Smith", "Design", None]
        assert record[2] is None

        # Another record with NULL in c2
        client.set(key="user:103", c1="Bob Doe", c2=NULL, c3="Remote")
        record_103 = client.get("user:103")
        print(f"[Get] user:103 -> {record_103}")
        assert record_103 == ["Bob Doe", None, "Remote"]

        client.close()
        print("[Done] Example 2 completed successfully.")


if __name__ == "__main__":
    main()

