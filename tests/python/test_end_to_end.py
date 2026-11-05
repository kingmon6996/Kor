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

import asyncio
import threading

import pytest

import kor
from kor import (
    Database,
    InvalidConfigurationError,
    InvalidValueError,
    PersistenceError,
    UniqueConstraintError,
    async_connect,
)
from kor import database as database_module


def test_sync_end_to_end_persistence_backup_restore_and_import(tmp_path):
    source_dir = tmp_path / "source"
    source = Database(path=source_dir, name="store", max_memory=16, snapshot_interval=90)
    source.create_index("score_order", "score", index_type="ordered")
    source.set("account:1", {"score": 7, "balance": 2**53 + 1})
    source.set("account:2", {"score": 9, "payload": b"\x00\xff"})
    source.set(
        "account:4",
        {
            "score": 12,
            "nested": [b"\x00\xff", {"empty": b"", "collision": {"$kor": "bytes", "data": "bm90IGJ5dGVz"}}],
        },
    )
    source.set("integer-boundaries", [-(2**63), 2**64 - 1])
    with source.transaction() as transaction:
        transaction.set("account:3", {"score": 11, "nested": [None, True, 2.5]})
        transaction.delete("account:2")

    backup_path = tmp_path / "backup.kr"
    backup = source.backup(backup_path)
    assert backup["status"] == "ok"

    source.set("after-backup", {"score": 13})
    export_path = tmp_path / "records.jsonl"
    source.export_data(export_path)
    assert '"schema_version":2' in export_path.read_text(encoding="utf-8").splitlines()[0]
    source_file = source_dir / "store.kr"
    bytes_before_verify = source_file.read_bytes()
    assert source.verify_integrity()["status"] == "pass"
    assert source_file.read_bytes() == bytes_before_verify

    restored_path = tmp_path / "restored" / "store.kr"
    restored_path.parent.mkdir()
    source.restore(backup_path, restored_path)

    restored = Database(path=restored_path.parent, name="store", max_memory=1, snapshot_interval=60)
    assert restored.get("account:1") == {"score": 7, "balance": 2**53 + 1}
    assert restored.max_memory == 16
    assert restored.snapshot_interval == 90
    assert restored.get("account:2") is None
    assert restored.get("account:3") == {"score": 11, "nested": [None, True, 2.5]}
    assert restored.get("account:4") == {
        "score": 12,
        "nested": [b"\x00\xff", {"empty": b"", "collision": {"$kor": "bytes", "data": "bm90IGJ5dGVz"}}],
    }
    assert restored.get("integer-boundaries") == [-(2**63), 2**64 - 1]
    assert restored.get("after-backup") is None
    assert [key for key, _ in restored.find_range("score", 7, None)] == [
        "account:1",
        "account:3",
        "account:4",
    ]
    assert [index.name for index in restored.list_indexes()] == ["score_order"]
    restored.close()

    imported = Database(path=tmp_path / "imported", name="imported", max_memory=16, snapshot_interval=90)
    imported.import_data(export_path, conflict="overwrite")
    assert imported.get("account:1") == {"score": 7, "balance": 2**53 + 1}
    assert imported.get("account:2") is None
    assert imported.get("account:3") == {"score": 11, "nested": [None, True, 2.5]}
    assert imported.get("account:4") == {
        "score": 12,
        "nested": [b"\x00\xff", {"empty": b"", "collision": {"$kor": "bytes", "data": "bm90IGJ5dGVz"}}],
    }
    assert imported.get("integer-boundaries") == [-(2**63), 2**64 - 1]
    assert imported.get("after-backup") == {"score": 13}
    imported.close()

    source.close()
    assert sorted(path.name for path in source_dir.iterdir()) == ["store.kr"]


def test_sync_reopen_uses_stored_configuration_and_rejects_identity_mismatch(tmp_path):
    first = Database(path=tmp_path, name="identity", max_memory=1, snapshot_interval=60)
    first.set("preserved", True)
    first.close()

    database_file = tmp_path / "identity.kr"
    original_bytes = database_file.read_bytes()
    reopened = Database(path=tmp_path, name="identity", max_memory=8, snapshot_interval=120)
    assert reopened.get("preserved") is True
    assert reopened.max_memory == 1
    assert reopened.snapshot_interval == 60
    assert database_file.read_bytes() == original_bytes
    reopened.close()

    wrong_identity_file = tmp_path / "wrong-name.kr"
    wrong_identity_file.write_bytes(original_bytes)
    wrong_identity = Database(path=tmp_path, name="wrong-name", max_memory=8, snapshot_interval=120)
    with pytest.raises(InvalidConfigurationError, match="identity stored"):
        wrong_identity.start()
    assert wrong_identity_file.read_bytes() == original_bytes


def test_physical_backup_blocks_writes_until_the_file_snapshot_is_published(tmp_path, monkeypatch):
    source = Database(path=tmp_path / "source", name="store", max_memory=16, snapshot_interval=60)
    source.set("state", "before")
    destination = tmp_path / "backup" / "store.kr"
    destination.parent.mkdir()
    copy_entered = threading.Event()
    release_copy = threading.Event()
    writer_started = threading.Event()
    writer_finished = threading.Event()
    errors = []
    original_copy = database_module._copy_file_atomically

    def blocked_copy(*args, **kwargs):
        copy_entered.set()
        if not release_copy.wait(5):
            raise TimeoutError("Timed out waiting to release the backup copy.")
        return original_copy(*args, **kwargs)

    def create_backup():
        try:
            source.backup(destination)
        except Exception as error:
            errors.append(error)

    def write_during_backup():
        writer_started.set()
        try:
            source.set("state", "after")
        except Exception as error:
            errors.append(error)
        finally:
            writer_finished.set()

    monkeypatch.setattr(database_module, "_copy_file_atomically", blocked_copy)
    backup_thread = threading.Thread(target=create_backup)
    backup_thread.start()
    assert copy_entered.wait(5)

    writer_thread = threading.Thread(target=write_during_backup)
    writer_thread.start()
    assert writer_started.wait(5)
    try:
        assert not writer_finished.wait(0.05)
    finally:
        release_copy.set()

    backup_thread.join(timeout=5)
    writer_thread.join(timeout=5)
    assert not backup_thread.is_alive()
    assert not writer_thread.is_alive()
    assert errors == []

    restored = Database(path=destination.parent, name="store", max_memory=16, snapshot_interval=60)
    assert restored.get("state") == "before"
    restored.close()
    assert source.get("state") == "after"
    source.close()


def test_failed_atomic_overwrite_preserves_the_existing_destination(tmp_path, monkeypatch):
    source = Database(path=tmp_path, name="overwrite", max_memory=16, snapshot_interval=60)
    source.set("value", "new")
    backup_path = tmp_path / "existing.kr"
    backup_path.write_bytes(b"previous valid destination")
    export_path = tmp_path / "existing.jsonl"
    export_path.write_bytes(b"previous valid export")

    def fail_replace(_source, _destination):
        raise OSError("injected replace failure")

    monkeypatch.setattr(database_module.os, "replace", fail_replace)
    with pytest.raises(PersistenceError):
        source.backup(backup_path, overwrite=True)
    assert backup_path.read_bytes() == b"previous valid destination"

    with pytest.raises(PersistenceError):
        source.export_data(export_path, overwrite=True)
    assert export_path.read_bytes() == b"previous valid export"
    source.close()


def test_public_restore_validates_and_preserves_destination_identity(tmp_path):
    source = Database(path=tmp_path / "source", name="store", max_memory=16, snapshot_interval=60)
    source.set("restored", {"value": True})
    backup_path = tmp_path / "backup.kr"
    source.backup(backup_path)

    active_source_destination = tmp_path / "active-source-restore" / "store.kr"
    active_source_destination.parent.mkdir()
    with pytest.raises(PersistenceError, match="source database is open"):
        kor.restore(source.path / "store.kr", active_source_destination)
    assert not active_source_destination.exists()
    source.close()

    destination_dir = tmp_path / "destination"
    destination_dir.mkdir()
    destination = destination_dir / "store.kr"
    result = kor.restore(backup_path, destination)
    assert result["status"] == "ok"

    mismatched_identity = destination_dir / "renamed.kr"
    with pytest.raises(PersistenceError, match="preserve database identity"):
        kor.restore(backup_path, mismatched_identity)
    assert not mismatched_identity.exists()

    restored = Database(path=destination_dir, name="store", max_memory=1, snapshot_interval=60)
    assert restored.get("restored") == {"value": True}
    restored.close()

    existing = destination.read_bytes()
    invalid_source = tmp_path / "invalid.kr"
    invalid_source.write_bytes(b"not a Kor database")
    with pytest.raises(kor.DatabaseFormatError):
        kor.restore(invalid_source, destination, overwrite=True)
    assert destination.read_bytes() == existing


def test_restore_refuses_to_replace_an_open_destination_database(tmp_path):
    source = Database(path=tmp_path / "source", name="store", max_memory=16, snapshot_interval=60)
    source.set("source-value", "snapshot")
    backup_path = tmp_path / "backup.kr"
    source.backup(backup_path)
    source.close()

    destination = Database(path=tmp_path / "destination", name="store", max_memory=16, snapshot_interval=60)
    destination.set("destination-value", "keep")
    with pytest.raises(PersistenceError, match="open in this process"):
        kor.restore(backup_path, destination.path / "store.kr", overwrite=True)
    assert destination.get("destination-value") == "keep"
    destination.close()


def test_async_connection_reopens_sync_database_and_persists_native_values(tmp_path):
    synchronous = Database(path=tmp_path, name="cross-api", max_memory=16, snapshot_interval=90)
    synchronous.set("minimum-signed-integer", -(2**63))
    synchronous.set("large-integer", 2**64 - 1)
    synchronous.set("bytes", b"\x00\xff")
    with pytest.raises(InvalidValueError):
        synchronous.set("out-of-range-integer", 2**64)
    synchronous.create_index("score_lookup", "score", index_type="ordered")
    synchronous.set("record", {"score": 5})
    synchronous.close()

    async def scenario():
        database = await async_connect(path=tmp_path, name="cross-api")
        assert database.max_memory == 16
        assert database.snapshot_interval == 90
        assert await database.get("minimum-signed-integer") == -(2**63)
        assert await database.get("large-integer") == 2**64 - 1
        assert await database.get("bytes") == b"\x00\xff"
        assert [key for key, _ in await database.find_range("score", 5, None)] == ["record"]
        async with database.transaction() as transaction:
            await transaction.set("async-write", {"score": 8})
        await database.close()

    asyncio.run(scenario())

    reopened = Database(path=tmp_path, name="cross-api", max_memory=1, snapshot_interval=60)
    assert reopened.get("large-integer") == 2**64 - 1
    assert reopened.get("async-write") == {"score": 8}
    assert [index.name for index in reopened.list_indexes()] == ["score_lookup"]
    assert sorted(path.name for path in tmp_path.iterdir()) == ["cross-api.kr"]
    reopened.close()


def test_atomic_import_with_unique_index_rolls_back_all_records(tmp_path):
    source = Database(path=tmp_path, name="import-source", max_memory=16, snapshot_interval=60)
    source.set("first", {"email": "duplicate@example.test"})
    source.set("second", {"email": "duplicate@example.test"})
    export_path = tmp_path / "invalid-import.jsonl"
    source.export_data(export_path)
    source.close()

    destination = Database(path=tmp_path, name="import-target", max_memory=16, snapshot_interval=60)
    destination.create_index("email_unique", "email", unique=True)
    with pytest.raises(UniqueConstraintError):
        destination.import_data(export_path, conflict="overwrite", atomic=True)
    assert destination.count() == 0
    assert destination.list_indexes()[0].name == "email_unique"
    destination.close()
