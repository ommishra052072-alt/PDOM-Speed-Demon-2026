# Track D Optimization Research

## Objective
Investigate and validate a low-risk optimization for Track D (Traverse), while preserving the original implementation as the baseline.

## Optimization: In-degree-zero target shortcut

In a directed graph, a vertex with zero incoming edges cannot be reached from any different source vertex.

The candidate implementation maintains a compact bitset called `has_in`:
- A bit is set for each vertex that appears as the destination of an edge.
- The bitset is populated during graph parsing.
- Before Dijkstra, queries with `s != t` are answered with `-1` when the target has no incoming edges.
- The `s == t` check is performed first, preserving the correct answer of zero.

## Memory overhead

For 1,740,512 vertices:
- 27,196 64-bit words
- 217,568 bytes
- Approximately 212.5 KiB

## Correctness validation

Testing was performed in a Linux sandbox using GCC 13.3:

`g++ -std=c++20 -O3 -DNDEBUG -static`

### Randomized fuzz testing
- 8,000 generated cases
- 165,602 queries
- 58,679 shortcut-eligible queries
- Zero mismatches reported

### Official seed-1 datasets

| Dataset | Nodes | Edges | Queries | Result |
|---|---:|---:|---:|---|
| Tiny | 348 | 1,000 | 40 | Passed |
| Sample | 174,051 | 500,000 | 200 | Passed |
| Full | 1,740,512 | 5,000,000 | 2,000 | Passed |

Baseline and candidate outputs matched expected outputs byte for byte.

Full dataset expected output SHA-256:

`A23DBDF0A3E49CC8901310A9EE1DC6A9A569A85297B6DA75C2A7002CB2ED277E`

## Source integrity

Original baseline SHA-256:

`75D9165A959C8F4A18FAEA58794618914C30B34D6324900A464EFF0A76011FFA`

Candidate SHA-256:

`45A511746D58C296D2907E42C4CDB9DB28F36CBD313CE3C58564FB453643DFA8`

## Limitations
- Testing used GCC 13.3, not the official GCC 15 Docker image.
- The Linux sandbox used one worker thread.
- Only seed 1 was tested at full scale.
- No controlled performance benchmark has been completed.
- No speedup is claimed.

## Next steps
1. Benchmark baseline and candidate under identical conditions.
2. Validate multithreaded execution.
3. Test with the official compiler and Docker environment.
4. Investigate further optimizations using measured evidence.
