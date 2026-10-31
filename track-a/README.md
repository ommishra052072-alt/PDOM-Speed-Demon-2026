# Track A: Ingest

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/a-ingest.md).

## Approach

Our solution processes the server request access log using a high-throughput, multi-threaded C++20 ingest pipeline:

1. **Zero-Copy Memory Mapping**: The input `requests.log` is mapped into process memory using `mmap` with `MADV_SEQUENTIAL`.
2. **Parallel Chunk Ingestion**: The input memory region is partitioned into balanced chunks across 4 worker threads, adjusting partition boundaries to align with preceding newline characters.
3. **Lock-Free Thread-Local Parsing & Interning**: Each worker independently scans its chunk without global synchronization. Timestamps and endpoints are parsed into thread-local hash tables (FNV-1a 64-bit hashing with quadratic probing), and integer status codes and latencies are accumulated directly into per-thread vectors.
4. **Aggregated Merge & Percentile Calculation**: After workers join, thread-local groups are combined into a global storage table. Active `(hour, endpoint)` pairs are lexicographically sorted. For each group, latency distributions are sorted in-place to calculate exact p50, p95, and p99 percentiles along with error rates (PPM).
5. **Buffered Output Streaming**: Results are formatted into a 1 MB output buffer and streamed directly to `summary.txt` via block writes.

## Dependencies

None. Standard C++20 library and POSIX system calls only (`dependency_count = 0`).

## With another 24 hours

1. **SIMD Timestamp Parsing**: Implement AVX2 / SSE4.2 vectorized ASCII validation and digit conversion to parse fixed-format ISO-8601 timestamps in parallel.
2. **Parallel Group Sorting & Merge**: Distribute group latency sorting across worker threads prior to final serialization to further compress total processing latency.
3. **Radix Sorting for Latencies**: Replace `std::sort` on group latencies with a 16-bit or 32-bit counting/radix sort since server latencies fall within bounded integer ranges.
