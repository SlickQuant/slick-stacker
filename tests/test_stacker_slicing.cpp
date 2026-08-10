// Copyright 2026 Slick Quant
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "harness.hpp"

using namespace testing_support;
using slick::stacker::k_null_price;
using slick::stacker::side_t;

namespace {

stacker_config base_cfg() {
    stacker_config cfg;
    cfg.tick_size = 10;
    cfg.levels = 1;
    cfg.stack_qty = 30;
    cfg.ack_required = false;
    return cfg;
}

using buy_harness = harness<side_t::buy>;
using sell_harness = harness<side_t::sell>;

}  // namespace

TEST(StackerSlicing, MaxOrderQtySplitsALevelIntoSeveralOrders) {
    auto cfg = base_cfg();
    cfg.max_order_qty = 10;
    buy_harness h{cfg};

    h.quote(1000, 25);
    h.ack_all();

    EXPECT_EQ(h.working(1000), 25);
    EXPECT_EQ(h.orders_at(1000), 3u) << "10 + 10 + 5";
    EXPECT_EQ(h.exec.log[0].qty, 10);
    EXPECT_EQ(h.exec.log[1].qty, 10);
    EXPECT_EQ(h.exec.log[2].qty, 5);
    EXPECT_CONSISTENT(h);
}

TEST(StackerSlicing, MaxOrdersPerLevelCapsTheLevel) {
    auto cfg = base_cfg();
    cfg.levels = 0;
    cfg.max_order_qty = 10;
    cfg.max_orders_per_level = 2;
    buy_harness h{cfg};

    h.quote(1000, 25);
    h.settle();
    EXPECT_EQ(h.orders_at(1000), 2u);
    EXPECT_EQ(h.working(1000), 20) << "the level is left short rather than over-ordered";
    EXPECT_EQ(h.target(1000), 25);
    EXPECT_CONSISTENT(h);
}

TEST(StackerSlicing, MinOrderQtyLeavesAnOddLotUnquoted) {
    auto cfg = base_cfg();
    cfg.levels = 0;
    cfg.max_order_qty = 10;
    cfg.min_order_qty = 4;
    buy_harness h{cfg};

    h.quote(1000, 23);  // 10 + 10 + 3, and the 3 is below the minimum
    h.settle();
    EXPECT_EQ(h.orders_at(1000), 2u);
    EXPECT_EQ(h.working(1000), 20);
    EXPECT_CONSISTENT(h);
}

TEST(StackerSlicing, QtyIncrementRoundsOrdersDown) {
    auto cfg = base_cfg();
    cfg.levels = 0;
    cfg.qty_increment = 5;
    buy_harness h{cfg};

    h.quote(1000, 23);
    h.settle();
    EXPECT_EQ(h.working(1000), 20) << "23 rounds down to a multiple of 5";
    EXPECT_EQ(h.orders_at(1000), 1u);
    EXPECT_CONSISTENT(h);
}

TEST(StackerSlicing, MinOrderQtyStopsAReductionSplittingIntoAnOddLot) {
    auto cfg = base_cfg();
    cfg.levels = 0;
    cfg.min_order_qty = 10;
    buy_harness h{cfg};

    h.quote(1000, 25);
    h.ack_all();
    ASSERT_EQ(h.orders_at(1000), 1u);

    // Trimming to 5 would leave an order below the minimum, so the order goes
    // entirely and the level is left empty rather than illegal.
    h.quote(1000, 5);
    h.settle();
    EXPECT_EQ(h.acked(1000), 0);
    EXPECT_EQ(h.orders_at(1000), 0u);
    EXPECT_CONSISTENT(h);
}

// --- queue-gap gating ------------------------------------------------------
//
// Stacking several of our own orders back to back at one price puts them all
// behind the same queue. The gate holds off the next order at a level until
// enough market quantity has arrived behind the last one.

TEST(StackerSlicing, QueueGapHoldsBackTheSecondOrderAtALevel) {
    auto cfg = base_cfg();
    cfg.levels = 1;
    cfg.stack_qty = 30;
    cfg.max_order_qty = 10;
    cfg.queue_gap = 50;
    buy_harness h{cfg};

    h.quote(1000, 10);
    h.ack_all();

    EXPECT_EQ(h.orders_at(1000), 1u);
    EXPECT_EQ(h.orders_at(990), 1u) << "the first order at a level is never gated";
    EXPECT_EQ(h.working(990), 10);
    ASSERT_CONSISTENT(h);

    // Reconciling again changes nothing: there is no queue behind us yet.
    const auto mark = h.exec.mark();
    h.settle();
    EXPECT_EQ(h.exec.count(mock_executor::kind::place, mark), 0u);
    EXPECT_EQ(h.working(990), 10);
    ASSERT_CONSISTENT(h);
}

TEST(StackerSlicing, QueueGapReleasesOnceTheMarketQueuesBehindUs) {
    auto cfg = base_cfg();
    cfg.levels = 1;
    cfg.stack_qty = 30;
    cfg.max_order_qty = 10;
    cfg.queue_gap = 50;
    buy_harness h{cfg};

    h.quote(1000, 10);
    h.ack_all();
    ASSERT_EQ(h.working(990), 10);

    const auto id = h.exec.log[1].id;  // the order resting at 990
    ASSERT_EQ(h.exec.orders[id].price, 990);

    // 200 in the book, 100 of it ahead of us: 100 behind, comfortably over the
    // 50 required.
    h.st.on_book_level(990, 200);
    h.st.on_queue_position(id, 100);
    h.reconcile();
    h.ack_all();
    EXPECT_EQ(h.orders_at(990), 2u);
    EXPECT_EQ(h.working(990), 20);
    ASSERT_CONSISTENT(h);

    // The new order has no queue position of its own yet, so the level is
    // gated again until one arrives.
    const auto mark = h.exec.mark();
    h.settle();
    EXPECT_EQ(h.exec.count(mock_executor::kind::place, mark), 0u);
    EXPECT_CONSISTENT(h);
}

TEST(StackerSlicing, QueueGapNotMetKeepsTheLevelAsItIs) {
    auto cfg = base_cfg();
    cfg.levels = 1;
    cfg.stack_qty = 30;
    cfg.max_order_qty = 10;
    cfg.queue_gap = 50;
    buy_harness h{cfg};

    h.quote(1000, 10);
    h.ack_all();
    const auto id = h.exec.log[1].id;

    // Only 20 behind us: not enough.
    h.st.on_book_level(990, 120);
    h.st.on_queue_position(id, 100);
    h.settle();
    EXPECT_EQ(h.orders_at(990), 1u);
    EXPECT_EQ(h.working(990), 10);
    EXPECT_CONSISTENT(h);
}

TEST(StackerSlicing, QueueGapDoesNotGateTheTopLevel) {
    auto cfg = base_cfg();
    cfg.levels = 0;
    cfg.max_order_qty = 10;
    cfg.queue_gap = 1000;
    buy_harness h{cfg};

    h.quote(1000, 30);
    h.ack_all();
    EXPECT_EQ(h.orders_at(1000), 3u) << "the quote itself is never held back";
    EXPECT_EQ(h.working(1000), 30);
    EXPECT_CONSISTENT(h);
}

TEST(StackerSlicing, ZeroQueueGapDisablesTheGate) {
    auto cfg = base_cfg();
    cfg.levels = 1;
    cfg.stack_qty = 30;
    cfg.max_order_qty = 10;
    cfg.queue_gap = 0;
    buy_harness h{cfg};

    h.quote(1000, 10);
    h.ack_all();
    EXPECT_EQ(h.orders_at(990), 3u);
    EXPECT_EQ(h.working(990), 30);
    EXPECT_CONSISTENT(h);
}

// --- crossing protection ---------------------------------------------------

TEST(StackerSlicing, WillNotPlaceThroughTheOppositeSide) {
    auto cfg = base_cfg();
    cfg.levels = 2;
    cfg.stack_qty = 10;
    buy_harness h{cfg};

    h.st.on_opposite_top(1000);
    h.quote(1000, 25);
    h.settle();
    EXPECT_EQ(h.working(1000), 0) << "a buy at the offer would cross";
    EXPECT_EQ(h.working(990), 10) << "the levels behind it are still fine";
    EXPECT_EQ(h.working(980), 10);
    ASSERT_CONSISTENT(h);

    // The offer lifts, and the quote can go in.
    h.st.on_opposite_top(1010);
    h.settle();
    EXPECT_EQ(h.working(1000), 25);
    EXPECT_CONSISTENT(h);
}

TEST(StackerSlicing, SellSideCrossingProtection) {
    auto cfg = base_cfg();
    cfg.levels = 1;
    cfg.stack_qty = 10;
    sell_harness h{cfg};

    h.st.on_opposite_top(1000);  // best bid
    h.quote(1000, 25);
    h.settle();
    EXPECT_EQ(h.working(1000), 0) << "an offer at the bid would cross";
    EXPECT_EQ(h.working(1010), 10);
    ASSERT_CONSISTENT(h);

    h.st.on_opposite_top(990);
    h.settle();
    EXPECT_EQ(h.working(1000), 25);
    EXPECT_CONSISTENT(h);
}

TEST(StackerSlicing, NullOppositeTopDisablesCrossingProtection) {
    auto cfg = base_cfg();
    cfg.levels = 0;
    buy_harness h{cfg};

    h.st.on_opposite_top(1000);
    h.quote(1000, 25);
    h.settle();
    ASSERT_EQ(h.working(1000), 0);

    h.st.on_opposite_top(k_null_price);
    h.settle();
    EXPECT_EQ(h.working(1000), 25);
    EXPECT_CONSISTENT(h);
}

TEST(StackerSlicing, BookUpdatesOutsideTheBandAreIgnored) {
    buy_harness h{base_cfg()};
    h.quote(1000, 25);
    h.ack_all();
    ASSERT_CONSISTENT(h);

    h.st.on_book_level(500, 1000);   // nowhere near the stack
    h.st.on_book_level(1005, 1000);  // off the tick grid
    EXPECT_CONSISTENT(h);
    EXPECT_EQ(h.working(1000), 25);
}
