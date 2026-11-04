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

"""Example 3: Default NULL Values for Unset Keys & Omitted Columns

Demonstrates:
- Querying an unset / non-existent key returns a list of NULLs matching the column count.
- Setting only a subset of columns (omitting later columns) automatically populates
  unspecified columns with NULL (None).
- Verifying the base null state for keys.
"""

from pathlib import Path
from tempfile import TemporaryDirectory
from kor import Database, db


def main() -> None:
    with TemporaryDirectory(prefix="kor-ex3-") as tmp:
        db_path = Path(tmp)

        # Initialize a 4-column database
        hex_token = Database(db_path, "defaults_db", 4)
        client = db.connect(db_path, "defaults_db", hex_token)

        # 1. Querying an unset key: all 4 columns must be NULL (None)
        unset_result = client.get("non_existent_key")
        print(f"[Get Unset] non_existent_key -> {unset_result}")
        assert unset_result == [None, None, None, None]
        assert len(unset_result) == 4

        # 2. Setting only c1 and c3; omitting c2 and c4
        client.set(key="user:partial", c1="Alice", c3="Active")
        partial_result = client.get("user:partial")
        print(f"[Get Partial] user:partial -> {partial_result}")
        assert partial_result == ["Alice", None, "Active", None]

        # 3. Setting with only positional first column
        client.set("user:single", "Bob")
        single_result = client.get("user:single")
        print(f"[Get Single] user:single -> {single_result}")
        assert single_result == ["Bob", None, None, None]

        client.close()
        print("[Done] Example 3 completed successfully.")


if __name__ == "__main__":
    main()

