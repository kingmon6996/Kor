<p align="center">
  <h1 align="center">Kor</h1>
  <p align="center">
    <strong>A native, embedded key-value database — single-file, zero dependencies, blazing fast.</strong>
  </p>
  <p align="center">
    <a href="https://pypi.org/project/kor-db/"><img src="https://img.shields.io/pypi/v/kor-db?color=blue&label=PyPI" alt="PyPI"></a>
    <a href="https://pypi.org/project/kor-db/"><img src="https://img.shields.io/pypi/pyversions/kor-db?label=Python" alt="Python"></a>
    <a href="LICENSE"><img src="https://img.shields.io/badge/License-Apache%202.0-green.svg" alt="License"></a>
    <a href="https://pypi.org/project/kor-db/"><img src="https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey" alt="Platform"></a>
  </p>
</p>

---

Kor is a high-performance, embedded key-value database written in **C++20** with a first-class **Python SDK**. Everything — metadata, records, write-ahead log — lives in a **single `.kr` file**. No sidecar files. No external dependencies. No server process. Just one file and sub-microsecond operations.

```python
from kor import Database, db

# Create a database → get a secure HexToken
hex_token = Database("data", "users", 3)

# Connect and start working
client = db.connect("data", "users", hex_token)
client.set("user:1", ["Alice", 30, "Engineering"])
print(client.get("user:1"))  # ['Alice', 30, 'Engineering']
client.close()
```

---

## Table of Contents

- [Why Kor?](#why-kor)
- [Installation](#installation)
- [Quick Start](#quick-start)
- [Core Concepts](#core-concepts)
- [API Guide](#api-guide)
- [Benchmarks — Kor vs RocksDB](#benchmarks--kor-vs-rocksdb)
- [Architecture](#architecture)
- [API Reference](#api-reference)
- [Exceptions](#exceptions)
- [Documentation](#documentation)
- [License](#license)

---

## Why Kor?

| Feature | Kor | Typical Embedded DBs |
|---|---|---|
| **Storage** | Single `.kr` file | Multiple files (WAL, meta, SSTs…) |
| **Sidecar files** | **Zero** | Often 3–10+ |
| **Authentication** | Built-in 128-bit HexToken | None / bolted on |
| **Atomic transactions** | ✅ Context manager | Varies |
| **Async support** | ✅ Native asyncio | Rare |
| **Multi-column values** | ✅ First-class | Keys → single blob |
| **Point-read speed** | **4.3 M ops/s** | ~161 K ops/s (RocksDB) |
| **Deletion speed** | **10.7 M ops/s** | ~332 K ops/s (RocksDB) |

> **Kor won 9 out of 9 benchmark tests against RocksDB at 10 million records.** See [Benchmarks](#benchmarks--kor-vs-rocksdb).

---

## Installation

```bash
pip install kor-db
```

**Requires Python 3.10 – 3.14.** Pre-built wheels available for Windows, Linux (manylinux 2.28), and macOS (10.15+) on x86-64.

---

## Quick Start

```python
from kor import Database, db

# 1. Initialize — creates "myapp/store.kr" and returns a hex token
hex_token = Database("myapp", "store", 3)   # 3 columns per key

# 2. Connect
client = db.connect("myapp", "store", hex_token)

# 3. Write records
client.set("user:1", ["Alice", 30, "Engineering"])
client.set("user:2", ["Bob", 25, "Design"])
client.set(key="user:3", c1="Charlie", c2=35, c3="Marketing")

# 4. Read records
print(client.get("user:1"))   # ['Alice', 30, 'Engineering']
print(client.get("unknown"))  # [None, None, None]  ← base null

# 5. Bulk operations (atomic)
client.set_many({
    "user:4": ["Diana", 28, "Sales"],
    "user:5": ["Eve", 32, "Security"],
})
results = client.get_many(["user:4", "user:5"])

# 6. Transactions
with client.transaction() as tx:
    tx.set("user:6", ["Frank", 40, "Legal"])
    tx.delete("user:5")
    # Commits on exit; rolls back on exception

# 7. Scan with pagination
page = client.scan(prefix="user:", limit=100)
for key, values in page["records"]:
    print(key, values)

# 8. Close — persists everything to the .kr file
client.close()
```

---

## Core Concepts

### The `.kr` Single-File Model

Kor stores **everything** in one file:

```
myapp/store.kr    ← Your entire database. One file. That's it.
```

**No `.wal`, `.meta`, `.info`, `.snap`, `.txn`, or any other sidecar files.** The `.kr` file contains the magic header, JSON metadata (hex token, column count, timestamps), and all records.

On open, data is loaded into RAM for sub-microsecond operations. On close, a consolidated snapshot is written atomically back to disk. Backup = copy one file. Migration = move one file.

### HexToken Authentication

Every database is secured with a **128-bit cryptographic hex token**:

```python
hex_token = Database("data", "users", 3)
# Returns: '0x4f8a3c2e1b9d7e6f5a4b3c2d1e0f8a9b'
```

- Generated once at creation, stored in the `.kr` metadata
- Required for every connection — wrong token → `AuthenticationError`
- Re-calling `Database()` on an existing file returns the stored token

### Multi-Column Values

Each key maps to a **fixed-width list** defined at creation:

```python
hex = Database("db", "employees", 3)  # 3 columns
client = db.connect("db", "employees", hex)

client.set("emp:1", ["Alice", 30, "Eng"])     # list format
client.set("emp:2", "Bob", 25, "Design")      # positional args
client.set(key="emp:3", c1="Charlie", c2=35, c3="Mkt")  # named columns

client.get("emp:1")  # ['Alice', 30, 'Eng']
```

### Base Null Behavior

Missing keys return a **null-filled list** — not `None`, not an exception:

```python
client.get("nonexistent")  # [None, None, None]
```

### No Subscript Access

Subscript syntax is **explicitly disabled** with clear error messages:

```python
client["key"]            # TypeError: 'Database' object is not subscriptable.
                         #   Use get() and set() methods instead.
client["key"] = value    # TypeError: 'Database' object does not support item assignment.
                         #   Use get() and set() methods instead.
del client["key"]        # TypeError: 'Database' object does not support item deletion.
                         #   Use get() and set() methods instead.
```

---

## API Guide

### Initialization & Connection

```python
from kor import Database, db

# Initialize (creates .kr file, returns HexToken)
hex_token = Database("path", "name", number_of_values)

# Connect
client = db.connect("path", "name", hex_token)
```

### Read & Write

```python
# Set — three equivalent ways (raises DuplicateKeyError if key exists)
client.set("key", ["v1", "v2", "v3"])                    # list
client.set("key", "v1", "v2", "v3")                      # positional
client.set(key="key", c1="v1", c2="v2", c3="v3")         # named columns
# Explicitly replace an existing record:
client.set("key", ["v1", "v2", "v3"], overwrite=True)

# Get
client.get("key")                 # ['v1', 'v2', 'v3']
client.get("missing")            # [None, None, None]

# Delete & check
client.delete("key")
client.contains("key")           # False
client.count()                   # total records
```

### Bulk Operations

```python
# Raises DuplicateKeyError for any key that already exists
client.set_many({"k1": [1, 2, 3], "k2": [4, 5, 6]})
# To allow overwriting existing keys:
client.set_many({"k1": [1, 2, 3], "k2": [4, 5, 6]}, overwrite=True)
client.get_many(["k1", "k2"])             # [[1,2,3], [4,5,6]]
client.delete_many(["k1", "k2"])
client.contains_many(["k1", "k2"])        # [False, False]
```

### Transactions

```python
# Context manager — auto commit/rollback
with client.transaction() as tx:
    tx.set("k1", [1, 2, 3])
    tx.set(key="k2", c1=4, c2=5, c3=6)
    tx.delete("k3")

# Explicit control
tx = client.transaction()
tx.set("k1", [1, 2, 3])
tx.commit()   # or tx.rollback()
```

### Scan & Pagination

```python
cursor = None
while True:
    page = client.scan(prefix="user:", after=cursor, limit=100)
    for key, values in page["records"]:
        process(key, values)
    if not page["has_more"]:
        break
    cursor = page["next_cursor"]
```

### Backup, Restore & Export

```python
client.backup("backup/store.kr")
kor.restore("backup/store.kr", "data/store.kr", overwrite=True)
client.export_data("data.jsonl", format="jsonl")
client.import_data("data.jsonl", conflict="overwrite", atomic=True)
```

### Async API

```python
import kor

async with await kor.async_connect(path="data", name="store") as adb:
    await adb.set("user:1", ["Alice", 30, "Engineering"])
    print(await adb.get("user:1"))

    async with adb.transaction() as tx:
        await tx.set("user:2", ["Bob", 25, "Design"])
```

### Other

```python
client.storage_stats()                         # storage metrics
client.diagnostics()                           # runtime health
client.verify_integrity(level="structural")    # integrity check
db.connect("path", "name", hex, read_only=True)  # read-only mode
```

---

## Benchmarks — Kor vs RocksDB

**10 million records. Kor wins all 9 tests.**

| # | Test | Records | Kor Ops/s | RocksDB Ops/s | Speedup |
|:---:|------|--------:|-----------:|---------------:|---------:|
| 1 | Sequential Bulk Ingestion | 10,000,000 | 1,674,882 | 1,055,985 | **1.59×** |
| 2 | Batched Bulk Ingestion (10K batches) | 10,000,000 | 2,468,840 | 593,205 | **4.16×** |
| 3 | Transactional Insertion | 50,000 | 1,528,382 | 656,770 | **2.33×** |
| 4 | Record Overwrites (Upserts) | 100,000 | 2,131,397 | 785,840 | **2.71×** |
| 5 | Point Queries (Reads) | 50,000 | 4,329,044 | 161,103 | **26.87×** |
| 6 | Single Unbatched Deletion | 10,000 | 10,718,114 | 332,446 | **32.24×** |
| 7 | Bulk Batched Deletion | 200,000 | 25,413,223 | 3,698,812 | **6.87×** |
| 8 | Transactional Deletion | 50,000 | 10,530,507 | 1,286,890 | **8.18×** |
| 9 | Prefix/Range Deletion | 50,000 | 1,630,785 | 1,323,459 | **1.23×** |

**Highlights:** 26.87× faster reads · 32.24× faster deletes · 4.16× faster batch ingestion

---

## Architecture

```
┌─────────────────────────┐
│   Python Application    │
└────────────┬────────────┘
             │
┌────────────▼────────────┐
│   kor (Python SDK)      │
│   Database · HexToken   │
│   Transactions · Async  │
└────────────┬────────────┘
             │
┌────────────▼────────────┐
│   _kor (pybind11)       │
│   C++20 ↔ Python bridge │
└────────────┬────────────┘
             │
┌────────────▼────────────┐
│   kor_engine (C++20)    │
│   Storage · WAL · Scan  │
│   Serialization         │
└────────────┬────────────┘
             │
┌────────────▼────────────┐
│   <name>.kr (ROM file)  │
│   Single-file database  │
└─────────────────────────┘
```

---

## API Reference

### Lifecycle

| Method | Returns | Description |
|---|---|---|
| `Database(path, name, num_values)` | `HexToken` | Create/open database |
| `db.connect(path, name, hex)` | `Database` | Connect to database |
| `client.close()` | — | Persist & close |

### CRUD

| Method | Returns | Description |
|---|---|---|
| `set(key, value, *, overwrite=False)` | — | Write columns (list / positional / named); raises `DuplicateKeyError` if key exists |
| `get(key)` | `list` | Read columns (`[None,…]` if missing) |
| `delete(key)` | — | Remove a key |
| `contains(key)` | `bool` | Key exists? |
| `count()` | `int` | Total records |

### Bulk (Atomic)

| Method | Returns | Description |
|---|---|---|
| `set_many(dict, *, overwrite=False)` | — | Batch write; raises `DuplicateKeyError` for any existing key |
| `get_many(list)` | `list[list]` | Batch read (≤ 10,000 keys) |
| `delete_many(list)` | — | Batch delete |
| `contains_many(list)` | `list[bool]` | Batch existence check |

### Transactions

| Method | Returns | Description |
|---|---|---|
| `transaction()` | context manager | Begin atomic transaction |
| `tx.set(key, value)` | — | Write in transaction |
| `tx.delete(key)` | — | Delete in transaction |
| `tx.commit()` | — | Commit explicitly |
| `tx.rollback()` | — | Roll back explicitly |

### Query & Maintenance

| Method | Returns | Description |
|---|---|---|
| `scan(prefix, after, limit)` | `dict` | Paginated prefix scan |
| `storage_stats()` | `dict` | Storage metrics |
| `diagnostics()` | `dict` | Runtime health |
| `verify_integrity(level)` | `dict` | Check database integrity |
| `backup(dest)` | `dict` | Physical backup |
| `export_data(dest)` | `dict` | JSONL export |
| `import_data(source)` | `dict` | JSONL import |

---

## Exceptions

All importable from `kor`:

| Exception | When |
|---|---|
| `AuthenticationError` | Hex token mismatch |
| `DuplicateKeyError` | Key already exists and `overwrite=False` |
| `InvalidConfigurationError` | Invalid config |
| `PersistenceError` | Storage / I/O failure |
| `RecoveryError` | WAL recovery failure |
| `StorageClosedError` | Operation on closed database |
| `ReadOnlyError` | Write on read-only connection |
| `TransactionStateError` | Invalid transaction state |
| `TransactionCommitError` | Commit failure |
| `MemoryLimitExceededError` | Memory budget exceeded |
| `InvalidKeyError` | Bad key format |
| `InvalidValueError` | Bad value / column mismatch |
| `SerializationError` | Serialization failure |

## Documentation

| Document | Description |
|---|---|
| [Getting Started](docs/getting-started.md) | Installation, initialization, connection, first CRUD |
| [API Reference](docs/api-reference.md) | Complete method reference with examples |
| [Architecture & Internals](docs/architecture.md) | `.kr` file format, ROM engine, storage model |
| [Benchmarks](docs/benchmarks.md) | Full 10M-record benchmark methodology and results |

---

## License

```
Copyright 2026 Pravanjan Roy (kingmon6996)

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0
```

---

<p align="center"><sub>Kor v0.1.0 · Built with C++20 + pybind11 · Made by <a href="https://github.com/kingmon6996">Pravanjan Roy</a></sub></p>
