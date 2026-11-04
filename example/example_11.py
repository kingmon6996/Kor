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

"""Example 11: Hexadecimal Authentication Token Security & Normalization

Demonstrates:
- Connecting using exact hex tokens, uppercase tokens, or 0x-prefixed/raw tokens.
- Automatic rejection (AuthenticationError) when an incorrect hex is provided.
- Ensuring only authorized callers with the valid hex token can mount the database.
"""

from pathlib import Path
from tempfile import TemporaryDirectory
from kor import Database, db, AuthenticationError


def main() -> None:
    with TemporaryDirectory(prefix="kor-ex11-") as tmp:
        db_path = Path(tmp)

        # Initialize database and get hex token
        hex_token = Database(db_path, "secure_vault", 2)
        print(f"[Init] Generated Hex Token: {hex_token}")

        # 1. Connect with standard token string
        client = db.connect(db_path, "secure_vault", hex_token)
        client.set(key="secret:key", c1="AES256", c2="super_secret_payload")
        client.close()

        # 2. Connect with raw hex string (stripping any leading 0x)
        hex_raw = str(hex_token).lower().removeprefix("0x")
        client_raw = db.connect(db_path, "secure_vault", hex_raw)
        assert client_raw.get("secret:key") == ["AES256", "super_secret_payload"]
        client_raw.close()
        print("[Auth] Raw hex token (without 0x) connection succeeded.")

        # 3. Connect with explicit 0x-prefixed hex string
        hex_0x = f"0x{hex_raw}"
        client_0x = db.connect(db_path, "secure_vault", hex_0x)
        assert client_0x.get("secret:key") == ["AES256", "super_secret_payload"]
        client_0x.close()
        print("[Auth] 0x-prefixed hex token connection succeeded.")

        # 4. Connect with uppercase hex string
        hex_upper = hex_raw.upper()
        client_upper = db.connect(db_path, "secure_vault", hex_upper)
        assert client_upper.get("secret:key") == ["AES256", "super_secret_payload"]
        client_upper.close()
        print("[Auth] Uppercase hex token connection succeeded.")

        # 5. Attempt connection with invalid hex token
        bad_token = "0xdeadbeefbadf00d1234567890abcdef1"
        try:
            db.connect(db_path, "secure_vault", bad_token)
            raise AssertionError("Connection with bad hex token should have failed!")
        except AuthenticationError as exc:
            print(f"[Auth] Unauthorized access rejected as expected: {exc}")

        print("[Done] Example 11 completed successfully.")


if __name__ == "__main__":
    main()
