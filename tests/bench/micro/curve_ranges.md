# Curve index measurements

Measured on 2026-10-03 in Ubuntu under WSL2 (20 logical CPUs, 15 GB RAM) with
clang 21.1.8 and lld: a `Release` build with jemalloc, IPO off, fault injection
off and documentation embedding off. Nothing else ran during the timings. Every
query time is the median of seven warm `SELECT max(id)` runs after one warmup,
taken from psql `\timing`; `max(id)` makes the scan stream every candidate
instead of answering from metadata. Index sizes are the index directories
after the server stops.

SQL numbers come from [curve_index_bench.sh](../../../scripts/perf/curve_index_bench.sh):

```bash
CURVE_BENCH_BIN=build_bench/bin/serened scripts/perf/curve_index_bench.sh
```

## Numeric tuples, one million rows

Three DOUBLE columns uniform in `[0, 1000)`, independent or strongly correlated,
and cubic boxes around 500. The baseline is an inverted index on the three
columns with `BETWEEN` on each, answered by intersecting granular ranges; the
curve index is `curve()` with default options and `sdb_box_contains`.

| Data | Box | Rows | Granular ms | Curve ms |
| --- | --- | ---: | ---: | ---: |
| independent | ±0.5 | 0 | 2.33 | 1.12 |
| independent | ±5 | 0 | 27.92 | 1.12 |
| independent | ±25 | 167 | 21.34 | 1.74 |
| independent | one axis ±5 | 10,001 | 4.58 | 4.50 |
| correlated | ±0.5 | 625 | 1.92 | 2.00 |
| correlated | ±5 | 9,624 | 22.24 | 2.36 |
| correlated | ±25 | 49,622 | 15.23 | 3.14 |
| correlated | one axis ±5 | 10,001 | 4.29 | 4.67 |

| Index | Granular | Curve |
| --- | ---: | ---: |
| independent: build | 0.36 s | 1.85 s |
| independent: size | 100 MB | 427 MB |
| correlated: build | 0.38 s | 2.41 s |
| correlated: size | 87 MB | 364 MB |

Selective boxes over several dimensions are where the curve wins: 12 to 25
times faster than intersecting per-column ranges, because each column's range
alone matches tens of thousands of rows that the intersection then discards.
Correlated data narrows the gap, and a small correlated box (625 rows) is a tie.
A box that leaves a dimension open is not sent to the curve index at all; it is
evaluated by the vectorized scalar predicate over the index's included columns,
on par with the per-column index: a curve cover of such a box selects almost
every row.

The curve index is five to six times slower to build and four times larger than
the per-column index. Most of its size is the term dictionary:
doubles reach one row per cell long before level 64, so the deep terms are
unique per row.

### Level step

`level_step` decides which levels a point is indexed at. The query cover keeps
full precision and expands cells between indexed levels into their descendants,
so a larger step trades query terms for index size and build time.

Three dimensions, one million rows; query time and number of query terms for the
independent (ind) and correlated (cor) boxes:

| Step | Build ind / cor | ±0.5 ind | ±5 ind | ±25 ind | ±25 cor |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 3.24 / 4.14 s | 1.29 ms, 65 terms | 1.31 ms, 64 | 1.58 ms, 64 | 3.32 ms |
| 2 | 1.92 / 2.14 s | 1.32 ms, 126 | 1.42 ms, 110 | 1.73 ms, 211 | 3.12 ms |
| 3 | 1.30 / 1.60 s | 1.62 ms, 756 | 1.50 ms, 432 | 2.15 ms, 1,233 | 3.62 ms |

Two dimensions, one million independent rows; the per-column index answers these
boxes in 1.03, 1.11 and 1.57 ms and builds in 0.28 s:

| Step | Build | ±0.5 | ±2 (16 rows) | ±10 (393 rows) |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 2.29 s | 1.21 ms, 64 terms | 1.31 ms, 64 | 1.55 ms, 64 |
| 2 | 1.23 s | 1.11 ms, 130 | 1.46 ms, 130 | 1.71 ms, 182 |
| 3 | 0.87 s | 1.20 ms, 226 | 1.23 ms, 193 | 1.83 ms, 613 |
| 4 | 0.62 s | 1.49 ms, 694 | 1.22 ms, 145 | 1.73 ms, 441 |

Every query term is a term dictionary lookup, so the default caps the expansion
at 16 terms per cell: step 3 in two dimensions, 2 in three and four, 1 above.
In two dimensions the per-column index is already fast at this size and the
curve only matches it.

## Posting model

[curve_ranges.cpp](./curve_ranges.cpp) runs the production encoder,
covering and term generation over 8,192 points held in memory, and
count postings read. They isolate the algorithm, not SereneDB's posting format
or planner, so their CPU times only compare the curve methods with each other.

```bash
build_bench/bin/serenedb-bench-micro-curve_ranges --benchmark_min_time=0.05s --benchmark_repetitions=3 --benchmark_report_aggregates_only=true
```

Budget 64, Morton:

| Dimensions | Data | Constrained | Granular postings | Curve postings | Returned |
| ---: | --- | --- | ---: | ---: | ---: |
| 2 | independent | all | 807 | 23 | 21 |
| 2 | independent | one | 423 | 8,174 | 423 |
| 2 | correlated | all | 853 | 424 | 380 |
| 3 | independent | all | 1,246 | 4 | 2 |
| 3 | correlated | all | 1,203 | 400 | 341 |
| 4 | independent | all | 1,721 | 2 | 1 |
| 4 | correlated | all | 1,704 | 387 | 326 |

Morton and Hilbert select the same cells and read the same postings; Hilbert
costs 25 to 55% more CPU to encode.

## Prior art

The [lindel registry entry](https://duckdb.org/community_extensions/extensions/lindel)
and its [implementation](https://github.com/Query-farm/lindel/tree/v1.5/duckdb_lindel_rust)
were checked. A temporary executable linked `lindel` 0.1.1 to generate 16
two-dimensional 64-bit Hilbert vectors, pinned in
`SpaceFillingCurve.LindelHilbertReference`; lindel is not a dependency.

## Earlier checks

These checks used the earlier WSL build that produced the Release measurements
above.

- `SpaceFillingCurve` gtests under AddressSanitizer and UndefinedBehaviorSanitizer,
  including the lindel vectors, randomized no-false-negative boxes for every
  level step and level-step candidate narrowing, and the options serialization test.
- The curve, scan-metrics and documentation sqllogic files with both
  wire protocols, the geographic index files, and the crash-recovery test.
- The full `sdb` sqllogic suite on a Debug build: 894 files pass; the five that
  fail need what the local setup lacks (embedded documentation, a TLS listener,
  an iceberg version hint) and do not touch these indexes.

## Checks after updating to main

On 2026-10-03, the numeric branch was rebuilt from main `2be6a28e7` with DuckDB
`a10d8bfe2`, clang 21.1.8 and lld under WSL2. This Debug build uses the system
allocator, no IPO, fault injection, and embedded documentation.

- All 8 `SpaceFillingCurve` tests and 109 binary serialization tests pass.
- All 284 index SQL files and the curve documentation example pass with both
  wire protocols after generating the local Iceberg fixture.
- The full regular `sdb` suite covers 1,284 files per wire protocol. The extended
  run passes completely. The simple run initially has two environment failures:
  the missing generated Iceberg fixture and TLS offered on the plain listener.
  Generating the fixture with `gen_iceberg_fixture.py` and explicitly using
  `sslmode=disable` on the plain listener makes both files pass in both protocols.
- The crash-recovery test passes.
- The 12 three-dimensional budget-64 microbenchmark cases return the same rows
  as the exact intersection. For the independent fully bounded case, granular
  ranges read 1,246 postings and curve terms read 4, returning the same 2 rows.
  Debug CPU timings are not used to update the Release timing tables above.
