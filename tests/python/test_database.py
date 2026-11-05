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

from pathlib import Path
import threading
import time
from dataclasses import dataclass

import pytest

from kor import (
    And,
    Database,
    Eq,
    Exists,
    IndexExistsError,
    IndexNotFoundError,
    InvalidConfigurationError,
    InsufficientSystemMemoryError,
    MemoryUsageWarning,
    MemoryLimitExceededError,
    Not,
    Or,
    Range,
    PersistenceError,
    ReadOnlyError,
    RecoveryError,
    TransactionStateError,
    TransactionTooLargeError,
    UniqueConstraintError,
    UnsupportedIndexValueError,
    check_memory_budget,
)


@dataclass
class UserModel:
    name: str
    score: int

    def to_dict(self):
        return {"name": self.name, "score": self.score}

    @classmethod
    def from_dict(cls, value):
        return cls(**value)


def test_database_valid_configuration():
    db = Database(path="database", name="store", max_memory=16, snapshot_interval=300)

    assert db.path == Path("database").resolve()
    assert db.name == "store"
    assert db.max_memory == 16
    assert db.snapshot_interval == 300

    root_db = Database(name="store", max_memory=16, snapshot_interval=300)
    assert root_db.path == Path.cwd().resolve()
    assert root_db.name == "store"


def test_memory_budget_checker_rejects_over_available_ram(monkeypatch):
    monkeypatch.setattr("kor.memory.available_memory_bytes", lambda: 64 * 1024 * 1024)

    exact_limit = check_memory_budget(32)
    assert exact_limit["warning"] is False
    with pytest.raises(InsufficientSystemMemoryError, match="exceeds currently available RAM"):
        check_memory_budget(65)
    with pytest.warns(MemoryUsageWarning, match="more than 50%"):
        Database(name="memory-warning", max_memory=33)


def test_database_rechecks_memory_budget_on_startup(monkeypatch, tmp_path):
    available = {"bytes": 2 * 1024 * 1024 * 1024}
    monkeypatch.setattr("kor.memory.available_memory_bytes", lambda: available["bytes"])
    database = Database(path=tmp_path, name="startup-memory-warning", max_memory=512)

    available["bytes"] = 900 * 1024 * 1024
    with pytest.warns(MemoryUsageWarning, match="database startup"):
        database.start()
    database.close()


def test_database_startup_memory_error_happens_before_file_creation(monkeypatch, tmp_path):
    available = {"bytes": 2 * 1024 * 1024 * 1024}
    monkeypatch.setattr("kor.memory.available_memory_bytes", lambda: available["bytes"])
    database = Database(path=tmp_path, name="startup-memory-error", max_memory=512)

    available["bytes"] = 256 * 1024 * 1024
    with pytest.raises(InsufficientSystemMemoryError):
        database.start()
    assert not (tmp_path / "startup-memory-error.kr").exists()


def test_database_uses_automatic_checkpointing_without_interval_argument(tmp_path):
    database = Database(path=tmp_path, name="automatic-checkpoint", max_memory=16)
    assert database.snapshot_interval == 60

    for _ in range(24):
        database.set("latest", "x" * 4096)

    # Allow up to 15 s: Python 3.14 incremental GC can delay background threads
    deadline = time.monotonic() + 15
    stats = database.storage_stats()
    while stats["last_checkpoint_lsn"] == 0 and time.monotonic() < deadline:
        time.sleep(0.05)
        stats = database.storage_stats()

    assert stats["last_checkpoint_lsn"] > 0
    assert stats["last_auto_snapshot_error"] == ""
    database.close()

    reopened = Database(path=tmp_path, name="automatic-checkpoint", max_memory=16)
    assert reopened.get("latest") == "x" * 4096
    reopened.close()


def test_checkpoint_status_and_explicit_checkpoint_without_shrinking(tmp_path):
    database = Database(path=tmp_path, name="checkpoint-api", max_memory=16)
    database.set("alpha", {"value": 1})
    database_file = tmp_path / "checkpoint-api.kr"
    before_size = database_file.stat().st_size

    status = database.checkpoint_status()
    assert set(status) == {
        "last_attempt_lsn",
        "last_success_lsn",
        "last_attempt_duration_ns",
        "attempt_count",
        "success_count",
        "failure_count",
        "in_progress",
        "last_error",
    }
    result = database.checkpoint()

    status = database.storage_stats()
    assert result["checkpoint_performed"] or status["last_checkpoint_lsn"] > 0
    if result["checkpoint_performed"]:
        assert result["file_physically_shrunk"] is False
        assert result["file_size_after_bytes"] > before_size
    else:
        assert result["limitation"] == "No committed WAL records require a new checkpoint."
    stats = database.storage_stats()
    assert stats["last_checkpoint_lsn"] > 0
    file_size_after_checkpoint = database_file.stat().st_size
    repeated = database.checkpoint()
    assert repeated["checkpoint_performed"] is False
    assert repeated["file_size_after_bytes"] == file_size_after_checkpoint
    assert repeated["limitation"] == "No committed WAL records require a new checkpoint."
    database.close()

    reopened = Database(path=tmp_path, name="checkpoint-api", max_memory=16)
    assert reopened.get("alpha") == {"value": 1}
    reopened.close()


def test_bounded_scan_is_sorted_and_uses_exclusive_cursor(tmp_path):
    database = Database(path=tmp_path, name="scan", max_memory=16)
    for key in ("item:03", "item:01", "other:01", "item:02"):
        database.set(key, key)

    first = database.scan(prefix="item:", limit=2)
    assert first["records"] == [("item:01", "item:01"), ("item:02", "item:02")]
    assert first["has_more"] is True
    assert first["next_cursor"] == "item:02"

    second = database.scan(prefix="item:", after=first["next_cursor"], limit=2)
    assert second["records"] == [("item:03", "item:03")]
    assert second["has_more"] is False
    assert second["next_cursor"] is None
    with pytest.raises(InvalidConfigurationError):
        database.scan(limit=1001)
    database.close()


def test_multi_key_reads_preserve_input_order_and_missing_values(tmp_path):
    database = Database(path=tmp_path, name="multi-read", max_memory=16)
    database.set_many({"first": None, "second": "value"})

    assert database.get_many(["second", "missing", "first"]) == ["value", None, None]
    assert database.get_many(["missing"]) == [None]
    assert database.contains_many(["second", "missing", "first"]) == [True, False, True]
    assert database.get_many([]) == []
    with pytest.raises(InvalidConfigurationError):
        database.get_many([f"key:{index}" for index in range(10_001)])
    database.close()


def test_typed_models_and_structured_query_pagination(tmp_path):
    database = Database(path=tmp_path, name="model-query", max_memory=16)
    database.set_model("user:1", UserModel("Ada", 91))
    database.set("user:2", {"name": "Lin", "score": 75, "active": True})
    database.set("user:3", {"name": "Grace", "score": 88})

    assert database.get_model("user:1", UserModel) == UserModel("Ada", 91)
    assert database.get_model("missing", UserModel) is None
    query = And(Range("score", 80), Or(Eq("active", True), Not(Exists("active"))))
    first = database.query(query, prefix="user:", limit=1)
    assert first == {
        "records": [("user:1", {"name": "Ada", "score": 91})],
        "next_cursor": "user:1",
        "has_more": True,
    }
    second = database.query(query, prefix="user:", after=first["next_cursor"], limit=1)
    assert second == {
        "records": [("user:3", {"name": "Grace", "score": 88})],
        "next_cursor": None,
        "has_more": False,
    }
    database.close()


def test_composite_index_exact_lookup_uniqueness_and_persistence(tmp_path):
    database = Database(path=tmp_path, name="composite-index", max_memory=16)
    database.set("a", {"tenant": "acme", "email": "a@example.test"})
    database.set("b", {"tenant": "acme", "email": "b@example.test"})
    database.set("c", {"tenant": "other", "email": "a@example.test"})
    database.create_composite_index("tenant_email", ["tenant", "email"], unique=True)

    assert database.find_composite(["tenant", "email"], ["acme", "a@example.test"]) == [
        ("a", {"tenant": "acme", "email": "a@example.test"})
    ]
    assert database.find_composite_range(
        ["tenant", "email"],
        ["acme", "b@example.test"],
        ["acme", "z@example.test"],
    ) == [("b", {"tenant": "acme", "email": "b@example.test"})]
    assert database.list_indexes()[0].fields == ["tenant", "email"]
    with pytest.raises(UniqueConstraintError):
        database.set("duplicate", {"tenant": "acme", "email": "a@example.test"})
    assert database.contains("duplicate") is False
    database.checkpoint()
    database.close()

    reopened = Database(path=tmp_path, name="composite-index", max_memory=16)
    assert reopened.find_composite(["tenant", "email"], ["other", "a@example.test"])[0][0] == "c"
    reopened.close()


def test_batch_writes_are_atomic_and_index_aware(tmp_path):
    database = Database(path=tmp_path, name="batch", max_memory=16)
    database.set("existing", {"email": "used@example.test"})
    database.create_index("unique_email", "email", unique=True)

    with pytest.raises(UniqueConstraintError):
        database.set_many(
            {
                "first": {"email": "first@example.test"},
                "conflict": {"email": "used@example.test"},
            }
        )
    assert database.get("first") is None
    assert database.get("conflict") is None

    database.set_many(
        {
            "first": {"email": "first@example.test"},
            "second": {"email": "second@example.test"},
        }
    )
    database.delete_many(["first", "missing"])
    assert database.get("first") is None
    assert database.get("second") == {"email": "second@example.test"}
    database.close()


def test_read_only_open_rejects_all_storage_mutations(tmp_path):
    original = Database(path=tmp_path, name="reader", max_memory=16)
    original.set("saved", "value")
    original.close()
    path = tmp_path / "reader.kr"
    original_bytes = path.read_bytes()

    reader = Database(path=tmp_path, name="reader", read_only=True)
    assert reader.read_only is True
    assert reader.get("saved") == "value"
    assert reader.scan()["records"] == [("saved", "value")]
    with pytest.raises(ReadOnlyError):
        reader.set("new", "value")
    with pytest.raises(ReadOnlyError):
        reader.delete("absent")
    with pytest.raises(ReadOnlyError):
        reader.set_many({})
    with pytest.raises(ReadOnlyError):
        reader.delete_many([])
    with pytest.raises(ReadOnlyError):
        reader.create_index("idx", "field")
    with pytest.raises(ReadOnlyError):
        reader.drop_index("absent")
    with pytest.raises(ReadOnlyError):
        reader.import_data("unused.jsonl")
    with pytest.raises(ReadOnlyError):
        reader.transaction()
    with pytest.raises(ReadOnlyError):
        reader.checkpoint()
    with pytest.raises(ReadOnlyError):
        reader.compact()
    reader.close()
    assert path.read_bytes() == original_bytes

    with pytest.raises(PersistenceError, match="existing regular database file"):
        Database(path=tmp_path, name="missing", read_only=True).start()


def test_database_rejects_invalid_values():
    with pytest.raises(InvalidConfigurationError):
        Database(path="database", name="", max_memory=512, snapshot_interval=300)

    with pytest.raises(InvalidConfigurationError):
        Database(path="database", name="store", max_memory=0, snapshot_interval=300)

    with pytest.raises(InvalidConfigurationError):
        Database(path="database", name="store", max_memory=512, snapshot_interval=4)

    with pytest.raises(InvalidConfigurationError):
        Database(path="database", name="store.kr", max_memory=512, snapshot_interval=300)

    with pytest.raises(InvalidConfigurationError):
        Database(path="database", name="CON", max_memory=512, snapshot_interval=300)

    with pytest.raises(InvalidConfigurationError):
        Database(path="database", name="store", max_memory=True, snapshot_interval=300)

    with pytest.raises(InvalidConfigurationError):
        Database(path="database", name="store", max_memory=512, snapshot_interval=True)


def test_database_does_not_create_sidecar_files(tmp_path):
    db_dir = tmp_path / "database"
    db = Database(path=db_dir, name="store", max_memory=128, snapshot_interval=60)

    assert not db_dir.exists()
    assert not (db_dir / "store.kr").exists()
    assert not (db_dir / "store.wal").exists()
    assert not (db_dir / "store.meta").exists()


def test_database_start_initializes_single_file_database(tmp_path):
    db_dir = tmp_path / "database"
    db = Database(path=db_dir, name="store", max_memory=128, snapshot_interval=60)

    db.start()

    database_file = db_dir / "store.kr"
    assert database_file.exists()
    assert database_file.is_file()
    assert not (db_dir / "store.wal").exists()
    assert not (db_dir / "store.meta").exists()

    assert db._path == db_dir.resolve()


def test_database_rejects_constructor_kwargs_for_persistence_and_durability():
    with pytest.raises(TypeError):
        Database(path="database", name="store", max_memory=512, snapshot_interval=300, persistence="none")

    with pytest.raises(TypeError):
        Database(path="database", name="store", max_memory=512, snapshot_interval=300, durability="fast")


def test_transaction_context_commits_and_recovers_from_single_file(tmp_path):
    db_dir = tmp_path / "transactions"
    db = Database(path=db_dir, name="store", max_memory=16, snapshot_interval=60)
    with db.transaction() as transaction:
        transaction.set("user:1", {"name": "Alice"})
        transaction.set("user:2", {"name": "Bob"})
        transaction.set("cache:old", None)
        transaction.delete("cache:old")
        assert transaction.get("user:1") == {"name": "Alice"}
        assert transaction.contains("user:2")
        assert not transaction.contains("cache:old")

    assert db.get("user:1") == {"name": "Alice"}
    assert db.get("user:2") == {"name": "Bob"}
    assert db.get("cache:old", "missing") == "missing"
    db.close()

    reopened = Database(path=db_dir, name="store", max_memory=16, snapshot_interval=60)
    assert reopened.get("user:1") == {"name": "Alice"}
    assert reopened.get("user:2") == {"name": "Bob"}
    assert sorted(path.name for path in db_dir.iterdir()) == ["store.kr"]
    reopened.close()


def test_transaction_context_rolls_back_on_exception_and_clear_then_set(tmp_path):
    db = Database(path=tmp_path, name="rollback", max_memory=16, snapshot_interval=60)
    db.set("existing", 1)

    with pytest.raises(RuntimeError, match="abort"):
        with db.transaction() as transaction:
            transaction.set("existing", 2)
            transaction.set("temporary", True)
            raise RuntimeError("abort")

    assert db.get("existing") == 1
    assert db.get("temporary", None) is None

    with db.transaction() as transaction:
        transaction.clear()
        assert transaction.get("existing") is None
        transaction.set("replacement", 3)

    assert db.get("existing", "missing") == "missing"
    assert db.get("replacement") == 3
    db.close()


def test_transaction_explicit_lifecycle_and_close_waits_for_cleanup(tmp_path):
    db = Database(path=tmp_path, name="explicit", max_memory=16, snapshot_interval=60)
    transaction = db.transaction()
    transaction.set("key", "value")
    transaction.commit()
    assert transaction.committed()
    with pytest.raises(TransactionStateError):
        transaction.commit()
    with pytest.raises(TransactionStateError):
        transaction.rollback()

    rolled_back = db.transaction()
    rolled_back.rollback()
    rolled_back.rollback()
    with pytest.raises(TransactionStateError):
        rolled_back.commit()

    active = db.transaction()
    with pytest.raises(TransactionStateError):
        db.close()
    active.rollback()
    db.close()


def test_transaction_write_set_limit_rejects_without_mutating_database(tmp_path):
    db = Database(path=tmp_path, name="bounded", max_memory=1, snapshot_interval=60)
    with pytest.raises(TransactionTooLargeError):
        with db.transaction() as transaction:
            transaction.set("oversized", "x" * (1024 * 1024))

    assert db.get("oversized", "missing") == "missing"
    db.close()


def test_standalone_writer_waits_for_transaction_commit_without_gil_deadlock(tmp_path):
    db = Database(path=tmp_path, name="concurrent", max_memory=16, snapshot_interval=60)
    transaction = db.transaction()
    transaction.set("key", 1)

    writer_started = threading.Event()
    writer_finished = threading.Event()
    writer_errors = []

    def standalone_write():
        try:
            writer_started.set()
            db.set("key", 3)
        except Exception as exc:  # surface worker-thread errors in the test thread
            writer_errors.append(exc)
        finally:
            writer_finished.set()

    thread = threading.Thread(target=standalone_write)
    thread.start()
    assert writer_started.wait(2)
    assert not writer_finished.wait(0.05)
    transaction.commit()
    thread.join(timeout=2)

    assert not thread.is_alive()
    assert not writer_errors
    assert db.get("key") == 3
    db.close()


def test_secondary_index_equality_range_fallback_and_mutations(tmp_path):
    db = Database(path=tmp_path, name="query", max_memory=16, snapshot_interval=60)
    db.set("user:z", {"email": "same@example.test", "age": 30})
    db.set("user:a", {"email": "same@example.test", "age": 20})
    db.set("user:m", {"email": None, "age": 25})

    assert [key for key, _ in db.find("email", "same@example.test")] == ["user:a", "user:z"]
    db.create_index("email_lookup", "email", index_type="hash")
    db.create_index("age_order", "age", index_type="ordered")

    assert [key for key, _ in db.find("email", "same@example.test")] == ["user:a", "user:z"]
    assert [key for key, _ in db.find_range("age", 20, 30)] == ["user:a", "user:m", "user:z"]
    assert [key for key, _ in db.find_range("age", 20, 30, include_minimum=False)] == ["user:m", "user:z"]
    assert db.find_range("age", 30, 20) == []
    assert [key for key, _ in db.find("email", None)] == []
    assert next(index for index in db.list_indexes() if index.name == "email_lookup").entry_count == 2

    db.set("user:a", {"email": "changed@example.test", "age": 21})
    assert [key for key, _ in db.find("email", "same@example.test")] == ["user:z"]
    assert db._storage.memory_stats().index_bytes > 0
    assert [index.name for index in db.list_indexes()] == ["age_order", "email_lookup"]

    db.delete("user:z")
    assert db.find("email", "same@example.test") == []
    db.close()


def test_unique_indexes_enforce_constraints_for_writes_and_transactions(tmp_path):
    db = Database(path=tmp_path, name="unique", max_memory=16, snapshot_interval=60)
    db.set("one", {"email": "one@example.test"})
    db.set("two", {"email": "two@example.test"})
    db.create_index("email_unique", "email", unique=True)

    with db.transaction() as transaction:
        transaction.set("one", {"email": "two@example.test"})
        transaction.set("two", {"email": "one@example.test"})

    assert db.get("one")["email"] == "two@example.test"
    with pytest.raises(UniqueConstraintError):
        db.set("three", {"email": "two@example.test"})
    assert db.get("three") is None

    with pytest.raises(UnsupportedIndexValueError):
        db.set("three", {"email": ["not", "scalar"]})
    assert db.get("three") is None
    db.close()


def test_index_definitions_rebuild_after_reopen_and_drop_persists(tmp_path):
    db = Database(path=tmp_path, name="persisted-index", max_memory=16, snapshot_interval=60)
    db.set("a", {"age": 10})
    db.set("b", {"age": 20})
    db.create_index("age_lookup", "age", index_type="ordered")
    db.close()

    reopened = Database(path=tmp_path, name="persisted-index", max_memory=16, snapshot_interval=60)
    assert [index.name for index in reopened.list_indexes()] == ["age_lookup"]
    assert [key for key, _ in reopened.find_range("age", 10, None)] == ["a", "b"]
    with pytest.raises(IndexExistsError):
        reopened.create_index("age_lookup", "age")
    reopened.drop_index("age_lookup")
    with pytest.raises(IndexNotFoundError):
        reopened.drop_index("age_lookup")
    reopened.close()

    final = Database(path=tmp_path, name="persisted-index", max_memory=16, snapshot_interval=60)
    assert final.list_indexes() == []
    assert sorted(path.name for path in tmp_path.iterdir()) == ["persisted-index.kr"]
    final.close()


def test_storage_stats_and_compaction_reclaim_wal_and_recover_indexes(tmp_path):
    db = Database(path=tmp_path, name="compaction", max_memory=32, snapshot_interval=60)
    db.set("account:1", {"email": "one@example.test", "score": 7})
    db.set("account:2", {"email": "two@example.test", "score": 9})
    db.create_index("email_unique", "email", unique=True)
    db.create_index("score_order", "score", index_type="ordered")

    for _ in range(80):
        db.set("temporary", "x" * 4096)
    db.delete("temporary")

    before = db.storage_stats()
    assert before["file_size_bytes"] == (tmp_path / "compaction.kr").stat().st_size
    assert before["reusable_free_space_bytes"] == 0
    assert before["physical_shrinking_supported"] is True
    if before["last_checkpoint_lsn"] == 0:
        assert before["wal_bytes_required"] > 0
        assert before["snapshot_bytes_retained"] == 0
    else:
        assert before["snapshot_bytes_retained"] > 0

    result = db.compact()
    after_compaction = db.storage_stats()
    assert result["checkpoint_performed"] or after_compaction["last_checkpoint_lsn"] > 0
    if result["checkpoint_performed"]:
        assert result["file_physically_shrunk"] is True
        assert result["file_size_after_bytes"] < result["file_size_before_bytes"]
        assert result["logical_wal_bytes_reclaimed"] > 0
        assert result["file_length_reduction_bytes"] == (
            result["file_size_before_bytes"] - result["file_size_after_bytes"]
        )
    else:
        assert result["limitation"]
    assert after_compaction["snapshot_bytes_retained"] > 0
    assert [key for key, _ in db.find("email", "one@example.test")] == ["account:1"]
    assert [key for key, _ in db.find_range("score", 7, 9)] == ["account:1", "account:2"]

    with pytest.raises(UniqueConstraintError):
        db.set("account:3", {"email": "one@example.test", "score": 10})
    db.set("after_compaction", True)
    with db.transaction() as transaction:
        transaction.set("transaction_after_compaction", {"score": 11})
    db.close()

    reopened = Database(path=tmp_path, name="compaction", max_memory=32, snapshot_interval=60)
    assert reopened.get("account:1") == {"email": "one@example.test", "score": 7}
    assert reopened.get("after_compaction") is True
    assert reopened.get("transaction_after_compaction") == {"score": 11}
    assert [index.name for index in reopened.list_indexes()] == ["email_unique", "score_order"]
    assert [key for key, _ in reopened.find_range("score", 7, None)] == [
        "account:1",
        "account:2",
        "transaction_after_compaction",
    ]
    assert sorted(path.name for path in tmp_path.iterdir()) == ["compaction.kr"]
    reopened.close()


def test_compaction_of_empty_database_is_a_safe_noop(tmp_path):
    db = Database(path=tmp_path, name="empty-compact", max_memory=8, snapshot_interval=60)
    initial_size = (tmp_path / "empty-compact.kr").stat().st_size if (tmp_path / "empty-compact.kr").exists() else None
    result = db.compact()

    assert result["checkpoint_performed"] is False
    assert result["file_physically_shrunk"] is False
    assert result["file_size_before_bytes"] == result["file_size_after_bytes"]
    assert result["limitation"]
    assert db.storage_stats()["record_count"] == 0
    assert initial_size is None
    db.close()


def test_corrupt_checkpoint_fails_recovery_without_rewriting_the_file(tmp_path):
    db = Database(path=tmp_path, name="checkpoint-corrupt", max_memory=16, snapshot_interval=60)
    for _ in range(24):
        db.set("churn", "x" * 4096)
    db.delete("churn")
    result = db.compact()
    assert result["checkpoint_performed"] is True
    database_file = tmp_path / "checkpoint-corrupt.kr"
    db.close()

    contents = bytearray(database_file.read_bytes())
    checkpoint_offset = contents.find(b"OPSK")
    assert checkpoint_offset >= 0
    contents[checkpoint_offset + 48] ^= 0x01
    database_file.write_bytes(contents)
    corrupted_contents = database_file.read_bytes()

    reopened = Database(path=tmp_path, name="checkpoint-corrupt", max_memory=16, snapshot_interval=60)
    with pytest.raises(RecoveryError):
        reopened.start()
    assert database_file.read_bytes() == corrupted_contents


def test_compaction_memory_budget_failure_keeps_database_unchanged(tmp_path):
    db = Database(path=tmp_path, name="compaction-budget", max_memory=1, snapshot_interval=60)
    large_value = "x" * 500_000
    for _ in range(5):
        db.set("large", large_value)

    database_file = tmp_path / "compaction-budget.kr"
    size_before = database_file.stat().st_size
    with pytest.raises(MemoryLimitExceededError):
        db.compact()

    assert database_file.stat().st_size == size_before
    assert db.get("large") == large_value
    db.close()


def test_database_backup_restore_and_jsonl_round_trip(tmp_path):
    db = Database(path=tmp_path / "source", name="store", max_memory=64, snapshot_interval=60)
    db.set("user:1", {"name": "Alice", "score": 7})
    db.set("user:2", [1, 2, 3])

    backup_path = tmp_path / "backup" / "store.kr"
    backup_path.parent.mkdir(parents=True, exist_ok=True)
    result = db.backup(backup_path, overwrite=False)
    assert result["status"] == "ok"
    assert backup_path.exists()
    assert db.verify_integrity()["status"] == "pass"

    export_path = tmp_path / "backup" / "store.jsonl"
    exported = db.export_data(export_path, overwrite=False)
    assert exported["status"] == "ok"

    restored_path = tmp_path / "restored" / "store.kr"
    restored_path.parent.mkdir(parents=True, exist_ok=True)
    restored = db.restore(backup_path, restored_path, overwrite=True)
    assert restored["status"] == "ok"

    reopened = Database(path=restored_path.parent, name="store", max_memory=64, snapshot_interval=60)
    assert reopened.get("user:1") == {"name": "Alice", "score": 7}
    assert reopened.get("user:2") == [1, 2, 3]
    reopened.close()

    import_db = Database(path=tmp_path / "imported", name="imported", max_memory=64, snapshot_interval=60)
    imported = import_db.import_data(export_path, conflict="overwrite", atomic=True)
    assert imported["status"] == "ok"
    assert import_db.get("user:1") == {"name": "Alice", "score": 7}
    assert import_db.get("user:2") == [1, 2, 3]
    db.close()
    import_db.close()
