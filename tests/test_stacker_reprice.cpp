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
    cfg.levels = 0;
    cfg.ack_required = false;
    cfg.prefer_modify = true;
    return cfg;
}

using buy_harness = harness<side_t::buy>;
using sell_harness = harness<side_t::sell>;

}  // namespace

// Repricing an order costs one message where cancelling and re-sending costs
// two, and keeps the order slot. On a stack that walks with the market this is
// the difference between one message per tick and two.
TEST(StackerReprice, WalkingTheQuoteCostsOneMessagePerTick) {
    buy_harness h{base_cfg()};
    h.quote(1000, 20);
    h.ack_all();

    const auto mark = h.exec.mark();
    h.quote(1010, 20);
    h.ack_all();

    EXPECT_EQ(h.exec.total(mark), 1u);
    EXPECT_EQ(h.exec.count(mock_executor::kind::modify, mark), 1u);
    EXPECT_EQ(h.exec.count(mock_executor::kind::cancel, mark), 0u);
    EXPECT_EQ(h.exec.count(mock_executor::kind::place, mark), 0u);
    EXPECT_EQ(h.working(1010), 20);
    EXPECT_EQ(h.working(1000), 0);
    EXPECT_EQ(h.st.live_order_count(), 1u) << "the same order followed the quote";
    EXPECT_CONSISTENT(h);
}

TEST(StackerReprice, DisabledFallsBackToCancelAndReplace) {
    auto cfg = base_cfg();
    cfg.prefer_modify = false;
    buy_harness h{cfg};
    h.quote(1000, 20);
    h.ack_all();

    const auto mark = h.exec.mark();
    h.quote(1010, 20);
    h.settle();

    EXPECT_EQ(h.exec.count(mock_executor::kind::modify, mark), 0u);
    EXPECT_EQ(h.exec.count(mock_executor::kind::cancel, mark), 1u);
    EXPECT_EQ(h.exec.count(mock_executor::kind::place, mark), 1u);
    EXPECT_EQ(h.working(1010), 20);
    EXPECT_EQ(h.working(1000), 0);
    EXPECT_CONSISTENT(h);
}

TEST(StackerReprice, WorksWhenTheQuoteBacksAway) {
    buy_harness h{base_cfg()};
    h.quote(1000, 20);
    h.ack_all();

    const auto mark = h.exec.mark();
    h.quote(990, 20);
    h.ack_all();

    EXPECT_EQ(h.exec.total(mark), 1u) << "surplus in front, deficit behind: same one message";
    EXPECT_EQ(h.exec.count(mock_executor::kind::modify, mark), 1u);
    EXPECT_EQ(h.working(990), 20);
    EXPECT_EQ(h.working(1000), 0);
    EXPECT_CONSISTENT(h);
}

TEST(StackerReprice, SellSideWalksToo) {
    sell_harness h{base_cfg()};
    h.quote(1000, 20);
    h.ack_all();

    const auto mark = h.exec.mark();
    h.quote(1010, 20);
    h.ack_all();
    EXPECT_EQ(h.exec.count(mock_executor::kind::modify, mark), 1u);
    EXPECT_EQ(h.working(1010), 20);
    EXPECT_EQ(h.working(1000), 0);
    EXPECT_CONSISTENT(h);
}

TEST(StackerReprice, LadderShiftNeverCancels) {
    auto cfg = base_cfg();
    cfg.levels = 3;
    cfg.stack_qty = 10;
    buy_harness h{cfg};
    h.quote(1000, 20);
    h.ack_all();

    const auto mark = h.exec.mark();
    h.quote(1010, 20);
    h.settle();

    EXPECT_EQ(h.exec.count(mock_executor::kind::cancel, mark), 0u)
        << "the level that dropped off the bottom should be repriced, not cancelled";
    EXPECT_EQ(h.working(1010), 20);
    EXPECT_EQ(h.working(1000), 10);
    EXPECT_EQ(h.working(990), 10);
    EXPECT_EQ(h.working(980), 10);
    EXPECT_EQ(h.working(970), 0);
    EXPECT_CONSISTENT(h);
}

// A reprice sends the order to the back of the destination's queue whatever its
// size, so the extra quantity rides across for free. Growing the order in the
// same message saves the separate new order that topping the level up
// afterwards would otherwise need.
TEST(StackerReprice, GrowsTheMovedOrderToFillTheDestination) {
    auto cfg = base_cfg();
    cfg.levels = 3;
    cfg.stack_qty = 10;
    buy_harness h{cfg};

    h.quote(1000, 20);
    h.ack_all();
    ASSERT_EQ(h.working(970), 10) << "the rung that is about to fall off the bottom";

    const auto mark = h.exec.mark();
    h.quote(1010, 20);
    h.settle();

    // The 10 at 970 moves to 1010 and becomes 20 on the way; 1000 is trimmed
    // from 20 to 10. Nothing is placed and nothing is cancelled.
    EXPECT_EQ(h.exec.count(mock_executor::kind::modify, mark), 2u);
    EXPECT_EQ(h.exec.count(mock_executor::kind::place, mark), 0u);
    EXPECT_EQ(h.exec.count(mock_executor::kind::cancel, mark), 0u);

    EXPECT_EQ(h.working(1010), 20);
    EXPECT_EQ(h.orders_at(1010), 1u) << "one order, not a moved 10 plus a fresh 10";
    EXPECT_EQ(h.working(1000), 10);
    EXPECT_EQ(h.working(990), 10);
    EXPECT_EQ(h.working(980), 10);
    EXPECT_EQ(h.working(970), 0);
    EXPECT_EQ(h.st.live_order_count(), 4u);
    EXPECT_CONSISTENT(h);
}

TEST(StackerReprice, GrowthIsCappedByMaxOrderQty) {
    auto cfg = base_cfg();
    cfg.levels = 3;
    cfg.stack_qty = 10;
    cfg.max_order_qty = 15;
    buy_harness h{cfg};

    h.quote(1000, 20);
    h.ack_all();
    ASSERT_EQ(h.orders_at(1000), 2u) << "20 slices into 15 + 5";
    const auto orders_before = h.st.live_order_count();

    const auto mark = h.exec.mark();
    h.quote(1010, 20);
    h.settle();

    EXPECT_EQ(h.working(1010), 20);
    EXPECT_EQ(h.orders_at(1010), 2u) << "no single order may exceed 15";

    // The rung falling off the bottom grows to the 15 cap on its way up, and
    // the pass then finds a second spare order -- the 5-lot left over from
    // slicing the old top level -- and moves that too. Nothing is placed and
    // nothing is cancelled, so the order count is unchanged.
    EXPECT_EQ(h.exec.count(mock_executor::kind::place, mark), 0u);
    EXPECT_EQ(h.exec.count(mock_executor::kind::cancel, mark), 0u);
    EXPECT_EQ(h.st.live_order_count(), orders_before);
    EXPECT_EQ(h.working(1000), 10);
    EXPECT_CONSISTENT(h);
}

TEST(StackerReprice, MovedOrderShrinksWhenTheDestinationWantsLess) {
    auto cfg = base_cfg();
    cfg.levels = 1;
    cfg.stack_qty = 4;
    buy_harness h{cfg};

    h.quote(1000, 20);
    h.ack_all();
    ASSERT_EQ(h.working(990), 4);

    // Drop the quote to nothing so the top order becomes surplus, while the
    // rung below it still wants its 4.
    const auto mark = h.exec.mark();
    h.quote(1010, 0);
    h.settle();

    EXPECT_EQ(h.working(1010), 0);
    EXPECT_EQ(h.working(1000), 4) << "resized down on the way, not cancelled and replaced";
    EXPECT_EQ(h.exec.count(mock_executor::kind::place, mark), 0u);
    EXPECT_CONSISTENT(h);
}

// An order carrying more than its level can spare is trimmed where it stands
// rather than dragged to another price. Moving it would take the source level
// below its own target.
TEST(StackerReprice, OrderLargerThanTheSurplusIsTrimmedInPlace) {
    auto cfg = base_cfg();
    cfg.levels = 1;
    cfg.stack_qty = 10;
    buy_harness h{cfg};
    h.quote(1000, 20);
    h.ack_all();
    ASSERT_EQ(h.working(1000), 20);
    ASSERT_EQ(h.working(990), 10);

    const auto mark = h.exec.mark();
    h.st.set_stack_qty(30);
    h.st.quote(1000, 5);
    h.settle();

    EXPECT_EQ(h.working(1000), 5);
    EXPECT_EQ(h.working(990), 30);
    EXPECT_EQ(h.exec.count(mock_executor::kind::cancel, mark), 0u);
    EXPECT_EQ(h.orders_at(1000), 1u) << "the front order kept its place in the queue";
    EXPECT_CONSISTENT(h);
}

TEST(StackerReprice, RespectsMaxOrdersPerLevel) {
    auto cfg = base_cfg();
    cfg.levels = 2;
    cfg.stack_qty = 10;
    cfg.max_order_qty = 5;
    cfg.max_orders_per_level = 1;
    buy_harness h{cfg};

    h.quote(1000, 20);
    h.settle();
    EXPECT_EQ(h.orders_at(1000), 1u);
    EXPECT_EQ(h.orders_at(990), 1u);
    EXPECT_EQ(h.working(1000), 5) << "one order of five is all the level may hold";
    ASSERT_CONSISTENT(h);

    h.quote(1010, 20);
    h.settle();
    EXPECT_LE(h.orders_at(1010), 1u);
    EXPECT_LE(h.orders_at(1000), 1u);
    EXPECT_CONSISTENT(h);
}

// If nothing is movable the pass must still finish. The guard is that whichever
// cursor cannot make progress advances, so the loop is bounded by the width of
// the band however the deltas are arranged.
TEST(StackerReprice, TerminatesWhenNothingCanBeMoved) {
    auto cfg = base_cfg();
    cfg.levels = 3;
    cfg.stack_qty = 10;
    cfg.min_order_qty = 100;  // no candidate can satisfy the minimum
    buy_harness h{cfg};

    h.quote(1000, 20);
    h.reconcile();
    h.quote(1010, 20);
    h.reconcile();
    h.quote(980, 40);
    h.reconcile();
    SUCCEED() << "reconcile returned";
    EXPECT_CONSISTENT(h);
}

// Each move used to restart the scan of the source level at its tail, so an
// order at the tail that could not move was tried again for every order moved
// from in front of it. For an order the executor refuses, that meant a fresh
// refused modify each time. The scan now resumes where it left off.
TEST(StackerReprice, UnmovableTailOrderIsTriedOncePerPass) {
    auto cfg = base_cfg();
    cfg.max_order_qty = 10;
    buy_harness h{cfg};

    h.quote(1000, 40);
    h.ack_all();
    ASSERT_EQ(h.orders_at(1000), 4u);
    const auto tail = h.exec.log[3].id;
    h.exec.refuse_modify_id = tail;

    const auto mark = h.exec.mark();
    h.quote(1010, 40);

    EXPECT_EQ(h.exec.refused, 1u) << "the refused tail order was retried";
    EXPECT_EQ(h.exec.count(mock_executor::kind::modify, mark), 3u)
        << "the three orders in front of it still moved";
    EXPECT_EQ(h.working(1010), 40);
    EXPECT_EQ(h.working(1000), 0);
    EXPECT_CONSISTENT(h);
}

// A destination that cannot take a reprice -- here because it would cross --
// is the destination cursor's problem. It used to exhaust the source cursor
// instead, so nothing further down the band was repriced either.
TEST(StackerReprice, BlockedDestinationDoesNotStopRepricesBehindIt) {
    auto cfg = base_cfg();
    cfg.levels = 2;
    cfg.stack_qty = 10;
    buy_harness h{cfg};

    h.quote(1000, 25);
    h.ack_all();
    ASSERT_EQ(h.working(980), 10);

    h.st.on_opposite_top(1020);
    const auto mark = h.exec.mark();
    h.quote(1020, 25);
    h.ack_all();

    EXPECT_EQ(h.working(1020), 0) << "the quote itself would cross";
    EXPECT_EQ(h.working(1010), 10);
    EXPECT_EQ(h.working(1000), 10);
    EXPECT_EQ(h.working(990), 0);
    EXPECT_EQ(h.working(980), 0);
    EXPECT_EQ(h.exec.count(mock_executor::kind::place, mark), 0u)
        << "1010 is filled by moving the order from 980, not by a new one";
    EXPECT_CONSISTENT(h);
}

TEST(StackerReprice, RepeatedWalkKeepsOneOrderAlive) {
    buy_harness h{base_cfg()};
    h.quote(1000, 20);
    h.ack_all();
    const auto first = h.last_placed();

    for (price_t px = 1010; px <= 1200; px += 10) {
        h.quote(px, 20);
        h.ack_all();
        ASSERT_CONSISTENT(h);
        ASSERT_EQ(h.st.live_order_count(), 1u) << "at " << px;
    }
    EXPECT_EQ(h.working(1200), 20);
    EXPECT_EQ(h.last_placed(), first) << "no new order was ever needed";
    EXPECT_EQ(h.exec.count(mock_executor::kind::place), 1u);
    EXPECT_EQ(h.exec.count(mock_executor::kind::modify), 20u);
    EXPECT_CONSISTENT(h);
}

// The ring is indexed by depth modulo its capacity, so what has to stay inside
// the capacity is the *live* band, not the absolute price range. A quote can
// therefore walk arbitrarily far without the grid ever being rebuilt, as long
// as it does not leave orders strewn across more than `level_capacity` ticks.
TEST(StackerReprice, LongWalkNeverRebuildsTheGrid) {
    auto cfg = base_cfg();
    cfg.levels = 2;
    cfg.stack_qty = 10;
    buy_harness h{cfg};

    h.quote(1000, 20);
    h.ack_all();
    const auto rebases = h.st.rebase_count();

    // test_traits::level_capacity is 64 ticks; walk a hundred past it.
    for (price_t px = 1010; px <= 2000; px += 10) {
        h.quote(px, 20);
        h.ack_all();
        ASSERT_CONSISTENT(h) << "at " << px;
    }
    h.settle(16);

    EXPECT_EQ(h.st.rebase_count(), rebases) << "a narrow band never outgrows the ring";
    EXPECT_EQ(h.working(2000), 20);
    EXPECT_EQ(h.working(1990), 10);
    EXPECT_EQ(h.working(1980), 10);
    EXPECT_EQ(h.exec.venue_qty_at(2000), 20);
    EXPECT_EQ(h.exec.venue_qty_at(1990), 10);
    EXPECT_EQ(h.exec.venue_qty_at(1980), 10);
    for (price_t px = 1000; px <= 1970; px += 10) {
        ASSERT_EQ(h.exec.venue_qty_at(px), 0) << "left behind at " << px;
    }
    EXPECT_CONSISTENT(h);
}

// A price that is not on the tick grid means the grid itself is wrong -- the
// instrument re-ticked, or the feed is lying. The stacker flattens and rebuilds
// around the new price rather than guessing.
TEST(StackerReprice, OffGridQuoteRebuildsTheGrid) {
    auto cfg = base_cfg();
    cfg.levels = 2;
    cfg.stack_qty = 10;
    buy_harness h{cfg};

    h.quote(1000, 20);
    h.ack_all();
    const auto rebases = h.st.rebase_count();
    ASSERT_EQ(h.exec.venue_qty_at(1000), 20);

    h.quote(1005, 20);  // half a tick off the grid
    h.settle(16);

    EXPECT_GT(h.st.rebase_count(), rebases);
    EXPECT_EQ(h.working(1005), 20);
    EXPECT_EQ(h.working(995), 10);
    EXPECT_EQ(h.working(985), 10);
    EXPECT_EQ(h.exec.venue_qty_at(1000), 0) << "the old stack must be flattened";
    EXPECT_EQ(h.exec.venue_qty_at(990), 0);
    EXPECT_CONSISTENT(h);
}

TEST(StackerReprice, TickSizeChangeRebuildsTheGrid) {
    auto cfg = base_cfg();
    cfg.levels = 2;
    cfg.stack_qty = 10;
    buy_harness h{cfg};

    h.quote(1000, 20);
    h.ack_all();
    ASSERT_EQ(h.exec.venue_qty_at(990), 10);

    cfg.tick_size = 25;
    h.st.configure(cfg);
    h.settle(16);

    EXPECT_EQ(h.working(1000), 20);
    EXPECT_EQ(h.working(975), 10);
    EXPECT_EQ(h.working(950), 10);
    EXPECT_EQ(h.exec.venue_qty_at(990), 0) << "orders on the old grid must be withdrawn";
    EXPECT_EQ(h.exec.venue_qty_at(980), 0);
    EXPECT_CONSISTENT(h);
}
