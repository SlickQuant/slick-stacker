# Performance

Treat these numbers as shape, not specification — re-run on your own hardware
before designing a budget around them.

## Environment

| | |
| --- | --- |
| Source | per section — see [Provenance](#provenance) |
| CPU | AMD Ryzen 9 5900HX, 8 cores / 16 threads, 32 KiB L1d, 512 KiB L2 per core, 16 MiB L3 |
| OS | Windows 11 Pro 10.0.26200 |
| Compiler | MSVC 19.44.35215, C++20 |
| Flags | Release: `/MD /O2 /Ob2 /DNDEBUG`, plus `/O2 /W4` from `benchmarks/CMakeLists.txt` |
| Library | google/benchmark v1.8.3 |
| `Traits` | default (`level_capacity = 256`, `max_orders = 256`, `max_levels = 64`) |
| Pinning | one logical CPU (affinity mask `0x10`), `HIGH_PRIORITY_CLASS` |

**The machine was not idle** while these were taken — other processes held it at
50–80% CPU. The figures are therefore upper bounds, and identical code measured
within ±15% of itself from one benchmark to the next (±3% in geometric mean).
Re-run on a quiet machine before quoting an absolute figure.

## Provenance

Each section was measured on the source tree of one commit, unmodified except
where noted. Later commits have not been re-measured, so check out the listed
commit before comparing a figure against your own run.

| Section | Measured on |
| --- | --- |
| [Quoting](#quoting), [Slicing](#slicing) | `78d2b7d` |
| [Events](#events) | `ab3fdeb` |
| [What resizing on reprice is worth](#what-resizing-on-reprice-is-worth) | `9643a24` (initial implementation); the "capped" variant is that tree with the reprice resizing taken out |

## Reproducing

```sh
git checkout <commit from the table above>
cmake -B build -DSLICK_STACKER_BUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target run_benchmarks
```

Make sure `SLICK_STACKER_VALIDATE` is **not** defined. It turns on invariant
checks that are O(capacity) and will dominate every measurement.

## Method

Every figure below is a measured median, not an estimate or a projection. Each
benchmark was run on its own (`--benchmark_filter`), five repetitions of at least
0.05 s, and the best median of three rounds is reported:

```sh
start /wait /high /affinity 10 bench_quote.exe --benchmark_filter=^BM_WalkOneTick/4$ ^
    --benchmark_repetitions=5 --benchmark_min_time=0.05s --benchmark_report_aggregates_only=true
```

Variance *between* separate invocations on a loaded machine easily swamps a 10%
change, so never compare a number from one session against a number from
another. To attribute a change to a code change, build both variants and
alternate them benchmark by benchmark — A, B, A, B — so that load drift lands on
both equally, then compare best medians. `benchmarks/ab_compare.py` does exactly
this given two build directories, and CI runs it on every change against its base
commit.

Every benchmark ends each iteration in `bench_support::keep`, which escapes the
stacker and a running checksum of every message the executor was asked to send,
then clobbers memory. The optimiser therefore has to assume the stacker's state
and the messages are read afterwards, and cannot drop or simplify the measured
work. `msgs/iter` reports what each iteration actually sent, so a change in
behaviour shows up next to a change in time.

## Quoting

`Arg` is the number of rungs behind the quote.

| Benchmark | 1 | 4 | 8 | 16 | msgs/iter |
| --- | --- | --- | --- | --- | --- |
| `BM_RequoteUnchanged` | 5.7 ns | 5.7 ns | 5.8 ns | 5.8 ns | 0 |
| `BM_RequoteTopSizeOnly` | 194 ns | 216 ns | 285 ns | 385 ns | 1 |
| `BM_WalkOneTick` | 347 ns | 482 ns | 446 ns | 579 ns | 2 |
| `BM_JumpWholeStack` | 783 ns | 1348 ns | 2375 ns | 3811 ns | rungs + 1 |
| `BM_PullAndRebuild` | 573 ns | 1319 ns | 2275 ns | 4879 ns | 2 × (rungs + 1) |

`BM_WalkOneTick/4` above `/8` is the noise band described under
[Environment](#environment), not a property of the code.

`BM_WalkOneTick` includes draining the acknowledgements, and sends
**2 messages per tick regardless of ladder depth** — the reprice pass carries
the ladder along rather than rebuilding it, resizing the moved order to what its
destination wants so no separate order has to be placed behind it.

Re-asserting an unchanged quote is flat in the number of rungs because it does
not touch them: `quote()` compares against the quote already in force and
returns without marking the stacker dirty. Since that is what a strategy does on
most ticks, it is the number worth optimising, and the one worth watching for
regressions.

`BM_JumpWholeStack` is the worst case — a price move large enough that no level
survives, so every rung is repriced to its new price.

## Slicing

`Arg` is `max_order_qty` against a level target of 40 across 4 rungs, so smaller
values mean more orders for the same shape.

| `max_order_qty` | time | msgs/iter | live orders |
| --- | --- | --- | --- |
| 40 | 385 ns | 2 | 6 |
| 10 | 783 ns | 4 | 8 |
| 5 | 1803 ns | 8 | 16 |

Cost tracks order count rather than level count, which is what you would want:
the per-level work is amortised and the per-order work is what scales.

`BM_WalkDeepQueue` takes that to its limit: 4 rungs of 40 one-lot orders each,
200 orders live, so every one-tick walk moves 40 orders out of one level. It
runs at **7.0 µs, 40 messages per tick** — about 176 ns per order moved, sent
and acknowledged. The reprice pass walks each source queue once per pass rather
than restarting at its tail after every move, so a queue whose back orders
cannot move (being cancelled, too large for the surplus, or refused by the
executor) costs one look at each of them, not one per order moved.

## Events

| Benchmark | time |
| --- | --- |
| `BM_RouteEvent_Hashed` | 9.1 ns |
| `BM_RouteEvent_UserData` | 6.8 ns |
| `BM_QueueFeed_GateShut` | 17.8 ns |
| `BM_BookFeed_GateShut` | 23.0 ns |
| `BM_OnFilled` | 44.1 ns |
| `BM_EventBatchThenReconcile/1` | 124 ns |
| `BM_EventBatchThenReconcile/8` | 421 ns |
| `BM_EventBatchThenReconcile/32` | 1.44 µs |

This table was measured on `ab3fdeb`, after the rest of the file and in one
session of its own: each benchmark alternated against the previous build, pinned, best median of
four rounds of five repetitions, from google-benchmark's `real_time`. (On
Windows `cpu_time` comes from a clock that ticks every 15.6 ms, which quantizes
short runs to tens of percent; do not compare on it.)

`BM_RouteEvent_*` isolates identifier routing: look the order up and record a
new queue position, with the queue-gap gate off so nothing else happens. The
position changes on every call, so the unchanged-value early return is never
what is measured. The user-data variant is the same work with the hash probe
removed — see the README on `set_order_user_data`.

`BM_QueueFeed_GateShut` and `BM_BookFeed_GateShut` feed the queue-gap gate at
the levels it is holding back, with the gate staying shut — the common case at
a busy level. Each call has to decide that the update does not open the gate,
and mark nothing dirty. Both handlers test the gate's new state first, since
that is what fails, and reach its old state and the level's other conditions
only on the rare update that passes; that took the queue feed from 32 to 18 ns.
The book feed gained nothing measurable from the same reordering. It starts by
turning the price into a ring depth, a 64-bit division by `tick_size`, which
is the likelier cost there.

`BM_OnFilled` fills each order a unit at a time at the price it rests at, as a
venue would, so every fill's target update lands on that order's own level.
The orders are sized so that none completes during a run.

`BM_EventBatchThenReconcile` is the shape the API is built around. Every event
is a fill, spread across all nine levels — an update the next reconcile
genuinely depends on — and the reconcile runs every pass over the band to
re-decide each level. With `refill_on_fill` off a fill takes the target down
with the resting quantity, so every level stays exactly at target and the
reconcile sends nothing. The state is not literally unchanged — each fill adds
to an order's filled quantity and takes one off its level's target — but the
orders are sized so that none completes and no target reaches zero during a
run, so every iteration walks the same levels and orders and reaches the same
decisions. One fill plus one reconcile costs 124 ns,
of which the reconcile is roughly 80; thirty-two fills plus one reconcile cost
1.44 µs, about 45 ns per event — the reconcile has been amortised down to a few
nanoseconds each. This is why the event handlers never send.

Book and queue updates that cannot change the next reconcile — the gate is off,
the value did not change, the level is not being held back, or the gate stays
shut after the update — record the value and do not mark the stacker dirty, so
they never cost a reconcile at all. Only an update that opens the gate does.

## What resizing on reprice is worth

The reprice pass sizes a moved order to what its destination wants rather than
capping it at what the order already held, which removes the separate order that
would otherwise be needed to top the destination up. Both variants built from
the same source and run back to back, five repetitions each, medians.

These were measured at the initial implementation (`9643a24`), with the
benchmark harness of that time (before the state sink described under [Method](#method)), on a
different session from the tables above. Read the relative column only.

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
  plus slack plus recent price travel — typically well under thirty entries, and
  never more than `level_capacity`, since a ladder that does not fit in the ring
  is rejected by `validate` and clamped by the stacker.
- **Order work** is index-linked list traversal within a level. No pointer
  chasing between allocations; the whole slot pool is one array.
- **Accounting** is `detach`/`attach` around each mutation: at most three level
  counters touched, on cache lines the pass is already reading.
- **Routing** is one hash probe, or one indexed load with the user-data hook.

Nothing allocates after construction, and there are no locks, atomics or virtual
calls on any path.
