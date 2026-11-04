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

"""Example 14: Asynchronous Database API with Hex Authentication

Demonstrates:
- Connecting asynchronously via kor.async_connect(path, name, hex=hex_token).
- Asynchronous set, get, set_many, get_many, count, and transaction methods.
- Column-based data definition and list retrieval in an async context.
"""

import asyncio
from pathlib import Path
from tempfile import TemporaryDirectory
import kor
from kor import Database, NULL


async def run_async_demo() -> None:
    with TemporaryDirectory(prefix="kor-ex14-") as tmp:
        db_path = Path(tmp)

        # 1. Initialize database with 3 columns
        hex_token = Database(db_path, "async_db", 3)
        print(f"[Async Init] Database created with token: {hex_token}")

        # 2. Connect asynchronously
        async with await kor.async_connect(path=db_path, name="async_db", hex=hex_token) as client:
            # 3. Asynchronous column set
            await client.set(key="job:101", c1="render_video", c2="queued", c3=NULL)
            print("[Async Set] job:101 set.")

            # 4. Asynchronous get returning list
            job_val = await client.get("job:101")
            print(f"[Async Get] job:101 -> {job_val}")
            assert job_val == ["render_video", "queued", None]

            # 5. Async batch set and get
            batch_data = {
                "job:102": ["compress_audio", "running", 50],
                "job:103": ["notify_user", "pending", None],
            }
            await client.set_many(batch_data)
            assert await client.count() == 3

            results = await client.get_many(["job:101", "job:102", "job:missing"])
            assert results[0] == ["render_video", "queued", None]
            assert results[1] == ["compress_audio", "running", 50]
            assert results[2] == [None, None, None]
            print(f"[Async Batch] Verified get_many: {results}")

            # 6. Async transaction
            async with client.transaction() as tx:
                await tx.set("job:104", ["cleanup", "done", 100])

            assert await client.get("job:104") == ["cleanup", "done", 100]
            print("[Async Transaction] Committed job:104.")

        print("[Done] Example 14 completed successfully.")


def main() -> None:
    asyncio.run(run_async_demo())


if __name__ == "__main__":
    main()

