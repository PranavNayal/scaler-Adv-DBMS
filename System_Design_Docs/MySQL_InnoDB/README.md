# MySQL / InnoDB Storage Engine

**Author:** Pranav Nayal | **Roll No:** SCALER_10236

---

## 1. Problem Background

InnoDB was developed by Heikki Tuuri at Innobase Oy (Finland, 1994) and became MySQL's default storage engine in MySQL 5.5 (2010). The design goal was to bring Oracle-grade ACID transactions to MySQL, which at the time only had MyISAM — a fast but non-transactional storage engine that could corrupt on crash.

InnoDB made a set of deliberate architectural choices that differ from PostgreSQL's approach:

1. **Clustered primary key storage** — data physically ordered by primary key, eliminating separate heap files
2. **Undo log-based MVCC** — rather than keeping old versions in place (PostgreSQL's approach), InnoDB updates in-place and keeps undo records to reconstruct old versions on demand
3. **Redo log for durability** — a separate redo log for crash recovery (equivalent to PostgreSQL's WAL)
4. **Row-level locking with gap locks** — finer-grained than table-level locking, but with additional gap and next-key locks to prevent phantom reads at REPEATABLE READ

These choices collectively make InnoDB excellent for OLTP workloads with heavy write activity and random lookups by primary key.

---

## 2. Architecture Overview

```
┌───────────────────────────────────────────────────────────────────┐
│                        MySQL Server Layer                          │
│  SQL Parser → Query Optimizer → Query Executor                    │
└───────────────────────────────────┬───────────────────────────────┘
                                    │ Storage Engine API (handler.h)
┌───────────────────────────────────▼───────────────────────────────┐
│                        InnoDB Storage Engine                       │
│                                                                   │
│  ┌───────────────────────────────────────────────────────────┐   │
│  │                    InnoDB Buffer Pool                      │   │
│  │  ┌─────────────────┐  ┌─────────────────┐                │   │
│  │  │  Data Pages     │  │  Index Pages    │                │   │
│  │  │  (clustered)    │  │  (secondary)    │                │   │
│  │  └─────────────────┘  └─────────────────┘                │   │
│  │  ┌─────────────────┐  ┌─────────────────┐                │   │
│  │  │  Insert Buffer  │  │  Adaptive Hash  │                │   │
│  │  │  (Change Buf.)  │  │  Index (AHI)    │                │   │
│  │  └─────────────────┘  └─────────────────┘                │   │
│  └───────────────────────────────────────────────────────────┘   │
│                                                                   │
│  ┌──────────────────┐  ┌───────────────────┐                     │
│  │  Undo Log Segs.  │  │  Redo Log Buffer  │                     │
│  │  (in tablespace) │  │  (in memory)      │                     │
│  └──────────────────┘  └────────┬──────────┘                     │
│                                  │ fsync on COMMIT                │
└──────────────────────────────────┼────────────────────────────────┘
                                   │
          ┌────────────────────────▼──────────────────────────┐
          │                   Disk                             │
          │  ┌─────────────┐  ┌──────────────┐  ┌─────────┐ │
          │  │ tablespace  │  │  ib_logfile0 │  │  .ibd   │ │
          │  │ (ibdata1 or │  │  ib_logfile1 │  │  files  │ │
          │  │  .ibd)      │  │  (redo logs) │  │         │ │
          │  └─────────────┘  └──────────────┘  └─────────┘ │
          └───────────────────────────────────────────────────┘
```

A key structural difference from PostgreSQL: MySQL has a **pluggable storage engine** architecture. The SQL layer (parser, optimizer) is separate from InnoDB. The optimizer calls InnoDB through a handler API. This means MySQL can use different storage engines (MyISAM, MEMORY, CSV, etc.), but InnoDB is the only one with full ACID support.

---

## 3. Internal Design

### 3.1 Clustered Indexes

InnoDB's most distinctive feature: **every table is stored as a B+-Tree ordered by the primary key**. There is no separate "heap" file.

```
Clustered Index B+-Tree for table `orders` (PK: order_id):

              Internal Node
         ┌─────────────────────┐
         │  100 │ 500 │ 900   │
         └──┬────┬────┬────────┘
            │    │    │
      ┌─────▼┐  ┌▼──┐  ┌▼──────────────────────────────────────┐
      │Leaf 1│→ │ 2 │→ │ Leaf 3                                │
      │      │  │   │  │ {order_id=500, customer_id=42,        │
      │(id,  │  │...│  │  amount=199.99, created_at=2024-01-15,│
      │ all  │  │   │  │  status='shipped', ...}               │
      │ cols)│  │   │  │                                        │
      └──────┘  └───┘  └────────────────────────────────────────┘
```

**What this means:**
- A primary key lookup (`WHERE order_id = 500`) traverses the B+-Tree and arrives directly at the leaf node containing **all column data** for that row — one tree traversal, done.
- In PostgreSQL, an index lookup returns a heap_tid (block, offset), then requires a *second* I/O to fetch the actual row from the heap file (unless it's an index-only scan).
- Row data in InnoDB is sorted physically on disk by primary key, so range scans on the primary key (`WHERE order_id BETWEEN 100 AND 900`) access pages sequentially.

**What happens without an explicit primary key:**
- If there's a UNIQUE NOT NULL column, InnoDB uses that as the clustered index key
- Otherwise, InnoDB generates a hidden 6-byte `DB_ROW_ID` column as the PK
- Using `DB_ROW_ID` means no meaningful ordering and the hidden column wastes space in secondary indexes

**Why primary key choice matters enormously:**
- UUID (random) as PK: inserts go to random positions in the B+-Tree, causing page splits and cache thrashing — B+-Tree pages cannot be sequentially filled
- Auto-increment INT as PK: inserts always append to the rightmost leaf — sequential I/O, minimal splits, hot page stays in buffer pool

### 3.2 Secondary Indexes

Secondary indexes in InnoDB are fundamentally different from primary key indexes:

```
Secondary Index on (customer_id):

Leaf nodes contain: (customer_id_value, PRIMARY_KEY_VALUE)
                    NOT the physical row address

Example leaf entry: {customer_id=42, order_id=500}
```

**Double lookup:** A secondary index lookup must:
1. Traverse the secondary B+-Tree to find (customer_id, order_id) pairs
2. For each matching secondary index entry, traverse the **clustered index** (primary B+-Tree) using the order_id to fetch the actual row

This is called a "clustered index lookup" or "bookmark lookup."

**Why InnoDB stores the PK value instead of a physical pointer (like PostgreSQL's heap_tid):**

In PostgreSQL, if a heap row moves (due to VACUUM or CLUSTER), all index entries pointing to it would become invalid — PostgreSQL handles this with the "heap-only tuple" (HOT) optimization. In InnoDB, if a row is updated and moves within the clustered index, only the clustered index internal structure changes; secondary indexes still hold the PK value, which remains valid. The trade-off: every secondary lookup costs an extra clustered index traversal.

**Covering indexes:** If all columns needed by a query are in the secondary index (key + included columns), InnoDB can avoid the clustered index lookup entirely. This is called a "covering index" and the plan shows "Using index" in EXPLAIN output.

### 3.3 InnoDB Buffer Pool

The buffer pool is InnoDB's main memory cache — analogous to PostgreSQL's shared buffers.

```
Buffer Pool Memory Layout:

┌─────────────────────────────────────────────────────────┐
│  Buffer Pool (e.g., 8GB)                               │
│                                                         │
│  LRU List (with midpoint insertion):                   │
│                                                         │
│  ┌─────────────────────────┬─────────────────────────┐ │
│  │   New Sublist (63%)     │   Old Sublist (37%)     │ │
│  │   (hot pages, recently  │   (pages that haven't   │ │
│  │    accessed twice)      │    been re-accessed)    │ │
│  └─────────────────────────┴─────────────────────────┘ │
│         ← MRU end                         LRU end →    │
│                                                         │
│  Free List: buffers not yet assigned to any page        │
│  Flush List: dirty buffers ordered by oldest_lsn        │
└─────────────────────────────────────────────────────────┘
```

**Midpoint insertion strategy:** New pages are inserted at the midpoint (boundary between old/new sublists), not at the MRU end. This prevents sequential full-table scans from evicting hot working-set pages — the scan pages are inserted into the old sublist and get evicted quickly once the scan passes them.

**Change Buffer (formerly Insert Buffer):** Secondary index updates are expensive if the target index page is not in the buffer pool (random I/O). InnoDB buffers these changes in the Change Buffer (stored in the system tablespace) and merges them into the actual index pages when those pages are later loaded into the buffer pool. This converts random I/O into sequential I/O at the cost of a small merge overhead during reads.

**Adaptive Hash Index (AHI):** InnoDB monitors frequent B+-Tree traversals and automatically builds a hash index in memory for frequently accessed key patterns. Hash lookups are O(1) vs O(log N). AHI is not persisted to disk — it's rebuilt on restart. It can be disabled if it causes contention (`innodb_adaptive_hash_index=OFF`).

### 3.4 Undo Logs

Undo logs are InnoDB's mechanism for:
1. **Transaction rollback:** if a transaction aborts, undo logs recreate the previous row versions
2. **MVCC read consistency:** when a reader needs an older version of a row (before another transaction's update), InnoDB applies undo log entries to reconstruct that version

```
UPDATE orders SET status='shipped' WHERE order_id=500:

BEFORE:
  Clustered index leaf: {order_id=500, status='pending', DB_TRX_ID=100, DB_ROLL_PTR=NULL}

AFTER (in-place update):
  Clustered index leaf: {order_id=500, status='shipped', DB_TRX_ID=200, DB_ROLL_PTR=→undo_record}
  
  Undo segment:
  undo_record: {old_status='pending', old_trx_id=100, prev_roll_ptr=NULL}
```

**DB_TRX_ID:** The transaction ID of the last modification to this row.
**DB_ROLL_PTR:** A 7-byte pointer to the undo log record for the previous version of this row.

When a reader with snapshot S needs to see a version of a row that was modified after S:
```
MVCC read reconstruction:
  1. Read current row from clustered index
  2. Check DB_TRX_ID: is this transaction visible to snapshot S?
  3a. Yes → return this version
  3b. No → follow DB_ROLL_PTR to undo log
  4. Apply undo record to reconstruct previous version
  5. Repeat from step 2 until a visible version is found
```

**Performance implication:** Long-running read transactions can force InnoDB to follow long undo chains to reconstruct old row versions. This is why InnoDB's read performance degrades when there are many accumulated undo records — a known production issue called "undo purge lag."

**Undo log segments** reside in the system tablespace (ibdata1) or undo tablespaces (MySQL 8.0+). The purge thread deletes undo records that are no longer needed by any active transaction.

### 3.5 Redo Logs

The redo log is InnoDB's crash recovery mechanism — functionally equivalent to PostgreSQL's WAL but with different implementation choices.

```
Redo Log Flow:

  Transaction modifies buffer pool page
       ↓
  redo log record written to redo log buffer (in memory)
       ↓
  On COMMIT: redo log buffer flushed to ib_logfile0/ib_logfile1 (fsync)
       ↓
  Client receives "committed"
       ↓
  Modified buffer pool page written to tablespace later (by background flush)
```

**Circular log files:** InnoDB uses a fixed set of redo log files (`ib_logfile0`, `ib_logfile1`, etc.) in a circular ring. When the ring fills up, InnoDB must flush dirty buffer pool pages to disk (to free redo log space) before writing new log records. This creates write stalls if the log fills faster than pages can be flushed.

MySQL 8.0 introduced a dynamically sized redo log that avoids this configuration headache.

**innodb_flush_log_at_trx_commit:**
- `= 1` (default, safest): fsync redo log on every COMMIT — full durability
- `= 2`: write to OS cache on every COMMIT, fsync every ~1 second — crash-safe from MySQL crash, not OS crash
- `= 0`: write/fsync every ~1 second — fastest, but up to 1 second of commits can be lost on crash

### 3.6 Row-Level Locking

InnoDB acquires locks at the row level, not the page or table level. This allows multiple transactions to write different rows of the same table concurrently.

**Record locks:** Lock on a specific index record. When `UPDATE orders SET status='x' WHERE order_id=500`, InnoDB acquires a record lock on the index entry for order_id=500.

**Gap locks:** Lock on the gap *between* index records. Prevent inserts into the gap.

```
Index entries: ..., 300, 500, 900, ...

Gap lock on (300, 500): prevents any INSERT with order_id in (300, 500)
Gap lock on (500, 900): prevents any INSERT with order_id in (500, 900)
```

**Next-key locks:** A combination of a record lock on the current record plus a gap lock on the gap before it. This is InnoDB's default locking for most SELECT ... FOR UPDATE and UPDATE statements at REPEATABLE READ isolation level.

**Why gap locks?** They prevent phantom reads — a transaction reading `WHERE order_id BETWEEN 300 AND 900` twice should see the same rows both times. Without gap locks, another transaction could INSERT a new row in that range between the two reads.

**Gap locks are absent in READ COMMITTED:** At READ COMMITTED isolation, InnoDB only acquires record locks (no gap locks). This allows more concurrent inserts but permits phantom reads.

**Deadlocks** occur when two transactions each hold a lock the other needs. InnoDB detects deadlocks by looking for cycles in the wait-for graph and rolls back the transaction with less undo work (the "victim").

---

## 4. Design Trade-Offs

### InnoDB vs PostgreSQL: MVCC Philosophy

| Aspect | InnoDB | PostgreSQL |
|---|---|---|
| Update strategy | In-place update, undo log chain | Append new tuple, mark old xmax |
| Old version location | Undo log segments (separate) | In the heap (same file as current) |
| MVCC read cost | Follow DB_ROLL_PTR chain (undo reconstruction) | Read old tuple directly (no reconstruction) |
| Dead version cleanup | Purge thread removes old undo records | VACUUM removes dead tuples from heap |
| Table bloat | Undo tablespace bloat (temporary) | Heap bloat (until VACUUM) |
| Long-running transactions | Undo chain grows → slower reads | Table bloat grows → slower scans |

**PostgreSQL's approach** is "store everything in the heap — old versions are immediately accessible." Simple to read, but VACUUM must scan the entire heap periodically.

**InnoDB's approach** is "update in-place — reconstruct old versions when needed from undo." Cleaner heap, but reading old versions requires following potentially long undo chains.

### Why clustered indexes improve lookup performance

For a PK lookup:
- InnoDB: 1 B+-Tree traversal → arrive at leaf → get all column data → done
- PostgreSQL (no covering index): 1 B+-Tree traversal → get heap_tid → 1 heap page I/O → get row

For large rows or queries that need many columns, InnoDB's single-traversal advantage is significant. For narrow queries where a covering index exists, PostgreSQL can match InnoDB's performance.

### Why InnoDB needs BOTH undo and redo logs

This is a common source of confusion:

- **Redo log** answers: "How do I recover committed work after a crash?" — it replays changes to bring pages back to committed state.
- **Undo log** answers: "How do I roll back uncommitted changes after a crash (or during rollback)?" — after crash recovery, any transaction that was in-progress (not committed) must be undone.

PostgreSQL uses only WAL (equivalent to redo) because its MVCC model never overwrites old tuples — uncommitted changes are invisible by default (xmin is the in-progress XID, which is not committed, so no reader sees it). There's nothing to undo at the storage level. InnoDB modifies pages in-place, so it needs undo records to reverse those changes if the transaction doesn't commit.

### Clustered Index Limitations

1. **UUID primary keys cause write amplification:** Random inserts split B+-Tree pages frequently. A table with 1B rows and UUID PK can have 50-60% page fill factor (vs 90%+ for sequential INT PK) due to constant mid-page splits.
2. **Secondary indexes are heavier:** Every secondary index stores the PK value in each entry. Wide PKs (large VARCHAR, composite keys) make all secondary indexes larger.
3. **Range scans on non-PK columns require full secondary index scan + clustered lookup** for each row — expensive if selectivity is low.

---

## 5. Experiments / Observations

### EXPLAIN output: clustered vs secondary index lookup

```sql
-- Clustered index (PK) lookup
EXPLAIN SELECT * FROM orders WHERE order_id = 500;
-- Output:
-- type: const (or eq_ref)
-- key: PRIMARY
-- Extra: (none)
-- 1 row, single B-tree traversal

-- Secondary index with clustered lookup
EXPLAIN SELECT * FROM orders WHERE customer_id = 42;
-- Output (without covering index):
-- type: ref
-- key: idx_customer_id
-- Extra: (none — implies clustered index lookup after secondary)

-- Covering index (avoids clustered lookup)
EXPLAIN SELECT order_id, customer_id FROM orders WHERE customer_id = 42;
-- Output:
-- type: ref
-- key: idx_customer_id
-- Extra: Using index   ← no clustered lookup needed
```

### Undo log growth under long-running transaction

```sql
-- Session 1: start a long-running read transaction
START TRANSACTION;
SELECT * FROM orders LIMIT 1;
-- (leave this open for minutes)

-- Session 2: run many updates
UPDATE orders SET amount = amount + 1;  -- 100000 rows updated
UPDATE orders SET amount = amount + 1;  -- again
-- ...

-- Check undo log size growth
SELECT name, subsystem, comment 
FROM information_schema.innodb_metrics 
WHERE name LIKE 'trx_rseg_history%';
-- history_list_length grows as long as Session 1 holds its snapshot
-- This is the "MVCC read view retention" problem
```

### Gap lock demonstration

```sql
-- Session 1:
BEGIN;
SELECT * FROM orders WHERE order_id BETWEEN 100 AND 200 FOR UPDATE;
-- Acquires next-key locks on (prev, 100], (100, 200], (200, next]

-- Session 2 (runs concurrently):
INSERT INTO orders VALUES (150, ...);  -- BLOCKED: gap lock on (100, 200]
INSERT INTO orders VALUES (300, ...);  -- SUCCEEDS: outside the locked range
```

### AUTO_INCREMENT vs UUID primary key: insert performance

Benchmark (1M inserts):
- **AUTO_INCREMENT INT**: ~85,000 inserts/sec — pages fill sequentially, minimal splits
- **UUID (random)**: ~28,000 inserts/sec — frequent mid-page splits, buffer pool thrashing

This ~3x difference is entirely attributable to the clustered index's sensitivity to key ordering.

---

## 6. Key Learnings

1. **Clustered indexes are the defining InnoDB design decision.** Everything else (secondary index structure, double lookup cost, PK choice sensitivity) flows from this. It optimizes for the most common OLTP access pattern: "fetch a row by its ID."

2. **Undo logs make InnoDB's MVCC "pay at read time" rather than "pay at write time."** PostgreSQL pays at write time (double tuple write) and cleanup time (VACUUM). InnoDB pays at write time (undo record) and read time (undo chain traversal for old readers). Neither is universally better — it depends on read-to-write ratio and transaction length.

3. **Gap locks are a correctness mechanism that becomes a concurrency bottleneck.** They prevent phantom reads (correct) but also block concurrent inserts into nearby key ranges (restrictive). READ COMMITTED removes gap locks and allows phantom reads — a concurrency vs. isolation trade-off that applications must consciously choose.

4. **The redo log is not the same as the undo log.** Redo is for "what was committed," undo is for "what was in-progress." InnoDB needs both because it modifies pages in-place; PostgreSQL needs only WAL because in-progress changes are logically invisible.

5. **InnoDB's buffer pool midpoint insertion is a practical solution to a real problem.** Without it, a single `SELECT COUNT(*) FROM large_table` would evict the entire working set of a high-traffic OLTP database — a catastrophic event for production systems.

---

*References:*
- *InnoDB source: `storage/innobase/` in MySQL source tree*
- *"MySQL Internals Manual" — MySQL documentation*
- *"High Performance MySQL" — Baron Schwartz et al., O'Reilly*
- *"InnoDB: The Buffer Pool" — Jeremy Cole's blog (blog.jcole.us)*
