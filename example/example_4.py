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

"""Example 4: Enforcing Strict Method Access (No Subscripting)

Demonstrates:
- Attempting dict-like subscript access (e.g., client["user:103"] or client["k"] = v)
  raises a TypeError.
- Showing that only client.get(...) and client.set(...) are supported for record access.
"""

from pathlib import Path
from tempfile import TemporaryDirectory
import pytest
from kor import Database, db


def main() -> None:
    with TemporaryDirectory(prefix="kor-ex4-") as tmp:
        db_path = Path(tmp)

        hex_token = Database(db_path, "strict_db", 2)
        client = db.connect(db_path, "strict_db", hex_token)

        # Storing with set()
        client.set(key="user:103", c1="Alice", c2="Developer")
        print("[Set] user:103 set successfully via client.set()")

        # 1. Attempting subscript read: client["user:103"] -> TypeError
        try:
            val = client["user:103"]  # type: ignore[index]
            raise AssertionError(f"Expected TypeError, got {val}")
        except TypeError as exc:
            print(f"[Verified] Subscript read blocked with TypeError: {exc}")

        # 2. Attempting subscript write: client["user:104"] = "Bob" -> TypeError
        try:
            client["user:104"] = ["Bob", "Tester"]  # type: ignore[index]
            raise AssertionError("Expected TypeError on subscript write")
        except TypeError as exc:
            print(f"[Verified] Subscript write blocked with TypeError: {exc}")

        # 3. Attempting subscript delete: del client["user:103"] -> TypeError
        try:
            del client["user:103"]  # type: ignore[index]
            raise AssertionError("Expected TypeError on subscript delete")
        except TypeError as exc:
            print(f"[Verified] Subscript delete blocked with TypeError: {exc}")

        # 4. Correct retrieval via get()
        res = client.get("user:103")
        print(f"[Get] client.get('user:103') -> {res}")
        assert res == ["Alice", "Developer"]

        client.close()
        print("[Done] Example 4 completed successfully.")


if __name__ == "__main__":
    main()

