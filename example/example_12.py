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

"""Example 12: Storage Statistics, RAM Metrics & Integrity Verification

Demonstrates:
- Inspecting RAM usage and engine storage statistics via client.storage_stats().
- Viewing diagnostic info with client.diagnostics().
- Verifying database integrity with client.verify_integrity().
"""

from pathlib import Path
from tempfile import TemporaryDirectory
from kor import Database, db


def main() -> None:
    with TemporaryDirectory(prefix="kor-ex12-") as tmp:
        db_path = Path(tmp)

        hex_token = Database(db_path, "diag_db", 3)
        client = db.connect(db_path, "diag_db", hex_token)

        # Ingest sample records
        for i in range(100):
            client.set(key=f"record:{i}", c1=f"val_{i}", c2=i * 10, c3=f"desc_{i}")

        # 1. Inspect storage stats
        stats = client.storage_stats()
        print("[Storage Stats]")
        print(f"  Live record count: {stats.get('live_record_count')}")
        print(f"  Live data bytes:   {stats.get('logical_live_data_bytes_estimate')} bytes")
        print(f"  WAL appended:      {stats.get('wal_bytes_appended')} bytes")

        # 2. Diagnostics
        diag = client.diagnostics()
        print("\n[Diagnostics]")
        print(f"  Name:     {diag.get('name')}")
        print(f"  Path:     {diag.get('path')}")
        print(f"  Started:  {diag.get('started')}")

        # 3. Verify integrity
        integrity = client.verify_integrity(level="full")
        print(f"\n[Integrity Check] Status: {integrity.get('status')}")
        assert integrity.get("status") == "pass"

        client.close()
        print("[Done] Example 12 completed successfully.")


if __name__ == "__main__":
    main()

