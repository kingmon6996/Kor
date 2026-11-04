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

from ._version import __version__

try:
    from . import _kor
except ImportError as exc:  # pragma: no cover
    raise ImportError(
        "The Kor native extension could not be imported. Build the project with pip install -e . "
        "or compile the CMake target before importing kor."
    ) from exc

from .database import (
    Database,
    HexToken,
    NULL,
    AuthenticationError,
    DuplicateKeyError,
    InvalidConfigurationError,
    _resolve_database_file_path,
    _restore_database_file,
)


def connect(path=None, name="", hex="", *, read_only: bool = False) -> Database:
    """Connect to a Kor database stored in ROM using RAM for faster working."""
    return Database.connect(path=path, name=name, hex=hex, read_only=read_only)


# db alias for Database so `db.connect(...)` works via `from kor import db` or `import kor as db`
db = Database


def backup(source, destination, *, overwrite: bool = False):
    source_path = _resolve_database_file_path(source)
    if not source_path.exists():
        raise FileNotFoundError(source_path)
    config = _kor.read_database_config(str(source_path))
    db = Database(
        path=source_path.parent,
        name=config["name"],
        max_memory=int(config["max_memory_mib"]),
        snapshot_interval=int(config["snapshot_interval_seconds"]),
        read_only=True,
    )
    try:
        db.start()
        return db.backup(destination, overwrite=overwrite)
    finally:
        db.close()


def restore(source, destination, *, overwrite: bool = False):
    source_path = _resolve_database_file_path(source)
    target_path = _restore_database_file(source_path, Path(destination), overwrite=overwrite)
    return {"source": str(source_path), "destination": str(target_path), "status": "ok"}


def verify_backup(path):
    file_path = Path(path)
    if not file_path.exists():
        raise FileNotFoundError(file_path)
    config = _kor.read_database_config(str(file_path))
    db = Database(
        path=file_path.parent,
        name=config["name"],
        max_memory=int(config["max_memory_mib"]),
        snapshot_interval=int(config["snapshot_interval_seconds"]),
        read_only=True,
    )
    try:
        db.start()
        return db.verify_integrity()
    finally:
        db.close()


Engine = _kor.Engine
Storage = _kor.Storage
Value = _kor.Value
IndexInfo = _kor.IndexInfo
Error = _kor.Error
InitializationError = _kor.InitializationError
InvalidKeyError = _kor.InvalidKeyError
InvalidValueError = _kor.InvalidValueError
SerializationError = _kor.SerializationError
StorageClosedError = _kor.StorageClosedError
ReadOnlyError = _kor.ReadOnlyError
DatabaseFormatError = _kor.DatabaseFormatError
UnsupportedFormatVersionError = _kor.UnsupportedFormatVersionError
PersistenceError = _kor.PersistenceError
RecoveryError = _kor.RecoveryError
MemoryLimitExceededError = _kor.MemoryLimitExceededError
TransactionError = _kor.TransactionError
TransactionStateError = _kor.TransactionStateError
TransactionTooLargeError = _kor.TransactionTooLargeError
TransactionCommitError = _kor.TransactionCommitError
IndexError = _kor.IndexError
InvalidIndexDefinitionError = _kor.InvalidIndexDefinitionError
IndexExistsError = _kor.IndexExistsError
IndexNotFoundError = _kor.IndexNotFoundError
UniqueConstraintError = _kor.UniqueConstraintError
UnsupportedIndexValueError = _kor.UnsupportedIndexValueError
native_version = _kor.native_version

from .async_api import (
    AsyncCommitOutcomeUnknownError,
    AsyncCheckpointStatus,
    AsyncCompactionResult,
    AsyncDatabase,
    AsyncDatabaseClosedError,
    AsyncStorageStats,
    AsyncTransaction,
    AsyncTransactionContext,
    async_connect,
)
from .query import And, Eq, Exists, Filter, KorModel, Not, Or, Range
from .memory import (
    InsufficientSystemMemoryError,
    MemoryUsageWarning,
    available_memory_bytes,
    check_memory_budget,
)

__all__ = [
    "__version__",
    "Engine",
    "Storage",
    "Value",
    "IndexInfo",
    "Error",
    "Database",
    "HexToken",
    "NULL",
    "connect",
    "db",
    "InitializationError",
    "InvalidKeyError",
    "InvalidValueError",
    "SerializationError",
    "StorageClosedError",
    "ReadOnlyError",
    "DatabaseFormatError",
    "UnsupportedFormatVersionError",
    "PersistenceError",
    "RecoveryError",
    "MemoryLimitExceededError",
    "TransactionError",
    "TransactionStateError",
    "TransactionTooLargeError",
    "TransactionCommitError",
    "IndexError",
    "InvalidIndexDefinitionError",
    "IndexExistsError",
    "IndexNotFoundError",
    "UniqueConstraintError",
    "UnsupportedIndexValueError",
    "AuthenticationError",
    "DuplicateKeyError",
    "InvalidConfigurationError",
    "native_version",
    "backup",
    "restore",
    "verify_backup",
    "AsyncDatabase",
    "AsyncTransaction",
    "AsyncTransactionContext",
    "AsyncStorageStats",
    "AsyncCompactionResult",
    "AsyncDatabaseClosedError",
    "AsyncCommitOutcomeUnknownError",
    "AsyncCheckpointStatus",
    "async_connect",
    "Filter",
    "KorModel",
    "Eq",
    "Range",
    "Exists",
    "And",
    "Or",
    "Not",
    "InsufficientSystemMemoryError",
    "MemoryUsageWarning",
    "available_memory_bytes",
    "check_memory_budget",
    "_kor",
]
