# Track A: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 32.05 | 228 | 0 |
| 1 | C++ single-threaded rewrite: mmap sequential read, zero-allocation parsing, FNV-1a interning for endpoints/hours, and flat table aggregation (commit 5553739) | run-4e6b4da73851 | 0.600 | 41.1 | 44.75 |
| 2 | A05 4-worker parallel ingest: newline-aligned memory chunking, thread-local open-addressing hash tables, per-worker vector sorting, and final global merge | *pending board run* | *pending* | *pending* | *pending* |

### Step Details and Methodology

#### Step 1: Single-Threaded Zero-Allocation Ingest (Commit 5553739)
- **Source SHA-256**: `e762217e7ea5980ef6f931c43d13b3d38bc62aae73906d476e147de53452cfa7`
- **Methodology**: Replaced the naive Python reference with a high-performance C++20 single-threaded pipeline using `mmap(..., MAP_PRIVATE)` with `MADV_SEQUENTIAL`, custom zero-allocation whitespace scanner, FNV-1a hash table interning for endpoints and hours, flat vector latency storage, and a 1 MB buffered file writer.
- **Official Board Result**: Run ID `run-4e6b4da73851`, $T = 0.600378\text{ s}$, $M = 41.14\text{ MB}$, Points = $44.75$.
- **Local Pre-Submission Benchmark**: Middle-three wall time: $4,228.00\text{ ms}$, in-process compute: $3,454.16\text{ ms}$, peak RSS: $252.13\text{ MB}$.

#### Step 2: Parallel Chunked Ingest — Candidate A05 (4 Workers)
- **Source SHA-256**: `7d11e1303158736629f7ed63ee03e0d5ab612cb4d47b7dd2a9dc99f32beeb894`
- **Configuration**: 4 parallel worker threads (`std::thread`).
- **Methodology**:
  - The mapped input file is partitioned into 4 balanced segments with newline boundary alignment.
  - Each worker independently parses its segment with zero inter-thread locks, interning endpoints and hours into thread-local hash tables (`ThreadLocalData`) and accumulating latencies directly.
  - Main thread performs a fast final reduction merging the thread-local tables into global storage, sorts keys lexicographically, sorts per-group latency vectors for exact percentile computation, and streams the output via buffered I/O.
- **Parity Verification**: 10/10 verified runs on the full official dataset (`238,836,234 bytes`, `3,100,000 lines`) producing identical output SHA-256 `495ca09682b9673ef836a506496177680bb4a3af54255174befddb1f83481cd3` (`10,113,024 bytes`, `126,908 lines`).
- **Local Measurements (Middle-Three Means)**:
  - Container wall time: **$2,382.43\text{ ms}$** (vs. $4,228.00\text{ ms}$ for Step 1, **-43.7% wall time reduction**).
  - In-process compute time: **$1,578.44\text{ ms}$** (vs. $3,454.16\text{ ms}$ for Step 1, **54.3% compute reduction / 2.19x faster**).
  - Peak RSS: **$272.96\text{ MB}$** (Max peak RSS: $273.29\text{ MB}$).
- **Official Board Result**: Pending live evaluation following push.
