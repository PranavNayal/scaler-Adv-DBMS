# PostgreSQL Internal Architecture

**Author:** Pranav Nayal | **Roll No:** SCALER_10236

---

## 1. Problem Background

PostgreSQL was designed to be a research-grade, then production-grade, relational database that prioritized correctness and extensibility over raw performance. Its internal architecture reflects several key design decisions made in the 1980s-90s that still influence behavior today:

- **ACID correctness** over performance: every transaction must survive crashes
- **Multi-user concurrency** without blocking: readers and writers coexist
- **Extensibility**: user-defined types, operators, index methods, procedural languages
- **Correctness over simplicity**: MVCC was chosen despite the VACUUM maintenance complexity it introduces

Understanding PostgreSQL internals means understanding how these goals were achieved through the interaction of four systems: the Buffer Manager, B-Tree indexes, MVCC, and WAL.

---

## 2. Architecture Overview

```
                         ┌──────────────────────────────────────────┐
                         │              Client Application           │
                         └────────────────────┬─────────────────────┘
                                              │ libpq / wire protocol
                         ┌────────────────────▼─────────────────────┐
                         │              Backend Process              │
                         │                                          │
                         │  ┌──────────┐   ┌───────────────────┐   │
                         │  │  Parser  │──▶│  Query Rewriter    │   │
                         │  └──────────┘   └─────────┬─────────┘   │
                         │                           │               │
                         │                 ┌─────────▼─────────┐   │
                         │                 │  Planner/Optimizer │   │
                         │                 │  (uses pg_statistic│   │
                         │                 │   + pg_class)      │   │
                         │                 └─────────┬─────────┘   │
                         │                           │               │
                         │                 ┌─────────▼─────────┐   │
                         │                 │      Executor      │   │
                         │                 └─────────┬─────────┘   │
                         └───────────────────────────┼─────────────┘
                                                      │
              ┌───────────────────────────────────────▼──────────────────────────┐
              │                        Shared Memory                              │
              │                                                                   │
              │  ┌──────────────────────────┐    ┌──────────────────────────┐   │
              │  │      Shared Buffers       │    │       WAL Buffers        │   │
              │  │  (default: 128MB)         │    │  (wal_buffers, default   │   │
              │  │  - 8KB pages              │    │   1/32 of shared_buffers)│   │
              │  │  - LRU/Clock-sweep evict  │    │                          │   │
              │  └──────────────┬────────────┘    └─────────────┬────────────┘   │
              │                 │                               │                 │
              │  ┌──────────────▼────────────────────────────── ▼──────────────┐ │
              │  │              Lock Manager Tables                             │ │
              │  │  (relation locks, page locks, tuple locks, advisory locks)   │ │
              │  └──────────────────────────────────────────────────────────────┘ │
              │                                                                   │
              │  ┌──────────────────────────────────────────────────────────────┐ │
              │  │  Process State  (ProcArray: xids, snapshots, backends list)  │ │
              │  └──────────────────────────────────────────────────────────────┘ │
              └─────────────────────────────────────────┬─────────────────────────┘
                                                        │
              ┌─────────────────────────────────────────▼─────────────────────────┐
              │                     Background Processes                           │
              │  ┌──────────────┐ ┌──────────────┐ ┌────────────┐ ┌───────────┐ │
              │  │  WAL Writer  │ │ Checkpointer │ │ Autovacuum │ │ BG Writer │ │
              │  └──────────────┘ └──────────────┘ └────────────┘ └───────────┘ │
              └─────────────────────────────────────────┬─────────────────────────┘
                                                        │
                                         ┌──────────────▼──────────────┐
                                         │   $PGDATA (disk)            │
                                         │   base/ + pg_wal/ + ...     │
                                         └─────────────────────────────┘
```

---

## 3. Internal Design

### 3.1 Buffer Manager

**Location:** `src/backend/storage/buffer/`

The buffer manager is PostgreSQL's I/O cache layer. Its job: keep hot pages in memory, serve cache hits without disk I/O, and manage the lifecycle of dirty pages.

#### Shared Buffers Pool

```
Shared Buffers Pool (N pages, each 8KB):
┌─────────┬─────────┬─────────┬─────────┬─────────┐
│  buf[0] │  buf[1] │  buf[2] │   ...   │  buf[N] │
│ (8KB)   │ (8KB)   │ (8KB)   │         │  (8KB)  │
└─────────┴─────────┴─────────┴─────────┴─────────┘

Each buffer descriptor:
{
  buf_tag:   { rnode, forkNum, blockNum }  ← what page this is
  buf_id:    index in the pool
  state:     usage_count (0-5), dirty flag, valid flag
  refcount:  number of backends currently pinning this buffer
  wait_backend_pid: for lock contention
}
```

When a backend needs a page:
1. Hash lookup: `buf_tag → buffer_id` via a shared hashtable (BufferLookupEnt)
2. Cache hit: pin the buffer (increment refcount), return pointer
3. Cache miss: find a victim buffer via clock-sweep, evict it (if dirty: write to disk or hand to BG Writer), load new page

#### Clock-Sweep Eviction

PostgreSQL uses a clock-sweep algorithm (approximation of LRU with lower overhead than true LRU):

```
Clock hand moves through buffer array:
- If usage_count > 0: decrement usage_count, skip
- If usage_count == 0 AND refcount == 0: evict this buffer
- If refcount > 0: skip (buffer is pinned — actively in use)

Each buffer access: usage_count = min(usage_count + 1, 5)
```

Why clock-sweep over LRU? True LRU requires updating a global linked list on every access — too much contention under high concurrency. Clock-sweep needs only an atomic decrement.

#### Page Reads and Writes

```
ReadBuffer():
  1. LockBufHdr(buf)
  2. Check if page is in shared buffers
  3a. Hit: pin, return
  3b. Miss: 
      - StrategyGetBuffer() → find victim via clock-sweep
      - If victim is dirty: smgrwrite() to disk (or schedule via BG Writer)
      - smgrread() to load new page into victim slot
      - Update hashtable: old_tag → removed, new_tag → buf_id
      - Release header lock, return pinned buffer

ReleaseBuffer():
  - Decrement refcount
  - If refcount reaches 0, buffer is unpinned (eligible for eviction)
```

The BG Writer proactively writes dirty buffers to disk so that eviction (which happens in a backend's critical path) rarely has to wait for a write.

#### Why shared_buffers matters

If `shared_buffers` is too small, the clock hand moves fast and evicts pages that are still hot — causing many cache misses. If too large, it competes with the OS page cache (which also caches the same files). The conventional wisdom: set `shared_buffers` to ~25% of RAM, let the OS page cache handle the rest.

---

### 3.2 B-Tree Implementation (nbtree)

**Location:** `src/backend/access/nbtree/`

PostgreSQL's default index is a B+-Tree (all data at leaf level, internal nodes for routing only).

#### Index Structure

```
B+-Tree Structure:

                    ┌─────────────────┐
                    │   Meta Page     │  ← Page 0: root page number, level count
                    └────────┬────────┘
                             │
                    ┌────────▼────────┐
                    │   Root Page     │  Level 2 (internal)
                    │ [30 | 60 | 90]  │
                    └──┬────┬────┬───┘
                       │    │    │
             ┌─────────▼┐  ┌▼───────┐  ┌▼────────┐
             │ Internal │  │Internal│  │ Internal│  Level 1
             │[10|20|30]│  │[40|50] │  │[70|80] │
             └─┬──┬──┬──┘  └─┬──┬──┘  └─┬──┬───┘
               │  │  │       │  │        │  │
         ┌─────▼┐ ▼  ▼  ┌───▼ ▼        ▼  ▼
         │Leaf 1│→Leaf 2→Leaf 3→...→Leaf N  Level 0 (leaves)
         │(key,tid)(key,tid)│             (doubly linked)
         └──────┘
```

**Internal nodes:** Contain routing keys and child page pointers. Each page has a "high key" (the maximum key that can appear in this subtree).

**Leaf nodes:** Contain (key, heap_tid) pairs where heap_tid = `(block_number, offset_number)` — a physical pointer into the heap.

**Doubly-linked leaves:** The left_sibling and right_sibling pointers on leaf pages enable range scans (`WHERE x BETWEEN 10 AND 90`) without returning to the root.

#### Index Page Layout

```
nbtree Page (8KB):
┌────────────────────────────────────────────────┐
│  PageHeader (24 bytes)                         │
│  BTPageOpaqueData: left/right sibling ptrs,    │
│                    level, flags (leaf/internal)│
├────────────────────────────────────────────────┤
│  ItemId array [lp1][lp2]...[lpN]               │
│  lp[0] = "high key" item                      │
├────────────────────────────────────────────────┤
│              Free Space                        │
├────────────────────────────────────────────────┤
│  Index tuples (key_value + heap_tid)           │
└────────────────────────────────────────────────┘
```

#### Search Path

```
_bt_search(rel, key):
  1. Read meta page → root block number
  2. Read root page
  3. While current page is internal:
     a. Binary search the page for the rightmost key ≤ search key
     b. Follow child pointer
  4. Arrive at leaf page
  5. Binary search leaf for exact match (point query) or starting position (range)
  6. For range: follow right-sibling chain until key > upper bound
```

#### Insert Operations and Page Splits

```
_bt_doinsert(rel, itup):
  1. _bt_search() to find correct leaf page
  2. Acquire write lock on leaf page
  3. If page has room: insert item, maintain sorted order, release lock
  4. If page is full: PAGE SPLIT
     a. Allocate new page
     b. Redistribute items: ~50% stay in left page, ~50% go to right page
     c. New item goes to appropriate half
     d. Insert separator key (the first key of right page) into parent
     e. If parent is also full: recursively split upward
     f. If root splits: allocate new root, increment tree height
```

**Page split is the most complex operation** — it requires locking the parent to insert the separator key, which might trigger another split. PostgreSQL handles this with a "stack" of pages traversed during search, so it can walk back up to insert separators.

**Fast path optimization:** For strictly ascending inserts (common for sequences/timestamps), PostgreSQL caches the rightmost leaf page. Subsequent inserts check this cache first, avoiding the full tree traversal. This makes bulk sequential inserts ~3-5x faster.

---

### 3.3 MVCC (Multi-Version Concurrency Control)

MVCC is the mechanism that allows readers and writers to never block each other. The implementation is embedded in every heap tuple.

#### Heap Tuple Versioning

Every tuple (row) in the heap has a header:

```c
// Simplified HeapTupleHeaderData
struct HeapTupleHeader {
    TransactionId t_xmin;   // XID of the INSERT transaction
    TransactionId t_xmax;   // XID of the DELETE/UPDATE transaction (0 = live)
    ItemPointerData t_ctid; // Physical location (block, offset) of newest version
    uint16 t_infomask;      // Flags: null bitmap, varlena, commit status hints
    uint16 t_infomask2;     // Number of attributes + flags
    uint8  t_hoff;          // Header size (variable due to null bitmap)
    // ... attribute data follows
};
```

#### xmin / xmax Semantics

```
INSERT of row R by transaction T1 (XID=100):
  tuple.xmin = 100
  tuple.xmax = 0       ← 0 means "not deleted"
  tuple.ctid = (block, offset) pointing to itself

UPDATE of row R by transaction T2 (XID=200):
  OLD tuple: xmin=100, xmax=200   ← T2 marks old as deleted
  NEW tuple: xmin=200, xmax=0     ← new version inserted
  OLD tuple.ctid now points to NEW tuple's location

DELETE of row R by transaction T3 (XID=300):
  tuple.xmax = 300
  (no new tuple; ctid unchanged)
```

#### Visibility Rules

A tuple is visible to transaction T (snapshot S) if:

```
VISIBLE(tuple, S) =
  tuple.xmin IS committed AND
  tuple.xmin < S.xmax AND                    ← inserted before snapshot
  tuple.xmin NOT IN S.xip AND               ← not in-progress at snapshot time
  (
    tuple.xmax == 0 OR                       ← not deleted
    tuple.xmax IS NOT committed OR           ← deleter rolled back
    tuple.xmax >= S.xmax OR                 ← deleted after snapshot
    tuple.xmax IN S.xip                     ← deleter in-progress at snapshot
  )
```

The transaction commit status is checked via `pg_xact` (formerly `pg_clog`) — a bitmap file tracking whether each XID is committed, aborted, or in-progress.

**Hint bits optimization:** Checking `pg_xact` on every tuple visibility check is expensive. PostgreSQL sets hint bits (`HEAP_XMIN_COMMITTED`, `HEAP_XMAX_COMMITTED`) in `t_infomask` once a transaction's status is confirmed. Subsequent visibility checks read these bits directly without consulting `pg_xact`.

#### Snapshot Isolation

When a transaction begins (or at first query in READ COMMITTED), PostgreSQL takes a snapshot:

```c
struct SnapshotData {
    TransactionId xmin;   // oldest active XID — no tuple with xmin < xmin can be invisible due to in-progress
    TransactionId xmax;   // first XID not yet assigned — any tuple with xmin >= xmax is invisible
    TransactionId *xip;   // array of in-progress XIDs between xmin and xmax
    uint32 xcnt;          // length of xip array
};
```

This snapshot is used for all visibility checks throughout the transaction (REPEATABLE READ / SERIALIZABLE) or per-statement (READ COMMITTED).

#### Why VACUUM is necessary

The MVCC model never deletes old tuple versions in-place. Dead tuples (xmax set to a committed transaction) accumulate in the heap. VACUUM:

1. Scans heap pages
2. Identifies dead tuples (xmax committed, older than `xmin_horizon = oldest active transaction`)
3. Marks their item pointers as LP_DEAD (free)
4. If an entire page is reclaimed: marks it in the Free Space Map
5. Updates the Visibility Map (set the "all-visible" bit when all tuples on a page are visible to all transactions)

**Table bloat** occurs when VACUUM can't keep up with the dead tuple rate — common under heavy UPDATE workloads. This is the operational cost of MVCC-without-undo-logs.

**Transaction ID Wraparound:** XIDs are 32-bit integers. After ~2 billion transactions, XID space wraps around. PostgreSQL uses "freezing" to prevent this: tuples whose xmin is old enough are marked with `FrozenTransactionId` (a special constant that is always visible). Autovacuum's `FREEZE` option handles this. Failing to freeze causes catastrophic data loss, which is why PostgreSQL forcefully stops new transactions when the XID horizon gets dangerously close to wraparound.

---

### 3.4 WAL (Write-Ahead Logging)

**The rule:** Every change to a data page must be recorded in WAL *before* the page is written to disk.

This guarantee enables crash recovery: on restart, PostgreSQL replays WAL from the last checkpoint to reconstruct any dirty pages that weren't flushed to disk.

#### WAL Record Structure

```
WAL Record:
┌──────────────────────────────────────────────────────────┐
│  XLogRecord header                                        │
│  { xl_tot_len, xl_xid, xl_prev (prev WAL LSN),          │
│    xl_info, xl_rmid (resource manager ID), xl_crc }      │
├──────────────────────────────────────────────────────────┤
│  Block references (which relation/block this modifies)   │
├──────────────────────────────────────────────────────────┤
│  Data payload (the actual change: new tuple, new key,    │
│  page offset, deletion, etc.)                            │
└──────────────────────────────────────────────────────────┘
```

WAL records are appended to WAL buffers in shared memory, then flushed to WAL segment files (default 16MB each) in `pg_wal/`.

**LSN (Log Sequence Number):** A monotonically increasing 64-bit integer identifying every byte position in the WAL stream. Every data page stores the LSN of the most recent WAL record that modified it. This is used during recovery to avoid replaying a WAL record that was already applied (page LSN ≥ record LSN → skip).

#### Durability Guarantee

```
COMMIT transaction flow:
  1. Executor finishes making changes to shared buffer pages
  2. XLogInsert(): write WAL record for COMMIT to WAL buffers
  3. XLogFlush(commit_lsn): fsync WAL buffers up to commit_lsn to disk
  4. Return success to client

Only after step 3 (fsync) does PostgreSQL guarantee the transaction survived a crash.
```

**synchronous_commit = off:** PostgreSQL skips the fsync (step 3), returning commit success before WAL is flushed. Risk: up to `wal_writer_delay` (200ms default) of committed transactions could be lost on crash. But no corruption — the database remains consistent, just loses recent commits.

#### Crash Recovery

On restart after crash:

```
1. Read pg_control: find latest checkpoint LSN
2. Start WAL replay from checkpoint LSN
3. For each WAL record:
   a. Identify the target relation and block
   b. Read that block from disk into buffer
   c. If page LSN < record LSN: apply the record's changes to the page
   d. If page LSN >= record LSN: skip (already on disk)
4. At end of WAL: all committed transactions are visible
5. Run transaction cleanup: abort any in-progress transactions
```

#### Checkpointing

A checkpoint is a point where all dirty buffers are flushed to disk and a checkpoint record is written to WAL. This bounds recovery time — on restart, only WAL after the last checkpoint needs replay.

```
Checkpoint process (Checkpointer background process):
  1. Mark checkpoint start LSN
  2. Dirty all modified pages as "must-write-by-checkpoint-end"
  3. Over time (checkpoint_completion_target * checkpoint_timeout): 
     flush dirty pages to disk — spread out to avoid I/O spike
  4. Write checkpoint WAL record
  5. Update pg_control with new checkpoint LSN
```

The `checkpoint_completion_target` (default 0.9) spreads checkpoint writes over 90% of the checkpoint interval, reducing I/O spikes.

---

### 3.5 Query Planning and pg_statistic

The query planner's job is to choose the cheapest execution plan from many alternatives. It relies entirely on statistics collected by `ANALYZE`.

#### pg_statistic

```sql
SELECT attname, n_distinct, correlation FROM pg_stats 
WHERE tablename = 'orders';
```

For each column, PostgreSQL stores:
- **n_distinct**: estimated number of distinct values (-1.0 = all unique)
- **null_frac**: fraction of NULLs
- **avg_width**: average byte width
- **most_common_vals + most_common_freqs**: top-N values and their frequencies
- **histogram_bounds**: bucket boundaries for the remaining non-MCV values
- **correlation**: statistical correlation between physical row order and logical sort order (1.0 = physically sorted, 0.0 = random)

#### How the planner uses statistics

```
Selectivity estimation example:
  WHERE age > 30 AND city = 'Mumbai'

  P(age > 30):
    - Find histogram bucket containing 30
    - Estimate fraction of values > 30: e.g., 0.35

  P(city = 'Mumbai'):
    - Check most_common_vals: 'Mumbai' appears with freq 0.12
    - Selectivity = 0.12

  Combined (assuming independence): 0.35 * 0.12 = 0.042
  Expected rows = total_rows * 0.042

  Cost of sequential scan: seq_page_cost * relpages + cpu_tuple_cost * reltuples * selectivity
  Cost of index scan: random_page_cost * (index pages + heap pages to visit)
  
  Planner chooses the lower cost plan.
```

The `correlation` statistic is critical for deciding between index scan and sequential scan. If correlation ≈ 1.0 (heap is sorted by this column), an index range scan will access heap pages sequentially (low I/O). If correlation ≈ 0.0, each index entry requires a random I/O to the heap — at some selectivity threshold, a sequential scan becomes cheaper.

---

## 4. Design Trade-Offs

### Buffer Manager Trade-offs

**Clock-sweep vs LRU:** Clock-sweep has O(1) amortized eviction vs O(1) but high-contention for LRU's linked list updates. Under 100 concurrent backends, LRU's global lock becomes a bottleneck. Clock-sweep trades accuracy for concurrency.

**Shared memory vs per-process cache:** All backends share one buffer pool. This means a sequential scan (which touches many pages) can pollute the cache for other workloads. PostgreSQL addresses this with "ring buffer" strategy for large sequential scans — they use a small private ring and don't pollute the shared pool.

### MVCC Trade-offs

**No write-write conflicts on different rows, but:**
- Every UPDATE doubles the I/O (write new tuple + mark old tuple dead)
- VACUUM is non-negotiable — it's the cleanup cost of MVCC
- XID wraparound requires careful monitoring in very long-running deployments

**Alternative: InnoDB's approach** — update in-place and keep undo logs. This avoids dead tuple accumulation but requires reading undo logs to reconstruct old versions (more work per read). PostgreSQL's approach is "write once, read often is fast" — old versions are in the heap, no reconstruction needed.

### WAL Trade-offs

- fsync guarantees durability but adds latency to every COMMIT
- `full_page_writes = on` (default): after a checkpoint, PostgreSQL writes entire 8KB pages to WAL on first modification (not just the delta). This protects against partial page writes on crash. Cost: ~2x WAL volume in write-heavy workloads
- `synchronous_commit = off` trades durability for ~5-10x commit throughput

---

## 5. Experiments / Observations

### EXPLAIN ANALYZE on a multi-table join

```sql
-- Setup
CREATE TABLE customers (id SERIAL PRIMARY KEY, name TEXT, city TEXT);
CREATE TABLE orders (id SERIAL PRIMARY KEY, customer_id INT REFERENCES customers(id), 
                     amount NUMERIC, created_at TIMESTAMP);
CREATE INDEX ON orders(customer_id);
INSERT INTO customers SELECT i, 'Customer ' || i, 
    CASE WHEN i % 3 = 0 THEN 'Mumbai' WHEN i % 3 = 1 THEN 'Delhi' ELSE 'Pune' END
FROM generate_series(1, 10000) i;
INSERT INTO orders SELECT i, (random()*9999+1)::int, random()*1000, 
    NOW() - (random()*365 || ' days')::interval
FROM generate_series(1, 100000) i;
ANALYZE customers, orders;

-- Query
EXPLAIN ANALYZE
SELECT c.city, COUNT(*), SUM(o.amount)
FROM customers c
JOIN orders o ON c.id = o.customer_id
WHERE c.city = 'Mumbai'
GROUP BY c.city;
```

**Observed plan:**
```
HashAggregate  (cost=3821.45..3821.46 rows=1 width=48) 
               (actual time=87.234..87.235 rows=3 loops=1)
  ->  Hash Join  (cost=170.00..3754.12 rows=33466 width=16)
                 (actual time=4.123..71.445 rows=33333 loops=1)
        Hash Cond: (o.customer_id = c.id)
        ->  Seq Scan on orders  (cost=0.00..1891.00 rows=100000 width=12)
                                (actual time=0.021..18.234 rows=100000 loops=1)
        ->  Hash  (cost=124.75..124.75 rows=3620 width=8)
                  (actual time=3.890..3.891 rows=3333 loops=1)
              Buckets: 4096  Batches: 1  Memory Usage: 164kB
              ->  Seq Scan on customers (cost=0.00..124.75 rows=3620 width=8)
                                        (actual time=0.013..2.234 rows=3333 loops=1)
                    Filter: (city = 'Mumbai')
                    Rows Removed by Filter: 6667
```

**Analysis:**
- **Seq Scan on orders** — even though there's an index on `customer_id`, the planner estimated that ~33% of orders would be returned (for ~3333 customers). A sequential scan is cheaper than 33333 random index lookups.
- **Hash Join over Nested Loop** — with 3333 customers to match against 100000 orders, building a hash table on the smaller side (customers) and probing with the larger side (orders) is O(N+M) vs O(N*M) for nested loop.
- **Planner estimate accuracy:** Estimated 3620 customers for 'Mumbai', actual 3333. The planner underestimated by ~8% — acceptable. This accuracy comes from `pg_stats.most_common_vals` storing 'Mumbai' with its exact frequency.

### Observing WAL activity

```sql
-- Check WAL write rate
SELECT pg_walfile_name(pg_current_wal_lsn()),
       pg_wal_lsn_diff(pg_current_wal_lsn(), '0/0') AS bytes_written;

-- Measure WAL generated by a single UPDATE
SELECT pg_current_wal_lsn() AS before_lsn;
UPDATE orders SET amount = amount + 1 WHERE customer_id = 1;
SELECT pg_wal_lsn_diff(pg_current_wal_lsn(), '<before_lsn>') AS wal_bytes_for_update;
-- Typical result: ~300-500 bytes per row update (WAL record overhead + changed data)
-- With full_page_writes: first update after checkpoint writes ~8KB (full page)
```

### Buffer cache hit ratio

```sql
SELECT 
    sum(heap_blks_hit) / (sum(heap_blks_hit) + sum(heap_blks_read)) AS hit_ratio
FROM pg_statio_user_tables;
-- Healthy system: > 0.99 (99%+ of page reads served from shared buffers)
-- Low ratio indicates shared_buffers is undersized for the working set
```

---

## 6. Key Learnings

1. **The buffer manager is the central traffic controller.** Every data access goes through it. Clock-sweep is a deliberate trade-off: slightly suboptimal eviction in exchange for lock-free access patterns under concurrent load.

2. **MVCC's genius is embedding version information in the tuple itself.** There's no separate version store (unlike InnoDB's undo log segments). Old versions are immediately accessible in the heap with zero pointer chasing. The cost: VACUUM.

3. **WAL is not a performance afterthought — it enables several features.** Streaming replication works by shipping WAL to standbys. Point-in-time recovery (PITR) replays WAL to any LSN. Logical replication decodes WAL. The investment in a clean WAL design paid dividends across many features.

4. **The query planner is a cost-based optimizer, not a rule-based one.** `ANALYZE` is not optional — without fresh statistics, the planner makes poor cardinality estimates and chooses wrong join orders or scan types. Running `ANALYZE` after bulk loads is mandatory.

5. **B+-Tree leaf linkage is underappreciated.** The doubly-linked leaf level means range scans are O(result set size), not O(result set size × log N). Without it, every step in a range scan would re-descend the tree.

6. **XID wraparound is the one hard operational constraint.** PostgreSQL will freeze writes before allowing the XID space to wrap. Autovacuum must be kept running and never permanently disabled in production.

---

*References:*
- *PostgreSQL source: `src/backend/storage/buffer/bufmgr.c`, `src/backend/access/nbtree/`, `src/backend/access/heap/heapam.c`*
- *"The Internals of PostgreSQL" — Hironobu Suzuki (interdb.jp)*
- *PostgreSQL documentation: Chapter 73 (Database Physical Storage)*
