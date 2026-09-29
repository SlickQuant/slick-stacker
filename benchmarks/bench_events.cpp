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

/// An order the stack placed, with the price it rests at.
template <class Executor>
struct placed {
    typename Executor::order_id_t id;
    price_t price;
};

/// Build the stack for `quote_qty` and return the orders it placed whose price
/// satisfies `keep_order`.
template <class Executor, class Pred>
std::vector<placed<Executor>> build(Executor& exec,
                                    slick::stacker::stacker<Executor, side_t::buy>& st,
                                    qty_t quote_qty, Pred keep_order) {
    st.quote(k_base, quote_qty);
    st.reconcile();
    std::vector<placed<Executor>> orders;
    orders.reserve(exec.pending.size());
    for (const auto& m : exec.pending) {
        if (m.kind == Executor::k_place && keep_order(m.price)) {
            orders.push_back({m.id, m.price});
        }
    }
    exec.drain(st);
    return orders;
}

constexpr auto k_any_order = [](price_t) { return true; };

stacker_config event_config(int levels, qty_t slice) {
    auto cfg = make_config(levels);
    cfg.stack_qty = 100;
    cfg.max_order_qty = slice;
    cfg.max_orders_per_level = 64;
    return cfg;
}

/// Resting size large enough that no order completes however many unit fills a
/// run delivers. Rebuilding the stack mid-run would need PauseTiming, whose own
/// cost is two orders of magnitude larger than the thing being measured, and an
/// order that finished would turn every later fill into a failed lookup.
constexpr qty_t k_bottomless = 1'000'000'000'000;

stacker_config fill_config() {
    auto cfg = event_config(8, k_bottomless);
    cfg.stack_qty = k_bottomless;
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
    const auto orders = build(exec, st, 10, k_any_order);
    if (orders.empty()) {
        state.SkipWithError("no orders were placed");
        return;
    }

    std::size_t i = 0;
    qty_t pos = 0;
    for (auto _ : state) {
        st.on_queue_position(orders[i].id, ++pos);
        i = (i + 1) == orders.size() ? 0 : i + 1;
        keep(st, exec);
    }
    state.counters["orders"] = benchmark::Counter(static_cast<double>(orders.size()));
}

/// Identifier hashed into an open-addressing table.
static void BM_RouteEvent_Hashed(benchmark::State& state) { routing<bench_executor>(state); }
BENCHMARK(BM_RouteEvent_Hashed);

/// Identifier carrying the slot index in the executor's own order record: no
/// hash, no extra cache line.
static void BM_RouteEvent_UserData(benchmark::State& state) { routing<bench_executor_ud>(state); }
BENCHMARK(BM_RouteEvent_UserData);

// The market-data feeds behind the queue-gap gate at the levels it is holding
// back, with the gate staying shut -- the common case at a busy level, and one
// that must cost no reconcile. Every rung wants 100 and holds one order of 10;
// the gap asked for is far more than the book ever shows, so no update opens
// it. What is measured is the handler deciding that, on every call.
template <class Feed>
static void gate_shut_feed(benchmark::State& state, Feed feed) {
    bench_executor exec;
    auto cfg = event_config(8, 10);
    cfg.queue_gap = 1'000'000;
    slick::stacker::stacker<bench_executor, side_t::buy> st{exec, cfg};
    const auto held = build(exec, st, 10, [](price_t px) { return px != k_base; });
    if (held.empty()) {
        state.SkipWithError("no rung orders were placed");
        return;
    }
    // Settle the acknowledgements `build` played back, then give every held
    // order a queue position and every rung a book, so the gate is evaluated
    // in full rather than refused for want of data.
    st.reconcile();
    exec.drain(st);
    for (const auto& o : held) {
        st.on_book_level(o.price, 500);
        st.on_queue_position(o.id, 100);
    }
    if (st.dirty()) {
        state.SkipWithError("the gate opened during setup");
        return;
    }

    std::size_t i = 0;
    qty_t n = 0;
    for (auto _ : state) {
        feed(st, held[i], ++n);
        i = (i + 1) == held.size() ? 0 : i + 1;
        keep(st, exec);
    }
    if (st.dirty()) {
        state.SkipWithError("a gate-shut update marked the stacker dirty");
    }
    state.counters["levels"] = benchmark::Counter(static_cast<double>(held.size()));
}

/// Our place in the queue moves; the book behind us stays short of the gap.
static void BM_QueueFeed_GateShut(benchmark::State& state) {
    gate_shut_feed(state, [](auto& st, const auto& o, qty_t n) {
        st.on_queue_position(o.id, 100 + (n % 400));
    });
}
BENCHMARK(BM_QueueFeed_GateShut);

/// The book at our price moves, never by enough to open the gap.
static void BM_BookFeed_GateShut(benchmark::State& state) {
    gate_shut_feed(state, [](auto& st, const auto& o, qty_t n) {
        st.on_book_level(o.price, 500 + (n % 400));
    });
}
BENCHMARK(BM_BookFeed_GateShut);

// Fills are the highest-rate event that changes state, and the only one that
// has to touch level accounting on every occurrence.
//
// Each fill carries the price of the order it hits, as a venue's would: the
// fill consumes the target of the level at that price, so feeding one price
// for every order would drive every fill's target update into the top level.
static void BM_OnFilled(benchmark::State& state) {
    bench_executor exec;
    slick::stacker::stacker<bench_executor, side_t::buy> st{exec, fill_config()};
    const auto orders = build(exec, st, k_bottomless, k_any_order);
    if (orders.empty()) {
        state.SkipWithError("no orders were placed");
        return;
    }

    std::size_t i = 0;
    for (auto _ : state) {
        st.on_filled(orders[i].id, 1, orders[i].price);
        i = (i + 1) == orders.size() ? 0 : i + 1;
        keep(st, exec);
    }
    state.counters["orders"] = benchmark::Counter(static_cast<double>(orders.size()));
}
BENCHMARK(BM_OnFilled);

// A burst of events followed by one reconcile is the shape the API is built
// around: the whole point of the handlers not sending is that N events cost one
// recalculation, not N.
//
// The events have to be ones the next reconcile actually depends on, or there
// is no recalculation to amortise. So they are fills, spread across every
// level of the stack: each one marks the stacker dirty, and the reconcile then
// runs every pass over the band to re-decide each level. With `refill_on_fill`
// off a fill takes the level's target down with its resting quantity, so the
// reconcile finds nothing to send and the state is the same at the start of
// every iteration.
static void BM_EventBatchThenReconcile(benchmark::State& state) {
    const auto batch = static_cast<std::size_t>(state.range(0));
    bench_executor exec;
    slick::stacker::stacker<bench_executor, side_t::buy> st{exec, fill_config()};
    const auto orders = build(exec, st, k_bottomless, k_any_order);
    if (orders.empty()) {
        state.SkipWithError("no orders were placed");
        return;
    }

    const auto sent = exec.sent;
    std::size_t next = 0;
    for (auto _ : state) {
        for (std::size_t i = 0; i < batch; ++i) {
            st.on_filled(orders[next].id, 1, orders[next].price);
            next = (next + 1) == orders.size() ? 0 : next + 1;
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
