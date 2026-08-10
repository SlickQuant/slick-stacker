# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[semantic versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

First release.

### Added

- `stacker<Executor, Side, Traits>` — single-sided order stacker. Takes one
  price and quantity and maintains a configured ladder of orders behind it.
- Order entry as a compile-time policy, constrained by the `OrderExecutor`
  concept. Optional `flush()`, `can_act()` and user-data hooks are detected and
  used when present.
- Ladder shape: `levels`, `level_gap_ticks`, a uniform `stack_qty` or a per-rung
  `qty_profile`, and `max_level_qty`.
- Order sizing: `max_order_qty`, `min_order_qty`, `qty_increment` and
  `max_orders_per_level`.
- Reprice pass — surplus orders are moved into levels that need quantity rather
  than cancelled and re-sent, and are resized to what the destination wants on
  the way. A ladder walking one tick costs two messages at any depth and places
  no new orders.
- Deferred actions for venues that will not act on an unacknowledged order
  (`ack_required`), including free reinstatement of a deferred cancel when the
  target returns.
- `max_inflight_modifies` — chained replaces for venues that accept a modify
  against an order whose previous one is still unanswered, removing a round trip
  between the strategy deciding and the venue hearing about it. Defaults to 1,
  which is the wait-for-the-answer behaviour.
- Queue-gap gating (`queue_gap`, fed by `on_queue_position`), slack retention
  (`slack_levels`) and quantity hysteresis (`qty_hysteresis`).
- Crossing protection via `on_opposite_top`.
- Overfill guard: adds are suppressed while a reduction the executor refused to
  send is outstanding.
- `stacker_pair` — both sides plus the guard that keeps them off each other's
  prices.
- `function_executor` — a `std::function`-backed adapter for non-hot-path use.
- `validate()` — rebuilds level accounting from the underlying orders and
  compares against the incrementally maintained values.
- 137 unit tests and two benchmark suites.
