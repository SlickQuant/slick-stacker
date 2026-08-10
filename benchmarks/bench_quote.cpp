// Copyright 2026 Slick Quant
// SPDX-License-Identifier: MIT

#include <benchmark/benchmark.h>
#include <slick/stacker/stacker.hpp>

#include "bench_support.hpp"

using namespace bench_support;
using slick::stacker::side_t;

namespace {

using stacker_type = slick::stacker::stacker<bench_executor, side_t::buy>;

constexpr price_t k_base = 1'000'000;
constexpr price_t k_tick = 25;

}  // namespace

// The common case by a wide margin: the strategy re-asserts the quote it
// already has. Nothing should be sent and almost nothing should be computed.
static void BM_RequoteUnchanged(benchmark::State& state) {
    bench_executor exec;
    auto cfg = make_config(static_cast<int>(state.range(0)));
    stacker_type st{exec, cfg};
    st.quote(k_base, 25);
    st.reconcile();
    exec.drain(st);
    st.reconcile();
    exec.drain(st);

    for (auto _ : state) {
        st.quote(k_base, 25);
        st.reconcile();
        benchmark::DoNotOptimize(exec.pending.size());
    }
    state.counters["messages"] = benchmark::Counter(
        static_cast<double>(exec.next_id - 1), benchmark::Counter::kAvgThreads);
}
BENCHMARK(BM_RequoteUnchanged)->Arg(1)->Arg(4)->Arg(8)->Arg(16);

// Only the top level changes, so the ladder behind it must not be touched.
static void BM_RequoteTopSizeOnly(benchmark::State& state) {
    bench_executor exec;
    auto cfg = make_config(static_cast<int>(state.range(0)));
    stacker_type st{exec, cfg};
    st.quote(k_base, 25);
    st.reconcile();
    exec.drain(st);

    qty_t size = 25;
    for (auto _ : state) {
        size = size == 25 ? 26 : 25;
        st.quote(k_base, size);
        st.reconcile();
        exec.drain(st);
    }
}
BENCHMARK(BM_RequoteTopSizeOnly)->Arg(1)->Arg(4)->Arg(8)->Arg(16);

// The quote follows the market a tick at a time. This is the cost that matters
// on a fast instrument, and the pass that the reprice logic exists to make
// cheap in messages as well as in cycles.
static void BM_WalkOneTick(benchmark::State& state) {
    bench_executor exec;
    auto cfg = make_config(static_cast<int>(state.range(0)));
    stacker_type st{exec, cfg};
    st.quote(k_base, 25);
    st.reconcile();
    exec.drain(st);

    bool up = true;
    std::uint64_t messages = 0;
    std::uint64_t steps = 0;
    for (auto _ : state) {
        up = !up;
        st.quote(up ? k_base + k_tick : k_base, 25);
        st.reconcile();
        messages += exec.pending.size();
        exec.drain(st);
        ++steps;
    }
    state.counters["msgs/step"] =
        benchmark::Counter(static_cast<double>(messages) / static_cast<double>(steps ? steps : 1));
}
BENCHMARK(BM_WalkOneTick)->Arg(1)->Arg(4)->Arg(8)->Arg(16);

// A jump large enough that no level survives: the whole stack is rebuilt.
static void BM_JumpWholeStack(benchmark::State& state) {
    bench_executor exec;
    auto cfg = make_config(static_cast<int>(state.range(0)));
    stacker_type st{exec, cfg};
    st.quote(k_base, 25);
    st.reconcile();
    exec.drain(st);

    bool high = false;
    for (auto _ : state) {
        high = !high;
        st.quote(high ? k_base + 40 * k_tick : k_base, 25);
        st.reconcile();
        exec.drain(st);
    }
}
BENCHMARK(BM_JumpWholeStack)->Arg(1)->Arg(4)->Arg(8)->Arg(16);

static void BM_PullAndRebuild(benchmark::State& state) {
    bench_executor exec;
    auto cfg = make_config(static_cast<int>(state.range(0)));
    stacker_type st{exec, cfg};
    st.quote(k_base, 25);
    st.reconcile();
    exec.drain(st);

    for (auto _ : state) {
        st.pull();
        st.reconcile();
        exec.drain(st);
        st.quote(k_base, 25);
        st.reconcile();
        exec.drain(st);
    }
}
BENCHMARK(BM_PullAndRebuild)->Arg(1)->Arg(4)->Arg(8)->Arg(16);

// Slicing multiplies the order count without changing the level count, so this
// separates per-order cost from per-level cost.
static void BM_WalkWithSlicing(benchmark::State& state) {
    bench_executor exec;
    auto cfg = make_config(4);
    cfg.max_order_qty = state.range(0);
    stacker_type st{exec, cfg};
    st.quote(k_base, 40);
    st.reconcile();
    exec.drain(st);

    bool up = true;
    for (auto _ : state) {
        up = !up;
        st.quote(up ? k_base + k_tick : k_base, 40);
        st.reconcile();
        exec.drain(st);
    }
    state.counters["orders"] = benchmark::Counter(static_cast<double>(st.live_order_count()));
}
BENCHMARK(BM_WalkWithSlicing)->Arg(40)->Arg(10)->Arg(5);

BENCHMARK_MAIN();
