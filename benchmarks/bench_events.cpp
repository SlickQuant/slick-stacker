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

/// Build a stack of `orders` working orders and return their identifiers.
template <class Executor>
std::vector<typename Executor::order_id_t> build(
    Executor& exec, slick::stacker::stacker<Executor, side_t::buy>& st, int orders) {
    st.quote(k_base, 10);
    st.reconcile();
    std::vector<typename Executor::order_id_t> ids;
    ids.reserve(static_cast<std::size_t>(orders));
    for (const auto& m : exec.pending) {
        if (m.kind == Executor::k_place) {
            ids.push_back(m.id);
        }
    }
    exec.drain(st);
    return ids;
}

stacker_config event_config(int levels, qty_t slice) {
    auto cfg = make_config(levels);
    cfg.stack_qty = 100;
    cfg.max_order_qty = slice;
    cfg.max_orders_per_level = 64;
    return cfg;
}

}  // namespace

// `on_queue_position` is the purest measure of the event-routing path: look the
// identifier up, write two fields, set a flag. Everything else an event handler
// does is on top of this.
template <class Executor>
static void routing(benchmark::State& state) {
    Executor exec;
    slick::stacker::stacker<Executor, side_t::buy> st{exec, event_config(8, 10)};
    auto ids = build(exec, st, 0);
    if (ids.empty()) {
        state.SkipWithError("no orders were placed");
        return;
    }

    std::size_t i = 0;
    for (auto _ : state) {
        st.on_queue_position(ids[i], 100);
        i = (i + 1) == ids.size() ? 0 : i + 1;
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
    st.quote(k_base, 1'000'000);
    st.reconcile();

    std::vector<bench_executor::order_id_t> ids;
    ids.reserve(64);
    for (const auto& m : exec.pending) {
        if (m.kind == bench_executor::k_place) {
            ids.push_back(m.id);
        }
    }
    exec.drain(st);
    if (ids.empty()) {
        state.SkipWithError("no orders were placed");
        return;
    }

    std::size_t i = 0;
    for (auto _ : state) {
        st.on_filled(ids[i], 1, k_base);
        i = (i + 1) == ids.size() ? 0 : i + 1;
    }
    state.counters["orders"] = benchmark::Counter(static_cast<double>(ids.size()));
}
BENCHMARK(BM_OnFilled);

// A burst of events followed by one reconcile is the shape the API is built
// around: the whole point of the handlers not sending is that N events cost one
// recalculation, not N.
static void BM_EventBatchThenReconcile(benchmark::State& state) {
    const auto batch = static_cast<std::size_t>(state.range(0));
    bench_executor exec;
    auto cfg = event_config(8, 10);
    cfg.refill_on_fill = true;
    slick::stacker::stacker<bench_executor, side_t::buy> st{exec, cfg};
    st.quote(k_base, 100);
    st.reconcile();

    std::vector<bench_executor::order_id_t> ids;
    ids.reserve(1024);
    for (const auto& m : exec.pending) {
        if (m.kind == bench_executor::k_place) {
            ids.push_back(m.id);
        }
    }
    exec.drain(st);

    for (auto _ : state) {
        for (std::size_t i = 0; i < batch && i < ids.size(); ++i) {
            st.on_queue_position(ids[i], static_cast<qty_t>(50 + i));
        }
        st.reconcile();
        exec.drain(st);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(batch));
}
BENCHMARK(BM_EventBatchThenReconcile)->Arg(1)->Arg(8)->Arg(32);

BENCHMARK_MAIN();
