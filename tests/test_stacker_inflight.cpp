// Copyright 2026 Slick Quant
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "harness.hpp"

using namespace testing_support;
using slick::stacker::side_t;

namespace {

/// Most venues will not accept a modify or cancel for an order they have not
/// acknowledged yet. The stacker records the intent against the order and sends
/// it from the acknowledgement, so the level's accounting is correct straight
/// away even though the message is still minutes -- or microseconds -- away.
stacker_config strict_cfg() {
    stacker_config cfg;
    cfg.tick_size = 10;
    cfg.levels = 0;
    cfg.ack_required = true;
    return cfg;
}

using buy_harness = harness<side_t::buy>;

mock_executor::order_id_t placed_at(const mock_executor& exec, price_t px) {
    for (const auto& m : exec.log) {
        if (m.type == mock_executor::kind::place && m.price == px) {
            return m.id;
        }
    }
    return mock_executor::invalid_order_id;
}

}  // namespace

TEST(StackerInFlight, ReductionOnAnUnackedOrderIsDeferred) {
    buy_harness h{strict_cfg()};
    h.st.quote(1000, 25);
    h.reconcile();
    ASSERT_EQ(h.exec.total(), 1u);

    const auto mark = h.exec.mark();
    h.st.quote(1000, 10);
    h.reconcile();

    EXPECT_EQ(h.exec.total(mark), 0u) << "nothing may go out while the order is unacknowledged";
    EXPECT_EQ(h.working(1000), 10) << "but the intent counts immediately";
    ASSERT_CONSISTENT(h);

    // Reconciling again must not queue a second copy of the same intent.
    h.reconcile();
    EXPECT_EQ(h.exec.total(mark), 0u);
    ASSERT_CONSISTENT(h);
}

TEST(StackerInFlight, DeferredReductionFiresOnTheAcknowledgement) {
    buy_harness h{strict_cfg()};
    h.st.quote(1000, 25);
    h.reconcile();
    h.st.quote(1000, 10);
    h.reconcile();

    const auto mark = h.exec.mark();
    h.ack_all();

    EXPECT_EQ(h.exec.count(mock_executor::kind::modify, mark), 1u);
    EXPECT_EQ(h.exec.log.back().qty, 10);
    EXPECT_EQ(h.working(1000), 10);
    EXPECT_EQ(h.acked(1000), 10);
    EXPECT_CONSISTENT(h);
}

TEST(StackerInFlight, DeferredCancelFiresOnTheAcknowledgement) {
    buy_harness h{strict_cfg()};
    h.st.quote(1000, 25);
    h.reconcile();

    h.st.pull();
    h.reconcile();
    EXPECT_EQ(h.exec.count(mock_executor::kind::cancel), 0u);
    EXPECT_EQ(h.working(1000), 0);
    ASSERT_CONSISTENT(h);

    h.ack_all();
    EXPECT_EQ(h.exec.count(mock_executor::kind::cancel), 1u);
    EXPECT_EQ(h.st.live_order_count(), 0u);
    EXPECT_CONSISTENT(h);
}

// The cheapest quantity available is quantity we already have but had decided
// to give up. If the target comes back before the cancel has gone out, the
// order is simply reinstated -- no message at all.
TEST(StackerInFlight, DeferredCancelIsReclaimedWhenTheTargetReturns) {
    buy_harness h{strict_cfg()};
    h.st.quote(1000, 25);
    h.reconcile();
    const auto mark = h.exec.mark();

    h.st.quote(1000, 0);
    h.reconcile();
    ASSERT_EQ(h.working(1000), 0);
    ASSERT_EQ(h.exec.total(mark), 0u);

    h.st.quote(1000, 25);
    h.reconcile();
    EXPECT_EQ(h.exec.total(mark), 0u) << "reinstating a deferred cancel must be free";
    EXPECT_EQ(h.working(1000), 25);
    EXPECT_EQ(h.st.live_order_count(), 1u);
    ASSERT_CONSISTENT(h);

    h.ack_all();
    EXPECT_EQ(h.exec.total(mark), 0u);
    EXPECT_EQ(h.acked(1000), 25);
    EXPECT_CONSISTENT(h);
}

TEST(StackerInFlight, DeferredRepriceMovesTheOrderOnAcknowledgement) {
    buy_harness h{strict_cfg()};
    h.st.quote(1000, 25);
    h.reconcile();
    const auto id = placed_at(h.exec, 1000);

    const auto mark = h.exec.mark();
    h.st.quote(1010, 25);
    h.reconcile();

    EXPECT_EQ(h.exec.total(mark), 0u);
    EXPECT_EQ(h.working(1010), 25) << "the order is already booked at its destination";
    EXPECT_EQ(h.working(1000), 0);
    EXPECT_EQ(h.orders_at(1010), 1u);
    EXPECT_EQ(h.orders_at(1000), 0u);
    ASSERT_CONSISTENT(h);

    h.ack_all();
    EXPECT_EQ(h.exec.count(mock_executor::kind::modify, mark), 1u);
    EXPECT_EQ(h.exec.orders[id].price, 1010);
    EXPECT_EQ(h.acked(1010), 25);
    EXPECT_EQ(h.working(1000), 0);
    EXPECT_CONSISTENT(h);
}

// Only one request per order is ever outstanding. Chaining modifies would mean
// keeping a per-order history of in-flight states, which is exactly the
// complexity this design exists to avoid.
TEST(StackerInFlight, NoSecondRequestWhileOneIsOutstanding) {
    auto cfg = strict_cfg();
    cfg.ack_required = false;  // even here, a modify in flight blocks the next
    buy_harness h{cfg};
    h.quote(1000, 25);
    h.ack_all();

    h.st.quote(1000, 20);
    h.reconcile();
    ASSERT_EQ(h.exec.count(mock_executor::kind::modify), 1u);
    h.ignore_pending();

    const auto mark = h.exec.mark();
    h.st.quote(1000, 15);
    h.reconcile();
    EXPECT_EQ(h.exec.total(mark), 0u) << "the second change waits for the first to land";
    EXPECT_EQ(h.working(1000), 15);
    ASSERT_CONSISTENT(h);

    // Confirm the first modify; the deferred second one goes out immediately.
    const auto id = placed_at(h.exec, 1000);
    h.exec.confirm_modify(id);
    h.st.on_replaced(id, 1000, 20);
    EXPECT_EQ(h.exec.count(mock_executor::kind::modify, mark), 1u);
    EXPECT_EQ(h.exec.log.back().qty, 15);
    ASSERT_CONSISTENT(h);
}

TEST(StackerInFlight, WithoutAckRequiredTheReductionGoesOutAtOnce) {
    auto cfg = strict_cfg();
    cfg.ack_required = false;
    buy_harness h{cfg};

    h.st.quote(1000, 25);
    h.reconcile();
    const auto mark = h.exec.mark();

    h.st.quote(1000, 10);
    h.reconcile();
    EXPECT_EQ(h.exec.count(mock_executor::kind::modify, mark), 1u)
        << "venues that accept action on an unacknowledged order should see it straight away";
    EXPECT_EQ(h.working(1000), 10);
    EXPECT_CONSISTENT(h);
}

TEST(StackerInFlight, FillAgainstAnOrderWithADeferredCancel) {
    buy_harness h{strict_cfg()};
    h.st.quote(1000, 25);
    h.reconcile();
    const auto id = placed_at(h.exec, 1000);

    h.st.pull();
    h.reconcile();
    ASSERT_EQ(h.working(1000), 0);

    // The order fills before its acknowledgement, and before the cancel we owe.
    h.st.on_filled(id, 25, 1000);
    ASSERT_CONSISTENT(h);

    h.ack_all();
    EXPECT_EQ(h.working(1000), 0);
    EXPECT_CONSISTENT(h);
}

TEST(StackerInFlight, LadderConvergesOverSeveralRoundTrips) {
    auto cfg = strict_cfg();
    cfg.levels = 3;
    cfg.stack_qty = 10;
    buy_harness h{cfg};

    h.st.quote(1000, 25);
    h.settle();
    ASSERT_EQ(h.working(1000), 25);
    ASSERT_EQ(h.working(970), 10);
    ASSERT_CONSISTENT(h);

    // Walk the quote a tick at a time without ever letting it settle in
    // between, then let it catch up.
    h.st.quote(1010, 25);
    h.reconcile();
    h.st.quote(1020, 30);
    h.reconcile();
    h.st.quote(1010, 20);
    h.reconcile();
    ASSERT_CONSISTENT(h);

    h.settle(16);
    EXPECT_EQ(h.working(1010), 20);
    EXPECT_EQ(h.working(1000), 10);
    EXPECT_EQ(h.working(990), 10);
    EXPECT_EQ(h.working(980), 10);
    EXPECT_EQ(h.working(1020), 0);
    EXPECT_EQ(h.working(970), 0);
    EXPECT_CONSISTENT(h);
}
