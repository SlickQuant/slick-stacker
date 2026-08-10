# Performance

Numbers below were measured on an 8-core x86-64 workstation (32 KiB L1d, 512 KiB
L2, 16 MiB L3) with MSVC 19.44 at `/O2`, single-threaded, default `Traits`
(`level_capacity = 256`, `max_orders = 256`). Treat them as shape, not
specification — re-run on your own hardware before designing a budget around
them.

## Reproducing

```sh
cmake -B build -DSLICK_STACKER_BUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target run_benchmarks
```

Make sure `SLICK_STACKER_VALIDATE` is **not** defined. It turns on invariant
checks that are O(capacity) and will dominate every measurement.

## Method

Every figure below is a measured median, not an estimate or a projection.

Run-to-run variance *within* one process is small — repeating each benchmark
eight times gives a coefficient of variation of 1–3%:

```sh
./bench_quote --benchmark_repetitions=8 --benchmark_report_aggregates_only=true
```

Variance *between* separate invocations on a loaded machine is much larger, and
easily swamps a 10% change. Never compare a number from one session against a
number from another. To attribute a change to a code change, build both variants
and run them back to back with repetitions, comparing medians — that is how the
comparison in the last section here was produced.

## Quoting

`Arg` is the number of rungs behind the quote.

| Benchmark | 1 | 4 | 8 | 16 |
| --- | --- | --- | --- | --- |
| `BM_RequoteUnchanged` | 1.9 ns | 1.9 ns | 1.9 ns | 1.9 ns |
| `BM_RequoteTopSizeOnly` | 72 ns | 88 ns | 112 ns | 170 ns |
| `BM_WalkOneTick` | 150 ns | 166 ns | 196 ns | 255 ns |
| `BM_JumpWholeStack` | 341 ns | 589 ns | 952 ns | 1536 ns |
| `BM_PullAndRebuild` | 225 ns | 531 ns | 928 ns | 1739 ns |

`BM_WalkOneTick` includes draining the acknowledgements, and reports
**2 messages per tick regardless of ladder depth** — the reprice pass carries
the ladder along rather than rebuilding it, resizing the moved order to what its
destination wants so no separate order has to be placed behind it.

Re-asserting an unchanged quote is flat in the number of rungs because it does
not touch them: `quote()` compares against the quote already in force and
returns without marking the stacker dirty. Since that is what a strategy does on
most ticks, it is the number worth optimising, and the one worth watching for
regressions.

`BM_JumpWholeStack` is the worst case — a price move large enough that no level
survives, so every rung is cancelled and re-established.

## Slicing

`Arg` is `max_order_qty` against a level target of 40 across 4 rungs, so smaller
values mean more orders for the same shape.

| `max_order_qty` | time | live orders |
| --- | --- | --- |
| 40 | 167 ns | 6 |
| 10 | 330 ns | 8 |
| 5 | 633 ns | 16 |

Cost tracks order count rather than level count, which is what you would want:
the per-level work is amortised and the per-order work is what scales.

## Events

| Benchmark | time |
| --- | --- |
| `BM_RouteEvent_Hashed` | 2.73 ns |
| `BM_RouteEvent_UserData` | 2.00 ns |
| `BM_OnFilled` | 5.7 ns |
| `BM_EventBatchThenReconcile/1` | 34 ns |
| `BM_EventBatchThenReconcile/8` | 50 ns |
| `BM_EventBatchThenReconcile/32` | 107 ns |

`BM_RouteEvent_*` isolates identifier routing: look the order up, write two
fields, set a flag. The user-data variant is the same work with the hash probe
removed — see the README on `set_order_user_data`. About 25% of the routing
cost, which is worth taking if your order records have a spare word.

`BM_EventBatchThenReconcile` is the shape the API is built around. Thirty-two
events plus one reconcile costs 107 ns, against 34 ns for one event plus one
reconcile: the reconcile dominates, and batching amortises it away. This is why
the event handlers never send.

## What resizing on reprice is worth

The reprice pass sizes a moved order to what its destination wants rather than
capping it at what the order already held, which removes the separate order that
would otherwise be needed to top the destination up. Both variants built from
the same source and run back to back, five repetitions each, medians:

| Benchmark | capped at order size | resized to destination | |
| --- | --- | --- | --- |
| `BM_WalkOneTick/1` | 171 ns | 152 ns | −11% |
| `BM_WalkOneTick/4` | 197 ns | 169 ns | −14% |
| `BM_WalkOneTick/8` | 225 ns | 199 ns | −12% |
| `BM_WalkOneTick/16` | 291 ns | 258 ns | −11% |
| `BM_JumpWholeStack/1` | 449 ns | 346 ns | −23% |
| `BM_JumpWholeStack/4` | 898 ns | 592 ns | −34% |
| `BM_JumpWholeStack/8` | 1493 ns | 912 ns | −39% |
| `BM_JumpWholeStack/16` | 2698 ns | 1570 ns | −42% |
| `BM_WalkWithSlicing/40` | 192 ns | 167 ns | −13% |
| `BM_WalkWithSlicing/10` | 339 ns | 333 ns | — |
| `BM_WalkWithSlicing/5` | 639 ns | 638 ns | — |

The gain grows with ladder depth on `JumpWholeStack` because more of the rebuild
becomes order reuse instead of cancel-and-place.

It disappears under heavy slicing, and that is expected rather than
disappointing: with `max_order_qty` at 10 or 5, growth is capped almost
immediately, so a top-up order is needed either way. Resizing on reprice buys
nothing when the configuration already forbids large orders.

## What costs what

- **Level work** is a contiguous scan of the live band. Bounded by the ladder
  plus slack plus recent price travel — typically well under thirty entries.
- **Order work** is index-linked list traversal within a level. No pointer
  chasing between allocations; the whole slot pool is one array.
- **Accounting** is `detach`/`attach` around each mutation: at most three level
  counters touched, on cache lines the pass is already reading.
- **Routing** is one hash probe, or one indexed load with the user-data hook.

Nothing allocates after construction, and there are no locks, atomics or virtual
calls on any path.
