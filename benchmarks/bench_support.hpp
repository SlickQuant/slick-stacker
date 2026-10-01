// Copyright 2026 Slick Quant
// SPDX-License-Identifier: MIT

#pragma once

#include <benchmark/benchmark.h>
#include <slick/stacker/stacker.hpp>

#include <cstdint>
#include <vector>

namespace bench_support {

using slick::stacker::order_type_t;
using slick::stacker::price_t;
using slick::stacker::qty_t;
using slick::stacker::side_t;
using slick::stacker::stacker_config;

/// The thinnest executor that still behaves like one: hands out sequential
/// identifiers and queues what it was asked to do so the benchmark can play the
/// venue back. Nothing here allocates once `pending` has grown.
struct bench_executor {
    using order_id_t = std::uint64_t;
    static constexpr order_id_t invalid_order_id = 0;

    enum : std::uint8_t { k_place, k_modify, k_cancel };

    struct message {
        std::uint8_t kind;
        order_id_t id;
        price_t price;
        qty_t qty;
    };

    bench_executor() { pending.reserve(4096); }

    order_id_t place(side_t, price_t price, qty_t qty, order_type_t) {
        const order_id_t id = next_id++;
        send({k_place, id, price, qty});
        return id;
    }

    bool modify(const order_id_t& id, price_t price, qty_t qty) {
        send({k_modify, id, price, qty});
        return true;
    }

    bool cancel(const order_id_t& id) {
        send({k_cancel, id, 0, 0});
        return true;
    }

    /// Queue a message and fold it into `checksum`, so every message the
    /// stacker decides to send is part of what the benchmark observes.
    void send(const message& m) {
        pending.push_back(m);
        checksum = checksum * 31 + (m.id ^ static_cast<std::uint64_t>(m.price) ^
                                    (static_cast<std::uint64_t>(m.qty) << 32) ^ m.kind);
        ++sent;
    }

    /// Acknowledge everything queued. Draining an acknowledgement can itself
    /// produce a message -- a deferred action firing -- so the loop re-reads
    /// the size and copies each entry rather than holding a reference.
    template <class Stacker>
    void drain(Stacker& st) {
        for (std::size_t i = 0; i < pending.size(); ++i) {
            const message m = pending[i];
            switch (m.kind) {
                case k_place:
                    st.on_accepted(m.id, m.price, m.qty);
                    break;
                case k_modify:
                    st.on_replaced(m.id, m.price, m.qty);
                    break;
                default:
                    st.on_canceled(m.id, 0);
                    break;
            }
        }
        pending.clear();
    }

    std::vector<message> pending;
    order_id_t next_id = 1;
    std::uint64_t checksum = 0;  ///< running hash of every message sent
    std::uint64_t sent = 0;      ///< messages sent since construction
};

/// End-of-iteration sink. Escapes the stacker and the executor's message
/// checksum and clobbers memory, so the optimiser has to assume the stacker's
/// state is read afterwards and can neither drop nor simplify the work the
/// iteration did to it.
template <class Stacker, class Executor>
inline void keep(Stacker& st, Executor& exec) {
    benchmark::DoNotOptimize(st);
    benchmark::DoNotOptimize(exec.checksum);
    benchmark::ClobberMemory();
}

/// Messages per iteration, reported as a counter so a change in what a
/// benchmark actually sends is visible next to a change in its time.
template <class Executor>
inline void report_messages(benchmark::State& state, const Executor& exec,
                            std::uint64_t sent_before) {
    const auto iters = state.iterations() != 0 ? state.iterations() : 1;
    state.counters["msgs/iter"] =
        static_cast<double>(exec.sent - sent_before) / static_cast<double>(iters);
}

/// Adds the optional user-data slot, which replaces the identifier hash with a
/// direct index on every event.
struct bench_executor_ud : bench_executor {
    static constexpr std::uint64_t k_mask = (1u << 16) - 1;

    bench_executor_ud() { user_data.assign(k_mask + 1, 0); }

    void set_order_user_data(const order_id_t& id, std::uint32_t v) { user_data[id & k_mask] = v; }
    [[nodiscard]] std::uint32_t get_order_user_data(const order_id_t& id) const {
        return user_data[id & k_mask];
    }

    std::vector<std::uint32_t> user_data;
};

inline stacker_config make_config(int levels) {
    stacker_config cfg;
    cfg.tick_size = 25;
    cfg.levels = static_cast<std::uint16_t>(levels);
    cfg.stack_qty = 10;
    cfg.ack_required = false;
    cfg.prefer_modify = true;
    return cfg;
}

}  // namespace bench_support
