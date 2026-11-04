# API Reference

Complete reference for every method in the Kor database SDK.

---

## Database Initialization

### `Database(path, name, number_of_values)`

Creates a new database or opens an existing one. Returns a `HexToken`.

```python
from kor import Database

hex_token = Database("data", "users", 3)
```

| Parameter | Type | Description |
|---|---|---|
| `path` | `str` | Directory where the `.kr` file will be stored |
| `name` | `str` | Database name — file will be `<path>/<name>.kr` |
| `number_of_values` | `int` | Number of columns each key holds (positive integer) |

**Returns:** `HexToken` — a 128-bit hex string (e.g. `'0x4f8a3c...'`)

**Behavior:**
- If the `.kr` file does not exist → creates it, generates a new hex token, returns it
- If the `.kr` file already exists → reads the stored hex token and returns it
- The hex token is stored in the file metadata and validated on every connection

**Raises:** `InvalidConfigurationError` if path, name, or number_of_values is invalid.

**Name rules:** ASCII-only, non-empty, no path separators or dots, no whitespace, must start with a letter or digit.

---

## Connection

### `db.connect(path, name, hex, *, read_only=False)`

Connect to an existing database.

```python
from kor import db

client = db.connect("data", "users", hex_token)
```

| Parameter | Type | Description |
|---|---|---|
| `path` | `str` | Directory containing the `.kr` file |
| `name` | `str` | Database name |
| `hex` | `str` | Hex token (with or without `0x` prefix, case-insensitive) |
| `read_only` | `bool` | If `True`, rejects all write operations |

**Returns:** `Database` instance (specifically `RomDatabase`)

**Raises:**
- `PersistenceError` if the `.kr` file does not exist
- `AuthenticationError` if the hex token does not match
- `InvalidConfigurationError` if name mismatches the stored name

---

## CRUD Operations

### `client.set(key, *values, *, overwrite=False, **kwargs)`

Set column values for a key. By default, raises `DuplicateKeyError` if the key already exists. Pass `overwrite=True` to replace an existing record.

```python
# Three equivalent calling conventions (all raise DuplicateKeyError if key exists):
client.set("user:1", ["Alice", 30, "Engineering"])       # list
client.set("user:1", "Alice", 30, "Engineering")         # positional
client.set(key="user:1", c1="Alice", c2=30, c3="Eng")   # named columns

# Explicitly replace an existing record:
client.set("user:1", ["Alice", 31, "Engineering"], overwrite=True)
```

| Parameter | Type | Description |
|---|---|---|
| `key` | `str` | The record key |
| `*values` | any | Column values as a list, positional args, or named kwargs |
| `overwrite` | `bool` | If `False` (default), raises `DuplicateKeyError` on existing key |
| `c1`, `c2`, … `c32` | any | Named columns (`c1`/`col1`/`column1` all accepted) |

**Named column aliases:** `c1`=`col1`=`column1`, `c2`=`col2`=`column2`, etc. Up to 32 columns.

**Raises:** `DuplicateKeyError` if key already exists and `overwrite=False`, `TypeError` if key is not a string, `ValueError` if too many values provided, `ReadOnlyError` in read-only mode, `StorageClosedError` if database is closed.

---

### `client.get(key, default=None)`

Get all column values for a key.

```python
result = client.get("user:1")      # ['Alice', 30, 'Engineering']
result = client.get("missing")     # [None, None, None]
```

| Parameter | Type | Description |
|---|---|---|
| `key` | `str` | The record key |
| `default` | any | Default value (only used if `number_of_values` is not set) |

**Returns:** `list` of column values. If the key does not exist, returns `[None, None, ..., None]` with length equal to `number_of_values`.

---

### `client.delete(key)`

Delete a key.

```python
client.delete("user:1")
```

---

### `client.contains(key)`

Check if a key exists.

```python
client.contains("user:1")   # True or False
```

**Returns:** `bool`

---

### `client.count()`

Get the total number of live records.

```python
n = client.count()   # 42
```

**Returns:** `int`

---

## Bulk Operations

All bulk write methods are **atomic** — either all operations succeed or none are applied.

### `client.set_many(entries, *, overwrite=False)`

Atomically insert multiple records. By default, raises `DuplicateKeyError` listing all keys that already exist. Pass `overwrite=True` to allow replacing existing records.

```python
# Insert new keys (raises DuplicateKeyError if any key already exists):
client.set_many({
    "user:1": ["Alice", 30, "Engineering"],
    "user:2": ["Bob", 25, "Design"],
})

# Replace existing keys explicitly:
client.set_many({
    "user:1": ["Alice", 31, "Engineering"],
}, overwrite=True)
```

| Parameter | Type | Description |
|---|---|---|
| `entries` | `dict[str, list]` | Keys mapped to column value lists |
| `overwrite` | `bool` | If `False` (default), raises `DuplicateKeyError` for any existing key |

**Raises:** `DuplicateKeyError` if any key already exists and `overwrite=False` (the entire batch is rejected atomically).

---

### `client.get_many(keys)`

Read multiple keys at once.

```python
results = client.get_many(["user:1", "user:2"])
# [['Alice', 30, 'Engineering'], ['Bob', 25, 'Design']]
```

| Parameter | Type | Description |
|---|---|---|
| `keys` | `list[str]` | Up to 10,000 keys |

**Returns:** `list[list]` — values in the same order as input keys. Missing keys return `[None, None, ...]`.

---

### `client.delete_many(keys)`

Atomically delete multiple keys.

```python
client.delete_many(["user:1", "user:2", "user:3"])
```

---

### `client.contains_many(keys)`

Check existence of multiple keys.

```python
flags = client.contains_many(["user:1", "user:2"])
# [True, False]
```

**Returns:** `list[bool]` — in the same order as input keys. Up to 10,000 keys.

---

## Transactions

### `client.transaction()`

Begin an atomic transaction. Use as a context manager or with explicit commit/rollback.

**Context manager (recommended):**

```python
with client.transaction() as tx:
    tx.set("k1", ["v1", "v2", "v3"])
    tx.set(key="k2", c1="a", c2="b", c3="c")
    tx.delete("k3")
    val = tx.get("k1")       # reads staged data
    tx.contains("k2")        # True (staged)
    # Commits on normal exit
    # Rolls back on exception
```

**Explicit control:**

```python
tx = client.transaction()
tx.set("k1", ["v1", "v2", "v3"])
tx.commit()    # apply all changes
# or
tx.rollback()  # discard all changes
```

**Transaction methods:**

| Method | Description |
|---|---|
| `tx.set(key, value, **kwargs)` | Stage a write (same calling conventions as `client.set`) |
| `tx.set_many(entries)` | Stage multiple writes |
| `tx.get(key)` | Read from staged writes first, then from database |
| `tx.contains(key)` | Check in staged writes first |
| `tx.delete(key)` | Stage a deletion |
| `tx.delete_many(keys)` | Stage multiple deletions |
| `tx.commit()` | Apply all staged changes atomically |
| `tx.rollback()` | Discard all staged changes |

**Rules:**
- Empty transactions commit as no-ops
- Repeated rollback is harmless
- Commit/rollback after either raises `TransactionStateError`
- Operations after commit/rollback raise `TransactionStateError`

---

## Scan & Pagination

### `client.scan(prefix="", *, after=None, limit=100)`

Return one key-ordered page of records matching a prefix.

```python
page = client.scan(prefix="user:", after=None, limit=100)
```

| Parameter | Type | Default | Description |
|---|---|---|---|
| `prefix` | `str` | `""` | Key prefix filter (empty = all keys) |
| `after` | `str \| None` | `None` | Exclusive cursor — start after this key |
| `limit` | `int` | `100` | Max records per page (1–1000) |

**Returns:** `dict` with:
- `records` — `list[tuple[str, list]]` of `(key, values)` pairs, sorted by key
- `next_cursor` — `str | None` — pass as `after` for the next page
- `has_more` — `bool` — whether more pages exist

---

## Diagnostics & Maintenance

### `client.storage_stats()`

Returns storage statistics as a dict: `record_count`, `file_size_bytes`, `wal_bytes_appended`, etc.

### `client.diagnostics()`

Returns runtime diagnostics: `health_state`, `lifecycle_state`, `record_count`, etc.

### `client.verify_integrity(*, level="structural")`

Check database integrity. `level` can be `"structural"` (fast) or `"full"` (scans all records).

### `client.backup(destination, *, overwrite=False)`

Copy the `.kr` file to `destination`. Atomic: if the copy fails, the destination is unchanged.

### `client.export_data(destination, *, format="jsonl", overwrite=False)`

Export all records to a JSONL file.

### `client.import_data(source, *, conflict="error", atomic=True)`

Import records from a JSONL file.

| `conflict` value | Behavior |
|---|---|
| `"error"` | Raise on existing key |
| `"overwrite"` | Replace existing key |
| `"skip"` | Keep existing key |

### `client.close()`

Flush all data to disk and close the database. Writes a consolidated snapshot to the `.kr` file via atomic temp-file replacement. Always call this when done.

---

## Exceptions

All importable from `kor`:

| Exception | When raised |
|---|---|
| `DuplicateKeyError` | `set()` or `set_many()` called on an existing key with `overwrite=False` |
| `AuthenticationError` | Hex token does not match the stored token |
| `InvalidConfigurationError` | Invalid path, name, or configuration |
| `PersistenceError` | Storage / I/O failure |
| `RecoveryError` | WAL recovery failure |
| `StorageClosedError` | Operation on a closed database |
| `ReadOnlyError` | Write attempted on a read-only connection |
| `TransactionStateError` | Invalid transaction state (e.g. already committed) |
| `TransactionCommitError` | Commit failure |
| `MemoryLimitExceededError` | Memory budget exceeded |
| `InvalidKeyError` | Bad key format |
| `InvalidValueError` | Bad value / column count mismatch |
| `SerializationError` | Serialization failure |

---

## HexToken Properties

The `HexToken` returned by `Database()` is a `str` subclass with extra properties:

| Property / Method | Type | Description |
|---|---|---|
| `hex_token.path` | `Path` | Directory of the `.kr` file |
| `hex_token.name` | `str` | Database name |
| `hex_token.number_of_values` | `int` | Column count |
| `hex_token.hex` | `str` | The hex string |

---

## Constants

### `NULL`

```python
from kor import NULL   # NULL = None
client.set(key="user:1", c1="Alice", c2=NULL, c3="Engineering")
```

Provided for readability. `NULL` is exactly `None`.

