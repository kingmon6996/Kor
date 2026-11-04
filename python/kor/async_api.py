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


from __future__ import annotations

import asyncio
import concurrent.futures
import logging
import threading
from collections.abc import Callable
from pathlib import Path
from typing import Any, TypedDict, TypeVar

from . import _kor, TransactionCommitError, TransactionStateError
from .database import (
    Database,
    InvalidConfigurationError,
    PersistenceError,
)
from .query import Filter, KorModel, ModelT

_LOGGER = logging.getLogger(__name__)
_DEFAULT_MAX_MEMORY_MIB = 64
_DEFAULT_SNAPSHOT_INTERVAL_SECONDS = 60
_MAX_QUEUED_OPERATIONS = 16

_Result = TypeVar("_Result")


class AsyncDatabaseClosedError(RuntimeError):
    """Raised when an operation is attempted on a closed async database."""


class AsyncCommitOutcomeUnknownError(PersistenceError):
    """Raised when an async transaction commit failed with an unknown outcome."""


class AsyncStorageStats(TypedDict):
    record_count: int
    configured_limit_bytes: int
    key_bytes: int
    value_bytes: int
    record_overhead_bytes: int
    index_bytes_estimate: int
    total_memory_bytes_estimate: int
    file_size_bytes: int
    logical_live_data_bytes_estimate: int
    wal_bytes_required: int
    snapshot_bytes_retained: int
    allocated_region_bytes: int
    reusable_free_space_bytes: int
    obsolete_bytes_estimate: int
    internal_fragmentation_estimate: int
    last_checkpoint_lsn: int
    compaction_generation: int
    last_auto_snapshot_error: str
    physical_shrinking_supported: bool
    closed: bool


class AsyncCheckpointStatus(TypedDict):
    last_attempt_lsn: int
    last_success_lsn: int
    last_attempt_duration_ns: int
    attempt_count: int
    success_count: int
    failure_count: int
    in_progress: bool
    last_error: str


class AsyncCompactionResult(TypedDict):
    file_size_before_bytes: int
    file_size_after_bytes: int
    logical_wal_bytes_reclaimed: int
    file_length_reduction_bytes: int
    checkpoint_staging_memory_estimate_bytes: int
    checkpoint_lsn: int
    compaction_generation: int
    checkpoint_performed: bool
    file_physically_shrunk: bool
    limitation: str


class AsyncDatabase:
    """Async facade over the synchronous Database and its native storage engine."""

    def __init__(self, database: Database) -> None:
        self._database: Database | None = None
        self._probe = database
        self._executor = concurrent.futures.ThreadPoolExecutor(
            max_workers=1,
            thread_name_prefix=f"kor-{database.name}",
        )
        self._loop = asyncio.get_running_loop()
        self._capacity = asyncio.Semaphore(_MAX_QUEUED_OPERATIONS)
        self._drained = asyncio.Event()
        self._drained.set()
        self._state_lock = threading.Lock()
        self._state = "opening"
        self._inflight = 0
        self._transactions: set[AsyncTransaction] = set()
        self._outcome_unknown = False
        self._writer_lock = asyncio.Lock()
        self._close_task: asyncio.Task[None] | None = None

    @property
    def path(self) -> Path:
        return self._probe.path

    @property
    def name(self) -> str:
        return self._probe.name

    @property
    def max_memory(self) -> int:
        return self._probe.max_memory

    @property
    def snapshot_interval(self) -> int:
        return self._probe.snapshot_interval

    @property
    def read_only(self) -> bool:
        return self._probe.read_only

    @property
    def is_closed(self) -> bool:
        with self._state_lock:
            return self._state == "closed"

    def transaction(self) -> AsyncTransactionContext:
        """Create an async context manager that owns the single-writer lease."""
        return AsyncTransactionContext(self)

    async def _open(self) -> None:
        future = self._executor.submit(self._open_database)
        wrapped = asyncio.wrap_future(future, loop=self._loop)
        try:
            self._database = await asyncio.shield(wrapped)
            self._probe = self._database
        except asyncio.CancelledError as cancellation:
            try:
                database = await asyncio.shield(wrapped)
            except Exception:
                _LOGGER.exception("Async database initialization failed after connection cancellation.")
            else:
                close_future = self._executor.submit(database.close)
                try:
                    await asyncio.shield(asyncio.wrap_future(close_future, loop=self._loop))
                except Exception as cleanup_error:
                    await asyncio.to_thread(self._executor.shutdown, True)
                    with self._state_lock:
                        self._state = "closed"
                    raise cleanup_error from cancellation
            await asyncio.to_thread(self._executor.shutdown, True)
            with self._state_lock:
                self._state = "closed"
            raise cancellation
        except Exception:
            await asyncio.to_thread(self._executor.shutdown, True)
            with self._state_lock:
                self._state = "closed"
            raise
        with self._state_lock:
            self._state = "open"

    def _open_database(self) -> Database:
        if self._probe._engine is not None:
            return self._probe
        database_path = self._probe.path / f"{self._probe.name}.kr"
        if database_path.exists():
            stored = _kor.read_database_config(str(database_path))
            if stored["name"] != self._probe.name:
                raise InvalidConfigurationError(
                    "The requested database name does not match the identity stored in the .kr file."
                )
            database = Database(
                path=self._probe.path,
                name=self._probe.name,
                max_memory=int(stored["max_memory_mib"]),
                snapshot_interval=int(stored["snapshot_interval_seconds"]),
                read_only=self._probe.read_only,
            )
        else:
            database = self._probe
        database.start()
        return database

    def _ensure_open(self) -> None:
        if asyncio.get_running_loop() is not self._loop:
            raise RuntimeError("An async database connection must be used on the event loop that opened it.")
        with self._state_lock:
            if self._state != "open":
                raise AsyncDatabaseClosedError("The async database is not open.")
            if self._outcome_unknown:
                raise AsyncCommitOutcomeUnknownError(
                    "A transaction commit outcome is unknown; close and reconnect before further operations."
                )

    async def _dispatch(
        self,
        operation: Callable[[], _Result],
        *,
        on_complete: Callable[[concurrent.futures.Future[Any]], None] | None = None,
        on_submitted: Callable[[concurrent.futures.Future[Any]], None] | None = None,
    ) -> _Result:
        self._ensure_open()
        await self._capacity.acquire()
        try:
            self._ensure_open()
            with self._state_lock:
                self._inflight += 1
                self._drained.clear()
            try:
                worker_future = self._executor.submit(operation)
            except RuntimeError:
                with self._state_lock:
                    self._inflight -= 1
                    if self._inflight == 0:
                        self._drained.set()
                raise
        except BaseException:
            self._capacity.release()
            raise

        if on_submitted is not None:
            on_submitted(worker_future)
        future = asyncio.wrap_future(worker_future, loop=self._loop)

        def finish() -> None:
            try:
                if on_complete is not None:
                    on_complete(worker_future)
            except Exception:
                _LOGGER.exception("An async database operation completion callback failed.")
            finally:
                with self._state_lock:
                    self._inflight -= 1
                    drained = self._inflight == 0
                self._capacity.release()
                if drained:
                    self._drained.set()
                if not future.cancelled():
                    future.exception()

        def worker_done(_: concurrent.futures.Future[Any]) -> None:
            self._loop.call_soon_threadsafe(finish)

        worker_future.add_done_callback(worker_done)
        try:
            return await asyncio.shield(future)
        except asyncio.CancelledError:
            raise

    async def _with_lock(
        self,
        lock: asyncio.Lock,
        operation: Callable[[], _Result],
        *,
        on_complete: Callable[[concurrent.futures.Future[Any]], None] | None = None,
    ) -> _Result:
        self._ensure_open()
        await lock.acquire()
        released = False
        cancelled = False
        worker_future: concurrent.futures.Future[Any] | None = None

        def release_once() -> None:
            nonlocal released
            if not released:
                released = True
                lock.release()

        def completed(future: concurrent.futures.Future[Any]) -> None:
            if on_complete is not None:
                on_complete(future)
            if cancelled:
                release_once()

        def submitted(future: concurrent.futures.Future[Any]) -> None:
            nonlocal worker_future
            worker_future = future

        try:
            return await self._dispatch(
                operation,
                on_complete=completed,
                on_submitted=submitted,
            )
        except asyncio.CancelledError:
            cancelled = True
            if worker_future is None or worker_future.done():
                release_once()
            raise
        finally:
            if not cancelled:
                release_once()

    async def _write(self, operation: Callable[[], _Result]) -> _Result:
        return await self._with_lock(self._writer_lock, operation)

    def _register_transaction(self, transaction: AsyncTransaction) -> None:
        self._ensure_open()
        with self._state_lock:
            self._transactions.add(transaction)

    def _finish_transaction(self, transaction: AsyncTransaction) -> None:
        with self._state_lock:
            registered = transaction in self._transactions
            self._transactions.discard(transaction)
        if registered and self._writer_lock.locked():
            self._writer_lock.release()

    def _mark_outcome_unknown(self) -> None:
        with self._state_lock:
            self._outcome_unknown = True

    async def get(self, key: str, default: Any = None) -> Any:
        return await self._dispatch(lambda: self._database.get(key, default))

    async def get_many(self, keys: Any) -> list[Any]:
        return await self._dispatch(lambda: self._database.get_many(keys))

    async def set_model(self, key: str, model: KorModel) -> None:
        await self._write(lambda: self._probe.set_model(key, model))

    async def get_model(self, key: str, model_type: type[ModelT]) -> ModelT | None:
        return await self._dispatch(
            lambda: self._probe.get_model(key, model_type)
        )

    async def query(
        self,
        filter: Filter,
        *,
        prefix: str = "",
        after: str | None = None,
        limit: int = 100,
    ) -> dict[str, Any]:
        return await self._dispatch(
            lambda: self._probe.query(
                filter, prefix=prefix, after=after, limit=limit
            )
        )

    async def contains_many(self, keys: Any) -> list[bool]:
        materialized_keys = Database._validate_multi_read_keys(keys)
        return await self._dispatch(
            lambda: self._native_storage().contains_many(materialized_keys)
        )

    async def set(self, key: str | None = None, *values: Any, **kwargs: Any) -> None:
        await self._write(lambda: self._database.set(key, *values, **kwargs))

    async def set_many(self, entries: dict[str, Any]) -> None:
        if not isinstance(entries, dict):
            raise TypeError("entries must be a dict mapping keys to values.")
        await self._write(lambda: self._database.set_many(entries))

    async def delete(self, key: str) -> None:
        await self._write(lambda: self._native_storage().erase(key))

    async def delete_many(self, keys: Any) -> None:
        if isinstance(keys, (str, bytes, bytearray)):
            raise TypeError("keys must be an iterable of strings, not a string.")
        materialized_keys = list(keys)
        await self._write(lambda: self._native_storage().delete_many(materialized_keys))

    async def clear(self) -> None:
        await self._write(lambda: self._native_storage().clear())

    async def contains(self, key: str) -> bool:
        return await self._dispatch(lambda: self._native_storage().exists(key))

    async def count(self) -> int:
        return await self._dispatch(lambda: self._native_storage().count())

    async def create_index(
        self,
        name: str,
        field: str,
        index_type: str = "hash",
        unique: bool = False,
    ) -> None:
        await self._write(
            lambda: self._native_storage().create_index(name, field, index_type, unique)
        )

    async def create_composite_index(
        self,
        name: str,
        fields: Any,
        *,
        unique: bool = False,
    ) -> None:
        if isinstance(fields, (str, bytes, bytearray)):
            raise TypeError("fields must be an iterable of 2 to 4 field names.")
        materialized_fields = list(fields)
        if any(not isinstance(field, str) for field in materialized_fields):
            raise TypeError("Every composite-index field must be a string.")
        await self._write(
            lambda: self._native_storage().create_composite_index(
                name, materialized_fields, unique
            )
        )

    async def drop_index(self, name: str) -> None:
        await self._write(lambda: self._native_storage().drop_index(name))

    async def list_indexes(self) -> list[_kor.IndexInfo]:
        return await self._dispatch(lambda: self._native_storage().list_indexes())

    async def find(self, field: str, value: Any) -> list[tuple[str, Any]]:
        return await self._dispatch(lambda: self._native_storage().find(field, value))

    async def find_range(
        self,
        field: str,
        minimum: Any = None,
        maximum: Any = None,
        *,
        include_minimum: bool = True,
        include_maximum: bool = True,
    ) -> list[tuple[str, Any]]:
        return await self._dispatch(
            lambda: self._native_storage().find_range(
                field,
                minimum,
                maximum,
                include_minimum,
                include_maximum,
            )
        )

    async def find_composite(self, fields: Any, values: Any) -> list[tuple[str, Any]]:
        if isinstance(fields, (str, bytes, bytearray)) or isinstance(values, (str, bytes, bytearray)):
            raise TypeError("fields and values must be non-string iterables.")
        materialized_fields = list(fields)
        materialized_values = list(values)
        return await self._dispatch(
            lambda: self._native_storage().find_composite(
                materialized_fields, materialized_values
            )
        )

    async def find_composite_range(
        self,
        fields: Any,
        minimum: Any = None,
        maximum: Any = None,
        *,
        include_minimum: bool = True,
        include_maximum: bool = True,
    ) -> list[tuple[str, Any]]:
        if isinstance(fields, (str, bytes, bytearray)):
            raise TypeError("fields must be a non-string iterable.")
        if minimum is not None and isinstance(minimum, (str, bytes, bytearray)):
            raise TypeError("minimum must be a non-string iterable or None.")
        if maximum is not None and isinstance(maximum, (str, bytes, bytearray)):
            raise TypeError("maximum must be a non-string iterable or None.")
        materialized_fields = list(fields)
        materialized_minimum = None if minimum is None else list(minimum)
        materialized_maximum = None if maximum is None else list(maximum)
        return await self._dispatch(
            lambda: self._native_storage().find_composite_range(
                materialized_fields,
                materialized_minimum,
                materialized_maximum,
                include_minimum,
                include_maximum,
            )
        )

    async def scan(
        self,
        prefix: str = "",
        *,
        after: str | None = None,
        limit: int = 100,
    ) -> dict[str, Any]:
        return await self._dispatch(
            lambda: self._database.scan(prefix=prefix, after=after, limit=limit)
        )

    async def storage_stats(self) -> AsyncStorageStats:
        return await self._dispatch(lambda: self._native_storage().storage_stats())

    async def checkpoint_status(self) -> AsyncCheckpointStatus:
        return await self._dispatch(lambda: self._native_storage().checkpoint_status())

    async def backup(self, destination: str | Path, *, overwrite: bool = False) -> dict[str, Any]:
        database = self._database
        if database is None:
            raise AsyncDatabaseClosedError("The async database is not open.")
        return await self._dispatch(lambda: database.backup(destination, overwrite=overwrite))

    async def restore(
        self,
        source: str | Path,
        destination: str | Path | None = None,
        *,
        overwrite: bool = False,
    ) -> dict[str, Any]:
        database = self._database
        if database is None:
            raise AsyncDatabaseClosedError("The async database is not open.")
        return await self._dispatch(
            lambda: database.restore(source, destination, overwrite=overwrite)
        )

    async def export_data(
        self,
        destination: str | Path,
        *,
        format: str = "jsonl",
        overwrite: bool = False,
    ) -> dict[str, Any]:
        database = self._database
        if database is None:
            raise AsyncDatabaseClosedError("The async database is not open.")
        return await self._dispatch(
            lambda: database.export_data(destination, format=format, overwrite=overwrite)
        )

    async def import_data(
        self,
        source: str | Path,
        *,
        conflict: str = "error",
        atomic: bool = True,
    ) -> dict[str, Any]:
        database = self._database
        if database is None:
            raise AsyncDatabaseClosedError("The async database is not open.")
        return await self._dispatch(
            lambda: database.import_data(source, conflict=conflict, atomic=atomic)
        )

    async def verify_integrity(self, *, level: str = "structural") -> dict[str, Any]:
        database = self._database
        if database is None:
            raise AsyncDatabaseClosedError("The async database is not open.")
        return await self._dispatch(lambda: database.verify_integrity(level=level))

    async def compact(self) -> AsyncCompactionResult:
        return await self._write(lambda: self._native_storage().compact())

    async def checkpoint(self) -> AsyncCompactionResult:
        return await self._write(lambda: self._native_storage().checkpoint())

    async def close(self) -> None:
        if asyncio.get_running_loop() is not self._loop:
            raise RuntimeError("An async database connection must be closed on its owning event loop.")
        with self._state_lock:
            if self._state == "closed":
                return
            if self._state == "open":
                unfinished = [
                    transaction
                    for transaction in self._transactions
                    if transaction._state
                    not in {"committing", "commit_cancelled", "rolling_back", "start_cancelled"}
                ]
                if unfinished:
                    raise TransactionStateError(
                        "Cannot close the async database while an async transaction is active."
                    )
                self._state = "closing"
            if self._close_task is None or self._close_task.done():
                self._close_task = self._loop.create_task(self._close_impl())
            close_task = self._close_task
        await asyncio.shield(close_task)

    async def _close_impl(self) -> None:
        try:
            await self._drained.wait()
            database = self._database
            if database is not None:
                future = self._executor.submit(database.close)
                await asyncio.shield(asyncio.wrap_future(future, loop=self._loop))
            await asyncio.to_thread(self._executor.shutdown, True)
            self._database = None
            with self._state_lock:
                self._state = "closed"
        except Exception:
            with self._state_lock:
                self._state = "failed"
                self._close_task = None
            raise

    def _native_storage(self) -> _kor.Storage:
        database = self._database
        if database is None or database._storage is None:
            raise AsyncDatabaseClosedError("The async database storage is unavailable.")
        return database._storage

    async def __aenter__(self) -> AsyncDatabase:
        self._ensure_open()
        return self

    async def __aexit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        await self.close()


class AsyncTransaction:
    """Serialized async operations on the native transaction write set."""

    def __init__(self, database: AsyncDatabase) -> None:
        self._database = database
        self._native: _kor.Transaction | None = None
        self._state = "starting"
        self._operation_lock = asyncio.Lock()

    async def _start(self) -> None:
        worker_future: concurrent.futures.Future[Any] | None = None

        def completed(future: concurrent.futures.Future[Any]) -> None:
            if self._state != "start_cancelled":
                return
            try:
                transaction = future.result()
            except Exception:
                self._state = "failed"
            else:
                self._native = transaction
                try:
                    transaction.rollback()
                except Exception:
                    self._state = "unknown"
                    self._database._mark_outcome_unknown()
                    _LOGGER.exception("Unable to roll back a transaction cancelled during creation.")
                else:
                    self._state = "rolled_back"
            self._database._finish_transaction(self)

        def submitted(future: concurrent.futures.Future[Any]) -> None:
            nonlocal worker_future
            worker_future = future

        try:
            self._native = await self._database._dispatch(
                lambda: self._database._native_storage().transaction(),
                on_complete=completed,
                on_submitted=submitted,
            )
        except asyncio.CancelledError:
            self._state = "start_cancelled"
            if worker_future is None:
                self._state = "failed"
                self._database._finish_transaction(self)
            elif worker_future.done():
                completed(worker_future)
            raise
        except Exception:
            self._database._finish_transaction(self)
            self._state = "failed"
            raise
        self._state = "active"

    def _ensure_active(self) -> _kor.Transaction:
        if self._state != "active" or self._native is None:
            raise TransactionStateError("Transaction is no longer active.")
        return self._native

    async def get(self, key: str) -> Any:
        return await self._database._with_lock(
            self._operation_lock,
            lambda: self._ensure_active().get(key),
        )

    async def contains(self, key: str) -> bool:
        return await self._database._with_lock(
            self._operation_lock,
            lambda: self._ensure_active().contains(key),
        )

    async def set(self, key: str, value: Any) -> None:
        await self._database._with_lock(
            self._operation_lock,
            lambda: self._ensure_active().set(key, value),
        )

    async def delete(self, key: str) -> None:
        await self._database._with_lock(
            self._operation_lock,
            lambda: self._ensure_active().delete(key),
        )

    async def clear(self) -> None:
        await self._database._with_lock(
            self._operation_lock,
            lambda: self._ensure_active().clear(),
        )

    async def commit(self) -> None:
        async with self._operation_lock:
            transaction = self._ensure_active()
            self._state = "committing"
            try:
                await self._database._dispatch(
                    transaction.commit,
                    on_complete=self._complete_commit,
                )
            except asyncio.CancelledError:
                if self._state == "committing":
                    self._state = "commit_cancelled"
                raise
            except Exception as error:
                self._complete_commit_error(error)
                raise
            else:
                self._complete_committed()

    def _complete_commit(self, future: concurrent.futures.Future[Any]) -> None:
        try:
            future.result()
        except Exception as error:
            self._complete_commit_error(error)
        else:
            self._complete_committed()

    def _complete_commit_error(self, error: Exception) -> None:
        if self._state in {"committed", "failed", "unknown"}:
            return
        if isinstance(error, (PersistenceError, TransactionCommitError)):
            self._state = "unknown"
            self._database._mark_outcome_unknown()
        else:
            self._state = "failed"
        self._database._finish_transaction(self)

    def _complete_committed(self) -> None:
        if self._state == "committed":
            return
        self._state = "committed"
        self._database._finish_transaction(self)

    async def rollback(self) -> None:
        async with self._operation_lock:
            if self._state == "rolled_back":
                return
            if self._state != "active" or self._native is None:
                raise TransactionStateError("Cannot roll back an inactive or committing transaction.")
            transaction = self._native
            self._state = "rolling_back"
            try:
                await self._database._dispatch(
                    transaction.rollback,
                    on_complete=self._complete_rollback,
                )
            except asyncio.CancelledError:
                raise
            except Exception:
                self._state = "failed"
                self._database._finish_transaction(self)
                raise
            else:
                self._complete_rolled_back()

    def _complete_rollback(self, future: concurrent.futures.Future[Any]) -> None:
        try:
            future.result()
        except Exception:
            self._state = "failed"
        else:
            self._state = "rolled_back"
        self._database._finish_transaction(self)

    def _complete_rolled_back(self) -> None:
        if self._state == "rolled_back":
            return
        self._state = "rolled_back"
        self._database._finish_transaction(self)


class AsyncTransactionContext:
    def __init__(self, database: AsyncDatabase) -> None:
        self._database = database
        self._transaction: AsyncTransaction | None = None

    async def __aenter__(self) -> AsyncTransaction:
        self._database._ensure_open()
        await self._database._writer_lock.acquire()
        transaction = AsyncTransaction(self._database)
        registered = False
        try:
            self._database._register_transaction(transaction)
            registered = True
            await transaction._start()
        except BaseException:
            if not registered and self._database._writer_lock.locked():
                self._database._writer_lock.release()
            raise
        self._transaction = transaction
        return transaction

    async def __aexit__(self, exc_type: Any, exc: Any, traceback: Any) -> bool:
        transaction = self._transaction
        if transaction is None:
            return False
        if exc_type is None:
            if transaction._state == "active":
                await transaction.commit()
            return False
        if transaction._state == "active":
            try:
                await transaction.rollback()
            except Exception as rollback_error:
                raise rollback_error from exc
        return False


async def async_connect(
    path: str | Path | None = None,
    name: str = "",
    hex: str | None = None,
    *,
    read_only: bool = False,
) -> AsyncDatabase:
    """Open an existing database or create one, optionally using a hex token."""
    if hex is not None:
        probe = Database.connect(path=path, name=name, hex=hex, read_only=read_only)
    else:
        probe = Database(
            path=path,
            name=name,
            max_memory=_DEFAULT_MAX_MEMORY_MIB,
            snapshot_interval=_DEFAULT_SNAPSHOT_INTERVAL_SECONDS,
            read_only=read_only,
        )
    database = AsyncDatabase(probe)
    await database._open()
    return database


__all__ = [
    "AsyncCommitOutcomeUnknownError",
    "AsyncCheckpointStatus",
    "AsyncCompactionResult",
    "AsyncDatabase",
    "AsyncDatabaseClosedError",
    "AsyncStorageStats",
    "AsyncTransaction",
    "AsyncTransactionContext",
    "async_connect",
]