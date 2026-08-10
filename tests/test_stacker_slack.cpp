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
    cfg.prefer_modify = false;  // isolate slack from the reprice pass
    return cfg;
}

using buy_harness = harness<side_t::buy>;

}  // namespace

TEST(StackerSlack, WithoutSlackTheTailIsCancelledImmediately) {
    buy_harness h{base_cfg()};
    h.quote(1000, 25);
    h.ack_all();
    ASSERT_EQ(h.working(980), 10);

    h.quote(1010, 25);
    h.settle();
    EXPECT_EQ(h.working(980), 0);
    EXPECT_CONSISTENT(h);
}

// One tick of flicker should not cost a cancel and a re-place on the tail of
// the stack. Slack levels keep working orders that have just fallen off the
// bottom, on the assumption the quote will come back.
TEST(StackerSlack, RetainsTheTailWhenTheQuoteImproves) {
    auto cfg = base_cfg();
    cfg.slack_levels = 1;
    buy_harness h{cfg};

    h.quote(1000, 25);
    h.ack_all();
    ASSERT_EQ(h.working(980), 10);

    const auto mark = h.exec.mark();
    h.quote(1010, 25);
    h.settle();

    EXPECT_EQ(h.working(1010), 25);
    EXPECT_EQ(h.working(1000), 10);
    EXPECT_EQ(h.working(990), 10);
    EXPECT_EQ(h.working(980), 10) << "one level of slack is kept rather than cancelled";
    EXPECT_EQ(h.exec.count(mock_executor::kind::cancel, mark), 0u);
    EXPECT_CONSISTENT(h);
}

// The quote itself has to move when the price moves -- that cost is
// unavoidable. What slack buys is that the tail of the ladder rides through the
// round trip untouched, keeping its queue position.
TEST(StackerSlack, FlickerLeavesTheTailOrderUndisturbed) {
    auto cfg = base_cfg();
    cfg.slack_levels = 1;
    buy_harness h{cfg};

    h.quote(1000, 25);
    h.ack_all();
    const auto tail = h.last_placed();  // the order resting at 980
    ASSERT_EQ(h.exec.orders[tail].price, 980);

    const auto mark = h.exec.mark();
    h.quote(1010, 25);
    h.settle();
    h.quote(1000, 25);
    h.settle();

    EXPECT_EQ(h.working(1000), 25);
    EXPECT_EQ(h.working(990), 10);
    EXPECT_EQ(h.working(980), 10);
    EXPECT_EQ(h.working(970), 0);
    EXPECT_CONSISTENT(h);

    EXPECT_TRUE(h.exec.orders[tail].live) << "the tail order survived the round trip";
    EXPECT_EQ(h.exec.orders[tail].price, 980);
    for (std::size_t i = mark; i < h.exec.log.size(); ++i) {
        EXPECT_NE(h.exec.log[i].id, tail) << "the tail order was touched by a message";
    }
}

TEST(StackerSlack, BeyondTheSlackWindowTheTailIsCancelled) {
    auto cfg = base_cfg();
    cfg.slack_levels = 1;
    buy_harness h{cfg};

    h.quote(1000, 25);
    h.ack_all();

    h.quote(1030, 25);  // three ticks up: 980 is now five levels behind
    h.settle(16);
    EXPECT_EQ(h.working(1030), 25);
    EXPECT_EQ(h.working(1020), 10);
    EXPECT_EQ(h.working(1010), 10);
    // 1000 is one past the ladder, so it is retained -- but trimmed to the
    // ladder's own rung size rather than left carrying the old quote size.
    EXPECT_EQ(h.working(1000), 10) << "one level of slack, capped at the rung size";
    EXPECT_EQ(h.working(990), 0);
    EXPECT_EQ(h.working(980), 0);
    EXPECT_CONSISTENT(h);
}

TEST(StackerSlack, RetainedLevelIsTrimmedToTheRungSize) {
    auto cfg = base_cfg();
    cfg.levels = 1;
    cfg.stack_qty = 10;
    cfg.slack_levels = 1;
    buy_harness h{cfg};

    h.quote(1000, 25);
    h.ack_all();
    ASSERT_EQ(h.working(1000), 25);

    // 1000 drops into slack carrying the old quote size; it must not sit two
    // ticks behind the market with more than any rung ever asks for.
    const auto mark = h.exec.mark();
    h.quote(1010, 25);
    h.settle();
    EXPECT_EQ(h.working(1010), 25);
    EXPECT_EQ(h.working(1000), 10);
    EXPECT_EQ(h.working(990), 10);
    EXPECT_EQ(h.exec.count(mock_executor::kind::cancel, mark), 0u)
        << "trimming keeps the order; only its size changes";
    EXPECT_CONSISTENT(h);
}

TEST(StackerSlack, SlackDoesNotCreateOrdersOfItsOwn) {
    auto cfg = base_cfg();
    cfg.slack_levels = 2;
    buy_harness h{cfg};

    h.quote(1000, 25);
    h.ack_all();
    // Slack retains levels that are already working; it never opens new ones.
    EXPECT_EQ(h.working(970), 0);
    EXPECT_EQ(h.working(960), 0);
    EXPECT_EQ(h.st.live_order_count(), 3u);
    EXPECT_CONSISTENT(h);
}

TEST(StackerSlack, PullClearsSlackToo) {
    auto cfg = base_cfg();
    cfg.slack_levels = 2;
    buy_harness h{cfg};

    h.quote(1000, 25);
    h.ack_all();
    h.quote(1010, 25);
    h.settle();
    ASSERT_GT(h.st.live_order_count(), 0u);

    h.st.pull();
    h.settle(16);
    EXPECT_EQ(h.st.live_order_count(), 0u);
    EXPECT_EQ(h.exec.live_orders(), 0u);
    EXPECT_CONSISTENT(h);
}

// --- quantity hysteresis ---------------------------------------------------

TEST(StackerHysteresis, SmallShortfallsAreNotToppedUp) {
    auto cfg = base_cfg();
    cfg.levels = 0;
    cfg.qty_hysteresis = 5;
    buy_harness h{cfg};

    h.quote(1000, 25);
    h.ack_all();
    const auto id = h.last_placed();

    const auto mark = h.exec.mark();
    h.st.on_filled(id, 3, 1000);   // consumes the target too, so no shortfall
    h.st.quote(1000, 25);          // ask for the size back: a shortfall of 3
    h.settle();
    EXPECT_EQ(h.exec.count(mock_executor::kind::place, mark), 0u)
        << "three short of twenty-five is not worth a message";
    EXPECT_EQ(h.working(1000), 22);
    EXPECT_CONSISTENT(h);
}

TEST(StackerHysteresis, ShortfallsAboveTheThresholdAreToppedUp) {
    auto cfg = base_cfg();
    cfg.levels = 0;
    cfg.qty_hysteresis = 5;
    buy_harness h{cfg};

    h.quote(1000, 25);
    h.ack_all();
    const auto id = h.last_placed();

    const auto mark = h.exec.mark();
    h.st.on_filled(id, 10, 1000);
    h.st.quote(1000, 25);
    h.settle();
    EXPECT_EQ(h.exec.count(mock_executor::kind::place, mark), 1u);
    EXPECT_EQ(h.working(1000), 25);
    EXPECT_CONSISTENT(h);
}

// Hysteresis is about not churning a level that is already doing its job. A
// level with nothing on it is always established, however small the target.
TEST(StackerHysteresis, AnEmptyLevelIsAlwaysEstablished) {
    auto cfg = base_cfg();
    cfg.levels = 1;
    cfg.stack_qty = 2;
    cfg.qty_hysteresis = 100;
    buy_harness h{cfg};

    h.quote(1000, 1);
    h.ack_all();
    EXPECT_EQ(h.working(1000), 1);
    EXPECT_EQ(h.working(990), 2);
    EXPECT_CONSISTENT(h);
}

// Reductions are never suppressed: carrying more than intended is a risk
// position, where carrying slightly less is only a missed opportunity.
TEST(StackerHysteresis, ReductionsAreNeverSuppressed) {
    auto cfg = base_cfg();
    cfg.levels = 0;
    cfg.qty_hysteresis = 20;
    buy_harness h{cfg};

    h.quote(1000, 25);
    h.ack_all();

    const auto mark = h.exec.mark();
    h.quote(1000, 24);
    h.settle();
    EXPECT_EQ(h.exec.total(mark), 1u);
    EXPECT_EQ(h.working(1000), 24);
    EXPECT_CONSISTENT(h);
}
