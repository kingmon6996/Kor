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

"""Example 9: Atomic Transactions (Commit and Rollback)

Demonstrates:
- Executing multiple mutations atomically inside with client.transaction() as tx.
- Committing changes upon successful context exit.
- Automatic rollback when an unhandled exception occurs inside the transaction.
- Explicit tx.rollback() support.
- DuplicateKeyError inside a transaction rolls back automatically.
"""

from pathlib import Path
from tempfile import TemporaryDirectory
from kor import Database, db, DuplicateKeyError


def main() -> None:
    with TemporaryDirectory(prefix="kor-ex9-") as tmp:
        db_path = Path(tmp)

        hex_token = Database(db_path, "tx_db", 2)
        client = db.connect(db_path, "tx_db", hex_token)

        # 1. Successful commit inside transaction
        with client.transaction() as tx:
            tx.set("account:A", ["Alice", 1000])
            tx.set("account:B", ["Bob", 500])

        assert client.get("account:A") == ["Alice", 1000]
        assert client.get("account:B") == ["Bob", 500]
        print("[Tx Commit] Successfully committed account creations.")

        # 2. Transaction rollback via exception
        try:
            with client.transaction() as tx:
                tx.set("account:A", ["Alice", 900], overwrite=True)
                tx.set("account:B", ["Bob", 600], overwrite=True)
                raise RuntimeError("Simulated network failure during transfer!")
        except RuntimeError as err:
            print(f"[Tx Exception] Caught expected error: {err}")

        # Values should be intact from previous commit
        assert client.get("account:A") == ["Alice", 1000]
        assert client.get("account:B") == ["Bob", 500]
        print("[Tx Rollback] Verified state rolled back to pre-transaction values.")

        # 3. DuplicateKeyError inside a transaction triggers automatic rollback
        try:
            with client.transaction() as tx:
                tx.set("account:C", ["Charlie", 200])  # new key — fine
                tx.set("account:A", ["Alice", 800])    # already exists → DuplicateKeyError
        except DuplicateKeyError as exc:
            print(f"[DuplicateKeyError] Transaction auto-rolled back: {exc}")

        # account:C must not have been committed (whole tx rolled back)
        assert client.contains("account:C") is False
        assert client.get("account:A") == ["Alice", 1000]
        print("[Verified] Partial writes were not committed after DuplicateKeyError.")

        # 4. Explicit tx.rollback()
        with client.transaction() as tx:
            tx.set("account:D", ["Diana", 300])
            tx.rollback()

        assert client.contains("account:D") is False
        assert client.get("account:D") == [None, None]
        print("[Explicit Rollback] Verified account:D not stored after rollback.")

        client.close()
        print("[Done] Example 9 completed successfully.")


if __name__ == "__main__":
    main()
