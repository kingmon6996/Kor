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
import shutil
import threading

import pytest

from kor import (
    Eq,
    AsyncCommitOutcomeUnknownError,
    AsyncDatabase,
    AsyncDatabaseClosedError,
    Database,
    DatabaseFormatError,
    InvalidConfigurationError,
    PersistenceError,
    PersistenceError,
    ReadOnlyError,
    TransactionStateError,
    UniqueConstraintError,
    async_connect,
)


def test_async_connect_uses_persisted_configuration_and_single_file(tmp_path):
    path = tmp_path / "async-existing"
    original = Database(path=path, name="store", max_memory=19, snapshot_interval=75)
    original.set("existing", {"value": 1})
    original.close()
    original_file = path / "store.kr"
    original_bytes = original_file.read_bytes()

    async def scenario():
        database = await async_connect(path=path, name="store")
        assert database.max_memory == 19
        assert database.snapshot_interval == 75
        assert await database.get("existing") == {"value": 1}
        await database.set("new", b"bytes")
        assert await database.contains("new")
        assert await database.count() == 2
        await database.close()
        await database.close()
        with pytest.raises(AsyncDatabaseClosedError):
            await database.get("existing")

    asyncio.run(scenario())
    assert original_file.read_bytes() != original_bytes
    assert sorted(entry.name for entry in path.iterdir()) == ["store.kr"]


def test_async_scan_batch_checkpoint_and_read_only(tmp_path):
    async def scenario():
        database = await async_connect(path=tmp_path, name="async-features")
        await database.set_many({"key:2": 2, "key:1": 1})
        assert await database.get_many(["key:1", "absent"]) == [1, None]
        assert await database.contains_many(["key:2", "absent"]) == [True, False]
        await database.set("user:1", {"name": "Ada", "score": 91})
        assert (await database.query(Eq("name", "Ada")))["records"] == [
            ("user:1", {"name": "Ada", "score": 91})
        ]
        await database.create_composite_index("tenant_email", ["tenant", "email"])
        await database.set_many(
            {
                "account:a": {"tenant": "acme", "email": "a@example.test"},
                "account:b": {"tenant": "acme", "email": "b@example.test"},
            }
        )
        assert [
            key
            for key, _ in await database.find_composite_range(
                ["tenant", "email"],
                ["acme", "a@example.test"],
                ["acme", "b@example.test"],
            )
        ] == ["account:a", "account:b"]
        page = await database.scan(prefix="key:", limit=1)
        assert page["records"] == [("key:1", 1)]
        assert page["has_more"] is True
        assert page["next_cursor"] == "key:1"
        assert (await database.scan(prefix="key:", after=page["next_cursor"]))["records"] == [
            ("key:2", 2)
        ]
        await database.delete_many(["key:1"])
        checkpoint = await database.checkpoint()
        checkpoint_status = await database.checkpoint_status()
        assert checkpoint["checkpoint_performed"] or (
            (await database.storage_stats())["last_checkpoint_lsn"] > 0
        )
        assert "attempt_count" in checkpoint_status
        await database.close()

        reader = await async_connect(path=tmp_path, name="async-features", read_only=True)
        assert reader.read_only is True
        assert await reader.get("key:2") == 2
        with pytest.raises(ReadOnlyError):
            await reader.set("blocked", True)
        await reader.close()

    asyncio.run(scenario())


def test_async_context_manager_and_new_database_defaults(tmp_path):
    async def scenario():
        async with await async_connect(path=tmp_path, name="context") as database:
            assert database.max_memory == 64
            assert database.snapshot_interval == 60
            await database.set("key", "value")
            assert await database.get("key") == "value"
        assert database.is_closed

    asyncio.run(scenario())
    assert sorted(entry.name for entry in tmp_path.iterdir()) == ["context.kr"]


def test_async_and_sync_handles_reject_duplicate_open_file(tmp_path):
    async def scenario():
        database = await async_connect(path=tmp_path, name="one-handle")
        with pytest.raises(PersistenceError, match="already open"):
            await async_connect(path=tmp_path, name="one-handle")
        assert await database.count() == 0
        await database.close()

        synchronous = Database(path=tmp_path, name="one-handle", max_memory=64, snapshot_interval=60)
        synchronous.start()
        with pytest.raises(PersistenceError, match="already open"):
            await async_connect(path=tmp_path, name="one-handle")
        synchronous.close()

        reopened = await async_connect(path=tmp_path, name="one-handle")
        await reopened.close()

    asyncio.run(scenario())


def test_cancelled_connection_initialization_closes_native_resources(tmp_path):
    async def scenario():
        candidate = Database(
            path=tmp_path,
            name="cancel-open",
            max_memory=64,
            snapshot_interval=60,
        )
        database = AsyncDatabase(candidate)
        started = threading.Event()
        release = threading.Event()
        open_database = database._open_database

        def delayed_open():
            started.set()
            release.wait()
            return open_database()

        database._open_database = delayed_open
        opening = asyncio.create_task(database._open())
        assert await asyncio.to_thread(started.wait, 2)
        opening.cancel()
        release.set()
        with pytest.raises(asyncio.CancelledError):
            await opening
        assert database.is_closed
        assert not any(
            thread.name.startswith("kor-cancel-open")
            for thread in threading.enumerate()
        )

        reopened = await async_connect(path=tmp_path, name="cancel-open")
        await reopened.close()

    asyncio.run(scenario())


def test_async_transactions_commit_rollback_and_preserve_indexes(tmp_path):
    async def scenario():
        database = await async_connect(path=tmp_path, name="transactions")
        await database.create_index("email_unique", "email", unique=True)
        await database.create_index("score_order", "score", index_type="ordered")
        async with database.transaction() as transaction:
            await transaction.set("user:1", {"email": "one@example.test", "score": 7})
            assert await transaction.get("user:1") == {
                "email": "one@example.test",
                "score": 7,
            }
            assert await transaction.contains("user:1")

        with pytest.raises(RuntimeError, match="abort"):
            async with database.transaction() as transaction:
                await transaction.set("user:2", {"email": "two@example.test", "score": 9})
                raise RuntimeError("abort")

        assert await database.get("user:1") == {"email": "one@example.test", "score": 7}
        assert await database.get("user:2") is None
        with pytest.raises(UniqueConstraintError):
            await database.set("user:3", {"email": "one@example.test", "score": 10})
        assert [key for key, _ in await database.find("email", "one@example.test")] == ["user:1"]
        assert [key for key, _ in await database.find_range("score", 7, 9)] == ["user:1"]
        await database.set("temporary", "x" * 4096)
        for _ in range(12):
            await database.set("churn", "x" * 4096)
        await database.delete("churn")
        await database.delete("temporary")
        stats = await database.storage_stats()
        assert stats["record_count"] == 1
        compaction = await database.compact()
        after_compaction = await database.storage_stats()
        assert compaction["checkpoint_performed"] or after_compaction["last_checkpoint_lsn"] > 0
        if compaction["checkpoint_performed"]:
            assert compaction["file_physically_shrunk"] is True
        else:
            assert compaction["limitation"]
        assert after_compaction["snapshot_bytes_retained"] > 0
        await database.close()

        reopened = await async_connect(path=tmp_path, name="transactions")
        assert await reopened.get("user:1") == {"email": "one@example.test", "score": 7}
        assert [index.name for index in await reopened.list_indexes()] == [
            "email_unique",
            "score_order",
        ]
        assert [key for key, _ in await reopened.find_range("score", 7, None)] == ["user:1"]
        await reopened.drop_index("email_unique")
        assert [index.name for index in await reopened.list_indexes()] == ["score_order"]
        await reopened.clear()
        assert await reopened.count() == 0
        await reopened.close()

    asyncio.run(scenario())


def test_async_backup_export_import_and_verify(tmp_path):
    async def scenario():
        database = await async_connect(path=tmp_path, name="async-backup")
        await database.set("alpha", {"value": 1})
        backup_path = tmp_path / "backup" / "async-backup.kr"
        backup_path.parent.mkdir(parents=True, exist_ok=True)
        backup = await database.backup(backup_path, overwrite=False)
        assert backup["status"] == "ok"
        verify = await database.verify_integrity()
        assert verify["status"] == "pass"

        export_path = tmp_path / "async-backup.jsonl"
        exported = await database.export_data(export_path, overwrite=False)
        assert exported["status"] == "ok"

        imported = await database.import_data(export_path, conflict="overwrite")
        assert imported["status"] == "ok"
        assert await database.get("alpha") == {"value": 1}
        await database.close()

    asyncio.run(scenario())


def test_async_transaction_cancellation_before_context_commit_rolls_back(tmp_path):
    async def scenario():
        database = await async_connect(path=tmp_path, name="cancel-before-commit")
        with pytest.raises(asyncio.CancelledError):
            async with database.transaction() as transaction:
                await transaction.set("pending", 1)
                raise asyncio.CancelledError
        assert await database.get("pending") is None
        await database.close()

    asyncio.run(scenario())


def test_async_transaction_commit_cancellation_does_not_retry_or_rollback(tmp_path):
    async def scenario():
        database = await async_connect(path=tmp_path, name="cancel-during-commit")
        context = database.transaction()
        transaction = await context.__aenter__()
        await transaction.set("committed", "value")

        native_transaction = transaction._native
        commit_started = threading.Event()
        finish_commit = threading.Event()

        class DelayedCommit:
            def commit(self):
                commit_started.set()
                finish_commit.wait()
                native_transaction.commit()

        transaction._native = DelayedCommit()
        commit_task = asyncio.create_task(transaction.commit())
        assert await asyncio.to_thread(commit_started.wait, 2)
        commit_task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await commit_task

        finish_commit.set()
        assert await database.get("committed") == "value"
        assert transaction._state == "committed"
        await database.close()

    asyncio.run(scenario())


def test_ambiguous_async_commit_requires_reconnect(tmp_path):
    async def scenario():
        database = await async_connect(path=tmp_path, name="ambiguous-commit")
        context = database.transaction()
        transaction = await context.__aenter__()
        await transaction.set("committed", "once")
        native_transaction = transaction._native

        class AmbiguousCommit:
            def commit(self):
                native_transaction.commit()
                raise PersistenceError("synthetic lost commit acknowledgement")

        transaction._native = AmbiguousCommit()
        with pytest.raises(PersistenceError, match="lost commit acknowledgement"):
            await transaction.commit()
        with pytest.raises(AsyncCommitOutcomeUnknownError):
            await database.get("committed")
        await database.close()

        reopened = await async_connect(path=tmp_path, name="ambiguous-commit")
        assert await reopened.get("committed") == "once"
        await reopened.close()

    asyncio.run(scenario())


def test_async_close_rejects_active_transaction_and_repeated_close_is_safe(tmp_path):
    async def scenario():
        database = await async_connect(path=tmp_path, name="close-active")
        context = database.transaction()
        transaction = await context.__aenter__()
        with pytest.raises(TransactionStateError):
            await database.close()
        await transaction.rollback()
        await database.close()
        await database.close()

    asyncio.run(scenario())


def test_async_dispatch_is_responsive_and_bounded(tmp_path):
    async def scenario():
        database = await async_connect(path=tmp_path, name="bounded")
        started = threading.Event()
        release = threading.Event()

        def blocking_operation():
            started.set()
            release.wait()
            return True

        tasks = [
            asyncio.create_task(database._dispatch(blocking_operation))
            for _ in range(20)
        ]
        assert await asyncio.to_thread(started.wait, 2)
        for _ in range(10):
            await asyncio.sleep(0)
        assert database._inflight == 16

        release.set()
        assert all(await asyncio.gather(*tasks))
        await database.close()

    asyncio.run(scenario())


def test_cancelled_inflight_operation_keeps_close_safe(tmp_path):
    async def scenario():
        database = await async_connect(path=tmp_path, name="cancel-close")
        started = threading.Event()
        release = threading.Event()

        def blocking_operation():
            started.set()
            release.wait()

        operation = asyncio.create_task(database._dispatch(blocking_operation))
        assert await asyncio.to_thread(started.wait, 2)
        operation.cancel()
        with pytest.raises(asyncio.CancelledError):
            await operation

        closing = asyncio.create_task(database.close())
        await asyncio.sleep(0)
        assert not closing.done()
        closing.cancel()
        with pytest.raises(asyncio.CancelledError):
            await closing
        release.set()
        await database.close()
        assert database.is_closed

    asyncio.run(scenario())


def test_async_connection_does_not_rewrite_invalid_file(tmp_path):
    database_file = tmp_path / "invalid.kr"
    database_file.write_bytes(b"not a database")
    original = database_file.read_bytes()

    async def scenario():
        with pytest.raises(DatabaseFormatError):
            await async_connect(path=tmp_path, name="invalid")

    asyncio.run(scenario())
    assert database_file.read_bytes() == original


def test_async_connection_identity_mismatch_fails_without_rewriting(tmp_path):
    first = Database(path=tmp_path, name="first", max_memory=8, snapshot_interval=30)
    first.start()
    first.close()
    original = tmp_path / "first.kr"
    alias = tmp_path / "alias.kr"
    original_bytes = original.read_bytes()
    shutil.copyfile(original, alias)
    alias_bytes = alias.read_bytes()

    async def scenario():
        with pytest.raises(InvalidConfigurationError):
            await async_connect(path=tmp_path, name="alias")

    asyncio.run(scenario())
    assert original.read_bytes() == original_bytes
    assert alias.read_bytes() == alias_bytes
