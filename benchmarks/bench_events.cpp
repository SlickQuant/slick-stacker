// Copyright 2026 Slick Quant
// SPDX-License-Identifier: MIT

#include <benchmark/benchmark.h>
#include <slick/stacker/stacker.hpp>

#include <vector>

#include "bench_support.hpp"

using namespace bench_support;
using slick::stacker::side_t;

namespace {

constexpr price_t k_base = 1'000'000;

/// Build the stack for `quote_qty` and return the identifiers of the orders it
/// placed whose price satisfies `keep_order`.
template <class Executor, class Pred>
std::vector<typename Executor::order_id_t> build(
    Executor& exec, slick::stacker::stacker<Executor, side_t::buy>& st, qty_t quote_qty,
    Pred keep_order) {
    st.quote(k_base, quote_qty);
    st.reconcile();
    std::vector<typename Executor::order_id_t> ids;
    ids.reserve(exec.pending.size());
    for (const auto& m : exec.pending) {
        if (m.kind == Executor::k_place && keep_order(m.price)) {
            ids.push_back(m.id);
        }
    }
    exec.drain(st);
    return ids;
}

constexpr auto k_any_order = [](price_t) { return true; };

stacker_config event_config(int levels, qty_t slice) {
    auto cfg = make_config(levels);
    cfg.stack_qty = 100;
    cfg.max_order_qty = slice;
    cfg.max_orders_per_level = 64;
    return cfg;
}

}  // namespace

// `on_queue_position` is the purest measure of the event-routing path: look the
// identifier up and record the new position. The gate is off, so nothing else
// happens -- everything else an event handler does is on top of this. The
// position changes on every call, so the unchanged-value early return is never
// what is being measured.
template <class Executor>
static void routing(benchmark::State& state) {
    Executor exec;
    slick::stacker::stacker<Executor, side_t::buy> st{exec, event_config(8, 10)};
    const auto ids = build(exec, st, 10, k_any_order);
    if (ids.empty()) {
        state.SkipWithError("no orders were placed");
        return;
    }

    std::size_t i = 0;
    qty_t pos = 0;
    for (auto _ : state) {
        st.on_queue_position(ids[i], ++pos);
        i = (i + 1) == ids.size() ? 0 : i + 1;
        keep(st, exec);
    }
    state.counters["orders"] = benchmark::Counter(static_cast<double>(ids.size()));
}

/// Identifier hashed into an open-addressing table.
static void BM_RouteEvent_Hashed(benchmark::State& state) { routing<bench_executor>(state); }
BENCHMARK(BM_RouteEvent_Hashed);

/// Identifier carrying the slot index in the executor's own order record: no
/// hash, no extra cache line.
static void BM_RouteEvent_UserData(benchmark::State& state) { routing<bench_executor_ud>(state); }
BENCHMARK(BM_RouteEvent_UserData);

// Fills are the highest-rate event that changes state, and the only one that
// has to touch level accounting on every occurrence.
//
// The orders are made deliberately large and filled a unit at a time, so none
// of them completes inside the measurement. Rebuilding the stack mid-run would
// need PauseTiming, whose own cost is two orders of magnitude larger than the
// thing being measured.
static void BM_OnFilled(benchmark::State& state) {
    bench_executor exec;
    auto cfg = event_config(8, 1'000'000);
    cfg.stack_qty = 1'000'000;
    slick::stacker::stacker<bench_executor, side_t::buy> st{exec, cfg};
    const auto ids = build(exec, st, 1'000'000, k_any_order);
    if (ids.empty()) {
        state.SkipWithError("no orders were placed");
        return;
    }

    std::size_t i = 0;
    for (auto _ : state) {
        st.on_filled(ids[i], 1, k_base);
        i = (i + 1) == ids.size() ? 0 : i + 1;
        keep(st, exec);
    }
    state.counters["orders"] = benchmark::Counter(static_cast<double>(ids.size()));
}
BENCHMARK(BM_OnFilled);

// A burst of events followed by one reconcile is the shape the API is built
// around: the whole point of the handlers not sending is that N events cost one
// recalculation, not N.
//
// The events have to be ones the next reconcile actually depends on, or there
// is no recalculation to amortise. So the queue-gap gate is on and never met:
// every rung wants 100, holds one order of 10, and is held back waiting for the
// market to queue up behind it. Each event moves the queue position of one of
// those held orders, which marks the stacker dirty, and the reconcile then runs
// every pass over the band and re-checks the gate at each rung -- sending
// nothing, so the state is the same at the start of every iteration.
static void BM_EventBatchThenReconcile(benchmark::State& state) {
    const auto batch = static_cast<std::size_t>(state.range(0));
    bench_executor exec;
    auto cfg = event_config(8, 10);
    cfg.queue_gap = 1'000'000;
    slick::stacker::stacker<bench_executor, side_t::buy> st{exec, cfg};
    const auto held = build(exec, st, 10, [](price_t px) { return px != k_base; });
    if (held.empty()) {
        state.SkipWithError("no rung orders were placed");
        return;
    }

    const auto sent = exec.sent;
    std::size_t next = 0;
    qty_t pos = 0;
    for (auto _ : state) {
        for (std::size_t i = 0; i < batch; ++i) {
            st.on_queue_position(held[next], ++pos);
            next = (next + 1) == held.size() ? 0 : next + 1;
        }
        st.reconcile();
        exec.drain(st);
        keep(st, exec);
    }
    report_messages(state, exec, sent);
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(batch));
}
BENCHMARK(BM_EventBatchThenReconcile)->Arg(1)->Arg(8)->Arg(32);

BENCHMARK_MAIN();
