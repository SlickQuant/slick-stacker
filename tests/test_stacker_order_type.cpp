// Copyright 2026 Slick Quant
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "harness.hpp"

using namespace testing_support;
using slick::stacker::order_type_t;
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

/// Every order the venue still holds is of kind `t`.
[[nodiscard]] bool all_live_are(const mock_executor& exec, order_type_t t) {
    for (const auto& [id, o] : exec.orders) {
        if (o.live && o.order_type != t) {
            return false;
        }
    }
    return true;
}

/// Every `place` sent since `from` asked for kind `t`.
[[nodiscard]] bool all_placed_are(const mock_executor& exec, std::size_t from, order_type_t t) {
    for (std::size_t i = from; i < exec.log.size(); ++i) {
        const auto& m = exec.log[i];
        if (m.type == mock_executor::kind::place && m.order_type != t) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST(StackerOrderType, DefaultsToLimit) {
    stacker_config cfg;
    EXPECT_EQ(cfg.order_type, order_type_t::limit);

    buy_harness h{base_cfg()};
    h.quote(1000, 25);
    ASSERT_FALSE(h.exec.log.empty());
    for (const auto& m : h.exec.log) {
        if (m.type == mock_executor::kind::place) {
            EXPECT_EQ(m.order_type, order_type_t::limit);
        }
    }
    EXPECT_CONSISTENT(h);
}

TEST(StackerOrderType, GtcIsPassedToTheExecutor) {
    auto cfg = base_cfg();
    cfg.order_type = order_type_t::gtc;
    buy_harness h{cfg};

    h.quote(1000, 25);
    ASSERT_EQ(h.exec.count(mock_executor::kind::place), 3u);
    for (const auto& m : h.exec.log) {
        if (m.type == mock_executor::kind::place) {
            EXPECT_EQ(m.order_type, order_type_t::gtc);
        }
    }

    // A resting type behaves exactly like a limit order as far as the stacker
    // is concerned -- how long the venue keeps it is the venue's business.
    h.ack_all();
    EXPECT_EQ(h.working(1000), 25);
    EXPECT_EQ(h.working(990), 10);
    EXPECT_EQ(h.working(980), 10);
    EXPECT_CONSISTENT(h);
}

TEST(StackerOrderType, ToString) {
    EXPECT_STREQ(to_string(order_type_t::limit), "limit");
    EXPECT_STREQ(to_string(order_type_t::gtc), "gtc");
}

TEST(StackerOrderType, RestingTypeStillRefillsWhenAsked) {
    auto cfg = base_cfg();
    cfg.levels = 0;
    cfg.order_type = order_type_t::gtc;
    cfg.refill_on_fill = true;
    buy_harness h{cfg};

    h.quote(1000, 25);
    h.ack_all();
    const auto id = h.last_placed();

    h.fill(id, 10);
    h.settle();
    EXPECT_EQ(h.target(1000), 25);
    EXPECT_EQ(h.working(1000), 25);
    EXPECT_CONSISTENT(h);
}

// Both kinds rest, so a GTC stack behaves exactly like a limit one through a
// price move -- repricing, trimming and all.
TEST(StackerOrderType, GtcStackWalksLikeAnyOther) {
    auto cfg = base_cfg();
    cfg.order_type = order_type_t::gtc;
    buy_harness h{cfg};

    h.quote(1000, 25);
    h.ack_all();
    const auto mark = h.exec.mark();

    h.quote(1010, 25);
    h.settle();
    EXPECT_EQ(h.working(1010), 25);
    EXPECT_EQ(h.working(1000), 10);
    EXPECT_EQ(h.working(990), 10);
    EXPECT_EQ(h.working(980), 0);
    EXPECT_EQ(h.exec.count(mock_executor::kind::cancel, mark), 0u);
    EXPECT_CONSISTENT(h);
}

// -- changing the type at runtime -------------------------------------------
//
// A modify carries price and quantity only, so an order already at the venue
// cannot be amended into a different kind. Everything working has to come out
// and go back as the new one.

TEST(StackerOrderType, ConfigureReplacesWorkingOrdersWithTheNewType) {
    buy_harness h{base_cfg()};
    h.quote(1000, 25);
    h.ack_all();
    ASSERT_EQ(h.exec.live_orders(), 3u);
    ASSERT_TRUE(all_live_are(h.exec, order_type_t::limit));
    const auto mark = h.exec.mark();

    auto cfg = base_cfg();
    cfg.order_type = order_type_t::gtc;
    h.st.configure(cfg);
    h.st.reconcile();

    // One cancel and one replacement per working order, in a single pass.
    EXPECT_EQ(h.exec.count(mock_executor::kind::cancel, mark), 3u);
    EXPECT_EQ(h.exec.count(mock_executor::kind::place, mark), 3u);
    EXPECT_EQ(h.exec.count(mock_executor::kind::modify, mark), 0u);
    EXPECT_TRUE(all_placed_are(h.exec, mark, order_type_t::gtc));
    EXPECT_CONSISTENT(h);

    h.settle();
    EXPECT_EQ(h.exec.live_orders(), 3u);
    EXPECT_TRUE(all_live_are(h.exec, order_type_t::gtc));
    EXPECT_EQ(h.working(1000), 25);
    EXPECT_EQ(h.working(990), 10);
    EXPECT_EQ(h.working(980), 10);
    EXPECT_CONSISTENT(h);
}

TEST(StackerOrderType, SetOrderTypeReplacesWorkingOrders) {
    buy_harness h{base_cfg()};
    h.quote(1000, 25);
    h.ack_all();
    const auto mark = h.exec.mark();

    h.st.set_order_type(order_type_t::gtc);
    EXPECT_TRUE(h.st.dirty());
    h.settle();

    EXPECT_EQ(h.exec.count(mock_executor::kind::cancel, mark), 3u);
    EXPECT_EQ(h.exec.count(mock_executor::kind::place, mark), 3u);
    EXPECT_TRUE(all_live_are(h.exec, order_type_t::gtc));
    EXPECT_EQ(h.working(1000), 25);
    EXPECT_CONSISTENT(h);
}

// The replacement is not a re-quote: it asks for what is still wanted, which a
// fill has already eaten into.
TEST(StackerOrderType, ReplacesOnlyWhatIsStillWanted) {
    auto cfg = base_cfg();
    cfg.levels = 0;
    buy_harness h{cfg};
    h.quote(1000, 25);
    h.ack_all();

    h.fill(h.last_placed(), 10);
    ASSERT_EQ(h.target(1000), 15);
    const auto mark = h.exec.mark();

    h.st.set_order_type(order_type_t::gtc);
    h.settle();

    ASSERT_EQ(h.exec.count(mock_executor::kind::place, mark), 1u);
    const auto& sent = h.exec.log.back();
    EXPECT_EQ(sent.qty, 15);
    EXPECT_EQ(sent.order_type, order_type_t::gtc);
    EXPECT_EQ(h.working(1000), 15);
    EXPECT_CONSISTENT(h);
}

// Re-asserting the kind already in force is not a change, and must not churn
// the stack.
TEST(StackerOrderType, SameTypeSendsNothing) {
    buy_harness h{base_cfg()};
    h.quote(1000, 25);
    h.ack_all();
    const auto mark = h.exec.mark();

    h.st.set_order_type(order_type_t::limit);
    h.st.configure(base_cfg());
    h.settle();

    EXPECT_EQ(h.exec.total(mark), 0u);
    EXPECT_EQ(h.exec.live_orders(), 3u);
    EXPECT_CONSISTENT(h);
}

TEST(StackerOrderType, ChangingTypeWithNothingWorkingCostsNoMessages) {
    buy_harness h{base_cfg()};
    h.st.set_order_type(order_type_t::gtc);
    EXPECT_EQ(h.exec.total(), 0u);

    h.quote(1000, 25);
    EXPECT_EQ(h.exec.count(mock_executor::kind::cancel), 0u);
    EXPECT_TRUE(all_placed_are(h.exec, 0, order_type_t::gtc));
    EXPECT_CONSISTENT(h);
}

// An order of the wrong kind is a candidate for removal, never for repricing:
// a modify would leave the venue holding the old kind at a new price.
TEST(StackerOrderType, StaleOrdersAreNotRepriced) {
    auto cfg = base_cfg();
    cfg.levels = 1;
    cfg.stack_qty = 10;
    cfg.prefer_modify = true;
    buy_harness h{cfg};

    h.quote(1000, 10);
    h.ack_all();
    ASSERT_EQ(h.exec.live_orders(), 2u);
    const auto mark = h.exec.mark();

    // The quote steps up and the kind changes in the same batch. Repricing the
    // order at 990 into 1010 would have been the cheap answer for the move
    // alone; it is not available here.
    h.st.set_order_type(order_type_t::gtc);
    h.st.quote(1010, 10);
    h.st.reconcile();

    EXPECT_EQ(h.exec.count(mock_executor::kind::modify, mark), 0u);
    EXPECT_EQ(h.exec.count(mock_executor::kind::cancel, mark), 2u);
    EXPECT_TRUE(all_placed_are(h.exec, mark, order_type_t::gtc));

    h.settle();
    EXPECT_EQ(h.working(1010), 10);
    EXPECT_EQ(h.working(1000), 10);
    EXPECT_EQ(h.working(990), 0);
    EXPECT_TRUE(all_live_are(h.exec, order_type_t::gtc));
    EXPECT_CONSISTENT(h);
}

// With `ack_required` the cancel cannot go out until the venue answers the
// original order. The level still stops counting it straight away, so the
// replacement goes out first and the cancel follows from the acknowledgement.
TEST(StackerOrderType, RetypeDefersOntoTheAcknowledgement) {
    auto cfg = base_cfg();
    cfg.levels = 0;
    cfg.ack_required = true;
    buy_harness h{cfg};

    h.quote(1000, 25);
    ASSERT_EQ(h.exec.count(mock_executor::kind::place), 1u);
    const auto stale = h.last_placed();
    const auto mark = h.exec.mark();

    h.st.set_order_type(order_type_t::gtc);
    h.st.reconcile();

    // Nothing could be sent against the unacknowledged order, but the level was
    // re-established immediately -- and as the new kind.
    EXPECT_EQ(h.exec.count(mock_executor::kind::cancel, mark), 0u);
    ASSERT_EQ(h.exec.count(mock_executor::kind::place, mark), 1u);
    EXPECT_EQ(h.exec.log.back().order_type, order_type_t::gtc);
    EXPECT_NE(h.exec.log.back().id, stale);
    EXPECT_CONSISTENT(h);

    h.settle();
    EXPECT_FALSE(h.exec.orders[stale].live);
    EXPECT_EQ(h.exec.live_orders(), 1u);
    EXPECT_TRUE(all_live_are(h.exec, order_type_t::gtc));
    EXPECT_EQ(h.working(1000), 25);
    EXPECT_CONSISTENT(h);
}

// A deferred cancel is normally free quantity for the add pass to reclaim.
// One held back for a retype is not: reinstating it would leave the wrong kind
// working.
TEST(StackerOrderType, DeferredRetypeCancelIsNotReclaimed) {
    auto cfg = base_cfg();
    cfg.levels = 0;
    cfg.ack_required = true;
    buy_harness h{cfg};

    h.quote(1000, 25);
    const auto stale = h.last_placed();
    h.st.set_order_type(order_type_t::gtc);
    h.st.reconcile();

    // Quote the same size again: the level wants exactly what the held-back
    // order carries, which is the case that would tempt a reclaim.
    h.st.quote(1000, 25);
    h.st.reconcile();

    h.settle();
    EXPECT_FALSE(h.exec.orders[stale].live);
    EXPECT_TRUE(all_live_are(h.exec, order_type_t::gtc));
    EXPECT_EQ(h.working(1000), 25);
    EXPECT_EQ(h.exec.venue_qty_at(1000), 25);
    EXPECT_CONSISTENT(h);
}

// A refused cancel leaves an order of the old kind standing, so the pass has to
// come back for it. Nothing is added in the meantime -- the stack is already
// carrying more than it means to.
TEST(StackerOrderType, RetypeSurvivesARefusedCancel) {
    auto cfg = base_cfg();
    cfg.levels = 1;
    buy_harness h{cfg};

    h.quote(1000, 25);
    h.ack_all();
    ASSERT_EQ(h.exec.live_orders(), 2u);
    const auto mark = h.exec.mark();

    h.exec.fail_cancel = true;
    h.st.set_order_type(order_type_t::gtc);
    h.st.reconcile();

    EXPECT_EQ(h.exec.total(mark), 0u);
    EXPECT_EQ(h.exec.refused, 2u);
    EXPECT_EQ(h.st.blocked_count(), 2u);
    EXPECT_TRUE(h.st.dirty());
    EXPECT_CONSISTENT(h);

    h.exec.fail_cancel = false;
    h.settle();

    EXPECT_EQ(h.st.blocked_count(), 0u);
    EXPECT_EQ(h.exec.count(mock_executor::kind::cancel, mark), 2u);
    EXPECT_EQ(h.exec.live_orders(), 2u);
    EXPECT_TRUE(all_live_are(h.exec, order_type_t::gtc));
    EXPECT_EQ(h.working(1000), 25);
    EXPECT_EQ(h.working(990), 10);
    EXPECT_CONSISTENT(h);
}

// Flipping back before the first retype has finished must leave the venue with
// the kind asked for last, not the one it was mid-way through adopting.
TEST(StackerOrderType, TypeChangedTwiceBeforeReconcileSettlesOnTheLast) {
    buy_harness h{base_cfg()};
    h.quote(1000, 25);
    h.ack_all();
    const auto mark = h.exec.mark();

    h.st.set_order_type(order_type_t::gtc);
    h.st.set_order_type(order_type_t::limit);
    h.settle();

    // Back to what the working orders already are: nothing to do.
    EXPECT_EQ(h.exec.total(mark), 0u);
    EXPECT_TRUE(all_live_are(h.exec, order_type_t::limit));
    EXPECT_CONSISTENT(h);
}
