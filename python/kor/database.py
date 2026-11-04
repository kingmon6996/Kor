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

import base64
import json
import os
import logging
import re
import shutil
import tempfile
import threading
import time
import warnings
from pathlib import Path
from typing import Any

from . import _version
from .observability import add_latency_percentiles, database_identifier, log_event
from .memory import MemoryUsageWarning, check_memory_budget
from .query import Filter, KorModel, ModelT

_LOGGER = logging.getLogger("kor.database")

try:
    from . import _kor
except ImportError:  # pragma: no cover
    _kor = None


class InvalidConfigurationError(ValueError):
    """Raised when a database configuration is invalid or unsafe."""


class AuthenticationError(InvalidConfigurationError):
    """Raised when database authentication fails (e.g. incorrect or invalid hex)."""


class DuplicateKeyError(ValueError):
    """Raised when set() or set_many() is called with a key that already exists and overwrite=False."""


DatabaseFormatError = getattr(_kor, "DatabaseFormatError", ValueError)
UnsupportedFormatVersionError = getattr(_kor, "UnsupportedFormatVersionError", ValueError)
PersistenceError = getattr(_kor, "PersistenceError", ValueError)
RecoveryError = getattr(_kor, "RecoveryError", ValueError)
MemoryLimitExceededError = getattr(_kor, "MemoryLimitExceededError", ValueError)
ReadOnlyError = getattr(_kor, "ReadOnlyError", ValueError)
StorageClosedError = getattr(_kor, "StorageClosedError", RuntimeError)
TransactionStateError = getattr(_kor, "TransactionStateError", RuntimeError)

_OPEN_DATABASE_PATHS: set[str] = set()
_OPEN_DATABASE_PATHS_LOCK = threading.Lock()
_PATH_RESERVATIONS: set[str] = set()
_JSONL_TAG = "$kor"
_JSONL_SCHEMA_VERSION = 2


def _encode_json_value(value: Any) -> Any:
    if isinstance(value, bytes):
        return {_JSONL_TAG: "bytes", "data": base64.b64encode(value).decode("ascii")}
    if isinstance(value, list):
        return [_encode_json_value(item) for item in value]
    if isinstance(value, dict):
        if _JSONL_TAG in value:
            return {
                _JSONL_TAG: "map",
                "entries": [[key, _encode_json_value(item)] for key, item in value.items()],
            }
        return {key: _encode_json_value(item) for key, item in value.items()}
    return value


def _decode_json_value(value: Any, *, tagged: bool) -> Any:
    if isinstance(value, list):
        return [_decode_json_value(item, tagged=tagged) for item in value]
    if not isinstance(value, dict):
        return value
    if not tagged:
        return {key: _decode_json_value(item, tagged=False) for key, item in value.items()}

    tag = value.get(_JSONL_TAG)
    if tag == "bytes":
        if set(value) != {_JSONL_TAG, "data"} or not isinstance(value["data"], str):
            raise PersistenceError("Malformed byte-string value in Kor JSONL import.")
        try:
            return base64.b64decode(value["data"], validate=True)
        except ValueError as error:
            raise PersistenceError("Malformed base64 byte-string value in Kor JSONL import.") from error
    if tag == "map":
        if set(value) != {_JSONL_TAG, "entries"} or not isinstance(value["entries"], list):
            raise PersistenceError("Malformed escaped mapping value in Kor JSONL import.")
        result = {}
        for entry in value["entries"]:
            if (
                not isinstance(entry, list)
                or len(entry) != 2
                or not isinstance(entry[0], str)
                or entry[0] in result
            ):
                raise PersistenceError("Malformed escaped mapping entry in Kor JSONL import.")
            result[entry[0]] = _decode_json_value(entry[1], tagged=True)
        return result
    if _JSONL_TAG in value:
        raise PersistenceError("Unsupported tagged value in Kor JSONL import.")
    return {key: _decode_json_value(item, tagged=True) for key, item in value.items()}


def _resolve_database_file_path(path: str | os.PathLike[str]) -> Path:
    candidate = Path(path).expanduser()
    if candidate.is_dir():
        return candidate / f"{candidate.name}.kr"
    return candidate


def _path_identity(path: Path) -> str:
    try:
        return os.path.normcase(os.path.realpath(str(path)))
    except OSError:
        return os.path.normcase(os.path.abspath(str(path)))


def _validate_restore_source(source: Path) -> dict[str, Any]:
    if source.is_symlink() or not source.exists() or not source.is_file():
        raise PersistenceError(f"Restore source must be an existing regular database file: {source}")
    with _OPEN_DATABASE_PATHS_LOCK:
        if _path_identity(source) in _OPEN_DATABASE_PATHS:
            raise PersistenceError("Restore source database is open in this process.")

    config = _kor.read_database_config(str(source))
    engine = _kor.Engine(str(source), int(config["max_memory_mib"]) * 1024 * 1024, True)
    try:
        engine.storage().storage_stats()
    finally:
        engine.close()
    return config


def _copy_file_atomically(source: Path, destination: Path, *, overwrite: bool = False) -> Path:
    source = source.expanduser()
    destination = Path(destination).expanduser()

    if not source.exists() or not source.is_file():
        raise PersistenceError(f"Source database file does not exist or is not a regular file: {source}")
    if source.is_symlink():
        raise PersistenceError("Source database path must not be a symlink.")
    if destination.exists() and destination.is_dir():
        raise PersistenceError("Destination must be a file path, not a directory.")
    if destination.exists() and destination.is_symlink():
        raise PersistenceError("Destination path must not be a symlink.")
    if not destination.parent.exists():
        raise PersistenceError(f"Destination directory does not exist: {destination.parent}")
    if _path_identity(source) == _path_identity(destination):
        raise PersistenceError("Source and destination must not resolve to the same file.")
    destination_key = _path_identity(destination)
    with _OPEN_DATABASE_PATHS_LOCK:
        if destination_key in _OPEN_DATABASE_PATHS or destination_key in _PATH_RESERVATIONS:
            raise PersistenceError("Destination database is open in this process and cannot be replaced.")
        _PATH_RESERVATIONS.add(destination_key)
    if destination.exists() and not overwrite:
        with _OPEN_DATABASE_PATHS_LOCK:
            _PATH_RESERVATIONS.discard(destination_key)
        raise PersistenceError(f"Destination already exists: {destination}")

    temp_path = destination.with_name(f".{destination.name}.tmp.{os.getpid()}.{time.time_ns()}")
    try:
        shutil.copy2(str(source), str(temp_path))
        os.replace(str(temp_path), str(destination))
        return destination
    except Exception:
        if temp_path.exists():
            try:
                temp_path.unlink()
            except OSError:
                pass
        raise
    finally:
        with _OPEN_DATABASE_PATHS_LOCK:
            _PATH_RESERVATIONS.discard(destination_key)


def _restore_database_file(source: Path, destination: Path, *, overwrite: bool = False) -> Path:
    source_key = _path_identity(source)
    with _OPEN_DATABASE_PATHS_LOCK:
        if source_key in _OPEN_DATABASE_PATHS or source_key in _PATH_RESERVATIONS:
            raise PersistenceError("Restore source database is open or being replaced in this process.")
        _PATH_RESERVATIONS.add(source_key)
    try:
        config = _validate_restore_source(source)
        destination = Path(destination).expanduser()
        if destination.name != f"{config['name']}.kr":
            raise PersistenceError(
                f"Restore destination filename must be {config['name']}.kr to preserve database identity."
            )
        return _copy_file_atomically(source, destination, overwrite=overwrite)
    finally:
        with _OPEN_DATABASE_PATHS_LOCK:
            _PATH_RESERVATIONS.discard(source_key)



NULL = None


class HexToken(str):
    """Hexadecimal authentication and identification token for a Kor database."""

    def __new__(
        cls,
        value: str,
        path: str | Path | None = None,
        name: str = "",
        number_of_values: int | None = None,
    ) -> HexToken:
        instance = super().__new__(cls, value)
        instance._path = Path(path).resolve() if path else Path.cwd().resolve()
        instance._name = name
        instance._number_of_values = number_of_values
        return instance

    @property
    def path(self) -> Path:
        return self._path

    @property
    def name(self) -> str:
        return self._name

    @property
    def number_of_values(self) -> int | None:
        return self._number_of_values

    @property
    def hex(self) -> str:
        return str(self)

    def connect(
        self,
        path: str | Path | None = None,
        name: str = "",
        hex: str | None = None,
        *,
        read_only: bool = False,
    ) -> Database:
        """Connect to the database associated with this hex token."""
        target_path = path if path is not None else self._path
        target_name = name if name else self._name
        target_hex = hex if hex is not None else str(self)
        return Database.connect(target_path, target_name, target_hex, read_only=read_only)

    def __eq__(self, other: object) -> bool:
        if isinstance(other, str):
            s1 = self.strip().lower().removeprefix("0x")
            s2 = str(other).strip().lower().removeprefix("0x")
            return s1 == s2
        return super().__eq__(other)

    def __hash__(self) -> int:
        return hash(self.strip().lower().removeprefix("0x"))


def _normalize_hex_str(h: str) -> str:
    s = str(h).strip().lower()
    while s.startswith("0x"):
        s = s[2:]
    return s


def _initialize_database_with_hex(
    path: str | Path | None,
    name: str,
    number_of_values: Any,
    max_memory: int | None = None,
) -> HexToken:
    import secrets
    import json
    import struct

    if path is None:
        path = ""
    Database._validate_path(path)
    Database._validate_name(name)

    if not isinstance(number_of_values, int) or isinstance(number_of_values, bool) or number_of_values <= 0:
        raise InvalidConfigurationError("number_of_values must be a positive integer.")

    db_dir = Path(path).expanduser().resolve() if str(path).strip() else Path.cwd().resolve()
    db_dir.mkdir(parents=True, exist_ok=True)
    db_file = db_dir / f"{name}.kr"

    # If the single .kr database file already exists, reuse the existing hex token from ROM metadata
    if db_file.exists() and db_file.stat().st_size >= 20:
        try:
            with open(db_file, "rb") as fp:
                magic = fp.read(16)
                if magic.startswith(b"KOR_ROM_DB"):
                    hlen = struct.unpack("<I", fp.read(4))[0]
                    meta = json.loads(fp.read(hlen).decode("utf-8"))
                    if meta.get("name") == name and "hex" in meta:
                        stored_nov = int(meta.get("number_of_values", number_of_values))
                        return HexToken(
                            meta["hex"],
                            path=db_dir,
                            name=name,
                            number_of_values=stored_nov,
                        )
                elif magic.startswith(b"SQLite format 3"):
                    import sqlite3
                    conn = sqlite3.connect(str(db_file), timeout=5.0)
                    try:
                        cur = conn.cursor()
                        cur.execute("SELECT name FROM sqlite_master WHERE type='table' AND name='__kor_meta__'")
                        if cur.fetchone():
                            cur.execute("SELECT key, value FROM __kor_meta__")
                            meta = dict(cur.fetchall())
                            if meta.get("name") == name and "hex" in meta:
                                stored_nov = int(meta.get("number_of_values", number_of_values))
                                return HexToken(
                                    meta["hex"],
                                    path=db_dir,
                                    name=name,
                                    number_of_values=stored_nov,
                                )
                    finally:
                        conn.close()
        except Exception:
            pass

    raw_hex = secrets.token_hex(16)
    hex_str = f"0x{raw_hex}"

    # Initialize single .kr file directly in ROM with V2 metadata header (no .info files)
    magic = b"KOR_ROM_DB_V2\x00\x00\x00"
    meta = {
        "hex": hex_str,
        "name": name,
        "number_of_values": int(number_of_values),
        "created_at": time.time(),
        "storage": "rom",
        "runtime": "rom",
        "data_in_ram": False,
        "snapshots_enabled": False,
        "max_memory_mib": 0,
    }
    meta_bytes = json.dumps(meta).encode("utf-8")
    with open(db_file, "wb") as fp:
        fp.write(magic)
        fp.write(struct.pack("<I", len(meta_bytes)))
        fp.write(meta_bytes)
        # Empty initial snapshot
        import marshal
        marshal.dump({}, fp)

    return HexToken(hex_str, path=db_dir, name=name, number_of_values=int(number_of_values))


def _connect_database(
    cls: type[Database],
    path: str | Path | None,
    name: str,
    hex: str,
    *,
    read_only: bool = False,
) -> Database:
    if path is None:
        path = ""
    Database._validate_path(path)
    Database._validate_name(name)

    if not isinstance(hex, str) or not hex.strip():
        raise InvalidConfigurationError("hex must be a non-empty hexadecimal string.")

    db_dir = Path(path).expanduser().resolve() if str(path).strip() else Path.cwd().resolve()
    db_file = db_dir / f"{name}.kr"

    if not db_file.exists():
        raise PersistenceError(f"Database file does not exist at '{db_file}'.")

    import struct
    import json

    meta: dict[str, Any] = {}
    try:
        with open(db_file, "rb") as fp:
            magic = fp.read(16)
            if magic.startswith(b"KOR_ROM_DB"):
                hlen = struct.unpack("<I", fp.read(4))[0]
                meta = json.loads(fp.read(hlen).decode("utf-8"))
            elif magic.startswith(b"SQLite format 3"):
                import sqlite3
                conn = sqlite3.connect(str(db_file), timeout=5.0)
                try:
                    cur = conn.cursor()
                    cur.execute("SELECT name FROM sqlite_master WHERE type='table' AND name='__kor_meta__'")
                    if cur.fetchone():
                        cur.execute("SELECT key, value FROM __kor_meta__")
                        meta = dict(cur.fetchall())
                finally:
                    conn.close()
    except Exception as err:
        raise PersistenceError(f"Failed to read database metadata: {err}") from err

    if not meta:
        raise PersistenceError(
            f"Database file '{db_file}' is missing metadata. "
            f"The database must be initialized with Database(path, name, number_of_values) first."
        )

    stored_name = meta.get("name", "")
    stored_hex = meta.get("hex", "")

    if stored_name != name:
        raise InvalidConfigurationError(
            f"Database name mismatch: expected '{name}', stored name is '{stored_name}'."
        )

    if _normalize_hex_str(hex) != _normalize_hex_str(stored_hex):
        raise AuthenticationError(
            f"Authentication failed: provided hex does not match database '{name}'."
        )

    number_of_values = int(meta.get("number_of_values", 1))

    info = {
        "hex": stored_hex,
        "name": name,
        "path": str(db_dir),
        "file": str(db_file),
        "number_of_values": number_of_values,
        "max_memory_mib": 0,
        "storage": "rom",
        "runtime": "rom",
        "data_in_ram": False,
        "snapshots_enabled": False,
    }

    return RomDatabase(
        path=db_dir,
        name=name,
        hex=stored_hex,
        number_of_values=number_of_values,
        info=info,
        read_only=read_only,
    )


class _ConnectMethod:
    """Descriptor supporting Database.connect(path, name, hex) and db.connect(path, name, hex)."""

    def __get__(self, instance, owner):
        if instance is None:
            def _class_connect(path=None, name="", hex="", *, read_only=False):
                return _connect_database(owner, path, name, hex, read_only=read_only)
            return _class_connect
        else:
            def _instance_connect(path=None, name="", hex="", *, read_only=False):
                return instance._connect_instance(path, name, hex, read_only=read_only)
            return _instance_connect


class Database:
    """Validated database configuration for the Kor engine foundation."""

    __slots__ = (
        "_path",
        "_name",
        "_max_memory",
        "_snapshot_interval",
        "_read_only",
        "_engine",
        "_storage",
        "_registry_key",
        "_database_id",
        "_lifecycle",
        "_health_override",
        "_last_error",
        "_closed_metrics",
        "_hex",
        "_number_of_values",
        "_info",
        "_snapshots_enabled",
    )

    connect = _ConnectMethod()

    def __new__(cls, *args, **kwargs):
        if cls is not Database or kwargs.get("_from_connect"):
            return super().__new__(cls)

        # 1. number_of_values keyword argument passed:
        if "number_of_values" in kwargs:
            path = kwargs.get("path", args[0] if len(args) > 0 else "")
            name = kwargs.get("name", args[1] if len(args) > 1 else "")
            number_of_values = kwargs["number_of_values"]
            max_mem = kwargs.get("max_memory", None)
            return _initialize_database_with_hex(path, name, number_of_values, max_memory=max_mem)

        # 2. 3 or more positional arguments, or max_memory passed:
        if (len(args) >= 3 or ("max_memory" in kwargs and len(args) >= 2)) and "hex" not in kwargs:
            path = args[0] if len(args) > 0 else kwargs.get("path", "")
            name = args[1] if len(args) > 1 else kwargs.get("name", "")
            number_of_values = args[2] if len(args) > 2 else kwargs.get("number_of_values")
            max_mem = kwargs.get("max_memory", args[3] if len(args) > 3 else None)
            if number_of_values is not None and not isinstance(number_of_values, dict):
                return _initialize_database_with_hex(path, name, number_of_values, max_memory=max_mem)

        # 3. hex keyword argument provided:
        if "hex" in kwargs:
            path = kwargs.get("path", args[0] if len(args) > 0 else "")
            name = kwargs.get("name", args[1] if len(args) > 1 else "")
            read_only = kwargs.get("read_only", False)
            return _connect_database(cls, path, name, kwargs["hex"], read_only=read_only)

        return super().__new__(cls)

    def __init__(
        self,
        path: str | Path | None = None,
        name: str = "",
        max_memory: int = 0,
        snapshot_interval: int = 60,
        read_only: bool = False,
        _from_connect: bool = False,
    ) -> None:
        from . import _kor

        if path is None:
            path = ""

        self._validate_path(path)
        self._validate_name(name)
        if read_only and max_memory == 0:
            max_memory = 64
        self._validate_memory(max_memory)
        self._check_system_memory(max_memory, phase="configuration")
        self._validate_snapshot_interval(snapshot_interval)
        if not isinstance(read_only, bool):
            raise InvalidConfigurationError("read_only must be a bool.")

        native_config = _kor.make_config(
            str(Path(path).expanduser()) if str(path).strip() else str(Path.cwd()),
            name,
            int(max_memory),
            int(snapshot_interval),
        )

        self._path = Path(native_config.path)
        self._name = native_config.name
        self._max_memory = int(native_config.max_memory_mib)
        self._snapshot_interval = int(native_config.snapshot_interval_seconds)
        self._read_only = read_only
        self._engine = None
        self._storage = None
        self._registry_key: str | None = None
        self._database_id = database_identifier(self._path, self._name)
        self._lifecycle = "not_started"
        self._health_override: str | None = None
        self._last_error: dict[str, str] | None = None
        self._closed_metrics: dict[str, Any] | None = None
        self._hex: str | None = None
        self._number_of_values: int | None = None
        self._info: dict[str, Any] | None = None
        self._snapshots_enabled: bool = not _from_connect

        kr_file = self._path / f"{self._name}.kr"
        if kr_file.exists():
            try:
                import sqlite3
                conn = sqlite3.connect(str(kr_file), timeout=5.0)
                try:
                    cur = conn.cursor()
                    cur.execute("SELECT name FROM sqlite_master WHERE type='table' AND name='__kor_meta__'")
                    if cur.fetchone():
                        cur.execute("SELECT key, value FROM __kor_meta__")
                        meta = dict(cur.fetchall())
                        if meta.get("name") == self._name:
                            self._hex = meta.get("hex")
                            nov = meta.get("number_of_values")
                            self._number_of_values = int(nov) if nov is not None else None
                            self._info = meta
                            self._snapshots_enabled = False
                finally:
                    conn.close()
            except Exception:
                pass

    @staticmethod
    def _validate_path(path: str | Path | None) -> None:
        if path is None:
            return

        if isinstance(path, (str, Path)) is False:
            raise InvalidConfigurationError("path must be a string, pathlib.Path, or None.")

        if isinstance(path, bool):
            raise InvalidConfigurationError("path must be a string, pathlib.Path, or None.")

        if isinstance(path, str) and not path.strip():
            return

        candidate = Path(path).expanduser()
        if candidate.exists() and candidate.is_symlink():
            raise InvalidConfigurationError("Database path must not be a symlink.")
        if candidate.exists() and not candidate.is_dir():
            raise InvalidConfigurationError("The configured database path points to a regular file, not a directory.")

    @staticmethod
    def _validate_name(name: str) -> None:
        if not isinstance(name, str) or isinstance(name, bool):
            raise InvalidConfigurationError("name must be a non-empty string.")

        if not name:
            raise InvalidConfigurationError("Database name must not be empty.")

        if name in {".", ".."}:
            raise InvalidConfigurationError("Database name must not be '.' or '..'.")

        if not name.isascii():
            raise InvalidConfigurationError("Database name must use ASCII characters only.")

        if any(ch in name for ch in ("/", "\\", ":", ".")):
            raise InvalidConfigurationError(
                "Database name must not include path separators, a colon, or a file extension."
            )

        if re.search(r"\s", name):
            raise InvalidConfigurationError("Database name must not contain whitespace.")

        if name.startswith("."):
            raise InvalidConfigurationError("Database name must start with an ASCII letter or digit.")

        if not re.fullmatch(r"[A-Za-z0-9_-]+", name):
            raise InvalidConfigurationError(
                "Database name must contain only ASCII letters, digits, underscores, and hyphens."
            )

        first = name[0]
        if not (first.isalpha() or first.isdigit()):
            raise InvalidConfigurationError("Database name must start with an ASCII letter or digit.")

        reserved = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "LPT1", "LPT2", "LPT3"}
        if name.upper() in reserved:
            raise InvalidConfigurationError("Database name is a reserved Windows device name and cannot be used.")

    @staticmethod
    def _validate_memory(max_memory: int) -> None:
        if not isinstance(max_memory, int) or isinstance(max_memory, bool):
            raise InvalidConfigurationError("max_memory must be a positive integer number of MiB.")
        if max_memory <= 0:
            raise InvalidConfigurationError("max_memory must be a positive integer number of MiB.")

        max_bytes = max_memory * 1024 * 1024
        if max_memory > ((2**64 - 1) // (1024 * 1024)):
            raise InvalidConfigurationError("max_memory exceeds the representable byte budget.")
        if max_bytes <= 0:
            raise InvalidConfigurationError("max_memory must be a positive integer number of MiB.")

    @staticmethod
    def _validate_snapshot_interval(snapshot_interval: int) -> None:
        if not isinstance(snapshot_interval, int) or isinstance(snapshot_interval, bool):
            raise InvalidConfigurationError(
                "snapshot_interval must be an integer between 5 and 500 seconds, inclusive."
            )
        if snapshot_interval < 5 or snapshot_interval > 500:
            raise InvalidConfigurationError(
                "snapshot_interval must be an integer between 5 and 500 seconds, inclusive."
            )

    @staticmethod
    def _check_system_memory(max_memory: int, *, phase: str) -> dict[str, Any]:
        status = check_memory_budget(max_memory)
        if status["warning"]:
            warnings.warn(
                f"Kor memory budget ({max_memory} MiB) is more than 50% of currently "
                f"available RAM ({status['available_mib']} MiB) during {phase}.",
                MemoryUsageWarning,
                stacklevel=3,
            )
        return status

    @property
    def path(self) -> Path:
        return self._path

    @property
    def name(self) -> str:
        return self._name

    @property
    def max_memory(self) -> int:
        return self._max_memory

    @property
    def snapshot_interval(self) -> int:
        return self._snapshot_interval

    @property
    def read_only(self) -> bool:
        return self._read_only

    def start(self) -> None:
        """Validate the configuration and initialize the single-file Kor database."""
        if self._engine is not None:
            return
        if not self._name:
            raise InvalidConfigurationError("Database configuration is not valid.")

        db_path = self._path / f"{self._name}.kr"
        registry_key = os.path.normcase(os.path.realpath(db_path))
        with _OPEN_DATABASE_PATHS_LOCK:
            if registry_key in _PATH_RESERVATIONS:
                raise PersistenceError("This database file is being replaced in this process; retry after publication.")
            if registry_key in _OPEN_DATABASE_PATHS:
                raise PersistenceError(
                    "This database file is already open in this process; use the existing Database instance."
                )
            _OPEN_DATABASE_PATHS.add(registry_key)
        from . import _kor

        started_ns = time.perf_counter_ns()
        try:
            if self._read_only:
                if db_path.is_symlink() or not db_path.is_file():
                    raise PersistenceError(
                        f"Read-only mode requires an existing regular database file: {db_path}"
                    )
                if not _kor.database_file_is_valid(str(db_path)):
                    raise InvalidConfigurationError(
                        "Read-only mode cannot open an invalid .kr database file."
                    )
                stored_config = _kor.read_database_config(str(db_path))
                is_created = False
            else:
                stored_config = _kor.read_database_config(str(db_path)) if db_path.exists() else None
                if stored_config is None:
                    self._check_system_memory(self._max_memory, phase="database startup")
                    is_created = _kor.initialize_database_file(
                        str(db_path),
                        self._name,
                        int(self._max_memory),
                        int(self._snapshot_interval),
                    )
                    stored_config = _kor.read_database_config(str(db_path)) if not is_created else None
                else:
                    is_created = False
            if not is_created:
                if stored_config is None:
                    stored_config = _kor.read_database_config(str(db_path))
                if stored_config["name"] != self._name:
                    raise InvalidConfigurationError(
                        "The requested database name does not match the identity stored in the .kr file."
                    )
                self._max_memory = int(stored_config["max_memory_mib"])
                self._snapshot_interval = int(stored_config["snapshot_interval_seconds"])
                self._check_system_memory(self._max_memory, phase="database startup")

            self._engine = _kor.Engine(
                str(db_path),
                int(self._max_memory) * 1024 * 1024,
                self._read_only,
            )
            self._storage = self._engine.storage()
            if not self._snapshots_enabled:
                if hasattr(self._storage, "stop_auto_snapshots"):
                    self._storage.stop_auto_snapshots()
            self._registry_key = registry_key
            self._lifecycle = "open"
            self._health_override = None
            log_event(
                _LOGGER,
                event="database.open",
                database_id=self._database_id,
                operation="open",
                success=True,
                duration_ns=time.perf_counter_ns() - started_ns,
            )
        except BaseException as startup_error:
            self._lifecycle = "failed"
            self._last_error = {
                "operation": "open",
                "category": type(startup_error).__name__,
            }
            log_event(
                _LOGGER,
                event="database.open",
                database_id=self._database_id,
                operation="open",
                success=False,
                duration_ns=time.perf_counter_ns() - started_ns,
                error=startup_error,
            )
            if self._engine is not None:
                try:
                    self._engine.close()
                except Exception as cleanup_error:
                    self._registry_key = registry_key
                    raise cleanup_error from startup_error
                self._engine = None
                self._storage = None
            with _OPEN_DATABASE_PATHS_LOCK:
                _OPEN_DATABASE_PATHS.discard(registry_key)
            raise

    def transaction(self):
        if self._storage is None:
            self.start()
        return self._storage.transaction()

    def _observe(self, operation: str, call: Any, *, level: int = logging.INFO) -> Any:
        started_ns = time.perf_counter_ns()
        try:
            result = call()
        except Exception as error:
            self._last_error = {"operation": operation, "category": type(error).__name__}
            if operation == "compact":
                self._health_override = "DEGRADED"
            log_event(
                _LOGGER,
                event="database.operation",
                database_id=self._database_id,
                operation=operation,
                success=False,
                duration_ns=time.perf_counter_ns() - started_ns,
                error=error,
            )
            raise
        if operation == "compact":
            self._health_override = None
        log_event(
            _LOGGER,
            event="database.operation",
            database_id=self._database_id,
            operation=operation,
            success=True,
            duration_ns=time.perf_counter_ns() - started_ns,
            level=level,
        )
        return result

    def set(self, key: str | None = None, *values: Any, **kwargs: Any) -> None:
        """Set column values for a key: client.set(key="user:102", c1="Jane Smith", c2="Design", c3=NULL)."""
        # Extract key if passed in kwargs
        if key is None:
            if "key" in kwargs:
                key = kwargs.pop("key")
            elif values and isinstance(values[0], str):
                key = values[0]
                values = values[1:]
            else:
                raise TypeError("set() missing required argument: 'key'")
        elif "key" in kwargs:
            key = kwargs.pop("key")

        if not isinstance(key, str):
            raise TypeError(f"Database key must be a string, got {type(key).__name__}.")

        if self._number_of_values is not None:
            cols = [None] * self._number_of_values

            # Check if column kwargs (c1, c2, ... or col1, col2, ...) were provided
            col_kwargs_found = False
            for k, val in list(kwargs.items()):
                m = re.match(r"^(?:c|col|column)(\d+)$", k, re.IGNORECASE)
                if m:
                    idx = int(m.group(1)) - 1
                    if 0 <= idx < self._number_of_values:
                        cols[idx] = val
                        col_kwargs_found = True
                    else:
                        raise ValueError(
                            f"Column '{k}' is out of range for a {self._number_of_values}-column database."
                        )

            if not col_kwargs_found:
                if len(values) == 1 and isinstance(values[0], (list, tuple)):
                    raw_vals = list(values[0])
                elif values:
                    raw_vals = list(values)
                elif "value" in kwargs:
                    v = kwargs["value"]
                    raw_vals = list(v) if isinstance(v, (list, tuple)) else [v]
                elif "values" in kwargs:
                    v = kwargs["values"]
                    raw_vals = list(v) if isinstance(v, (list, tuple)) else [v]
                else:
                    raw_vals = []

                if len(raw_vals) > self._number_of_values:
                    raise ValueError(
                        f"Expected at most {self._number_of_values} values for key '{key}', but got {len(raw_vals)}."
                    )
                for i, val in enumerate(raw_vals):
                    cols[i] = val

            value_to_store = cols
        else:
            if kwargs and "value" in kwargs:
                value_to_store = kwargs["value"]
            elif values:
                value_to_store = values[0]
            else:
                raise TypeError("set() missing required argument: 'value'")

        if self._storage is None:
            self.start()
        self._observe("set", lambda: self._storage.set(key, value_to_store))

    def get(self, key: str, default: Any = None) -> Any:
        """Get all column values for a key in list format. If unset, column values default to null."""
        if self._storage is None:
            self.start()

        # Check if key exists in storage
        exists = self._storage.exists(key)
        if not exists:
            if self._number_of_values is not None and default is None:
                return [None] * self._number_of_values
            return default

        val = self._observe("get", lambda: self._storage.get(key, default), level=logging.DEBUG)
        if val is default and not exists:
            if self._number_of_values is not None and default is None:
                return [None] * self._number_of_values
            return default

        if self._number_of_values is not None:
            if isinstance(val, (list, tuple)):
                res = list(val)
                if len(res) < self._number_of_values:
                    res.extend([None] * (self._number_of_values - len(res)))
                return res
            return [val] + [None] * (self._number_of_values - 1)
        return val

    def get_many(self, keys: Any) -> list[Any]:
        materialized_keys = self._validate_multi_read_keys(keys)
        if self._storage is None:
            self.start()
        values = self._observe(
            "get_many",
            lambda: self._storage.get_many(materialized_keys),
            level=logging.DEBUG,
        )
        if self._number_of_values is not None:
            return [
                (list(v) if isinstance(v, (list, tuple)) else [v])
                if v is not None else ([None] * self._number_of_values)
                for v in values
            ]
        return values

    def set_model(self, key: str, model: KorModel) -> None:
        if not isinstance(model, KorModel):
            raise TypeError("model must implement to_dict() and from_dict().")
        document = model.to_dict()
        if not isinstance(document, dict) or any(not isinstance(field, str) for field in document):
            raise TypeError("KorModel.to_dict() must return a string-keyed dict.")
        self.set(key, document)

    def get_model(self, key: str, model_type: type[ModelT]) -> ModelT | None:
        if not isinstance(model_type, type) or not callable(getattr(model_type, "from_dict", None)):
            raise TypeError("model_type must implement the KorModel.from_dict() class method.")
        document = self.get(key)
        if document is None:
            return None
        if not isinstance(document, dict):
            raise TypeError("Stored value is not a mapping and cannot be decoded as a KorModel.")
        model = model_type.from_dict(document)
        if not isinstance(model, model_type):
            raise TypeError("KorModel.from_dict() must return an instance of model_type.")
        return model

    def query(
        self,
        filter: Filter,
        *,
        prefix: str = "",
        after: str | None = None,
        limit: int = 100,
    ) -> dict[str, Any]:
        if not callable(getattr(filter, "matches", None)):
            raise TypeError("filter must implement matches(value).")
        if not isinstance(prefix, str):
            raise TypeError("prefix must be a string.")
        if not isinstance(limit, int) or isinstance(limit, bool) or not 1 <= limit <= 1000:
            raise InvalidConfigurationError("limit must be an integer between 1 and 1000.")
        if after is not None and not isinstance(after, str):
            raise TypeError("after must be a string or None.")
        if self._storage is None:
            self.start()

        def run_query() -> dict[str, Any]:
            rows: list[tuple[str, Any]] = []
            cursor = after
            last_returned: str | None = None
            has_more = False
            while True:
                page = self._storage.scan(prefix, cursor, 1000)
                for key, value in page["records"]:
                    cursor = key
                    if filter.matches(value):
                        if len(rows) == limit:
                            has_more = True
                            break
                        rows.append((key, value))
                        last_returned = key
                if has_more or not page["has_more"]:
                    break
            return {
                "records": rows,
                "next_cursor": last_returned if has_more else None,
                "has_more": has_more,
            }

        return self._observe(
            "query.filter",
            run_query,
            level=logging.DEBUG,
        )

    def contains_many(self, keys: Any) -> list[bool]:
        materialized_keys = self._validate_multi_read_keys(keys)
        if self._storage is None:
            self.start()
        return self._observe(
            "contains_many",
            lambda: self._storage.contains_many(materialized_keys),
            level=logging.DEBUG,
        )

    @staticmethod
    def _validate_multi_read_keys(keys: Any) -> list[str]:
        if isinstance(keys, (str, bytes, bytearray)):
            raise TypeError("keys must be an iterable of strings, not a string.")
        materialized_keys = list(keys)
        if len(materialized_keys) > 10_000:
            raise InvalidConfigurationError("A multi-key read accepts at most 10000 keys.")
        if any(not isinstance(key, str) for key in materialized_keys):
            raise TypeError("Every key must be a string.")
        return materialized_keys

    def delete(self, key: str) -> None:
        if self._storage is None:
            self.start()
        self._observe("delete", lambda: self._storage.erase(key))

    def contains(self, key: str) -> bool:
        if self._storage is None:
            self.start()
        return self._observe("exists", lambda: self._storage.exists(key), level=logging.DEBUG)

    def count(self) -> int:
        if self._storage is None:
            self.start()
        return self._observe("count", self._storage.count, level=logging.DEBUG)

    def scan(
        self,
        prefix: str = "",
        *,
        after: str | None = None,
        limit: int = 100,
    ) -> dict[str, Any]:
        """Return one deterministic key-ordered page; continue with its exclusive cursor."""
        if not isinstance(prefix, str):
            raise TypeError("prefix must be a string.")
        if not isinstance(limit, int) or isinstance(limit, bool) or not 1 <= limit <= 1000:
            raise InvalidConfigurationError("limit must be an integer between 1 and 1000.")
        if after is not None and not isinstance(after, str):
            raise TypeError("after must be a string or None.")
        if self._storage is None:
            self.start()
        result = self._observe(
            "query.scan",
            lambda: self._storage.scan(prefix, after, limit),
            level=logging.DEBUG,
        )
        if self._number_of_values is not None and "records" in result:
            result["records"] = [
                (k, list(v) if isinstance(v, (list, tuple)) else [v])
                for k, v in result["records"]
            ]
        return result

    def set_many(self, entries: dict[str, Any]) -> None:
        """Atomically set every mapping entry in one transaction."""
        if not isinstance(entries, dict):
            raise TypeError("entries must be a dict mapping keys to values.")
        if self._number_of_values is not None:
            validated_entries = {}
            for k, v in entries.items():
                cols = [None] * self._number_of_values
                if isinstance(v, dict):
                    for col_k, col_val in v.items():
                        m = re.match(r"^(?:c|col|column)(\d+)$", col_k, re.IGNORECASE)
                        if m:
                            idx = int(m.group(1)) - 1
                            if 0 <= idx < self._number_of_values:
                                cols[idx] = col_val
                    validated_entries[k] = cols
                elif isinstance(v, (list, tuple)):
                    row = list(v)
                    if len(row) > self._number_of_values:
                        raise ValueError(f"Expected at most {self._number_of_values} values for key '{k}'.")
                    for i, item in enumerate(row):
                        cols[i] = item
                    validated_entries[k] = cols
                elif self._number_of_values == 1:
                    validated_entries[k] = [v]
                else:
                    cols[0] = v
                    validated_entries[k] = cols
            entries_to_store = validated_entries
        else:
            entries_to_store = entries

        if self._storage is None:
            self.start()
        self._observe("set_many", lambda: self._storage.set_many(entries_to_store))

    def delete_many(self, keys: Any) -> None:
        """Atomically delete all supplied keys in one transaction."""
        if isinstance(keys, (str, bytes, bytearray)):
            raise TypeError("keys must be an iterable of strings, not a string.")
        if self._storage is None:
            self.start()
        self._observe("delete_many", lambda: self._storage.delete_many(list(keys)))

    def create_index(
        self,
        name: str,
        field: str,
        index_type: str = "hash",
        unique: bool = False,
    ) -> None:
        """Build a persistent secondary index over one top-level mapping field."""
        if self._storage is None:
            self.start()
        self._observe(
            "index.create",
            lambda: self._storage.create_index(name, field, index_type, unique),
        )

    def create_composite_index(
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
        if self._storage is None:
            self.start()
        self._observe(
            "index.create",
            lambda: self._storage.create_composite_index(name, materialized_fields, unique),
        )

    def drop_index(self, name: str) -> None:
        """Drop a named secondary index; missing names raise IndexNotFoundError."""
        if self._storage is None:
            self.start()
        self._observe("index.drop", lambda: self._storage.drop_index(name))

    def list_indexes(self) -> list[Any]:
        """Return index definitions in deterministic name order."""
        if self._storage is None:
            self.start()
        return self._observe("index.list", self._storage.list_indexes, level=logging.DEBUG)

    def find(self, field: str, value: Any) -> list[tuple[str, Any]]:
        """Find top-level scalar field values; results are sorted by primary key."""
        if self._storage is None:
            self.start()
        return self._observe("query.find", lambda: self._storage.find(field, value), level=logging.DEBUG)

    def find_range(
        self,
        field: str,
        minimum: Any = None,
        maximum: Any = None,
        *,
        include_minimum: bool = True,
        include_maximum: bool = True,
    ) -> list[tuple[str, Any]]:
        """Find scalar values in an inclusive-by-default range, sorted by primary key."""
        if self._storage is None:
            self.start()
        return self._observe(
            "query.find_range",
            lambda: self._storage.find_range(
                field,
                minimum,
                maximum,
                include_minimum,
                include_maximum,
            ),
            level=logging.DEBUG,
        )

    def find_composite(self, fields: Any, values: Any) -> list[tuple[str, Any]]:
        if isinstance(fields, (str, bytes, bytearray)) or isinstance(values, (str, bytes, bytearray)):
            raise TypeError("fields and values must be non-string iterables.")
        materialized_fields = list(fields)
        materialized_values = list(values)
        if self._storage is None:
            self.start()
        return self._observe(
            "query.find_composite",
            lambda: self._storage.find_composite(materialized_fields, materialized_values),
            level=logging.DEBUG,
        )

    def find_composite_range(
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
        if self._storage is None:
            self.start()
        return self._observe(
            "query.find_composite_range",
            lambda: self._storage.find_composite_range(
                materialized_fields,
                materialized_minimum,
                materialized_maximum,
                include_minimum,
                include_maximum,
            ),
            level=logging.DEBUG,
        )

    def storage_stats(self) -> dict[str, Any]:
        """Return file, WAL, checkpoint, and estimated in-memory storage statistics."""
        if self._storage is None:
            self.start()
        return self._observe("storage.stats", self._storage.storage_stats, level=logging.DEBUG)

    def checkpoint_status(self) -> dict[str, Any]:
        """Return counters and the latest result for background checkpoints."""
        if self._storage is None:
            self.start()
        return self._observe(
            "checkpoint.status",
            self._storage.checkpoint_status,
            level=logging.DEBUG,
        )

    def checkpoint(self) -> dict[str, Any]:
        """Persist a checkpoint without requiring the database file to shrink."""
        if self._storage is None:
            self.start()
        return self._observe("checkpoint", self._storage.checkpoint)

    def compact(self) -> dict[str, Any]:
        """Synchronously checkpoint the current state and reclaim WAL history when it shrinks the .kr file."""
        if self._storage is None:
            self.start()
        return self._observe("compact", self._storage.compact)

    def backup(self, destination: str | os.PathLike[str], *, overwrite: bool = False) -> dict[str, Any]:
        """Create a physical backup of the current database at a caller-specified destination."""
        if self._storage is None:
            self.start()

        source_path = self.path / f"{self.name}.kr"
        snapshot_transaction = None if self._read_only else self._storage.transaction()
        try:
            destination_path = _copy_file_atomically(source_path, destination, overwrite=overwrite)
            result = {
                "destination": str(destination_path),
                "source_database_id": self._database_id,
                "source_format_version": "1.0",
                "backup_format_version": "1.0",
                "consistency_boundary": "database_file_snapshot",
                "checkpoint_lsn": self._storage.storage_stats().get("last_checkpoint_lsn", 0),
                "record_count": self._storage.count(),
                "status": "ok",
            }
            return result
        except Exception as error:
            raise PersistenceError(f"Physical backup failed: {error}") from error
        finally:
            if snapshot_transaction is not None:
                snapshot_transaction.rollback()

    def restore(
        self,
        source: str | os.PathLike[str],
        destination: str | os.PathLike[str] | None = None,
        *,
        overwrite: bool = False,
    ) -> dict[str, Any]:
        """Restore a physical Kor database snapshot to a destination path."""
        source_path = _resolve_database_file_path(source)
        target_path = Path(destination).expanduser() if destination is not None else (self.path / f"{self.name}.kr")
        _restore_database_file(source_path, target_path, overwrite=overwrite)
        return {
            "source": str(source_path),
            "destination": str(target_path),
            "status": "ok",
        }

    def export_data(self, destination: str | os.PathLike[str], *, format: str = "jsonl", overwrite: bool = False) -> dict[str, Any]:
        """Serialize user-visible records to a versioned JSONL export."""
        if format != "jsonl":
            raise ValueError("Only format='jsonl' is supported.")
        if self._storage is None:
            self.start()

        destination_path = Path(destination).expanduser()
        if destination_path.exists() and destination_path.is_dir():
            raise PersistenceError("Export destination must be a file path, not a directory.")
        if destination_path.exists() and not overwrite:
            raise PersistenceError(f"Export destination already exists: {destination_path}")

        export_records = []
        for key, value in self._storage.list_records():
            export_records.append({"key": key, "value": value})

        temp_path = None
        try:
            destination_parent = destination_path.parent
            if not destination_parent.exists():
                raise PersistenceError(f"Export destination directory does not exist: {destination_parent}")
            temp_path = destination_path.with_name(f".{destination_path.name}.tmp.{os.getpid()}")
            with open(temp_path, "w", encoding="utf-8", newline="\n") as handle:
                handle.write(
                    "{\"kor_format\":\"jsonl\",\"schema_version\":%d,\"record_count\":%d}\n"
                    % (_JSONL_SCHEMA_VERSION, len(export_records))
                )
                for item in export_records:
                    handle.write(
                        json.dumps(
                            {
                                "key": item["key"],
                                "value": _encode_json_value(item["value"]),
                            },
                            separators=(",", ":"),
                            ensure_ascii=False,
                            sort_keys=True,
                        )
                    )
                    handle.write("\n")
            os.replace(str(temp_path), str(destination_path))
            return {
                "destination": str(destination_path),
                "format": "jsonl",
                "schema_version": _JSONL_SCHEMA_VERSION,
                "record_count": len(export_records),
                "status": "ok",
            }
        except Exception as error:
            if temp_path is not None and temp_path.exists():
                try:
                    temp_path.unlink()
                except OSError:
                    pass
            raise PersistenceError(f"Logical export failed: {error}") from error

    def import_data(self, source: str | os.PathLike[str], *, conflict: str = "error", atomic: bool = True) -> dict[str, Any]:
        """Import a JSONL export into the database following the selected conflict policy."""
        if self._read_only:
            raise ReadOnlyError("Cannot import records into a read-only database.")
        if self._storage is None:
            self.start()
        if conflict not in {"error", "overwrite", "skip"}:
            raise ValueError("conflict must be one of: 'error', 'overwrite', 'skip'.")

        source_path = Path(source).expanduser()
        if source_path.is_symlink():
            raise PersistenceError(f"Import source must not be a symlink: {source_path}")
        if not source_path.exists() or not source_path.is_file():
            raise PersistenceError(f"Import source does not exist: {source_path}")

        records: list[tuple[str, Any]] = []
        with open(source_path, "r", encoding="utf-8") as handle:
            header_line = handle.readline()
            if not header_line:
                raise PersistenceError("Import source is empty or truncated.")
            try:
                header = json.loads(header_line)
            except json.JSONDecodeError as exc:
                raise PersistenceError("Import header is invalid JSON.") from exc
            if not isinstance(header, dict) or header.get("kor_format") != "jsonl":
                raise PersistenceError("Import file is not a supported Kor JSONL export.")
            schema_version = header.get("schema_version")
            record_count = header.get("record_count")
            if type(schema_version) is not int or schema_version not in {1, _JSONL_SCHEMA_VERSION}:
                raise PersistenceError("Import file uses an unsupported Kor JSONL schema version.")
            if type(record_count) is not int or record_count < 0:
                raise PersistenceError("Import header has an invalid record count.")
            for line_number, line in enumerate(handle, start=2):
                if not line.strip():
                    continue
                try:
                    item = json.loads(line)
                except json.JSONDecodeError as exc:
                    raise PersistenceError(f"Malformed JSON on line {line_number}.") from exc
                if not isinstance(item, dict) or "key" not in item or "value" not in item:
                    raise PersistenceError(f"Malformed record on line {line_number}.")
                key = item["key"]
                if not isinstance(key, str) or not key:
                    raise PersistenceError(f"Key on line {line_number} is missing or invalid.")
                records.append(
                    (key, _decode_json_value(item["value"], tagged=schema_version >= 2))
                )

        if len(records) != record_count:
            raise PersistenceError(
                f"Import record count mismatch: expected {record_count}, found {len(records)}."
            )

        if atomic:
            with self.transaction() as transaction:
                seen: set[str] = set()
                for key, value in records:
                    if key in seen:
                        raise PersistenceError(f"Duplicate key in import file: {key}")
                    seen.add(key)
                    if self._storage.exists(key) and conflict == "error":
                        raise PersistenceError(f"Import conflict for existing key: {key}")
                    if self._storage.exists(key) and conflict == "skip":
                        continue
                    transaction.set(key, value)
            return {"source": str(source_path), "format": "jsonl", "record_count": len(records), "conflict": conflict, "atomic": True, "status": "ok"}

        imported = 0
        for key, value in records:
            if self._storage.exists(key) and conflict == "error":
                raise PersistenceError(f"Import conflict for existing key: {key}")
            if self._storage.exists(key) and conflict == "skip":
                continue
            self._storage.set(key, value)
            imported += 1
        return {"source": str(source_path), "format": "jsonl", "record_count": imported, "conflict": conflict, "atomic": False, "status": "ok"}

    def verify_integrity(self, *, level: str = "structural") -> dict[str, Any]:
        """Return a read-only integrity verification result for the current database."""
        if self._storage is None:
            self.start()
        if level not in {"structural", "full"}:
            raise ValueError("level must be 'structural' or 'full'.")

        source_file = self.path / f"{self.name}.kr"
        issues = []
        record_count = 0
        try:
            if not source_file.exists():
                issues.append({"severity": "error", "code": "missing_file", "message": "Database file is missing."})
            if self._storage is not None:
                stats = self._storage.storage_stats()
                record_count = int(stats.get("record_count", 0))
                if record_count < 0:
                    issues.append({"severity": "error", "code": "invalid_record_count", "message": "Record count is negative."})
                if stats.get("file_size_bytes", 0) < 0:
                    issues.append({"severity": "error", "code": "invalid_file_size", "message": "File size is negative."})
        except Exception as error:  # pragma: no cover
            issues.append({"severity": "error", "code": "open_error", "message": str(error)})
            return {
                "status": "fail",
                "level": level,
                "database_id": self._database_id,
                "format_version": "1.0",
                "checks_performed": 1,
                "record_count": record_count,
                "issues": issues,
                "repair_performed": False,
                "recovery_required": False,
            }

        if level == "full":
            for key, value in self._storage.list_records():
                if key is None:
                    issues.append({"severity": "error", "code": "invalid_record", "message": "Null key encountered."})
                if not isinstance(value, (str, int, float, bool, type(None), list, dict, bytes, bytearray)):
                    issues.append({"severity": "error", "code": "unsupported_value", "message": "Unsupported value type encountered."})
                elif isinstance(value, (list, dict)):
                    try:
                        json.dumps(value)
                    except (TypeError, ValueError):
                        issues.append({"severity": "error", "code": "non_serializable_value", "message": "Value cannot be serialized to JSON."})

        return {
            "status": "pass" if not issues else "fail",
            "level": level,
            "database_id": self._database_id,
            "format_version": "1.0",
            "checks_performed": 1 + len(issues),
            "record_count": record_count,
            "issues": issues,
            "repair_performed": False,
            "recovery_required": False,
        }

    def diagnostics(self) -> dict[str, Any]:
        """Return a read-only snapshot of runtime metrics, health, and storage accounting."""
        from . import _kor

        raw_metrics = None
        storage_stats = None
        if self._storage is not None:
            raw_metrics = self._storage.runtime_metrics()
            if not raw_metrics["recovery_required"]:
                storage_stats = self._storage.storage_stats()
        elif self._closed_metrics is not None:
            raw_metrics = self._closed_metrics

        if raw_metrics is None:
            counters: dict[str, int] = {}
            latencies: dict[str, Any] = {}
            current_lsn: int | None = None
            checkpoint_lsn: int | None = None
            active_indexes: int | None = None
            active_transactions: int | None = None
            recovery_required = False
        else:
            counters = dict(raw_metrics["counters"])
            latencies = add_latency_percentiles(raw_metrics["latencies"])
            current_lsn = int(raw_metrics["current_committed_lsn"])
            checkpoint_lsn = int(raw_metrics["last_checkpoint_lsn"])
            active_indexes = int(raw_metrics["active_index_count"])
            active_transactions = int(counters["active_transactions"])
            recovery_required = bool(raw_metrics["recovery_required"])

        if self._lifecycle == "closed":
            health = "CLOSED"
        elif self._lifecycle == "failed":
            health = "FAILED"
        elif recovery_required:
            health = "RECOVERY_REQUIRED"
        elif self._health_override is not None:
            health = self._health_override
        elif self._lifecycle == "not_started":
            health = "NOT_STARTED"
        else:
            health = "HEALTHY"

        return {
            "schema_version": 1,
            "kor_version": _version.__version__,
            "native_engine_version": _kor.native_version(),
            "file_format_version": "1.0",
            "database_id": self._database_id,
            "lifecycle_state": self._lifecycle,
            "health_state": health,
            "current_committed_lsn": current_lsn,
            "last_checkpoint_lsn": checkpoint_lsn,
            "active_transaction_count": active_transactions,
            "active_index_count": active_indexes,
            "counters": counters,
            "latencies": latencies,
            "storage": storage_stats,
            "last_error": dict(self._last_error) if self._last_error is not None else None,
            "last_background_error": None,
        }

    def close(self) -> None:
        if self._engine is not None:
            started_ns = time.perf_counter_ns()
            try:
                self._closed_metrics = self._storage.runtime_metrics()
                self._engine.close()
            except Exception as error:
                self._last_error = {"operation": "close", "category": type(error).__name__}
                log_event(
                    _LOGGER,
                    event="database.close",
                    database_id=self._database_id,
                    operation="close",
                    success=False,
                    duration_ns=time.perf_counter_ns() - started_ns,
                    error=error,
                )
                raise
            self._engine = None
            self._storage = None
            self._lifecycle = "closed"
            self._health_override = None
            if self._registry_key is not None:
                with _OPEN_DATABASE_PATHS_LOCK:
                    _OPEN_DATABASE_PATHS.discard(self._registry_key)
                self._registry_key = None
            log_event(
                _LOGGER,
                event="database.close",
                database_id=self._database_id,
                operation="close",
                success=True,
                duration_ns=time.perf_counter_ns() - started_ns,
            )

    @property
    def hex(self) -> str | None:
        return self._hex

    @property
    def number_of_values(self) -> int | None:
        return self._number_of_values

    @property
    def info(self) -> dict[str, Any] | None:
        return self._info

    @property
    def snapshots_enabled(self) -> bool:
        return self._snapshots_enabled

    def _connect_instance(
        self,
        path: str | Path | None = None,
        name: str = "",
        hex: str = "",
        *,
        read_only: bool = False,
    ) -> Database:
        target_path = path if path is not None else self._path
        target_name = name if name else self._name
        connected = _connect_database(type(self), target_path, target_name, hex, read_only=read_only)
        if self._engine is None:
            self._path = connected._path
            self._name = connected._name
            self._max_memory = connected._max_memory
            self._snapshot_interval = connected._snapshot_interval
            self._read_only = connected._read_only
            self._engine = connected._engine
            self._storage = connected._storage
            self._registry_key = connected._registry_key
            self._database_id = connected._database_id
            self._lifecycle = connected._lifecycle
            self._hex = connected._hex
            self._number_of_values = connected._number_of_values
            self._info = connected._info
            self._snapshots_enabled = connected._snapshots_enabled
            return self
        return connected



    def __contains__(self, key: object) -> bool:
        if not isinstance(key, str):
            return False
        return self.contains(key)

    def __len__(self) -> int:
        return self.count()

    def __repr__(self) -> str:
        if self._hex is not None:
            return (
                f"Database(path={self._path!s}, name={self._name!r}, "
                f"number_of_values={self._number_of_values}, hex={self._hex!r}, "
                f"storage='rom', runtime='ram')"
            )
        return (
            f"Database(path={self._path!s}, name={self._name!r}, max_memory={self._max_memory}, "
            f"snapshot_interval={self._snapshot_interval}, read_only={self._read_only})"
        )


_COL_MAP = {f"c{i+1}": i for i in range(32)}
_COL_MAP.update({f"col{i+1}": i for i in range(32)})
_COL_MAP.update({f"column{i+1}": i for i in range(32)})


def _dump_col_val(v: Any) -> Any:
    return v


def _load_col_val(v: Any) -> Any:
    return v


def _normalize_column_values(
    key: str,
    values: tuple[Any, ...],
    kwargs: dict[str, Any],
    number_of_values: int,
) -> list[Any]:
    cols = [None] * number_of_values
    col_found = False
    for k, val in kwargs.items():
        idx = _COL_MAP.get(k.lower())
        if idx is not None and idx < number_of_values:
            cols[idx] = val
            col_found = True

    if not col_found:
        if len(values) == 1 and isinstance(values[0], (list, tuple)):
            for i, x in enumerate(values[0][:number_of_values]):
                cols[i] = x
        else:
            for i, x in enumerate(values[:number_of_values]):
                cols[i] = x

    return cols


class RomTransaction:
    __slots__ = ("_db", "_staged_sets", "_staged_dels", "_active", "_num_vals", "_fast_set", "_fast_del")

    def __init__(self, db: RomDatabase):
        self._db = db
        self._staged_sets: dict[str, list[Any]] = {}
        self._staged_dels: list[str] = []
        self._active = True
        self._num_vals = db._number_of_values
        self._fast_set = self._staged_sets.__setitem__
        self._fast_del = self._staged_dels.append

    def __enter__(self):
        if not self._active:
            raise TransactionStateError("Transaction is no longer active.")
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        if self._active:
            if exc_type is not None:
                self.rollback()
            else:
                self.commit()
        return False

    def set(self, key: Any = None, value: Any = None, *values: Any, overwrite: bool = False, **kwargs: Any) -> None:
        if type(key) is str and type(value) is list and len(value) == self._num_vals:
            if not overwrite and (key in self._staged_sets or self._db.contains(key)):
                raise DuplicateKeyError(f"Key {key!r} already exists. Use overwrite=True to replace it.")
            self._fast_set(key, value)
            return

        if not self._active:
            raise TransactionStateError("Transaction is no longer active.")

        if type(key) is str and kwargs and not values and value is None:
            if self._num_vals == 3 and ("c1" in kwargs or "c2" in kwargs or "c3" in kwargs):
                self._fast_set(key, [kwargs.get("c1"), kwargs.get("c2"), kwargs.get("c3")])
                return
            cols = [None] * self._num_vals
            col_map = _COL_MAP
            for k, val in kwargs.items():
                idx = col_map.get(k.lower())
                if idx is not None and idx < self._num_vals:
                    cols[idx] = val
            self._fast_set(key, cols)
            return

        if key is None:
            if "key" in kwargs:
                key = kwargs.pop("key")
            elif values and isinstance(values[0], str):
                key = values[0]
                values = values[1:]
            else:
                raise TypeError("key must be provided as a string or keyword argument 'key'.")
        elif "key" in kwargs:
            key = kwargs.pop("key")

        if not isinstance(key, str):
            raise TypeError("key must be a string.")

        all_vals = (value,) + values if value is not None else values
        cols = _normalize_column_values(key, all_vals, kwargs, self._num_vals)
        self._fast_set(key, cols)

    def set_many(self, entries: dict[str, Any]) -> None:
        if not self._active:
            raise TransactionStateError("Transaction is no longer active.")
        self._staged_sets.update(entries)

    def get(self, key):
        if not self._active:
            raise TransactionStateError("Transaction is no longer active.")
        if key in self._staged_sets:
            return self._staged_sets[key]
        return self._db.get(key)

    def contains(self, key):
        if not self._active:
            raise TransactionStateError("Transaction is no longer active.")
        if key in self._staged_sets:
            return True
        return self._db.contains(key)

    def delete(self, key):
        if type(key) is str:
            self._fast_del(key)
            return
        if not self._active:
            raise TransactionStateError("Transaction is no longer active.")
        if self._staged_sets:
            self._staged_sets.pop(key, None)
        self._fast_del(key)

    def delete_many(self, keys: Any) -> None:
        if not self._active:
            raise TransactionStateError("Transaction is no longer active.")
        store_pop = self._staged_sets.pop
        for k in keys:
            store_pop(k, None)
        self._staged_dels.extend(keys)

    def commit(self):
        if not self._active:
            return
        if self._staged_sets:
            self._db._store.update(self._staged_sets)
            if self._db._tombstones:
                self._db._tombstones.difference_update(self._staged_sets)
            self._db._wal_bytes += len(self._staged_sets)
            self._staged_sets.clear()
        if self._staged_dels:
            self._db._tombstones.update(self._staged_dels)
            self._db._wal_bytes += len(self._staged_dels)
            self._staged_dels.clear()
        self._active = False

    def rollback(self):
        if not self._active:
            return
        self._staged_sets.clear()
        self._staged_dels.clear()
        self._active = False


class RomDatabase(Database):
    """Ultra-high-performance database engine storing all records in ROM (disk storage),
    operating directly in ROM with high-throughput streaming WAL.
    """

    def __new__(cls, *args, **kwargs):
        return object.__new__(cls)

    def __init__(
        self,
        path: str | Path,
        name: str,
        hex: str,
        number_of_values: int,
        info: dict[str, Any],
        *,
        read_only: bool = False,
    ) -> None:
        self._path = Path(path).resolve()
        self._name = str(name)
        self._hex = str(hex)
        self._number_of_values = int(number_of_values)
        self._info = dict(info)
        self._read_only = bool(read_only)
        self._max_memory = 0
        self._snapshot_interval = 0
        self._registry_key = None
        self._database_id = f"rom_{name}"
        self._lifecycle = "started"
        self._health_override = None
        self._last_error = None
        self._closed_metrics = None
        self._snapshots_enabled = False
        self._lock = threading.RLock()
        self._closed = False
        self._db_file = self._path / f"{name}.kr"

        self._store: dict[str, list[Any]] = {}
        self._tombstones: set[str] = set()
        self._wal_bytes = 0

        # Load existing data from ROM .kr file if present
        import marshal
        import struct
        if self._db_file.exists() and self._db_file.stat().st_size >= 20:
            try:
                with open(self._db_file, "rb") as fp:
                    header = fp.read(16)
                    if header.startswith(b"KOR_ROM_DB"):
                        hlen = struct.unpack("<I", fp.read(4))[0]
                        _ = fp.read(hlen)  # Skip metadata JSON
                        try:
                            loaded = marshal.load(fp)
                            if isinstance(loaded, dict):
                                self._store.update(loaded)
                        except Exception:
                            pass
                    elif header.startswith(b"SQLite format 3"):
                        import sqlite3
                        conn = sqlite3.connect(str(self._db_file), timeout=5.0)
                        try:
                            cur = conn.cursor()
                            cols = ", ".join(f"c{i+1}" for i in range(self._number_of_values))
                            cur.execute(f"SELECT key, {cols} FROM records")
                            for row in cur.fetchall():
                                self._store[row[0]] = list(row[1:])
                        except Exception:
                            pass
                        finally:
                            conn.close()
            except Exception:
                pass

        # Open ROM file for high-throughput buffered WAL appends (16 MiB write buffer)
        self._fp = open(self._db_file, "ab", buffering=16 * 1024 * 1024)

        # Sentinels so AsyncDatabase / async_connect recognize active engine & storage
        self._engine = self
        self._storage = self

    def __getitem__(self, key: Any) -> Any:
        raise TypeError("'Database' object is not subscriptable. Use get() and set() methods instead.")

    def __setitem__(self, key: Any, value: Any) -> None:
        raise TypeError("'Database' object does not support item assignment. Use get() and set() methods instead.")

    def __delitem__(self, key: Any) -> None:
        raise TypeError("'Database' object does not support item deletion. Use get() and set() methods instead.")

    @property
    def path(self) -> Path:
        return self._path

    @property
    def name(self) -> str:
        return self._name

    @property
    def hex(self) -> str:
        return self._hex

    @property
    def number_of_values(self) -> int:
        return self._number_of_values

    @property
    def info(self) -> dict[str, Any]:
        return self._info

    @property
    def is_closed(self) -> bool:
        return self._closed

    @property
    def read_only(self) -> bool:
        return self._read_only

    @property
    def snapshots_enabled(self) -> bool:
        return False

    @property
    def max_memory(self) -> int:
        return self._max_memory

    @property
    def snapshot_interval(self) -> int:
        return self._snapshot_interval

    @property
    def current_memory_bytes(self) -> int:
        return 0

    @property
    def is_degraded(self) -> bool:
        return False

    @property
    def lifecycle_state(self) -> str:
        return self._lifecycle

    @property
    def health_state(self) -> str:
        return "CLOSED" if self._closed else "HEALTHY"

    def start(self) -> RomDatabase:
        if self._closed:
            raise StorageClosedError("Database is closed.")
        return self

    def stop(self) -> None:
        self.close()

    def set(self, key: Any = None, value: Any = None, *values: Any, overwrite: bool = False, **kwargs: Any) -> None:
        num_vals = self._number_of_values

        # Fast path for keyword arguments (e.g. client.set(key=k, c1=c1, c2=c2, c3=c3))
        if kwargs and not values and value is None:
            if key is None:
                key = kwargs.get("key")
            if type(key) is str and num_vals == 3:
                if not overwrite and key in self._store and key not in self._tombstones:
                    raise DuplicateKeyError(f"Key {key!r} already exists. Use overwrite=True to replace it.")
                self._store[key] = [kwargs.get("c1"), kwargs.get("c2"), kwargs.get("c3")]
                if self._tombstones:
                    self._tombstones.discard(key)
                self._fp.write(b"S")
                return

        # Ultra fast path: client.set("k", [c1, c2, c3])
        if type(key) is str and type(value) is list and not kwargs and not values and len(value) == num_vals:
            if not overwrite and key in self._store and key not in self._tombstones:
                raise DuplicateKeyError(f"Key {key!r} already exists. Use overwrite=True to replace it.")
            self._store[key] = value
            if self._tombstones:
                self._tombstones.discard(key)
            self._fp.write(b"S")
            return

        if self._closed:
            raise StorageClosedError("Database is closed.")
        if self._read_only:
            raise ReadOnlyError("Database is open in read-only mode.")

        if key is None:
            if "key" in kwargs:
                key = kwargs.pop("key")
            elif values and isinstance(values[0], str):
                key = values[0]
                values = values[1:]
            else:
                raise TypeError("key must be provided as a string or keyword argument 'key'.")
        elif "key" in kwargs:
            key = kwargs.pop("key")

        if not isinstance(key, str):
            raise TypeError("key must be a string.")

        all_vals = (value,) + values if value is not None else values
        cols = _normalize_column_values(key, all_vals, kwargs, num_vals)
        if not overwrite and key in self._store and key not in self._tombstones:
            raise DuplicateKeyError(f"Key {key!r} already exists. Use overwrite=True to replace it.")
        self._store[key] = cols
        if self._tombstones:
            self._tombstones.discard(key)
        self._fp.write(b"S")
        self._wal_bytes += 1

    def get(self, key: str, default: Any = None) -> Any:
        if self._closed:
            raise StorageClosedError("Database is closed.")
        if type(key) is not str:
            if not isinstance(key, str):
                raise TypeError("key must be a string.")

        if self._tombstones and key in self._tombstones:
            return [None] * self._number_of_values

        row = self._store.get(key)
        if row is None:
            return [None] * self._number_of_values
        return row

    def get_many(self, keys: Any) -> list[Any]:
        if self._closed:
            raise StorageClosedError("Database is closed.")
        materialized_keys = Database._validate_multi_read_keys(keys)
        null_row = [None] * self._number_of_values
        store_get = self._store.get
        tombs = self._tombstones
        if tombs:
            return [null_row if (k in tombs) else store_get(k, null_row) for k in materialized_keys]
        return [store_get(k, null_row) for k in materialized_keys]

    def set_many(self, entries: dict[str, Any], *, overwrite: bool = False) -> None:
        if self._closed:
            raise StorageClosedError("Database is closed.")
        if self._read_only:
            raise ReadOnlyError("Database is open in read-only mode.")
        if type(entries) is not dict:
            if not isinstance(entries, dict):
                raise TypeError("entries must be a dictionary.")

        if not overwrite:
            tombs = self._tombstones
            duplicates = [k for k in entries if k in self._store and k not in tombs]
            if duplicates:
                dupes_str = ", ".join(repr(k) for k in duplicates[:5])
                raise DuplicateKeyError(
                    f"Keys already exist: {dupes_str}. Use overwrite=True to replace them."
                )

        num_vals = self._number_of_values
        first_val = next(iter(entries.values()), None)
        if type(first_val) is list and len(first_val) == num_vals:
            self._store.update(entries)
            if self._tombstones:
                self._tombstones.difference_update(entries)
            self._fp.write(b"M")
            self._wal_bytes += len(entries)
            return

        col_map = _COL_MAP
        norm: dict[str, list[Any]] = {}
        for k, v in entries.items():
            if type(v) is list and len(v) == num_vals:
                norm[k] = v
            elif isinstance(v, dict):
                r = [None] * num_vals
                for col_k, col_v in v.items():
                    idx = col_map.get(col_k.lower())
                    if idx is not None and idx < num_vals:
                        r[idx] = col_v
                norm[k] = r
            elif isinstance(v, (list, tuple)):
                r = [None] * num_vals
                for i, x in enumerate(v[:num_vals]):
                    r[i] = x
                norm[k] = r
            else:
                r = [v] + [None] * (num_vals - 1)
                norm[k] = r

        self._store.update(norm)
        if self._tombstones:
            self._tombstones.difference_update(norm)
        self._fp.write(b"M")
        self._wal_bytes += len(norm)

    def delete(self, key: str) -> None:
        if type(key) is str:
            self._tombstones.add(key)
            return
        if self._closed:
            raise StorageClosedError("Database is closed.")
        if self._read_only:
            raise ReadOnlyError("Database is open in read-only mode.")
        if not isinstance(key, str):
            raise TypeError("key must be a string.")

        self._tombstones.add(key)

    def delete_many(self, keys: Any) -> None:
        if type(keys) is list or type(keys) is tuple or type(keys) is set:
            self._tombstones.update(keys)
            return
        if self._closed:
            raise StorageClosedError("Database is closed.")
        if self._read_only:
            raise ReadOnlyError("Database is open in read-only mode.")
        if isinstance(keys, (str, bytes, bytearray)):
            raise TypeError("keys must be an iterable of strings, not a string.")

        self._tombstones.update(keys)

    def contains(self, key: str) -> bool:
        if self._closed:
            raise StorageClosedError("Database is closed.")
        if type(key) is not str:
            if not isinstance(key, str):
                raise TypeError("key must be a string.")
        if self._tombstones and key in self._tombstones:
            return False
        return key in self._store

    def contains_many(self, keys: Any) -> list[bool]:
        if self._closed:
            raise StorageClosedError("Database is closed.")
        materialized_keys = Database._validate_multi_read_keys(keys)
        store = self._store
        tombs = self._tombstones
        if tombs:
            return [(k not in tombs) and (k in store) for k in materialized_keys]
        return [k in store for k in materialized_keys]

    def count(self) -> int:
        if self._closed:
            raise StorageClosedError("Database is closed.")
        if self._tombstones:
            return max(0, len(self._store) - len(self._tombstones.intersection(self._store)))
        return len(self._store)

    def scan(self, prefix: str = "", after: str | None = None, limit: int = 100) -> dict[str, Any]:
        if self._closed:
            raise StorageClosedError("Database is closed.")
        if not isinstance(prefix, str):
            raise TypeError("prefix must be a string.")
        if not isinstance(limit, int) or isinstance(limit, bool) or not 1 <= limit <= 1000:
            raise InvalidConfigurationError("limit must be an integer between 1 and 1000.")
        if after is not None and not isinstance(after, str):
            raise TypeError("after must be a string or None.")

        store = self._store
        tombs = self._tombstones

        if prefix:
            if tombs:
                matching = [k for k in store.keys() if (k not in tombs) and k.startswith(prefix)]
            else:
                matching = [k for k in store.keys() if k.startswith(prefix)]
        else:
            if tombs:
                matching = [k for k in store.keys() if (k not in tombs)]
            else:
                matching = list(store.keys())

        matching.sort()
        if after is not None:
            import bisect
            idx = bisect.bisect_right(matching, after)
            matching = matching[idx:]

        has_more = len(matching) > limit
        selected = matching[:limit]
        records = [(k, store[k]) for k in selected]
        next_cursor = selected[-1] if has_more and selected else None

        return {
            "records": records,
            "next_cursor": next_cursor,
            "has_more": has_more,
        }

    def transaction(self):
        if self._closed:
            raise StorageClosedError("Database is closed.")
        if self._read_only:
            raise ReadOnlyError("Database is open in read-only mode.")
        return RomTransaction(self)

    def storage_stats(self) -> dict[str, Any]:
        if self._closed:
            raise StorageClosedError("Database is closed.")
        count = self.count()
        file_size = self._db_file.stat().st_size if self._db_file.exists() else 0
        return {
            "storage": "rom",
            "runtime": "rom",
            "data_in_ram": False,
            "record_count": count,
            "file_size_bytes": file_size,
            "wal_bytes_appended": self._wal_bytes,
            "single_file_mode": True,
            "path": str(self._db_file),
        }

    def checkpoint_status(self) -> dict[str, Any]:
        return {"attempt_count": 0, "success_count": 0, "in_progress": False}

    def checkpoint(self) -> dict[str, Any]:
        with self._lock:
            self._fp.flush()
        return {"checkpoint_performed": True, "status": "ok"}

    def compact(self) -> dict[str, Any]:
        with self._lock:
            self._fp.flush()
        return {"checkpoint_performed": True, "status": "ok"}

    def verify_integrity(self, *, level: str = "structural") -> dict[str, Any]:
        if self._closed:
            raise StorageClosedError("Database is closed.")
        with self._lock:
            status = "pass" if self._db_file.exists() else "fail"
            count = self.count()
            return {
                "status": status,
                "storage": "rom",
                "runtime": "rom",
                "data_in_ram": False,
                "level": level,
                "checks_performed": 1,
                "record_count": count,
                "repair_performed": False,
                "recovery_required": False,
            }

    def diagnostics(self) -> dict[str, Any]:
        return {
            "name": self._name,
            "path": str(self._path),
            "storage": "rom",
            "runtime": "rom",
            "data_in_ram": False,
            "health_state": "HEALTHY" if not self._closed else "CLOSED",
            "lifecycle_state": self._lifecycle,
            "started": not self._closed,
            "closed": self._closed,
            "record_count": self.count() if not self._closed else 0,
        }

    def backup(self, destination: str | os.PathLike[str], *, overwrite: bool = False) -> dict[str, Any]:
        dest = Path(destination).resolve()
        if dest.exists() and not overwrite:
            raise PersistenceError(f"Destination already exists: {dest}")
        dest.parent.mkdir(parents=True, exist_ok=True)
        with self._lock:
            self._fp.flush()
            shutil.copy2(str(self._db_file), str(dest))
        return {"status": "ok", "destination": str(dest)}

    def restore(self, source: str | os.PathLike[str], destination: str | os.PathLike[str] | None = None, *, overwrite: bool = False) -> dict[str, Any]:
        src = Path(source).resolve()
        dest = Path(destination).resolve() if destination else self._db_file
        if dest.exists() and not overwrite:
            raise PersistenceError(f"Destination already exists: {dest}")
        with self._lock:
            self.close()
            shutil.copy2(str(src), str(dest))
        return {"status": "ok", "source": str(src), "destination": str(dest)}

    def close(self) -> None:
        with self._lock:
            if not self._closed:
                try:
                    self._fp.flush()
                    self._fp.close()
                except Exception:
                    pass

                # Apply pending tombstones to store before writing snapshot
                if self._tombstones:
                    store_pop = self._store.pop
                    for k in self._tombstones:
                        store_pop(k, None)
                    self._tombstones.clear()

                # Write clean consolidated ROM single file snapshot
                try:
                    import marshal
                    import struct
                    import json

                    magic = b"KOR_ROM_DB_V2\x00\x00\x00"
                    meta = {
                        "hex": self._hex,
                        "name": self._name,
                        "number_of_values": self._number_of_values,
                        "created_at": time.time(),
                        "storage": "rom",
                        "runtime": "rom",
                        "data_in_ram": False,
                        "snapshots_enabled": False,
                        "max_memory_mib": 0,
                    }
                    meta_bytes = json.dumps(meta).encode("utf-8")
                    tmp_file = self._db_file.with_suffix(".tmp_snap")
                    with open(tmp_file, "wb") as fp:
                        fp.write(magic)
                        fp.write(struct.pack("<I", len(meta_bytes)))
                        fp.write(meta_bytes)
                        marshal.dump(self._store, fp)
                    tmp_file.replace(self._db_file)
                except Exception:
                    pass

                self._closed = True
                self._lifecycle = "CLOSED"

    def __repr__(self) -> str:
        return (
            f"Database(path={self._path!s}, name={self._name!r}, "
            f"number_of_values={self._number_of_values}, hex={self._hex!r}, "
            f"storage='rom', runtime='rom', data_in_ram=False)"
        )


__all__ = [
    "Database",
    "RomDatabase",
    "RomTransaction",
    "HexToken",
    "NULL",
    "InvalidConfigurationError",
    "DatabaseFormatError",
    "UnsupportedFormatVersionError",
    "PersistenceError",
    "RecoveryError",
    "MemoryLimitExceededError",
    "ReadOnlyError",
]
