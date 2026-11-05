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

import os

from kor import Database


def test_bounded_native_write_index_compaction_and_recovery_stress(tmp_path):
    profile = os.environ.get("KOR_STRESS_PROFILE", "quick")
    record_count = {"quick": 300, "extended": 1000}.get(profile)
    if record_count is None:
        raise ValueError("KOR_STRESS_PROFILE must be 'quick' or 'extended'.")

    database = Database(path=tmp_path, name="stress", max_memory=64, snapshot_interval=60)
    database.create_index("bucket_lookup", "bucket")
    expected = {}
    batch_size = 50
    for first in range(0, record_count, batch_size):
        with database.transaction() as transaction:
            for number in range(first, min(first + batch_size, record_count)):
                key = f"record:{number:05}"
                value = {"bucket": number % 17, "value": number, "payload": "x" * 64}
                transaction.set(key, value)
                expected[key] = value

    for number in range(0, record_count, 3):
        key = f"record:{number:05}"
        value = {"bucket": number % 17, "value": number + record_count, "payload": "y" * 64}
        database.set(key, value)
        expected[key] = value
    for number in range(1, record_count, 7):
        key = f"record:{number:05}"
        database.delete(key)
        expected.pop(key)

    for _ in range(24):
        database.set("churn", "z" * 4096)
        database.delete("churn")

    compacted = database.compact()
    stats = database.storage_stats()
    assert compacted["checkpoint_performed"] or stats["last_checkpoint_lsn"] > 0
    assert database.count() == len(expected)
    for bucket in range(17):
        actual = dict(database.find("bucket", bucket))
        assert actual == {
            key: value for key, value in expected.items() if value["bucket"] == bucket
        }

    backup_path = tmp_path / "stress-backup.kr"
    database.backup(backup_path)
    restore_dir = tmp_path / "restored"
    restore_dir.mkdir()
    database.restore(backup_path, restore_dir / "stress.kr")
    database.set("after-backup", {"bucket": 99, "value": -1})
    database.close()

    reopened = Database(path=tmp_path, name="stress", max_memory=1, snapshot_interval=5)
    assert reopened.count() == len(expected) + 1
    for key, value in expected.items():
        assert reopened.get(key) == value
    assert reopened.get("after-backup") == {"bucket": 99, "value": -1}
    assert [index.name for index in reopened.list_indexes()] == ["bucket_lookup"]
    reopened.close()

    restored = Database(path=restore_dir, name="stress", max_memory=1, snapshot_interval=5)
    assert restored.count() == len(expected)
    assert restored.get("after-backup") is None
    restored.close()
