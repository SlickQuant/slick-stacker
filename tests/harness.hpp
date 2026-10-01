// Copyright 2026 Slick Quant
// SPDX-License-Identifier: MIT

#pragma once

#include <gtest/gtest.h>
#include <slick/stacker/stacker.hpp>

#include <cstdint>
#include <vector>

#include "mock_executor.hpp"

namespace testing_support {

using slick::stacker::cancel_reason_t;
using slick::stacker::k_no_qty_limit;
using slick::stacker::reject_reason_t;
using slick::stacker::stacker_config;

/// Small sizes keep `validate()` -- which is O(capacity) -- fast enough to run
/// after every single step of every test.
struct test_traits {
    static constexpr std::uint16_t level_capacity = 64;
    static constexpr std::uint16_t max_orders = 32;
    static constexpr std::uint16_t max_levels = 16;
};

/// Drives a stacker against `mock_executor` and plays the venue back at it.
template <slick::stacker::side_t Side, class Executor = mock_executor, class Traits = test_traits>
class harness {
public:
    using stacker_type = slick::stacker::stacker<Executor, Side, Traits>;
    using order_id_t = typename Executor::order_id_t;

    explicit harness(const stacker_config& cfg) : st(exec, cfg) {}

    // -- driving -------------------------------------------------------------

    void quote(price_t px, qty_t q) {
        st.quote(px, q);
        st.reconcile();
    }

    void reconcile() { st.reconcile(); }

    /// Deliver the venue's response to every message sent since the last call.
    /// Acks land in the order they were sent, which is what a real session does
    /// on a single connection.
    void ack_all() { ack_all_adjusting(Executor::invalid_order_id, 0, 0); }

    /// As `ack_all()`, except the venue books `id` at `price`/`qty` rather than
    /// what was asked for -- the way a venue rounds a quantity down or slides a
    /// price when it accepts an order or a modify.
    void ack_all_adjusting(order_id_t id, price_t price, qty_t qty) {
        while (cursor_ < exec.log.size()) {
            auto m = exec.log[cursor_++];
            const bool adjust = m.id == id && m.type != mock_executor::kind::cancel;
            if (m.type == mock_executor::kind::modify) {
                exec.confirm_modify(m.id);
            }
            if (adjust) {
                m.price = price;
                m.qty = qty;
                exec.orders[m.id].price = price;
                exec.orders[m.id].qty = qty;
            }
            switch (m.type) {
                case mock_executor::kind::place:
                    st.on_accepted(m.id, m.price, m.qty);
                    break;
                case mock_executor::kind::modify:
                    st.on_replaced(m.id, m.price, m.qty);
                    break;
                case mock_executor::kind::cancel: {
                    auto& o = exec.orders[m.id];
                    const qty_t left = o.qty - o.filled;
                    o.live = false;
                    st.on_canceled(m.id, left, cancel_reason_t::requested);
                    break;
                }
            }
        }
    }

    /// Advance past the pending messages without responding, so the next
    /// `ack_all()` only handles what comes after. Used to leave orders in
    /// flight on purpose.
    void ignore_pending() { cursor_ = exec.log.size(); }

    /// Run to a fixed point: reconcile, ack everything, repeat. Real sessions
    /// converge over several round trips whenever the venue requires an
    /// acknowledgement before the next action.
    void settle(int max_rounds = 8) {
        for (int i = 0; i < max_rounds; ++i) {
            const std::size_t before = exec.log.size();
            st.reconcile();
            ack_all();
            if (exec.log.size() == before && !st.dirty()) {
                return;
            }
        }
        st.reconcile();
        ack_all();
    }

    void fill(order_id_t id, qty_t q) {
        auto& o = exec.orders[id];
        o.filled += q;
        if (o.filled >= o.qty) {
            o.live = false;
        }
        st.on_filled(id, q, o.price);
    }

    void reject_new(order_id_t id, reject_reason_t r = reject_reason_t::terminal) {
        exec.orders[id].live = false;
        st.on_rejected(id, r);
    }

    void reject_modify(order_id_t id, reject_reason_t r = reject_reason_t::terminal) {
        exec.drop_pending_modify(id);
        st.on_modify_rejected(id, r);
    }

    void reject_cancel(order_id_t id, reject_reason_t r = reject_reason_t::terminal) {
        st.on_cancel_rejected(id, r);
    }

    void unsolicited_cancel(order_id_t id, qty_t q) {
        auto& o = exec.orders[id];
        o.qty -= q;
        if (o.qty <= o.filled) {
            o.live = false;
        }
        st.on_canceled(id, q, cancel_reason_t::unsolicited);
    }

    // -- inspection ----------------------------------------------------------

    [[nodiscard]] qty_t working(price_t px) const { return st.working_at(px); }
    [[nodiscard]] qty_t target(price_t px) const { return st.target_at(px); }
    [[nodiscard]] qty_t acked(price_t px) const { return st.acked_at(px); }
    [[nodiscard]] std::uint16_t orders_at(price_t px) const { return st.order_count_at(px); }

    /// Ids of the messages sent since `from`, oldest first.
    [[nodiscard]] std::vector<order_id_t> ids_since(std::size_t from) const {
        std::vector<order_id_t> out;
        for (std::size_t i = from; i < exec.log.size(); ++i) {
            out.push_back(exec.log[i].id);
        }
        return out;
    }

    [[nodiscard]] order_id_t last_placed() const {
        for (std::size_t i = exec.log.size(); i-- > 0;) {
            if (exec.log[i].type == mock_executor::kind::place) {
                return exec.log[i].id;
            }
        }
        return Executor::invalid_order_id;
    }

    Executor exec;
    stacker_type st;

private:
    std::size_t cursor_ = 0;
};

/// Assert the stacker's internal accounting still adds up. Called after every
/// meaningful step so a divergence is attributed to the operation that caused
/// it rather than surfacing many steps later.
#define EXPECT_CONSISTENT(h) EXPECT_TRUE((h).st.validate()) << "stacker invariants violated"
#define ASSERT_CONSISTENT(h) ASSERT_TRUE((h).st.validate()) << "stacker invariants violated"

}  // namespace testing_support
