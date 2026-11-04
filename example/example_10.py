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

"""Example 10: ROM Storage with RAM Execution (No Snapshots)

Demonstrates:
- Persistent storage of data in ROM (disk files: .kr and .info).
- Operating entirely in RAM for fast in-memory query and write performance.
- Background periodic auto-snapshots are disabled (persisted directly via ROM WAL).
- Closing the client and reconnecting with the hex token loads the state back into RAM.
"""

from pathlib import Path
from tempfile import TemporaryDirectory
from kor import Database, db


def main() -> None:
    with TemporaryDirectory(prefix="kor-ex10-") as tmp:
        db_path = Path(tmp)

        # Session 1: Initialize database in ROM and populate in RAM
        print("=== Session 1: Creating and populating database ===")
        hex_token = Database(db_path, "store_rom", 3)
        client = db.connect(db_path, "store_rom", hex_token)

        # Store multiple records
        client.set(key="cfg:site", c1="Production", c2="v2.4.0", c3=True)
        client.set(key="cfg:db_pool", c1="Primary", c2="max_32", c3=32)
        assert client.count() == 2

        # Check files stored in ROM on disk (only single .kr file, no .info files)
        data_file = db_path / "store_rom.kr"
        info_file = db_path / "store_rom.info"
        assert data_file.exists(), "Single .kr database file should exist in ROM"
        assert not info_file.exists(), "No .info files allowed! Only single .kr file."
        print(f"[ROM File] Single file: {data_file.name} ({data_file.stat().st_size} bytes)")

        # Close session 1
        client.close()
        print("[Session 1] Closed.")

        # Session 2: Connect to existing ROM database using hex_token
        print("\n=== Session 2: Reconnecting from ROM into RAM ===")
        client2 = db.connect(db_path, "store_rom", hex_token)
        print(f"[Session 2] Connected: {client2.name}, columns: {client2.number_of_values}")
        assert client2.number_of_values == 3

        # Verify all records loaded from ROM into RAM
        site_cfg = client2.get("cfg:site")
        db_pool_cfg = client2.get("cfg:db_pool")
        print(f"[Session 2] cfg:site -> {site_cfg}")
        print(f"[Session 2] cfg:db_pool -> {db_pool_cfg}")

        assert site_cfg == ["Production", "v2.4.0", True]
        assert db_pool_cfg == ["Primary", "max_32", 32]
        assert client2.count() == 2

        client2.close()
        print("[Done] Example 10 completed successfully.")


if __name__ == "__main__":
    main()

