// Copyright 2026 Slick Quant
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "harness.hpp"

using namespace testing_support;
using slick::stacker::side_t;

namespace {

stacker_config top_only_cfg() {
    stacker_config cfg;
    cfg.tick_size = 10;
    cfg.levels = 0;
    cfg.ack_required = false;
    return cfg;
}

stacker_config ladder_cfg() {
    stacker_config cfg = top_only_cfg();
    cfg.levels = 2;
    cfg.stack_qty = 10;
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

TEST(StackerRejects, NewRejectRemovesTheOrder) {
    buy_harness h{ladder_cfg()};
    h.st.quote(1000, 25);
    h.reconcile();
    const auto id = placed_at(h.exec, 1000);
    ASSERT_NE(id, mock_executor::invalid_order_id);
    ASSERT_EQ(h.working(1000), 25);

    h.reject_new(id, reject_reason_t::risk_limit);
    EXPECT_EQ(h.working(1000), 0) << "a rejected order never existed";
    EXPECT_EQ(h.orders_at(1000), 0u);
    EXPECT_EQ(h.target(1000), 25) << "the target is unchanged, so it will be retried";
    ASSERT_CONSISTENT(h);

    h.settle();
    EXPECT_EQ(h.working(1000), 25);
    EXPECT_CONSISTENT(h);
}

TEST(StackerRejects, ModifyRejectRollsBackToTheAcknowledgedSize) {
    buy_harness h{top_only_cfg()};
    h.quote(1000, 25);
    h.ack_all();
    const auto id = placed_at(h.exec, 1000);

    h.st.quote(1000, 20);
    h.reconcile();
    ASSERT_EQ(h.exec.count(mock_executor::kind::modify), 1u);
    ASSERT_EQ(h.working(1000), 20) << "intent drops as soon as the modify goes out";
    ASSERT_CONSISTENT(h);
    h.ignore_pending();

    h.reject_modify(id);
    EXPECT_EQ(h.acked(1000), 25);
    EXPECT_EQ(h.working(1000), 25) << "the venue still has the original size";
    EXPECT_EQ(h.orders_at(1000), 1u);
    ASSERT_CONSISTENT(h);

    h.settle();
    EXPECT_EQ(h.working(1000), 20) << "and reconcile tries again";
    EXPECT_CONSISTENT(h);
}

TEST(StackerRejects, ModifyRejectRehomesARepricedOrder) {
    buy_harness h{top_only_cfg()};
    h.quote(1000, 25);
    h.ack_all();
    const auto id = placed_at(h.exec, 1000);

    h.st.quote(1010, 25);
    h.reconcile();
    ASSERT_EQ(h.exec.count(mock_executor::kind::modify), 1u);
    ASSERT_EQ(h.working(1010), 25);
    ASSERT_EQ(h.working(1000), 0);
    ASSERT_CONSISTENT(h);
    h.ignore_pending();

    h.reject_modify(id);
    EXPECT_EQ(h.working(1010), 0);
    EXPECT_EQ(h.working(1000), 25) << "the order is still resting where it was";
    EXPECT_EQ(h.orders_at(1000), 1u);
    EXPECT_EQ(h.orders_at(1010), 0u);
    ASSERT_CONSISTENT(h);

    h.settle();
    EXPECT_EQ(h.working(1010), 25);
    EXPECT_EQ(h.working(1000), 0);
    EXPECT_CONSISTENT(h);
}

// The order filled while our modify was in flight, so the reject and the fill
// race. Whichever lands first, the slot must end up retired exactly once.
TEST(StackerRejects, ModifyRejectAfterTheOrderAlreadyFilled) {
    buy_harness h{top_only_cfg()};
    h.quote(1000, 25);
    h.ack_all();
    const auto id = placed_at(h.exec, 1000);

    h.st.quote(1000, 20);
    h.reconcile();
    h.ignore_pending();

    h.st.on_filled(id, 25, 1000);
    ASSERT_EQ(h.st.live_order_count(), 1u) << "still in flight, so not retired yet";
    ASSERT_CONSISTENT(h);

    h.reject_modify(id, reject_reason_t::too_late_to_act);
    EXPECT_EQ(h.st.live_order_count(), 0u);
    EXPECT_EQ(h.working(1000), 0);
    EXPECT_CONSISTENT(h);
}

TEST(StackerRejects, CancelRejectLeavesTheOrderWorking) {
    buy_harness h{top_only_cfg()};
    h.quote(1000, 25);
    h.ack_all();
    const auto id = placed_at(h.exec, 1000);

    h.st.pull();
    h.reconcile();
    ASSERT_EQ(h.exec.count(mock_executor::kind::cancel), 1u);
    ASSERT_EQ(h.working(1000), 0);
    h.ignore_pending();

    h.reject_cancel(id, reject_reason_t::retryable);
    EXPECT_EQ(h.working(1000), 25) << "the order is still out there";
    EXPECT_EQ(h.orders_at(1000), 1u);
    ASSERT_CONSISTENT(h);

    const auto mark = h.exec.mark();
    h.settle();
    EXPECT_EQ(h.exec.count(mock_executor::kind::cancel, mark), 1u) << "and is cancelled again";
    EXPECT_EQ(h.st.live_order_count(), 0u);
    EXPECT_CONSISTENT(h);
}

TEST(StackerRejects, CancelRejectTooLateWaitsForTheTerminalEvent) {
    buy_harness h{top_only_cfg()};
    h.quote(1000, 25);
    h.ack_all();
    const auto id = placed_at(h.exec, 1000);

    h.st.pull();
    h.reconcile();
    h.ignore_pending();

    const auto mark = h.exec.mark();
    h.reject_cancel(id, reject_reason_t::too_late_to_act);
    h.reconcile();
    EXPECT_EQ(h.exec.total(mark), 0u) << "no point chasing an order that is finishing";
    ASSERT_CONSISTENT(h);

    h.st.on_filled(id, 25, 1000);
    EXPECT_EQ(h.st.live_order_count(), 0u);
    EXPECT_CONSISTENT(h);
}

TEST(StackerRejects, RefusedPlaceLeavesTheLevelShortAndRetries) {
    buy_harness h{ladder_cfg()};
    h.exec.fail_place = true;
    h.st.quote(1000, 25);
    h.reconcile();

    EXPECT_EQ(h.exec.log.size(), 0u);
    EXPECT_GT(h.exec.refused, 0u);
    EXPECT_EQ(h.working(1000), 0);
    EXPECT_EQ(h.st.live_order_count(), 0u);
    ASSERT_CONSISTENT(h);

    h.exec.fail_place = false;
    h.st.quote(1000, 25);  // any new event re-arms reconcile
    h.settle();
    EXPECT_EQ(h.working(1000), 25);
    EXPECT_EQ(h.working(990), 10);
    EXPECT_CONSISTENT(h);
}

// A reduction the executor will not send is real exposure: the venue is holding
// more than we want and we could not tell it otherwise. Growing anywhere else
// while that is true would compound the problem, so adds are held off until it
// clears.
TEST(StackerRejects, RefusedReductionBlocksFurtherAdds) {
    buy_harness h{ladder_cfg()};
    h.quote(1000, 25);
    h.ack_all();
    ASSERT_EQ(h.st.blocked_count(), 0u);

    h.exec.fail_modify = true;
    h.exec.fail_cancel = true;
    h.st.quote(1000, 5);  // wants 20 off the top level
    h.reconcile();

    EXPECT_GT(h.st.blocked_count(), 0u);
    EXPECT_EQ(h.acked(1000), 25) << "the venue still holds the full size";
    ASSERT_CONSISTENT(h);

    // While blocked, a level that wants more must not be topped up.
    const auto mark = h.exec.mark();
    h.st.set_stack_qty(40);
    h.reconcile();
    EXPECT_EQ(h.exec.count(mock_executor::kind::place, mark), 0u);
    ASSERT_CONSISTENT(h);

    // Once the executor accepts messages again the block lifts and the backlog
    // goes out.
    h.exec.fail_modify = false;
    h.exec.fail_cancel = false;
    h.settle();
    EXPECT_EQ(h.st.blocked_count(), 0u);
    EXPECT_EQ(h.working(1000), 5);
    EXPECT_EQ(h.working(990), 40);
    EXPECT_CONSISTENT(h);
}

TEST(StackerRejects, RejectStormLeavesConsistentState) {
    buy_harness h{ladder_cfg()};
    h.st.quote(1000, 25);
    h.reconcile();

    // Reject every order the stacker just sent.
    auto ids = h.ids_since(0);
    h.ignore_pending();
    for (auto id : ids) {
        h.reject_new(id, reject_reason_t::throttled);
        ASSERT_CONSISTENT(h);
    }
    EXPECT_EQ(h.st.live_order_count(), 0u);

    h.settle();
    EXPECT_EQ(h.working(1000), 25);
    EXPECT_EQ(h.working(990), 10);
    EXPECT_EQ(h.working(980), 10);
    EXPECT_CONSISTENT(h);
}
