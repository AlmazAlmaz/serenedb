---
title: Multidimensional Search
sidebar_position: 10
split: headings
---

import SqlLogicTest from "@site/src/components/SqlLogicTest";

Use `curve(...)` to index a numeric tuple together with native inverted-index terms, bounded
coverings and an exact scalar check of the candidates.

## Numeric tuples

A curve index is an expression index over `ROW(...)`, with two to eight
coordinates. The expression can combine several columns with different types.
Query the index by name and use the same tuple expression in
`sdb_box_contains(point, lower, upper)`:

<SqlLogicTest id="sql/indexes/inverted/curve-search/points" />

The endpoints are inclusive. A NULL component of a bound leaves that side of
that dimension unconstrained. An inverted interval matches nothing. A wholly
NULL point or bound makes the predicate NULL. A NULL coordinate contributes
UNKNOWN when constrained and is ignored when unconstrained. Combining dimensions
follows SQL AND: FALSE takes precedence over UNKNOWN. Thus partial boxes and
negated boxes retain the per-column comparison semantics. Missing coordinates
use a placeholder key for candidate generation and are checked against their
stored validity during recheck. A wholly NULL tuple produces no terms; its
`IS NULL` behavior is unchanged.

Coordinates support signed integers through BIGINT, unsigned integers through
UBIGINT, FLOAT/DOUBLE, DATE, TIMESTAMP and TIMESTAMPTZ. Integers retain all 64 bits;
they are not converted to floating point. Signed widths can be mixed within a
dimension, as can unsigned widths and FLOAT/DOUBLE. Bounds must use the same
family as their coordinate, except that integer bounds are accepted for FLOAT
and DOUBLE coordinates; cast other bounds explicitly. Dates, timestamps and
timestamps with time zones are distinct families. Decimal, 128-bit integers and
other timestamp resolutions require an explicit conversion before indexing.
Floating point values compare as in SQL: NaN equals NaN and sorts above
positive infinity, and negative zero equals positive zero.

The scalar predicate also works on an ordinary table. Against an index, a
positive predicate with constant bounds uses the term index, followed by the
same scalar predicate. Negated predicates, OR expressions and nonconstant bounds
retain scalar evaluation; an approximation is never negated and treated as exact.
A box with an open side, a NULL component in either bound, is evaluated by the
scalar predicate too: every curve cell constrains all dimensions at once, so the
index cannot prune by the dimensions that are left unconstrained.

## Options and bounds

The opclass requires parentheses. Its options are stored with the index:

| Option | Default | Meaning |
| --- | --- | --- |
| `curve` | `'morton'` | `'morton'` or `'hilbert'`. |
| `max_level` | `64` | 0 through 64 coordinate bits used for cells. Smaller values reduce depth and precision. Level 0 is the whole domain. |
| `max_cells` | `64` | 1 through 4096 covering cells per query. |
| `level_step` | `1 + 4 / dimensions` | 1 through `1 + 12 / dimensions`. A point is indexed at every `level_step`-th level and at `max_level`. |

There are no declared spatial bounds. Each axis uses the complete ordered
64-bit representation of its type. For doubles this is an order-preserving
IEEE-754 encoding, so levels measure representable bits rather than a fixed
physical distance. This avoids clipping coordinates or rebuilding an index when
the domain grows. Different columns can select different depth and cell budgets.

Covering considers both interior and boundary cells. When refining a cell would
exceed the budget, the parent remains in the cover. Coverage is widened rather
than truncated, so reducing the budget cannot lose matches.
The cell budget is not a limit on returned documents: a coarse cover
can require checking every row.

Numeric points are always indexed at the configured maximum depth, so a numeric
box needs only one query term per covering cell when every level is indexed.

A point writes a term for each indexed level: every `level_step`-th level and
`max_level`, `max_level / level_step + 2` terms at most. Most of the levels hold
no information for real data -- integer columns share their top levels in every
row, and doubles reach one row per cell long before level 64 -- so the step is
the main control over index size and build time. The query cover is computed at
full precision; a covering cell between two indexed levels is replaced by its
descendants on the next indexed level that intersect the box, at most
`2^(dimensions * (level_step - 1))` terms per cell. The candidates are therefore
never coarser than with `level_step = 1`, only the query carries more terms, and
every query term is a term dictionary lookup. The default keeps that expansion
at no more than 16 terms per cell: 3 for two dimensions, 2 for three and four,
and 1 from five dimensions up. On one million three-dimensional points a step
of 2 builds the index in 56 to 59% of the time of a step of 1 with queries at
most 12% slower, while a step of 3 builds faster still but turns a 64-cell
cover into up to 1,233 query terms.

## Choosing a curve

Morton interleaves the ordered coordinate bits. Hilbert transforms the axes
before interleaving them. Both use the same dyadic cell hierarchy and covering
policy here, so changing the curve does not change which candidate documents
are selected. Hilbert has extra encoding cost; better ordering locality does
not automatically imply fewer postings for this term representation.

A curve index pays off for boxes that are selective in several dimensions at
once, where each column's range alone matches many rows: on one million
independent three-dimensional points such boxes run 12 to 22 times faster than
intersecting per-column ranges. Correlated data narrows the gap, small boxes on
correlated data and two-dimensional data at that size are a tie, and a box with
an open side is evaluated by the scalar predicate. The index costs five to six
times the build time and four times the size of a per-column index. Benchmark
the application's own distributions before choosing a budget or a step.

For a streaming index scan, `rows_scanned` in JSON profiling counts hits emitted
by the posting filter after zonemap skipping and before
the column recheck. Compare this with the final result count to measure native
candidate pruning. It is not a count of compressed bytes or individual posting
iterator advances, and a metadata-only count can avoid streaming entirely.

The [lindel registry entry](https://duckdb.org/community_extensions/extensions/lindel)
and [implementation](https://github.com/Query-farm/lindel/tree/v1.5/duckdb_lindel_rust)
provide the prior-art reference for scalar Morton/Hilbert encoding. SereneDB
does not load lindel: term generation, coverings,
budgets and candidate selection belong to the native index.
