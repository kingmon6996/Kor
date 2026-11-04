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

"""Example 1: Basic Database Initialization & Connection

Demonstrates:
- Initializing the database with path, name, and column count (number_of_values).
- Receiving the unique hexadecimal authentication token.
- Connecting via db.connect(path, name, hex).
- Storing multi-column values and retrieving them in list format.
"""

from pathlib import Path
from tempfile import TemporaryDirectory
from kor import Database, db


def main() -> None:
    with TemporaryDirectory(prefix="kor-ex1-") as tmp:
        db_path = Path(tmp)

        # 1. Initialize database with 3 columns per key -> returns hex token
        hex_token = Database(db_path, "quickstart", 3)
        print(f"[Init] Database initialized with hex: {hex_token}")

        # 2. Connect from next time using db.connect(path, name, hex)
        client = db.connect(db_path, "quickstart", hex_token)
        print(f"[Connect] Connected to database: {client.name}")

        # 3. Store columns for a key
        client.set(key="emp:001", c1="Alice", c2="Engineering", c3=120000)

        # 4. get() returns all columns in list format
        values = client.get("emp:001")
        print(f"[Get] emp:001 values: {values} (type: {type(values).__name__})")
        assert values == ["Alice", "Engineering", 120000]

        client.close()
        print("[Done] Example 1 completed successfully.")


if __name__ == "__main__":
    main()

