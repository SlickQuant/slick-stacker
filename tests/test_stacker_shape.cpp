// Copyright 2026 Slick Quant
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <array>

#include "harness.hpp"

using namespace testing_support;
using slick::stacker::side_t;

namespace {

stacker_config base_cfg() {
    stacker_config cfg;
    cfg.tick_size = 10;
    cfg.levels = 3;
    cfg.stack_qty = 10;
    cfg.ack_required = false;  // shape tests converge in a single reconcile
    return cfg;
}

using buy_harness = harness<side_t::buy>;
using sell_harness = harness<side_t::sell>;

}  // namespace

TEST(StackerShape, BuildsQuotePlusLadderBeneath) {
    buy_harness h{base_cfg()};
    h.quote(1000, 25);

    EXPECT_EQ(h.exec.count(mock_executor::kind::place), 4u);
    EXPECT_EQ(h.exec.total(), 4u);
    EXPECT_EQ(h.working(1000), 25);
    EXPECT_EQ(h.working(990), 10);
    EXPECT_EQ(h.working(980), 10);
    EXPECT_EQ(h.working(970), 10);
    EXPECT_EQ(h.working(960), 0);
    ASSERT_CONSISTENT(h);

    h.ack_all();
    EXPECT_EQ(h.acked(1000), 25);
    EXPECT_EQ(h.acked(970), 10);
    EXPECT_CONSISTENT(h);
}

// A sell stack ladders upward. Everything else about it is identical, which is
// the point of indexing levels by depth rather than by price.
TEST(StackerShape, SellStackLaddersUpward) {
    sell_harness h{base_cfg()};
    h.quote(1000, 25);
    h.ack_all();

    EXPECT_EQ(h.working(1000), 25);
    EXPECT_EQ(h.working(1010), 10);
    EXPECT_EQ(h.working(1020), 10);
    EXPECT_EQ(h.working(1030), 10);
    EXPECT_EQ(h.working(990), 0);
    EXPECT_CONSISTENT(h);
}

TEST(StackerShape, RequoteAtSamePriceAndSizeSendsNothing) {
    buy_harness h{base_cfg()};
    h.quote(1000, 25);
    h.ack_all();

    const auto mark = h.exec.mark();
    h.quote(1000, 25);
    h.ack_all();
    EXPECT_EQ(h.exec.total(mark), 0u) << "steady state must be free";
    EXPECT_CONSISTENT(h);
}

TEST(StackerShape, TopSizeChangeTouchesOnlyTheTopLevel) {
    buy_harness h{base_cfg()};
    h.quote(1000, 25);
    h.ack_all();

    auto mark = h.exec.mark();
    h.quote(1000, 40);
    EXPECT_EQ(h.exec.total(mark), 1u);
    EXPECT_EQ(h.exec.log.back().type, mock_executor::kind::place);
    EXPECT_EQ(h.exec.log.back().price, 1000);
    EXPECT_EQ(h.exec.log.back().qty, 15);
    h.ack_all();
    EXPECT_EQ(h.working(1000), 40);
    EXPECT_CONSISTENT(h);

    // Shedding 20 from a level holding 25 + 15 takes two messages whichever way
    // it is done: the back order goes entirely and the front one is trimmed.
    // The trim is a modify rather than a cancel, so the order that keeps its
    // place in the queue is the one that had the best place to begin with.
    mark = h.exec.mark();
    h.quote(1000, 20);
    EXPECT_EQ(h.exec.total(mark), 2u);
    EXPECT_EQ(h.exec.count(mock_executor::kind::cancel, mark), 1u);
    EXPECT_EQ(h.exec.count(mock_executor::kind::modify, mark), 1u);
    EXPECT_EQ(h.exec.count(mock_executor::kind::place, mark), 0u);
    h.ack_all();
    EXPECT_EQ(h.working(1000), 20);
    EXPECT_EQ(h.orders_at(1000), 1u);
    EXPECT_EQ(h.working(990), 10);
    EXPECT_CONSISTENT(h);
}

TEST(StackerShape, QuoteWithZeroSizeKeepsTheLadder) {
    buy_harness h{base_cfg()};
    h.quote(1000, 25);
    h.ack_all();

    h.quote(1000, 0);
    h.settle();
    EXPECT_EQ(h.working(1000), 0);
    EXPECT_EQ(h.working(990), 10) << "stepping off the touch must not give up depth";
    EXPECT_EQ(h.working(980), 10);
    EXPECT_CONSISTENT(h);
}

TEST(StackerShape, PullTakesEverythingOut) {
    buy_harness h{base_cfg()};
    h.quote(1000, 25);
    h.ack_all();
    ASSERT_EQ(h.st.live_order_count(), 4u);

    h.st.pull();
    h.settle();
    EXPECT_EQ(h.working(1000), 0);
    EXPECT_EQ(h.working(990), 0);
    EXPECT_EQ(h.working(980), 0);
    EXPECT_EQ(h.working(970), 0);
    EXPECT_EQ(h.st.live_order_count(), 0u);
    EXPECT_EQ(h.exec.live_orders(), 0u);
    EXPECT_CONSISTENT(h);
}

TEST(StackerShape, QuoteAfterPullRebuilds) {
    buy_harness h{base_cfg()};
    h.quote(1000, 25);
    h.ack_all();
    h.st.pull();
    h.settle();

    h.quote(1000, 25);
    h.settle();
    EXPECT_EQ(h.working(1000), 25);
    EXPECT_EQ(h.working(970), 10);
    EXPECT_CONSISTENT(h);
}

TEST(StackerShape, PriceImprovesByOneTick) {
    buy_harness h{base_cfg()};
    h.quote(1000, 25);
    h.ack_all();

    h.quote(1010, 25);
    h.settle();

    EXPECT_EQ(h.working(1010), 25);
    EXPECT_EQ(h.working(1000), 10);
    EXPECT_EQ(h.working(990), 10);
    EXPECT_EQ(h.working(980), 10);
    EXPECT_EQ(h.working(970), 0) << "the level that dropped out of the ladder must be cleared";
    EXPECT_CONSISTENT(h);
}

TEST(StackerShape, PriceMovesAgainstByOneTick) {
    buy_harness h{base_cfg()};
    h.quote(1000, 25);
    h.ack_all();

    h.quote(990, 25);
    h.settle();

    EXPECT_EQ(h.working(1000), 0) << "nothing may be left in front of the quote";
    EXPECT_EQ(h.working(990), 25);
    EXPECT_EQ(h.working(980), 10);
    EXPECT_EQ(h.working(970), 10);
    EXPECT_EQ(h.working(960), 10);
    EXPECT_CONSISTENT(h);
}

TEST(StackerShape, LargePriceJumpRebuildsTheWholeStack) {
    buy_harness h{base_cfg()};
    h.quote(1000, 25);
    h.ack_all();

    h.quote(1500, 25);
    h.settle();

    EXPECT_EQ(h.working(1500), 25);
    EXPECT_EQ(h.working(1490), 10);
    EXPECT_EQ(h.working(1480), 10);
    EXPECT_EQ(h.working(1470), 10);
    EXPECT_EQ(h.working(1000), 0);
    EXPECT_EQ(h.working(990), 0);
    EXPECT_CONSISTENT(h);
}

TEST(StackerShape, LevelGapSkipsTicks) {
    auto cfg = base_cfg();
    cfg.level_gap_ticks = 3;
    buy_harness h{cfg};

    h.quote(1000, 25);
    h.ack_all();

    EXPECT_EQ(h.working(1000), 25);
    EXPECT_EQ(h.working(990), 0) << "levels between rungs must stay empty";
    EXPECT_EQ(h.working(980), 0);
    EXPECT_EQ(h.working(970), 10);
    EXPECT_EQ(h.working(940), 10);
    EXPECT_EQ(h.working(910), 10);
    EXPECT_EQ(h.working(880), 0);
    EXPECT_EQ(h.st.live_order_count(), 4u);
    EXPECT_CONSISTENT(h);
}

TEST(StackerShape, QtyProfileOverridesUniformSize) {
    static constexpr std::array<qty_t, 3> profile{30, 20, 5};
    auto cfg = base_cfg();
    cfg.qty_profile = profile;
    buy_harness h{cfg};

    h.quote(1000, 25);
    h.ack_all();

    EXPECT_EQ(h.working(1000), 25);
    EXPECT_EQ(h.working(990), 30);
    EXPECT_EQ(h.working(980), 20);
    EXPECT_EQ(h.working(970), 5);
    EXPECT_CONSISTENT(h);
}

TEST(StackerShape, ProfileAndGapCombine) {
    static constexpr std::array<qty_t, 2> profile{7, 3};
    auto cfg = base_cfg();
    cfg.levels = 2;
    cfg.level_gap_ticks = 2;
    cfg.qty_profile = profile;
    buy_harness h{cfg};

    h.quote(1000, 25);
    h.ack_all();

    EXPECT_EQ(h.working(1000), 25);
    EXPECT_EQ(h.working(990), 0);
    EXPECT_EQ(h.working(980), 7);
    EXPECT_EQ(h.working(960), 3);
    EXPECT_CONSISTENT(h);
}

TEST(StackerShape, SetLevelsGrowsAndShrinksTheLadder) {
    buy_harness h{base_cfg()};
    h.quote(1000, 25);
    h.ack_all();

    h.st.set_levels(5);
    h.settle();
    EXPECT_EQ(h.working(960), 10);
    EXPECT_EQ(h.working(950), 10);
    EXPECT_EQ(h.working(940), 0);
    EXPECT_CONSISTENT(h);

    h.st.set_levels(1);
    h.settle();
    EXPECT_EQ(h.working(990), 10);
    EXPECT_EQ(h.working(980), 0);
    EXPECT_EQ(h.working(950), 0);
    EXPECT_CONSISTENT(h);
}

TEST(StackerShape, SetStackQtyResizesEveryRung) {
    buy_harness h{base_cfg()};
    h.quote(1000, 25);
    h.ack_all();

    h.st.set_stack_qty(4);
    h.settle();
    EXPECT_EQ(h.working(1000), 25);
    EXPECT_EQ(h.working(990), 4);
    EXPECT_EQ(h.working(980), 4);
    EXPECT_EQ(h.working(970), 4);
    EXPECT_CONSISTENT(h);
}

TEST(StackerShape, ZeroLevelsQuotesTopOnly) {
    auto cfg = base_cfg();
    cfg.levels = 0;
    buy_harness h{cfg};

    h.quote(1000, 25);
    h.ack_all();
    EXPECT_EQ(h.working(1000), 25);
    EXPECT_EQ(h.working(990), 0);
    EXPECT_EQ(h.st.live_order_count(), 1u);
    EXPECT_CONSISTENT(h);
}

TEST(StackerShape, MaxLevelQtyCapsEveryLevel) {
    auto cfg = base_cfg();
    cfg.max_level_qty = 12;
    buy_harness h{cfg};

    h.quote(1000, 25);
    h.ack_all();
    EXPECT_EQ(h.working(1000), 12) << "the top level is capped too";
    EXPECT_EQ(h.working(990), 10);
    EXPECT_CONSISTENT(h);
}

TEST(StackerShape, NegativeQuoteSizeIsTreatedAsZero) {
    buy_harness h{base_cfg()};
    h.quote(1000, -5);
    h.ack_all();
    EXPECT_EQ(h.working(1000), 0);
    EXPECT_EQ(h.st.quote_qty(), 0);
    EXPECT_CONSISTENT(h);
}

TEST(StackerShape, ReconcileIsANoOpWhenClean) {
    buy_harness h{base_cfg()};
    h.quote(1000, 25);
    h.ack_all();
    h.reconcile();

    const auto mark = h.exec.mark();
    EXPECT_FALSE(h.st.dirty());
    h.reconcile();
    h.reconcile();
    EXPECT_EQ(h.exec.total(mark), 0u);
}

TEST(StackerShape, ConfigValidation) {
    stacker_config cfg;
    EXPECT_EQ(cfg.validate<test_traits>(), slick::stacker::config_error::ok);

    cfg = base_cfg();
    cfg.tick_size = 0;
    EXPECT_EQ(cfg.validate<test_traits>(),
              slick::stacker::config_error::tick_size_not_positive);

    cfg = base_cfg();
    cfg.level_gap_ticks = 0;
    EXPECT_EQ(cfg.validate<test_traits>(), slick::stacker::config_error::level_gap_not_positive);

    cfg = base_cfg();
    cfg.levels = 99;
    EXPECT_EQ(cfg.validate<test_traits>(), slick::stacker::config_error::too_many_levels);

    static constexpr std::array<qty_t, 2> two{1, 2};
    cfg = base_cfg();
    cfg.levels = 3;
    cfg.qty_profile = two;
    EXPECT_EQ(cfg.validate<test_traits>(),
              slick::stacker::config_error::qty_profile_size_mismatch);

    cfg = base_cfg();
    cfg.qty_increment = 0;
    EXPECT_EQ(cfg.validate<test_traits>(),
              slick::stacker::config_error::qty_increment_not_positive);

    cfg = base_cfg();
    cfg.min_order_qty = 10;
    cfg.max_order_qty = 5;
    EXPECT_EQ(cfg.validate<test_traits>(),
              slick::stacker::config_error::max_order_qty_below_min);

    cfg = base_cfg();
    cfg.max_orders_per_level = 0;
    EXPECT_EQ(cfg.validate<test_traits>(),
              slick::stacker::config_error::max_orders_per_level_zero);
}
