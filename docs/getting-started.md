# Getting Started

This guide walks you through installing Kor, creating your first database, connecting to it, and performing basic operations.

---

## Installation

Install from PyPI:

```bash
pip install kor-db
```

**Requirements:** Python 3.10 – 3.14. Pre-built wheels are available for Windows, Linux, and macOS on x86-64. No compiler needed when installing from a wheel.

---

## Your First Database

### Step 1: Initialize

Creating a database takes one line. You specify a **directory**, a **name**, and the **number of columns** each key will hold:

```python
from kor import Database

hex_token = Database("myapp", "store", 3)
print(hex_token)
# '0x4f8a3c2e1b9d7e6f5a4b3c2d1e0f8a9b'
```

This creates a single file `myapp/store.kr` — your entire database. The returned `HexToken` is a 128-bit cryptographic token stored inside that file. You need it to connect.

> **Important:** Save this hex token. Without it, you cannot connect to the database. If you lose it, call `Database("myapp", "store", 3)` again — it will read the stored token from the existing `.kr` file and return it.

### Step 2: Connect

```python
from kor import db

client = db.connect("myapp", "store", hex_token)
```

That's it. `client` is now a live database connection.

### Step 3: Write Data

Each key holds exactly 3 values (because we passed `3` during initialization). You can set them in three ways:

```python
# List format
client.set("user:1", ["Alice", 30, "Engineering"])

# Positional arguments
client.set("user:2", "Bob", 25, "Design")

# Named columns (c1, c2, c3)
client.set(key="user:3", c1="Charlie", c2=35, c3="Marketing")
```

### Step 4: Read Data

```python
print(client.get("user:1"))
# ['Alice', 30, 'Engineering']

print(client.get("user:2"))
# ['Bob', 25, 'Design']
```

**Missing keys return null-filled lists**, not `None` or exceptions:

```python
print(client.get("nonexistent"))
# [None, None, None]
```

### Step 5: Delete Data

```python
client.delete("user:3")
print(client.contains("user:3"))  # False
print(client.count())             # 2
```

### Step 6: Close

Always close your database when done. This persists all data back to the `.kr` file:

```python
client.close()
```

---

## Bulk Operations

All bulk writes are **atomic** — they either fully succeed or fully roll back.

```python
# Write many at once
client.set_many({
    "user:10": ["Alice", 30, "Engineering"],
    "user:11": ["Bob", 25, "Design"],
    "user:12": ["Charlie", 35, "Marketing"],
})

# Read many at once (up to 10,000 keys)
results = client.get_many(["user:10", "user:11", "user:12"])
# [['Alice', 30, 'Engineering'], ['Bob', 25, 'Design'], ['Charlie', 35, 'Marketing']]

# Delete many at once
client.delete_many(["user:10", "user:11"])

# Check many at once
flags = client.contains_many(["user:10", "user:11", "user:12"])
# [False, False, True]
```

---

## Transactions

Group multiple operations into an atomic transaction:

```python
with client.transaction() as tx:
    tx.set("user:20", ["New User", 22, "Intern"])
    tx.set(key="user:21", c1="Another", c2=23, c3="Intern")
    tx.delete("user:12")
    # Everything commits when the block exits normally
    # Everything rolls back if an exception is raised
```

You can also commit or roll back explicitly:

```python
tx = client.transaction()
tx.set("user:30", ["Explicit", 30, "Commit"])
tx.commit()   # or tx.rollback()
```

---

## Scanning & Pagination

Scan keys by prefix with cursor-based pagination:

```python
cursor = None
while True:
    page = client.scan(prefix="user:", after=cursor, limit=100)

    for key, values in page["records"]:
        print(key, values)

    if not page["has_more"]:
        break
    cursor = page["next_cursor"]
```

The `scan` method returns:
- `records` — list of `(key, values)` tuples, sorted by key
- `next_cursor` — pass as `after` to get the next page
- `has_more` — `True` if more pages exist

---

## Async API

Kor has native `asyncio` support:

```python
import kor

async with await kor.async_connect(path="myapp", name="store") as adb:
    await adb.set("user:1", ["Alice", 30, "Engineering"])
    print(await adb.get("user:1"))

    async with adb.transaction() as tx:
        await tx.set("user:2", ["Bob", 25, "Design"])
```

All synchronous methods have async equivalents (`await adb.get()`, `await adb.set_many()`, etc.).

---

## Backup & Restore

```python
# Backup
client.backup("backup/store.kr")

# Restore
import kor
kor.restore("backup/store.kr", "myapp/store.kr", overwrite=True)

# Export to JSONL
client.export_data("data.jsonl", format="jsonl")

# Import from JSONL
client.import_data("data.jsonl", conflict="overwrite", atomic=True)
```

---

## Read-Only Mode

Open a database without allowing any writes:

```python
client = db.connect("myapp", "store", hex_token, read_only=True)
client.get("user:1")          # Works
client.set("user:1", [...])   # Raises ReadOnlyError
```

---

## Key Rules to Remember

1. **One file** — everything is in `<path>/<name>.kr`
2. **HexToken required** — save the token returned by `Database()`
3. **Values are always lists** — `get()` returns `[c1, c2, c3]`
4. **Missing keys return null lists** — `[None, None, None]`, not exceptions
5. **No subscript access** — `client["key"]` raises `TypeError`; use `client.get("key")`
6. **Always close** — `client.close()` persists everything to disk

