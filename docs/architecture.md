# Architecture & Internals

How Kor stores, reads, and writes data under the hood.

---

## The Single `.kr` File

Everything Kor needs lives in one file — `<path>/<name>.kr`. There are no sidecar files of any kind.

```
myapp/
└── store.kr      ← your entire database
```

**What's inside:**

```
┌─────────────────────────────────────────┐
│  Magic: "KOR_ROM_DB_V2\x00\x00\x00"    │  16 bytes — identifies a valid Kor file
├─────────────────────────────────────────┤
│  Metadata length  (uint32, LE)          │  4 bytes
├─────────────────────────────────────────┤
│  JSON metadata                          │  variable
│    hex, name, number_of_values,         │
│    created_at, storage, runtime         │
├─────────────────────────────────────────┤
│  Python marshal'd dict                  │  variable — all key→column records
├─────────────────────────────────────────┤
│  WAL marker bytes  (runtime appends)    │  variable — "S" or "M" per write op
└─────────────────────────────────────────┘
```

---

## Layer Diagram

```
┌──────────────────────────┐
│    Python Application    │
└────────────┬─────────────┘
             │
┌────────────▼─────────────┐
│    kor  (Python SDK)      │   Public API: Database, HexToken, db.connect,
│                           │   async_connect, NULL, all exception classes
└────────────┬─────────────┘
             │
      ┌──────┴──────┐
      │             │
┌─────▼─────┐  ┌───▼────────────┐
│ RomEngine │  │ _kor (pybind11)│   Two engine modes — same .kr file target
│ (Python)  │  │ kor_engine C++ │
└─────┬─────┘  └───┬────────────┘
      │             │
┌─────▼─────────────▼──────┐
│   <name>.kr  on disk      │   Single ROM file — both engines read/write here
└───────────────────────────┘
```

---

## ROM Engine (Default)

When you call `Database(path, name, number_of_values)`, the **ROM engine** is activated.

### How it works

**On `Database()` (init):**
1. Generates a 128-bit hex token via `secrets.token_hex(16)`
2. Writes the `.kr` file: magic + JSON metadata + empty marshal'd dict
3. Returns a `HexToken` carrying the path, name, and column count

**On `db.connect()` (open):**
1. Opens the `.kr` file, reads and validates the magic header
2. Parses the JSON metadata, verifies the hex token matches
3. Loads the marshal'd dict into `self._store` (a Python `dict` in RAM)
4. Opens the file in **append mode** with a 16 MiB write buffer (for WAL)

**On `client.set()` (write):**
1. Writes the column list into `self._store[key]` (RAM dict — instant)
2. Appends a single byte `b"S"` to the open file handle (WAL marker)
3. Returns — no disk flush required per individual write

**On `client.delete()` (soft delete):**
1. Adds the key to `self._tombstones` (a Python `set` in RAM)
2. No immediate disk write

**On `client.get()` (read):**
1. Checks `self._tombstones` — if hit, returns `[None, None, ...]`
2. Looks up `self._store.get(key)` — if hit, returns the row
3. If not found, returns `[None] * number_of_values`

**On `client.close()` (persist):**
1. Flushes and closes the append file handle
2. Applies all tombstones: removes tombstoned keys from `self._store`
3. Writes a **clean consolidated snapshot** to `<name>.tmp_snap`:
   - magic header
   - updated JSON metadata
   - marshal'd dict of all live records
4. Atomically replaces `<name>.kr` with the temp file via `Path.replace()`

This means the `.kr` file is always consistent: if the process crashes mid-write, the original file is untouched.

### Why this design is fast

- **Reads** hit a Python `dict` in RAM — O(1), no disk I/O
- **Writes** update a RAM dict + append one byte — no fsync per write
- **Deletes** are set membership — O(1), no disk I/O
- **Persistence** happens once at close, as a single sequential write

---

## HexToken System

Authentication is built into the database, not bolted on. Every `.kr` file embeds a 128-bit token in its JSON metadata.

```
"hex": "0x4f8a3c2e1b9d7e6f5a4b3c2d1e0f8a9b"
```

`db.connect()` reads this field and compares it against the token you provide (case-insensitive, with or without `0x` prefix). A mismatch raises `AuthenticationError` before any data is accessed.

---

## Transactions (ROM Engine)

`RomTransaction` stages all changes in memory before committing:

```
client.transaction()
   │
   ├─ _staged_sets: dict[str, list]   ← pending writes
   └─ _staged_dels: list[str]         ← pending deletes

on commit():
   _store.update(_staged_sets)        ← apply writes (atomic dict merge)
   _tombstones.update(_staged_dels)   ← apply deletes
   staged cleared, tx marked inactive

on rollback():
   staged cleared, tx marked inactive (nothing touched)
```

Reads within a transaction see staged writes first, then the committed store.

---

## Multi-Column Model

The column count (`number_of_values`) is fixed at creation and stored in the `.kr` metadata. Every value in the store is a Python `list` of exactly that length.

Column addressing accepts three naming conventions:
- **Short:** `c1`, `c2`, `c3`, ..., `c32`
- **Medium:** `col1`, `col2`, ..., `col32`
- **Long:** `column1`, `column2`, ..., `column32`

All resolve to the same zero-indexed column slot.

---

## Native C++ Engine

When you use `Database(path, name, max_memory=512)` instead, the native C++ `kor_engine` takes over. It provides:

- **WAL-based durability** with dual-slot checksummed metadata
- **Crash recovery** — replays committed WAL frames on reopen
- **Secondary indexes** — hash and ordered, over scalar mapping fields
- **Background checkpointing** — dirty-state check every 100 ms
- **Memory budget enforcement** — rejects writes if the configured limit is exceeded

Both engines produce the same `.kr` file format at the header level, allowing the Python SDK to detect which format variant is present and dispatch accordingly.

---

## Concurrency

- **In-process:** One `Database` handle per `.kr` file per process. Opening the same file twice raises `PersistenceError`.
- **Multi-process:** Not supported. There is no cross-process file locking.
- **Thread safety (ROM engine):** An `RLock` guards close/checkpoint/compact. The GIL protects individual dict operations.
- **Thread safety (native engine):** Multi-level locking: writer gate → storage mutex → transaction mutex → lifecycle mutex.

---

## File Format Versioning

| Field | Value |
|---|---|
| Magic bytes | `KOR_ROM_DB_V2\x00\x00\x00` |
| Metadata encoding | JSON (UTF-8) |
| Data encoding | Python `marshal` |
| Endianness | Little-endian for binary fields |

The `V2` in the magic distinguishes the current ROM format from earlier SQLite-backed prototypes. The `_kor` native engine uses its own versioned WAL format within the same `.kr` envelope.

