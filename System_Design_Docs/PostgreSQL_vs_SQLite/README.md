# PostgreSQL vs SQLite — Architecture Comparison

**Author:** Pranav Nayal | **Roll No:** SCALER_10236

---

## 1. Problem Background

### Why do both exist?

These two databases solve fundamentally different problems — not just at a scale level, but at an *architectural identity* level.

**SQLite** was created in 2000 by D. Richard Hipp for the U.S. Navy to control guided missile destroyers. The key constraint: the system had to work with zero network dependency, no DBA, no server process. The entire database had to be a library that the application linked against. This shaped every decision SQLite made: single file, zero configuration, embedded in the process.

**PostgreSQL** descended from INGRES (Michael Stonebraker, UC Berkeley, 1977) → POSTGRES (1986) → PostgreSQL (1996). The design intent was a general-purpose, enterprise-grade RDBMS supporting multiple concurrent users, complex queries, and high correctness guarantees. It was built for situations where a dedicated server makes sense — shared data accessed by many clients.

The fundamental question each answers differently: *Who manages the database process?*
- SQLite: the application itself
- PostgreSQL: a long-running server daemon

---

## 2. Architecture Overview

### SQLite — Embedded, Serverless

```
┌─────────────────────────────────────────────────────┐
│                  Application Process                 │
│                                                     │
│  ┌──────────────────────────────────────────────┐  │
│  │              SQLite Library                   │  │
│  │  ┌──────────┐  ┌──────────┐  ┌───────────┐  │  │
│  │  │  Parser  │→ │  Query   │→ │  VM/VDBE  │  │  │
│  │  │  (Lemon) │  │ Optimizer│  │ (Bytecode)│  │  │
│  │  └──────────┘  └──────────┘  └─────┬─────┘  │  │
│  │                                     │         │  │
│  │  ┌──────────────────────────────────▼──────┐  │  │
│  │  │           B-Tree Layer                  │  │  │
│  │  └──────────────────────────────────┬──────┘  │  │
│  │                                     │         │  │
│  │  ┌──────────────────────────────────▼──────┐  │  │
│  │  │           Pager (Page Cache)            │  │  │
│  │  └──────────────────────────────────┬──────┘  │  │
│  └────────────────────────────────────┼──────────┘  │
└───────────────────────────────────────┼─────────────┘
                                        ▼
                              ┌─────────────────┐
                              │  Single .db File │
                              └─────────────────┘
```

SQLite compiles SQL into bytecode executed by an internal Virtual Database Engine (VDBE). The pager is the heart of SQLite — it manages the page cache, journaling, and locking. The entire state lives in one file.

### PostgreSQL — Client-Server, Multi-Process

```
┌─────────────┐        TCP/Unix Socket        ┌─────────────────────────────────┐
│   Client    │ ─────────────────────────────▶│         Postmaster              │
│ (psql/app)  │                               │   (Supervisor Process, PID 1)   │
└─────────────┘                               └──────────────┬──────────────────┘
                                                             │ fork()
                               ┌─────────────────────────────▼──────────────────┐
                               │           Backend Process (per connection)      │
                               │  Parser → Rewriter → Planner → Executor        │
                               └───────────────────────┬────────────────────────┘
                                                       │
                  ┌────────────────────────────────────▼────────────────────────┐
                  │                    Shared Memory                             │
                  │  ┌─────────────────┐  ┌──────────────┐  ┌───────────────┐ │
                  │  │  Shared Buffers │  │  WAL Buffers │  │  Lock Tables  │ │
                  │  │  (Buffer Pool)  │  │              │  │               │ │
                  │  └─────────────────┘  └──────────────┘  └───────────────┘ │
                  └──────────────────────────────┬──────────────────────────────┘
                                                 │
              ┌──────────────────────────────────▼──────────────────────────────┐
              │                     Background Processes                         │
              │  WAL Writer │ Checkpointer │ Autovacuum │ BG Writer │ Stats     │
              └──────────────────────────────────┬──────────────────────────────┘
                                                 │
                                    ┌────────────▼──────────────┐
                                    │   Data Directory (PGDATA) │
                                    │   base/ + pg_wal/ + ...   │
                                    └───────────────────────────┘
```

Key architectural difference: PostgreSQL **forks a new OS process per connection**. Each backend is completely isolated at the OS level. Shared state (buffer pool, lock tables) lives in shared memory accessible by all backends simultaneously.

---

## 3. Internal Design

### 3.1 Process Model

| Aspect | SQLite | PostgreSQL |
|---|---|---|
| Model | Single process (in-process library) | Multi-process (one OS process per connection) |
| Connection overhead | ~microseconds (function call) | ~milliseconds (fork + shared memory attach) |
| Isolation | Within the process | OS-level process isolation |
| Crash impact | Crash = application crash | Backend crash doesn't kill other connections |

**Why PostgreSQL forks per connection:** Process isolation means a misbehaving query cannot corrupt shared state in another backend. Memory corruption in one process is contained. This is the safety-over-efficiency trade-off. The cost is paid at connection time, which is why connection poolers (PgBouncer) are essential in production.

**Why SQLite is in-process:** The application *is* the server. There's no IPC overhead, no socket round-trips. A query is just a function call.

### 3.2 Database File Organization

**SQLite — Single File**

```
SQLite File Layout:
┌──────────────┐
│  Page 1      │  ← Database header (100 bytes) + first B-tree root
│  Page 2      │  ← Freelist trunk page OR B-tree node
│  Page 3      │  ← Data or index B-tree page
│   ...        │
│  Page N      │
└──────────────┘
```

Every SQLite database is a single file. Page size is configurable (512B–65536B, default 4096B). The first 100 bytes of page 1 are the database header containing magic bytes, page size, schema format version, and encoding. There are no tablespaces, no WAL directory (by default — WAL is an optional mode), no separate index files.

**PostgreSQL — Directory of Files**

```
$PGDATA/
├── base/
│   └── 16384/           ← OID of the database
│       ├── 16385        ← Table heap file (OID of the relation)
│       ├── 16385_fsm    ← Free Space Map
│       ├── 16385_vm     ← Visibility Map
│       └── 16386        ← Index file
├── pg_wal/              ← Write-Ahead Log segments (16MB each by default)
├── pg_xact/             ← Transaction commit status bitmaps
├── global/              ← Cluster-wide catalogs (pg_database, pg_authid)
└── postgresql.conf
```

Each table and index is a separate file. Large relations are split into 1GB segments. The Free Space Map tracks which pages have free space (for INSERT reuse). The Visibility Map is a 2-bit-per-page bitmap — one bit tracks whether all tuples on the page are visible to all transactions (enables index-only scans), one bit tracks whether all tuples are frozen.

### 3.3 Page Layout

Both databases use fixed-size pages but with different internal layouts.

**SQLite B-Tree Page:**

```
┌─────────────────────────────────────────────┐
│  Page Header (8 or 12 bytes)                │
│  (page type, freeblock offset, cell count,  │
│   cell content offset, fragmented bytes)    │
├─────────────────────────────────────────────┤
│  Cell Pointer Array  [ptr1][ptr2]...[ptrN]  │
│  (grows downward from top)                  │
├─────────────────────────────────────────────┤
│              Free Space                     │
├─────────────────────────────────────────────┤
│  Cell Content Area    [cellN]...[cell1]     │
│  (grows upward from bottom)                 │
└─────────────────────────────────────────────┘
```

Cells grow from the bottom of the page; the pointer array grows from the top. Free space is in the middle. Cells contain: payload size (varint), row key (varint), and payload data.

**PostgreSQL Heap Page:**

```
┌─────────────────────────────────────────────┐
│  PageHeaderData (24 bytes)                  │
│  (lsn, checksum, flags, free space ptrs,    │
│   special space ptr)                        │
├─────────────────────────────────────────────┤
│  ItemIdData array  [lp1][lp2]...[lpN]       │
│  (4 bytes each: offset + length + flags)    │
├─────────────────────────────────────────────┤
│              Free Space                     │
├─────────────────────────────────────────────┤
│  Tuple Data    [tupleN]...[tuple1]          │
│  Each tuple has HeapTupleHeaderData:        │
│  (xmin, xmax, ctid, infomask, natts, ...)   │
└─────────────────────────────────────────────┘
```

The critical difference: PostgreSQL tuples carry MVCC metadata (xmin/xmax) in the tuple header itself. SQLite tuples carry no such metadata.

### 3.4 Storage Engine / Table Storage

**SQLite:** Tables are stored as B-trees directly. Each row is a leaf cell in the B-tree. The rowid (integer primary key) is the B-tree key. Without an explicit INTEGER PRIMARY KEY, SQLite maintains a hidden rowid. Secondary indexes are separate B-trees that store the indexed column value + rowid.

**PostgreSQL:** Tables are heap files — rows are appended without ordering by key. This is the "heap" in "heap tuple." Pages within a relation are referenced by their block number. There is no inherent ordering of rows in the heap. Indexes are separate structures (B-tree, hash, GiST, etc.) containing pointers back to heap pages via `(block_number, offset)` pairs called CTIDs (Current TID).

This is a fundamental architecture difference:
- SQLite: **index-organized table** (the table IS the B-tree)
- PostgreSQL: **heap table** (table is unordered; indexes point into it)

### 3.5 Index Implementation

**SQLite B-Tree Indexes:**
- All indexes (including the table itself) are B-trees
- Internal nodes store separator keys and child page numbers
- Leaf nodes store actual data (for table B-trees) or key+rowid (for index B-trees)
- Search is O(log N), traversing from root page downward
- Page splits happen when a node overflows: a new root is created and the old root becomes a child

**PostgreSQL nbtree (B+-Tree):**
- Only leaf nodes contain data; internal nodes contain only routing keys
- Leaf pages are doubly-linked (left sibling + right sibling pointers) enabling range scans without returning to root
- Each index entry = (key_value, heap_tid) where heap_tid = (block, offset)
- High keys on each page define the upper bound of that page's key range
- Concurrent inserts use page-level locking (not tree-level), enabling much higher write throughput
- The `fastpath` optimization caches the rightmost leaf for sequential inserts (common for auto-increment PKs)

### 3.6 Concurrency Control

This is where the architectural divergence is most visible.

**SQLite Locking:**

```
UNLOCKED → SHARED → RESERVED → PENDING → EXCLUSIVE
```

SQLite uses file-level locking via OS primitives. Only one writer at a time, ever. Multiple readers can coexist with each other but not with a writer. The PENDING lock prevents new SHARED locks from being acquired while a writer waits, ensuring starvation-freedom for writers.

In WAL mode (SQLite 3.7+), readers and the single writer can coexist because readers read from the original database file while the writer appends to a WAL file. But still: only one concurrent writer.

**PostgreSQL MVCC:**

PostgreSQL uses Multi-Version Concurrency Control. Readers never block writers; writers never block readers. Each transaction sees a consistent snapshot of the database from when it started.

How: when a row is updated, PostgreSQL does NOT modify the existing tuple in place. Instead, it:
1. Marks the old tuple's `xmax` = current transaction ID (logical "deletion")
2. Inserts a NEW tuple with `xmin` = current transaction ID

A concurrent reader running at an earlier snapshot will still see the old tuple (xmax is set but to a transaction ID newer than the reader's snapshot). The reader is completely unblocked.

This is PostgreSQL's core trade-off: writes generate dead tuples that accumulate. VACUUM must periodically scan the heap and reclaim space from tuples whose xmax is older than the oldest active transaction.

### 3.7 Transaction Management & Durability

**SQLite Durability Modes:**

- *Rollback journal mode* (default): Before modifying a page, SQLite writes the original page to a journal file. If the process crashes mid-write, the journal is replayed on next open to restore the original state.
- *WAL mode*: Writes go to a WAL file. Readers use the original file. A checkpoint periodically merges WAL back into the main file. Better for concurrent read/write.

**PostgreSQL WAL:**

Every change to data pages is first recorded in the WAL (Write-Ahead Log) before the page is written to disk. The WAL record is smaller than the full page and is written sequentially, making it fast. On crash:
1. Replay WAL from last checkpoint forward
2. All committed transactions are recovered; uncommitted ones are not visible

The `fsync` call on WAL records is what guarantees durability — PostgreSQL does not return `COMMIT` success until the WAL record is durably written. This is configurable (`synchronous_commit`).

---

## 4. Design Trade-Offs

### Why SQLite works well for mobile/embedded

1. **Zero configuration:** No server to start, no port to configure, no user management. The app ships with the .db file.
2. **Single file portability:** Backup = copy one file. Migrate = move one file.
3. **Low memory footprint:** Page cache can be as small as a few hundred KB.
4. **No network latency:** Queries are function calls, not socket round-trips.
5. **Correctness:** Despite simplicity, SQLite is one of the most tested software artifacts on Earth (~100M lines of test code vs ~150K lines of production code).

**Where SQLite struggles:**
- Many concurrent writers: file-level locking serializes all writes
- Large databases with high write throughput: single writer becomes a bottleneck
- Separate client-server deployments: no network protocol (must be in-process)

### Why PostgreSQL is preferred for multi-user systems

1. **True concurrent writes:** MVCC allows many writers simultaneously (different rows/pages)
2. **Connection isolation:** Process-per-connection means one misbehaving query can't crash others
3. **Advanced concurrency:** Row-level locking, SELECT FOR UPDATE, advisory locks
4. **Rich ecosystem:** Logical replication, streaming replication, foreign data wrappers, extensions
5. **Scale:** Handles terabyte-scale databases; tablespaces allow data across multiple disks

**Where PostgreSQL struggles:**
- Overhead: minimum ~5MB per backend process; fork latency at connection time
- VACUUM: dead tuples must be cleaned; table bloat is a real operational concern
- Operational complexity: requires DBA knowledge, monitoring, tuning

### Summary Comparison Table

| Dimension | SQLite | PostgreSQL |
|---|---|---|
| Architecture | Embedded library | Client-server daemon |
| Concurrency | Single writer (WAL: 1 writer + N readers) | MVCC: N writers + N readers |
| File layout | Single .db file | Directory tree (base/, pg_wal/, etc.) |
| Table storage | B-tree (index-organized) | Heap (unordered) |
| Index type | B-tree only | B-tree, Hash, GiST, GIN, BRIN, SP-GiST |
| Durability | Rollback journal / WAL | WAL with fsync guarantees |
| Dead rows | Overwritten in-place | Accumulate → need VACUUM |
| Deployment | Zero config | Server setup required |
| Best for | Mobile, edge, embedded, testing | OLTP, multi-user, web apps, analytics |

---

## 5. Experiments / Observations

### Observation 1: SQLite single-writer contention

Running concurrent writers in Python against SQLite (default journal mode):

```python
# Thread 1 and Thread 2 both run: INSERT INTO t VALUES (...)
# Result: "database is locked" exception on Thread 2 ~100% of the time
# under any non-trivial write rate
```

Switching to WAL mode (`PRAGMA journal_mode=WAL`) allows Thread 2 to queue behind Thread 1 without an error, but the writes are still serialized.

### Observation 2: PostgreSQL MVCC dead tuple accumulation

```sql
-- Create a table and perform repeated updates
CREATE TABLE test_mvcc (id int, val text);
INSERT INTO test_mvcc VALUES (1, 'initial');

-- Run 10000 updates
UPDATE test_mvcc SET val = 'updated' WHERE id = 1;

-- Check dead tuples
SELECT n_dead_tup, n_live_tup FROM pg_stat_user_tables
WHERE relname = 'test_mvcc';
-- n_dead_tup: ~10000, n_live_tup: 1
-- Each UPDATE created a new tuple version; old ones are dead

-- After VACUUM:
VACUUM test_mvcc;
SELECT n_dead_tup FROM pg_stat_user_tables WHERE relname = 'test_mvcc';
-- n_dead_tup: 0
```

This demonstrates why VACUUM is not optional — without it, table bloat grows indefinitely.

### Observation 3: Page structure inspection

```sql
-- PostgreSQL: inspect raw page content
CREATE EXTENSION pageinspect;
SELECT * FROM heap_page_items(get_raw_page('test_mvcc', 0));
-- Shows: lp (line pointer), t_xmin, t_xmax, t_ctid, t_data
-- Old tuples show t_xmax set to a committed transaction ID
-- New tuples show t_xmax = 0 (still live)
```

---

## 6. Key Learnings

1. **Architecture follows deployment context, not capability.** SQLite is not a "lesser" database — it is the right tool for embedded/single-process use. The architectural choice of in-process vs. client-server dictates everything downstream.

2. **Concurrency is the hardest problem.** SQLite's simplicity comes from admitting it cannot solve concurrent writes efficiently. PostgreSQL's complexity (MVCC, VACUUM, process model) exists entirely to support true concurrency.

3. **There is no free lunch in storage.** PostgreSQL's heap storage + MVCC means dead tuples always accumulate — VACUUM is the cost of non-blocking reads. SQLite's in-place updates avoid this but pay with write serialization.

4. **Single-file portability is a genuine advantage.** The ability to treat a SQLite database like a regular file — copy, rsync, attach as read-only — is architecturally meaningful and not replicable in PostgreSQL without dump/restore.

5. **Index-organized tables (SQLite) vs. heap tables (PostgreSQL):** Index-organized gives you free primary key lookups but makes heap scans awkward and secondary indexes do a double lookup. Heap tables give you flexible storage but require separate index structures for all access patterns.

---

*References:*
- *SQLite file format documentation: https://www.sqlite.org/fileformat.html*
- *PostgreSQL source: src/backend/storage/, src/include/storage/bufpage.h*
- *"Architecture of a Database System" — Hellerstein, Stonebraker, Hamilton (2007)*
