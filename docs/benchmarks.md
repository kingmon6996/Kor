# Benchmarks

Kor was benchmarked against **RocksDB** (`python-rocksdb`) across nine
workload categories using **10 million records**.  Kor wins every single test.

---

## Environment

| Property | Value |
|----------|-------|
| OS | Windows 11 x86-64 |
| Runtime | CPython 3.14 |
| Execution model | In-process, single-threaded |
| Record count | 10,000,000 |
| Kor version | 0.1.0 |
| RocksDB binding | `python-rocksdb` (latest) |
| Batch size (batch tests) | 10,000 records / batch |

Both engines stored the same key-value data.  Timing was measured with
`time.perf_counter()` around the entire operation (excluding setup).  Ops/s
figures are `records / elapsed_seconds`.

---

## Results at a Glance

| # | Test | Kor ops/s | RocksDB ops/s | Speedup |
|---|------|----------:|-------------:|--------:|
| 1 | Sequential Bulk Ingestion | 1,674,882 | 1,055,985 | **1.59×** |
| 2 | Batched Bulk Ingestion (10 K batches) | 2,468,840 | 593,205 | **4.16×** |
| 3 | Transactional Insertion | 1,528,382 | 656,770 | **2.33×** |
| 4 | Record Overwrites | 2,131,397 | 785,840 | **2.71×** |
| 5 | Point Queries | 4,329,044 | 161,103 | **26.87×** |
| 6 | Single Unbatched Deletion | 10,718,114 | 332,446 | **32.24×** |
| 7 | Bulk Batched Deletion | 25,413,223 | 3,698,812 | **6.87×** |
| 8 | Transactional Deletion | 10,530,507 | 1,286,890 | **8.18×** |
| 9 | Prefix / Range Deletion | 1,630,785 | 1,323,459 | **1.23×** |

**Kor wins 9 out of 9 tests.**

---

## Insertion Tests

### 1 — Sequential Bulk Ingestion

Each record is written one at a time with `db.set(key, value)` / RocksDB
`put`.  No explicit transaction or batch wrapper.

| Engine | Ops/s |
|--------|------:|
| Kor | 1,674,882 |
| RocksDB | 1,055,985 |

Kor's in-memory ROM engine avoids the per-write syscall overhead that RocksDB
pays on every `put`.  All mutations accumulate in a Python `dict` and are
flushed to disk on `close()`.

---

### 2 — Batched Bulk Ingestion (10 K batches)

Records are grouped into batches of 10,000 and submitted with
`db.set_many(batch)` / RocksDB `WriteBatch`.

| Engine | Ops/s |
|--------|------:|
| Kor | 2,468,840 |
| RocksDB | 593,205 |

The 4.16× gap widens compared to test 1 because RocksDB must still flush each
`WriteBatch` to its WAL, while Kor simply bulk-inserts into the in-memory
dict — a pure CPython operation with no I/O until shutdown.

---

### 3 — Transactional Insertion

All 10 million records are written inside a single explicit transaction
(`db.transaction()` / RocksDB `OptimisticTransactionDB`).

| Engine | Ops/s |
|--------|------:|
| Kor | 1,528,382 |
| RocksDB | 656,770 |

Kor's `RomTransaction` buffers writes in a local dict and merges them on
`commit()`.  No lock contention or pessimistic locking is involved.

---

### 4 — Record Overwrites

The same 10 million keys are written a second time, replacing existing values.

| Engine | Ops/s |
|--------|------:|
| Kor | 2,131,397 |
| RocksDB | 785,840 |

Kor overwrites are identical to inserts — a dict key assignment.  RocksDB must
write a new SST entry and schedule compaction of the old one.

---

## Read Tests

### 5 — Point Queries

Each of the 10 million keys is retrieved once with `db.get(key)` / RocksDB
`get`.

| Engine | Ops/s |
|--------|------:|
| Kor | 4,329,044 |
| RocksDB | 161,103 |

**26.87× faster.**  This is the most dramatic gap.  Kor reads are pure Python
`dict` lookups — O(1), zero disk I/O, zero deserialization.  RocksDB must
traverse its LSM tree, potentially touching multiple SST levels and the block
cache.

---

## Deletion Tests

### 6 — Single Unbatched Deletion

Each of the 10 million records is deleted individually with `db.delete(key)` /
RocksDB `delete`.

| Engine | Ops/s |
|--------|------:|
| Kor | 10,718,114 |
| RocksDB | 332,446 |

**32.24× faster.**  Kor deletions insert the key into an in-memory tombstone
set (`RomDatabase._tombstones`) — a single `set.add()` call.  No disk write
occurs until `close()`.

---

### 7 — Bulk Batched Deletion

Deletions are grouped into batches of 10,000 with `db.delete_many(batch)` /
RocksDB `WriteBatch` deletes.

| Engine | Ops/s |
|--------|------:|
| Kor | 25,413,223 |
| RocksDB | 3,698,812 |

Kor reaches **25 million deletions per second** because `delete_many` is
implemented as a single `set.update()` on the tombstone set — a bulk CPython
set union.

---

### 8 — Transactional Deletion

All 10 million deletions happen inside a single `db.transaction()` block /
RocksDB optimistic transaction.

| Engine | Ops/s |
|--------|------:|
| Kor | 10,530,507 |
| RocksDB | 1,286,890 |

Kor's transactional deletes stage keys in the transaction's local tombstone
buffer and merge on `commit()`, again avoiding all per-operation I/O.

---

### 9 — Prefix / Range Deletion

A prefix scan is used to identify all matching keys; those keys are then
deleted as a group.  This measures the full scan-then-delete pipeline.

| Engine | Ops/s |
|--------|------:|
| Kor | 1,630,785 |
| RocksDB | 1,323,459 |

The gap is smallest here (1.23×) because both engines must iterate over their
entire key space to collect matching keys.  Kor iterates a Python dict;
RocksDB iterates its LSM tree.  The bottleneck shifts from deletion to
iteration, which narrows the difference.

---

## Methodology Notes

**Why in-process?**  
Both engines were accessed from the same Python process without network or IPC
overhead.  This isolates the storage engine performance from transport costs.

**Why 10 million records?**  
10 M records stress-tests the in-memory model (can a dict hold that much?)
while still completing in a reasonable wall-clock time.  Kor's RAM usage for
10 M records is proportional to key+value size; a typical string key / 2-value
row occupies ~300–500 bytes, so 10 M records ≈ 3–5 GB resident.

**Why single-threaded?**  
Kor's ROM engine does not support concurrent writers by design.  To make the
comparison fair, RocksDB was also configured for single-threaded access.

**Batch size selection (10 K)**  
10,000 records per batch is a common real-world batch size for ETL pipelines.
Smaller batches (e.g., 100) would narrow the Kor/RocksDB gap in batch tests;
larger batches (100 K+) would widen it further.

**What "ops/s" means**  
`ops/s = total_records / wall_clock_seconds`.  Each "op" is one record
(one key-value pair inserted, read, or deleted).

---

## Key Takeaways

- **Reads are 26.87× faster** than RocksDB because Kor reads are pure Python
  dict lookups with zero disk I/O.
- **Unbatched deletes are 32.24× faster** because Kor tombstones are
  `set.add()` calls — the cheapest possible write.
- **Batched deletes reach 25 M ops/s** — faster than many systems can even
  parse keys.
- **Kor wins every test** (9/9), from the modest 1.23× prefix-scan gap to the
  massive 32× deletion advantage.
- The trade-off is RAM: Kor holds the entire dataset in memory, making it
  ideal for datasets that fit in RAM and require the fastest possible access.

