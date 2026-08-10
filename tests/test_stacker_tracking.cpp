// Copyright 2026 Slick Quant
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "harness.hpp"

using namespace testing_support;
using slick::stacker::side_t;

namespace {

stacker_config base_cfg() {
    stacker_config cfg;
    cfg.tick_size = 10;
    cfg.levels = 2;
    cfg.stack_qty = 10;
    cfg.ack_required = false;
    return cfg;
}

using buy_harness = harness<side_t::buy>;

/// Identifier of the order the stacker placed at `px`, taken from the message
/// log rather than from the stacker's internals.
mock_executor::order_id_t placed_at(const mock_executor& exec, price_t px) {
    for (const auto& m : exec.log) {
        if (m.type == mock_executor::kind::place && m.price == px) {
            return m.id;
        }
    }
    return mock_executor::invalid_order_id;
}

}  // namespace

TEST(StackerTracking, AcceptMovesQuantityFromInFlightToAcked) {
    buy_harness h{base_cfg()};
    h.st.quote(1000, 25);
    h.reconcile();

    EXPECT_EQ(h.working(1000), 25);
    EXPECT_EQ(h.acked(1000), 0) << "nothing is acknowledged until the venue says so";
    ASSERT_CONSISTENT(h);

    h.ack_all();
    EXPECT_EQ(h.working(1000), 25);
    EXPECT_EQ(h.acked(1000), 25);
    EXPECT_CONSISTENT(h);
}

TEST(StackerTracking, PartialFillLeavesTheOrderWorking) {
    buy_harness h{base_cfg()};
    h.quote(1000, 25);
    h.ack_all();
    const auto id = placed_at(h.exec, 1000);
    ASSERT_NE(id, mock_executor::invalid_order_id);

    h.fill(id, 10);
    EXPECT_EQ(h.acked(1000), 15);
    EXPECT_EQ(h.st.live_order_count(), 3u) << "a partial fill must not retire the order";
    EXPECT_EQ(h.orders_at(1000), 1u);
    EXPECT_CONSISTENT(h);
}

// The caller authorised 25 once. Having filled 10 of it, the stack must sit at
// 15 and wait for the caller to decide whether to put the size back -- not
// quietly re-arm it.
TEST(StackerTracking, FillConsumesTheTargetByDefault) {
    buy_harness h{base_cfg()};
    h.quote(1000, 25);
    h.ack_all();
    const auto id = placed_at(h.exec, 1000);

    const auto mark = h.exec.mark();
    h.fill(id, 10);
    h.settle();

    EXPECT_EQ(h.target(1000), 15);
    EXPECT_EQ(h.working(1000), 15);
    EXPECT_EQ(h.exec.total(mark), 0u) << "no replenishment message may be sent";
    EXPECT_CONSISTENT(h);
}

TEST(StackerTracking, RefillOnFillReplenishesTheLevel) {
    auto cfg = base_cfg();
    cfg.refill_on_fill = true;
    buy_harness h{cfg};
    h.quote(1000, 25);
    h.ack_all();
    const auto id = placed_at(h.exec, 1000);

    const auto mark = h.exec.mark();
    h.fill(id, 10);
    h.settle();

    EXPECT_EQ(h.target(1000), 25);
    EXPECT_EQ(h.working(1000), 25);
    EXPECT_EQ(h.exec.count(mock_executor::kind::place, mark), 1u);
    EXPECT_CONSISTENT(h);
}

TEST(StackerTracking, FullFillRetiresTheOrder) {
    buy_harness h{base_cfg()};
    h.quote(1000, 25);
    h.ack_all();
    const auto id = placed_at(h.exec, 1000);

    h.fill(id, 25);
    EXPECT_EQ(h.acked(1000), 0);
    EXPECT_EQ(h.target(1000), 0);
    EXPECT_EQ(h.orders_at(1000), 0u);
    EXPECT_EQ(h.st.live_order_count(), 2u);
    EXPECT_CONSISTENT(h);
}

TEST(StackerTracking, FillsAcrossSeveralLevels) {
    buy_harness h{base_cfg()};
    h.quote(1000, 25);
    h.ack_all();

    h.fill(placed_at(h.exec, 1000), 25);
    h.fill(placed_at(h.exec, 990), 4);
    EXPECT_CONSISTENT(h);

    EXPECT_EQ(h.working(1000), 0);
    EXPECT_EQ(h.working(990), 6);
    EXPECT_EQ(h.working(980), 10);
    EXPECT_EQ(h.st.live_order_count(), 2u);
}

// Venues can execute against an order before its acknowledgement comes back.
// The stacker must not let the level's confirmed quantity go negative in the
// window between the two.
TEST(StackerTracking, FillArrivingBeforeTheAcknowledgement) {
    buy_harness h{base_cfg()};
    h.st.quote(1000, 25);
    h.reconcile();
    const auto id = placed_at(h.exec, 1000);
    ASSERT_NE(id, mock_executor::invalid_order_id);

    h.st.on_filled(id, 10, 1000);
    EXPECT_GE(h.acked(1000), 0);
    ASSERT_CONSISTENT(h);

    h.ack_all();
    EXPECT_EQ(h.acked(1000), 15);
    EXPECT_EQ(h.working(1000), 15);
    EXPECT_CONSISTENT(h);
}

TEST(StackerTracking, UnsolicitedFullCancelRemovesTheOrder) {
    buy_harness h{base_cfg()};
    h.quote(1000, 25);
    h.ack_all();
    const auto id = placed_at(h.exec, 1000);

    h.unsolicited_cancel(id, 25);
    EXPECT_EQ(h.acked(1000), 0);
    EXPECT_EQ(h.orders_at(1000), 0u);
    EXPECT_CONSISTENT(h);

    // The target still stands, so the next pass puts the quantity back.
    h.settle();
    EXPECT_EQ(h.working(1000), 25);
    EXPECT_CONSISTENT(h);
}

TEST(StackerTracking, UnsolicitedPartialCancelKeepsTheRemainder) {
    buy_harness h{base_cfg()};
    h.quote(1000, 25);
    h.ack_all();
    const auto id = placed_at(h.exec, 1000);

    h.unsolicited_cancel(id, 10);
    EXPECT_EQ(h.acked(1000), 15);
    EXPECT_EQ(h.orders_at(1000), 1u) << "the order is trimmed, not retired";
    ASSERT_CONSISTENT(h);

    h.settle();
    EXPECT_EQ(h.working(1000), 25);
    EXPECT_CONSISTENT(h);
}

TEST(StackerTracking, ModifyAcknowledgedAtADifferentLevel) {
    buy_harness h{base_cfg()};
    h.quote(1000, 25);
    h.ack_all();

    // Improve by a tick; the stack shifts and at least one order is repriced.
    h.quote(1010, 25);
    h.settle();

    EXPECT_EQ(h.working(1010), 25);
    EXPECT_EQ(h.working(1000), 10);
    EXPECT_EQ(h.working(990), 10);
    EXPECT_EQ(h.working(980), 0);
    EXPECT_CONSISTENT(h);
}

TEST(StackerTracking, EventsForUnknownIdentifiersAreIgnored) {
    buy_harness h{base_cfg()};
    h.quote(1000, 25);
    h.ack_all();
    ASSERT_CONSISTENT(h);

    constexpr mock_executor::order_id_t bogus = 999999;
    h.st.on_accepted(bogus, 1000, 5);
    h.st.on_replaced(bogus, 1000, 5);
    h.st.on_filled(bogus, 5, 1000);
    h.st.on_canceled(bogus, 5);
    h.st.on_rejected(bogus, reject_reason_t::terminal);
    h.st.on_modify_rejected(bogus, reject_reason_t::terminal);
    h.st.on_cancel_rejected(bogus, reject_reason_t::terminal);
    h.st.on_queue_position(bogus, 100);

    EXPECT_CONSISTENT(h);
    EXPECT_EQ(h.working(1000), 25);
}

TEST(StackerTracking, ZeroAndNegativeFillsAreIgnored) {
    buy_harness h{base_cfg()};
    h.quote(1000, 25);
    h.ack_all();
    const auto id = placed_at(h.exec, 1000);

    h.st.on_filled(id, 0, 1000);
    h.st.on_filled(id, -5, 1000);
    EXPECT_EQ(h.acked(1000), 25);
    EXPECT_CONSISTENT(h);
}

TEST(StackerTracking, EventBatchCostsOneReconcile) {
    buy_harness h{base_cfg()};
    h.quote(1000, 25);
    h.ack_all();

    const auto mark = h.exec.mark();
    // A burst of fills across the stack. None of the handlers may send.
    h.fill(placed_at(h.exec, 1000), 5);
    h.fill(placed_at(h.exec, 1000), 5);
    h.fill(placed_at(h.exec, 990), 3);
    h.fill(placed_at(h.exec, 980), 2);
    EXPECT_EQ(h.exec.total(mark), 0u) << "event handlers must never send";
    EXPECT_TRUE(h.st.dirty());

    h.reconcile();
    EXPECT_FALSE(h.st.dirty());
    EXPECT_CONSISTENT(h);
}

TEST(StackerTracking, FlushIsCalledOncePerReconcile) {
    buy_harness h{base_cfg()};
    EXPECT_EQ(h.exec.flushes, 0u);
    h.st.quote(1000, 25);
    h.reconcile();
    EXPECT_EQ(h.exec.flushes, 1u);

    h.reconcile();  // clean: no work, no flush
    EXPECT_EQ(h.exec.flushes, 1u);
}

// The executor user-data slot replaces the identifier hash with a direct index.
// It is a different code path through every event handler, so the tracking
// behaviour is re-checked against it end to end.
TEST(StackerTracking, UserDataLookupPathMatches) {
    harness<side_t::buy, mock_executor_ud> h{base_cfg()};
    h.quote(1000, 25);
    h.ack_all();
    EXPECT_EQ(h.working(1000), 25);
    EXPECT_EQ(h.working(990), 10);
    ASSERT_CONSISTENT(h);

    const auto id = placed_at(h.exec, 1000);
    h.fill(id, 10);
    EXPECT_EQ(h.acked(1000), 15);
    EXPECT_EQ(h.target(1000), 15);
    ASSERT_CONSISTENT(h);

    h.quote(990, 25);
    h.settle();
    EXPECT_EQ(h.working(1000), 0);
    EXPECT_EQ(h.working(990), 25);
    EXPECT_EQ(h.working(980), 10);
    EXPECT_EQ(h.working(970), 10);
    EXPECT_CONSISTENT(h);
}

TEST(StackerTracking, OrderPoolExhaustionIsSurvivable) {
    auto cfg = base_cfg();
    cfg.levels = 3;
    cfg.stack_qty = 100;
    cfg.max_order_qty = 1;  // one unit per order: 300 orders wanted, 32 available
    cfg.max_orders_per_level = 200;
    buy_harness h{cfg};

    h.quote(1000, 100);
    EXPECT_LE(h.st.live_order_count(), test_traits::max_orders);
    ASSERT_CONSISTENT(h);

    h.ack_all();
    h.settle();
    EXPECT_LE(h.st.live_order_count(), test_traits::max_orders);
    EXPECT_CONSISTENT(h);

    // Backing right off must still unwind cleanly.
    h.st.pull();
    h.settle(16);
    EXPECT_EQ(h.st.live_order_count(), 0u);
    EXPECT_CONSISTENT(h);
}
