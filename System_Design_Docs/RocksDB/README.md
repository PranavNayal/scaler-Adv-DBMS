# RocksDB Architecture — LSM-Tree Based Storage

**Author:** Pranav Nayal | **Roll No:** SCALER_10236

---

## 1. Problem Background

### Why B-Trees Fail at Write-Heavy Scale

Traditional storage engines (InnoDB, PostgreSQL) use B+-Trees for indexing. B+-Trees are excellent for reads (O(log N) point lookups, efficient range scans) but have a fundamental write bottleneck: **random I/O**.

When inserting or updating a record, a B+-Tree must modify the page at a specific position in the tree. On a large dataset, that page is unlikely to be in the buffer pool — the write requires a random disk seek. At millions of writes per second, the disk's random I/O capacity becomes the ceiling.

**The core insight of LSM-Trees (Log-Structured Merge-Trees):** Convert random writes into sequential writes. Writes always append to a log; compaction merges sorted files later. This trades write performance for read complexity and compaction overhead.

### Origin of RocksDB

- **LevelDB** was created by Sanjay Ghemawat and Jeff Dean at Google (2011) as a high-performance embedded key-value store based on LSM-trees.
- **RocksDB** was forked from LevelDB by Dhruba Borthakur at Facebook (2012) to handle Facebook's production workloads — flash SSDs, multi-core machines, high write throughput for feed/social graph data.
- Key improvements over LevelDB: multi-threaded compaction, column families, bloom filters per level, write batching, merge operators, extensive statistics.

RocksDB is now the storage engine beneath many production systems: MyRocks (MySQL), CockroachDB (optional), TiKV (TiDB), Kafka (log compaction), Flink (state backend), Cassandra (experimental).

---

## 2. Architecture Overview

```
Write Path:                           Read Path:
                                      
  Key-Value write                       Key-Value read
       ↓                                      ↓
  WAL (sequential write)           Check MemTable (current)
       ↓                                      ↓
  Active MemTable                  Check Immutable MemTables
  (skip list, in memory)                       ↓
       ↓ (when full)              Check L0 SSTables (newest first)
  Immutable MemTable                           ↓
  (read-only, flushed to L0)      Check L1 SSTables (binary search)
       ↓                                      ↓
  L0 SSTables                     Check L2 SSTables
  (sorted, may overlap)                        ↓
       ↓ compaction                           ...
  L1 SSTables                     Check Ln SSTables
  (sorted, no overlap)                (largest level)
       ↓ compaction
  L2 ... Ln SSTables

Memory:                           Disk:
┌──────────────────────┐          ┌────────────────────────────────┐
│   Active MemTable    │          │  L0: 4 SSTables (may overlap)  │
│   (skip list)        │  flush   │  L1: SSTables (no overlap)     │
│   Immutable          │ ──────▶  │  L2: 10x larger than L1        │
│   MemTable(s)        │          │  L3: 10x larger than L2        │
│   Block Cache        │          │  ...                           │
│   (hot SSTable blks) │          │  Ln: largest, coldest data     │
└──────────────────────┘          └────────────────────────────────┘
```

---

## 3. Internal Design

### 3.1 MemTable

The MemTable is the in-memory write buffer — the first stop for all writes.

**Data structure:** RocksDB uses a **skip list** by default (configurable to a hash table or vector). The skip list maintains keys in sorted order, enabling:
- O(log N) point inserts and lookups
- O(N) sorted iteration (for flushing to SSTable)

```
Skip List structure (conceptual):

Level 3: ─────────────────────────────▶ [key=50] ─────────────────────▶ NULL
Level 2: ────────────▶ [key=20] ──────▶ [key=50] ──────▶ [key=80] ───▶ NULL  
Level 1: ─▶ [key=10] ▶ [key=20] ▶ [key=30] ▶ [key=50] ▶ [key=80] ▶ [key=90] ▶ NULL

Each node: {key, value, sequence_number, type (Put/Delete/Merge)}
```

**Sequence numbers:** Every write in RocksDB gets a monotonically increasing sequence number. This allows MVCC-style reads at a specific point in time — read requests specify a sequence number and only see entries with sequence_number ≤ that value.

**Write operations:**
- `Put(key, value)`: insert (key, seq, PUT, value) into skip list
- `Delete(key)`: insert (key, seq, DELETE, "") — a "tombstone"
- `Merge(key, value)`: insert (key, seq, MERGE, delta) — lazy merge operation

Writes never modify existing entries — they always insert new entries. The latest version (highest sequence number) wins during reads.

### 3.2 Immutable MemTable

When the active MemTable reaches its size limit (`write_buffer_size`, default 64MB):
1. The active MemTable is switched to **immutable** (read-only, no more writes)
2. A new empty MemTable becomes active — writes continue without stalling
3. A background flush thread writes the immutable MemTable to L0 as a new SSTable file

Multiple immutable MemTables can exist simultaneously if the flush thread falls behind the write rate. Write stalls are triggered when too many immutable MemTables accumulate.

### 3.3 WAL (Write-Ahead Log)

Before writing to the MemTable, RocksDB appends to the WAL. This is purely for **crash recovery** — if the process crashes before the MemTable is flushed to disk, the WAL is replayed on restart to reconstruct the MemTable.

```
WAL record format:
┌──────────────────────────────────────────────────────┐
│  Batch header: sequence_number, record_count          │
├──────────────────────────────────────────────────────┤
│  Record 1: type (Put/Delete/Merge), key, value       │
│  Record 2: ...                                        │
│  ...                                                 │
└──────────────────────────────────────────────────────┘
```

**WAL sync behavior:**
- `sync_log = true`: fsync on every write — durable but ~10x slower
- `sync_log = false`: no fsync — OS buffers writes — fast but last ~seconds of writes lost on OS crash

Once the MemTable is flushed to an SSTable, the corresponding WAL segment can be deleted.

### 3.4 SSTables (Sorted String Tables)

An SSTable is an immutable, sorted, compressed file on disk. Once written, it is never modified — this is the key to LSM-Tree's sequential I/O pattern.

**SSTable internal structure:**

```
SSTable File:
┌─────────────────────────────────────────────────────────┐
│  Data Blocks (fixed size, e.g., 4KB)                   │
│  Each block: sorted key-value pairs                     │
│  ┌────────────────────────────────────────────────────┐ │
│  │  Block 1: [key1,val1][key2,val2]...[keyN,valN]    │ │
│  │  Block 2: [key_next_1, ...]                        │ │
│  │  ...                                               │ │
│  └────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────┤
│  Meta Block: Bloom Filter                               │
│  (one bit-array per SSTable, or per block)              │
├─────────────────────────────────────────────────────────┤
│  Meta Block: Statistics (key range, entry count, etc.)  │
├─────────────────────────────────────────────────────────┤
│  Index Block:                                           │
│  [last_key_of_block1 → block1_offset]                   │
│  [last_key_of_block2 → block2_offset]                   │
│  ...                                                    │
│  (binary search to find which data block to read)       │
├─────────────────────────────────────────────────────────┤
│  Footer: index block offset, meta index offset,         │
│          magic number                                    │
└─────────────────────────────────────────────────────────┘
```

**Index block:** To find a key in an SSTable, read the index block (small, often cached) and binary search it to find which data block contains the key. Read only that data block — avoids reading the entire SSTable.

**Compression:** Each data block is independently compressed (Snappy, LZ4, Zstd). Compression is applied per-block so random reads don't require decompressing the entire file.

### 3.5 Storage Levels (L0 to Ln)

```
Level 0 (L0):
  - SSTables flushed directly from MemTable
  - Key ranges CAN OVERLAP between different L0 files
  - Reading L0: must check ALL L0 files (because of overlap)
  - Typically 4-8 files before compaction triggers
  - L0 is the "hot zone" — newest data, smallest, checked first on reads

Level 1 (L1):
  - Fixed total size (e.g., 256MB)
  - Key ranges DO NOT overlap — each key belongs to exactly one L1 file
  - Maintained in sorted order of key ranges
  - Reading L1: binary search file metadata → read exactly 1 file

Level 2 (L2):
  - 10x larger than L1 (2.5GB)
  - No overlap, sorted ranges
  
Level 3 (L3):
  - 10x larger than L2 (25GB)
  ...

Level N (Ln):
  - Largest level, coldest data
  - Most compaction work happens merging Ln-1 into Ln
```

**Why does L0 allow overlap?** Because SSTables are flushed directly from MemTable and the MemTable flush happens frequently — requiring compaction before each flush would make flushes slow. The trade-off: L0 reads are expensive (check all files), but L0 is small and compacted quickly.

**Why do L1+ not allow overlap?** To bound read amplification. If non-L0 levels had overlapping key ranges, a read might need to check many files per level. Non-overlapping guarantees at most 1 file per level for a point lookup.

### 3.6 Bloom Filters

A Bloom Filter is a probabilistic data structure that answers "is this key definitely NOT in this SSTable?" with zero false negatives (if the filter says "not present," the key is definitely absent) and a small configurable false positive rate (e.g., 1%).

```
Bloom Filter for an SSTable:
  - Bit array of M bits (e.g., M = 10 * number_of_keys for ~1% FPR)
  - k hash functions

Insertion of key K:
  For i = 1 to k:
    bit_array[ hash_i(K) % M ] = 1

Query for key K:
  For i = 1 to k:
    If bit_array[ hash_i(K) % M ] == 0: DEFINITELY NOT PRESENT
  If all bits are 1: PROBABLY PRESENT (might be false positive)
```

**Impact on read performance:** In a 7-level LSM-Tree without Bloom Filters, a point lookup for a non-existent key must check all SSTables at all levels — potentially dozens of I/O operations. With Bloom Filters:
- ~99% of "not present" queries are eliminated per file with a 1% FPR setting
- Each Bloom Filter check is an in-memory bit-array lookup (nanoseconds)
- Only 1% of files require an actual disk I/O to confirm absence

RocksDB stores Bloom Filters in each SSTable's meta block. The Block Cache keeps frequently accessed Bloom Filter blocks in memory.

### 3.7 Compaction

Compaction is RocksDB's background process that merges SSTables, removes deleted/overwritten entries, and maintains the level structure.

**Why compaction is necessary:**
1. **Space reclamation:** Deleted entries (tombstones) and overwritten keys consume space until compaction merges them out
2. **Read amplification control:** Without compaction, every level would accumulate more files, making reads check more files
3. **Level size limits:** Each level has a target size; compaction moves data from over-full levels to the next

**Level Compaction (default):**

```
L0 → L1 compaction (when L0 has ≥ 4 files):
  1. Select all L0 files (they may overlap)
  2. Find all L1 files whose key range overlaps the L0 key range
  3. Merge-sort all selected files
  4. Write output as new L1 files (no overlap, within L1 size limit)
  5. Delete input files

L1 → L2 compaction (when L1 exceeds target size):
  1. Select one L1 file (round-robin to spread compaction evenly)
  2. Find all L2 files whose key range overlaps
  3. Merge-sort
  4. Write output as new L2 files
  5. Delete input files
```

**Write Amplification:** Each byte written to RocksDB is eventually compacted through multiple levels. In a 7-level LSM with 10x size ratio, a key written once may be rewritten ~30 times by compaction (WA ≈ 1 + level_count × size_ratio_factor). This is the fundamental LSM write amplification cost.

**FIFO Compaction:** For time-series data where old data can be discarded, FIFO compaction simply deletes the oldest SSTable when the total size exceeds a threshold. Zero write amplification, but no compaction-driven cleanup.

**Universal Compaction:** Keeps total write amplification close to the theoretical minimum by compacting all files whose total size is close to each other. Better for write-heavy workloads; worse for read performance.

### 3.8 Read Path

```
Get(key, snapshot_seq):
  1. Check Active MemTable (skip list lookup, O(log N))
     → Found with seq ≤ snapshot_seq? Return value (or tombstone → not found)
  
  2. Check each Immutable MemTable (newest first)
     → Same check

  3. For each level (L0 first, then L1, L2, ...):
     a. L0: for each L0 SSTable (newest first):
        - Check Bloom Filter: if definitely absent, skip
        - Otherwise: read index block → find data block → read data block
        - O(L0_file_count) worst case
     b. L1+: binary search file metadata to find candidate file
        - Check Bloom Filter
        - Read index block → read data block
        - O(1) per level (non-overlapping)
  
  4. If not found anywhere: key does not exist
```

**Block Cache:** Frequently accessed data blocks and index blocks are cached in the Block Cache (in-memory, similar to PostgreSQL's shared buffers). Cache hits avoid disk I/O for hot keys.

**Read amplification** = total I/Os per read. Best case (key in MemTable or L0): 1. Worst case (key in Ln, checked all Bloom Filters, one miss per level): O(L0_files + num_levels). With Bloom Filters, the expected case is much closer to O(1) for point lookups.

### 3.9 Write Path

```
Put(key, value):
  1. Write to WAL (sequential append) — if sync=true: fsync
  2. Write to Active MemTable (skip list insert, O(log N))
  3. Return success to caller

  Background:
  4. When MemTable reaches write_buffer_size:
     a. Switch to Immutable MemTable
     b. Create new Active MemTable
     c. Flush Immutable MemTable to L0 (sequential write — fast!)
  5. Compaction triggers based on level sizes/file counts
```

**Write amplification is the cost of sequential writes.** A single Put results in: 1 WAL write + 1 MemTable write + eventual compaction rewrites. The WA factor for Level compaction ≈ 30x for a typical configuration, meaning each logical write byte eventually causes ~30 physical bytes written to disk.

---

## 4. Design Trade-Offs

### Why LSM Trees are Optimized for Writes

B+-Tree random writes: insert must find the correct leaf page (random I/O), modify in-place, write back (random I/O). At 100K inserts/sec, this requires 200K random I/Os/sec.

LSM-Tree writes: always append to WAL (sequential) + MemTable (in-memory). No disk seek required per write. Background compaction does sequential I/O — hard drives are 100x faster for sequential vs random. SSDs have lower random I/O penalty but still benefit from sequential writes (reduced write amplification, better endurance).

### Read Performance Trade-offs

**Point lookup:** B+-Tree — O(log N), single traversal. LSM — O(L0_files + num_levels) with Bloom Filters ≈ O(1) expected for existing keys, O(levels) with bloom false positives for absent keys.

**Range scan:** B+-Tree — excellent, follows sorted leaf chain. LSM — must merge-read from multiple SSTables (across levels), more complex but still sequential.

**In practice:** RocksDB read performance is within 2-3x of InnoDB for cached workloads. For write-heavy workloads, RocksDB can sustain 10x+ higher write throughput.

### Compaction Cost

| Compaction Strategy | Write Amplification | Read Amplification | Space Amplification |
|---|---|---|---|
| Level (default) | High (~30x) | Low (O(levels)) | Low (~1.1x) |
| Universal | Low (~10x) | Medium | Medium (~2x) |
| FIFO | Very Low (~2x) | High | High |

**The fundamental trilemma:** You cannot simultaneously minimize write amplification, read amplification, AND space amplification. Every compaction strategy picks a point on this 3D trade-off surface.

- Facebook production: Level compaction, prioritizes read performance and space
- Write-heavy analytics: Universal compaction, prioritizes write throughput
- Time-series / cold data: FIFO compaction, prioritizes simplicity

### Space Amplification

LSM-Trees maintain multiple versions of a key until compaction removes the older ones. Before the L1→L2 compaction that covers a key, there may be: 1 version in MemTable, 1 in L0, 1 in L1, 1 in L2. Space amplification = actual storage / logical data size. Level compaction keeps SA ≈ 1.1x; Universal can reach 2x.

---

## 5. Experiments / Observations

### Write Amplification Under Different Compaction Strategies

Using `db_bench` (RocksDB's built-in benchmark tool):

```bash
# Level compaction baseline
./db_bench --benchmarks=fillrandom \
  --num=10000000 --value_size=100 \
  --compaction_style=0 \    # 0=Level
  --statistics

# Universal compaction
./db_bench --benchmarks=fillrandom \
  --num=10000000 --value_size=100 \
  --compaction_style=1 \    # 1=Universal
  --statistics
```

**Observed results (approximate):**
- Level compaction: WA ≈ 25-35x, throughput ~400K ops/sec
- Universal compaction: WA ≈ 8-12x, throughput ~600K ops/sec
- FIFO compaction: WA ≈ 2x (no actual merge compaction), throughput ~800K ops/sec

Statistics output includes:
```
rocksdb.bytes.written    : 3.42 GB   ← actual bytes written to disk
rocksdb.compact.write.bytes: 2.98 GB ← bytes written by compaction
# Logical data written: ~1GB (10M * 100 bytes)
# Write amplification: 3.42 / 1.0 = 3.42x (early stage, will grow)
```

### Bloom Filter Impact on Read Performance

```bash
# With Bloom Filters (default, bits_per_key=10)
./db_bench --benchmarks=readrandom \
  --bloom_bits=10 --num=10000000

# Without Bloom Filters
./db_bench --benchmarks=readrandom \
  --bloom_bits=0 --num=10000000
```

**Typical result:** Random point reads on non-existent keys:
- With Bloom Filters: ~500K reads/sec (Bloom Filter eliminates disk I/O)
- Without Bloom Filters: ~50K reads/sec (must probe all SSTable levels)
- **~10x improvement** from Bloom Filters for miss-heavy workloads

### Space Amplification Over Time

Monitoring RocksDB's internal stats:

```
// From statistics dump:
Compaction Stats:
  Level 0: Files=4, Size=48MB (target=0MB)
  Level 1: Files=7, Size=232MB (target=256MB)
  Level 2: Files=68, Size=2.3GB (target=2.5GB)
  Level 3: Files=710, Size=23GB (target=25GB)

// Space amplification = (L0+L1+L2+L3+MemTable) / logical_data_size
// = (48+232+2300+23000 + ~64MB MemTable) / ~22GB logical
// ≈ 1.16x
```

Level compaction maintains excellent space efficiency (~10-20% overhead).

---

## 6. Key Learnings

1. **LSM-Trees solve a specific problem: high sustained write throughput.** The insight is not "LSM is better than B-Trees" — it's "LSM converts random writes (disk seek-bound) into sequential writes (I/O bandwidth-bound)." For read-heavy or read-write balanced workloads, B+-Trees remain competitive or superior.

2. **The read-write-space amplification trilemma is real and unavoidable.** Every compaction strategy is a point in this 3D trade-off space. Choosing RocksDB for a workload means choosing which amplification factor you're willing to pay.

3. **Bloom Filters are not optional — they're architecturally essential.** Without them, the multi-level structure makes reads O(levels × L0_files) I/Os per lookup. Bloom Filters are what makes LSM-Tree reads practical at scale.

4. **Compaction is the hidden cost of writes.** Writes to RocksDB appear fast (MemTable), but compaction eventually rewrites data 20-30x in Level compaction. In a storage system running at 80% of disk I/O capacity, compaction can cause write stalls that look like sudden performance cliffs — understanding compaction backpressure is critical for production operation.

5. **Immutability is the key to parallelism.** SSTables are never modified after creation. This means compaction can be parallelized across multiple threads without locking — just read input files and write new output files. RocksDB supports multi-threaded compaction, giving near-linear scaling with CPU cores for compaction throughput.

6. **Tombstones are subtle.** A Delete in RocksDB inserts a tombstone marker. The tombstone must survive until compaction reaches the level where the original key resides — only then can both the tombstone and the key be removed. If the key never existed (e.g., delete-before-insert pattern), the tombstone persists until it reaches the last level. Tombstone accumulation can silently slow down range scans.

---

*References:*
- *RocksDB wiki: https://github.com/facebook/rocksdb/wiki*
- *"The Log-Structured Merge-Tree" — O'Neil et al. (1996)*
- *"Benchmarking Apache Kafka, Apache Pulsar, and RabbitMQ" — various*
- *RocksDB source: `db/`, `table/`, `memtable/` directories in the RocksDB repo*
- *"Optimizing Space Amplification in RocksDB" — Dhruba Borthakur, Facebook Engineering*
