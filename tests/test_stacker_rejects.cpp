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

/// Messages of kind `k` sent at `px` since `from`.
std::size_t sent_at(const mock_executor& exec, mock_executor::kind k, price_t px,
                    std::size_t from = 0) {
    std::size_t n = 0;
    for (std::size_t i = from; i < exec.log.size(); ++i) {
        n += exec.log[i].type == k && exec.log[i].price == px ? 1u : 0u;
    }
    return n;
}

/// Reconcile and answer one batch, rejecting every new order and modify the
/// stacker sent at `px` with `r` and acknowledging everything else -- a venue
/// that will never take quantity at that price. Returns how many messages
/// went out.
template <class H>
std::size_t round_trip_rejecting(H& h, price_t px, reject_reason_t r, std::size_t& cursor) {
    h.reconcile();
    const std::size_t sent = h.exec.log.size() - cursor;
    for (; cursor < h.exec.log.size(); ++cursor) {
        const auto m = h.exec.log[cursor];
        if (m.price == px && m.type == mock_executor::kind::place) {
            h.reject_new(m.id, r);
        } else if (m.price == px && m.type == mock_executor::kind::modify) {
            h.reject_modify(m.id, r);
        } else if (m.type == mock_executor::kind::place) {
            h.st.on_accepted(m.id, m.price, m.qty);
        } else if (m.type == mock_executor::kind::modify) {
            h.exec.confirm_modify(m.id);
            h.st.on_replaced(m.id, m.price, m.qty);
        } else {
            auto& o = h.exec.orders[m.id];
            o.live = false;
            h.st.on_canceled(m.id, o.qty - o.filled);
        }
    }
    h.ignore_pending();
    return sent;
}

}  // namespace

TEST(StackerRejects, NewRejectRemovesTheOrder) {
    buy_harness h{ladder_cfg()};
    h.st.quote(1000, 25);
    h.reconcile();
    const auto id = placed_at(h.exec, 1000);
    ASSERT_NE(id, mock_executor::invalid_order_id);
    ASSERT_EQ(h.working(1000), 25);

    h.reject_new(id, reject_reason_t::retryable);
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

    h.reject_modify(id, reject_reason_t::retryable);
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

    h.reject_modify(id, reject_reason_t::throttled);
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

// --- terminal rejects -------------------------------------------------------
//
// A reason other than `throttled` or `retryable` says the venue will refuse the
// same request again. Resending it on every event batch would be a reject loop,
// so the stacker stops asking until the caller changes what it wants.

// Every terminal reason, run through a venue that will never take quantity at
// the top price: the first reject is the last message sent there.
TEST(StackerRejects, TerminalNewRejectIsNotResent) {
    for (const auto r :
         {reject_reason_t::unknown, reject_reason_t::risk_limit, reject_reason_t::invalid_price,
          reject_reason_t::invalid_qty, reject_reason_t::self_match_prevention,
          reject_reason_t::market_closed, reject_reason_t::terminal}) {
        SCOPED_TRACE(static_cast<int>(r));
        buy_harness h{ladder_cfg()};
        h.st.quote(1000, 25);
        std::size_t cursor = 0;
        round_trip_rejecting(h, 1000, r, cursor);
        ASSERT_CONSISTENT(h);
        EXPECT_TRUE(h.st.rejected_at(1000));
        EXPECT_EQ(h.target(1000), 25) << "the caller's intent is reported as it was";
        EXPECT_EQ(h.working(1000), 0);

        for (int i = 0; i < 4; ++i) {
            EXPECT_EQ(round_trip_rejecting(h, 1000, r, cursor), 0u);
        }
        EXPECT_EQ(sent_at(h.exec, mock_executor::kind::place, 1000), 1u);
        EXPECT_FALSE(h.st.dirty());
        EXPECT_FALSE(h.st.rejected_at(990)) << "only the refused price is latched";
        EXPECT_EQ(h.working(990), 10);
        EXPECT_EQ(h.working(980), 10);
        EXPECT_CONSISTENT(h);
    }
}

TEST(StackerRejects, RetryableNewRejectIsResent) {
    for (const auto r : {reject_reason_t::throttled, reject_reason_t::retryable}) {
        SCOPED_TRACE(static_cast<int>(r));
        buy_harness h{ladder_cfg()};
        h.st.quote(1000, 25);
        std::size_t cursor = 0;
        round_trip_rejecting(h, 1000, r, cursor);
        EXPECT_FALSE(h.st.rejected_at(1000));
        round_trip_rejecting(h, 1000, r, cursor);
        EXPECT_EQ(sent_at(h.exec, mock_executor::kind::place, 1000), 2u);
        EXPECT_CONSISTENT(h);
    }
}

// Re-asserting the same quote is not the caller asking again; changing the
// level's target is.
TEST(StackerRejects, LatchLiftsWhenTheTargetChanges) {
    buy_harness h{ladder_cfg()};
    h.st.quote(1000, 25);
    std::size_t cursor = 0;
    round_trip_rejecting(h, 1000, reject_reason_t::risk_limit, cursor);
    ASSERT_TRUE(h.st.rejected_at(1000));

    h.st.quote(1000, 25);
    h.settle();
    EXPECT_EQ(sent_at(h.exec, mock_executor::kind::place, 1000), 1u);
    EXPECT_TRUE(h.st.rejected_at(1000));

    h.st.quote(1000, 20);
    EXPECT_TRUE(h.st.rejected_at(1000)) << "not until the shape is applied";
    h.settle();
    EXPECT_FALSE(h.st.rejected_at(1000));
    EXPECT_EQ(sent_at(h.exec, mock_executor::kind::place, 1000), 2u);
    EXPECT_EQ(h.working(1000), 20);
    EXPECT_CONSISTENT(h);
}

// A quote that walks away from a refused price and comes back asks for it anew.
TEST(StackerRejects, LatchLiftsWhenTheQuoteMovesAway) {
    buy_harness h{top_only_cfg()};
    h.st.quote(1000, 25);
    std::size_t cursor = 0;
    round_trip_rejecting(h, 1000, reject_reason_t::invalid_price, cursor);
    ASSERT_TRUE(h.st.rejected_at(1000));

    h.quote(990, 25);
    h.ack_all();
    EXPECT_EQ(h.working(990), 25);
    h.quote(1000, 25);
    EXPECT_EQ(sent_at(h.exec, mock_executor::kind::modify, 1000), 1u)
        << "the order at 990 is repriced back up";
    h.ack_all();
    EXPECT_EQ(h.working(1000), 25);
    EXPECT_CONSISTENT(h);
}

TEST(StackerRejects, ClearRejectsRetries) {
    buy_harness h{ladder_cfg()};
    h.st.quote(1000, 25);
    std::size_t cursor = 0;
    round_trip_rejecting(h, 1000, reject_reason_t::risk_limit, cursor);
    ASSERT_TRUE(h.st.rejected_at(1000));
    h.reconcile();
    ASSERT_FALSE(h.st.dirty());

    h.st.clear_rejects();
    EXPECT_FALSE(h.st.rejected_at(1000));
    EXPECT_TRUE(h.st.dirty());
    h.settle();
    EXPECT_EQ(sent_at(h.exec, mock_executor::kind::place, 1000), 2u);
    EXPECT_EQ(h.working(1000), 25);
    EXPECT_CONSISTENT(h);

    h.st.clear_rejects();
    EXPECT_FALSE(h.st.dirty()) << "nothing was latched, so nothing to do";
}

// A shrink the venue will not take: the order is cancelled and replaced at the
// smaller size instead, and the modify is never sent again.
TEST(StackerRejects, TerminalModifyRejectOfAShrinkCancelsInstead) {
    buy_harness h{top_only_cfg()};
    h.quote(1000, 25);
    h.ack_all();
    const auto id = placed_at(h.exec, 1000);

    h.st.quote(1000, 20);
    h.reconcile();
    ASSERT_EQ(h.exec.count(mock_executor::kind::modify), 1u);
    h.ignore_pending();

    h.reject_modify(id, reject_reason_t::invalid_qty);
    EXPECT_FALSE(h.st.rejected_at(1000)) << "the price was never in question";
    ASSERT_CONSISTENT(h);

    const auto mark = h.exec.mark();
    h.settle();
    EXPECT_EQ(h.exec.count(mock_executor::kind::modify, mark), 0u);
    EXPECT_EQ(h.exec.count(mock_executor::kind::cancel, mark), 1u);
    EXPECT_EQ(h.exec.count(mock_executor::kind::place, mark), 1u);
    EXPECT_EQ(h.working(1000), 20);
    EXPECT_EQ(h.exec.venue_qty_at(1000), 20);
    EXPECT_FALSE(h.st.dirty());
    EXPECT_CONSISTENT(h);
}

// The order is not repriced into the refused price again, and no new order is
// sent there in its place. What is left at the old price is no longer wanted.
TEST(StackerRejects, TerminalModifyRejectOfARepriceLatchesTheDestination) {
    buy_harness h{top_only_cfg()};
    h.quote(1000, 25);
    h.ack_all();

    h.st.quote(1010, 25);
    std::size_t cursor = h.exec.mark();
    round_trip_rejecting(h, 1010, reject_reason_t::invalid_price, cursor);
    ASSERT_EQ(sent_at(h.exec, mock_executor::kind::modify, 1010), 1u);
    EXPECT_TRUE(h.st.rejected_at(1010));
    ASSERT_CONSISTENT(h);

    for (int i = 0; i < 4; ++i) {
        round_trip_rejecting(h, 1010, reject_reason_t::invalid_price, cursor);
    }
    EXPECT_EQ(sent_at(h.exec, mock_executor::kind::modify, 1010), 1u);
    EXPECT_EQ(sent_at(h.exec, mock_executor::kind::place, 1010), 0u);
    EXPECT_EQ(h.exec.count(mock_executor::kind::cancel), 1u) << "the order left at 1000";
    EXPECT_EQ(h.working(1000), 0);
    EXPECT_EQ(h.working(1010), 0);
    EXPECT_EQ(h.st.live_order_count(), 0u);
    EXPECT_FALSE(h.st.dirty());
    EXPECT_CONSISTENT(h);
}

// "Too late" says the order is finishing, not that the destination is bad: the
// quantity still goes there, as a new order.
TEST(StackerRejects, TooLateModifyRejectLatchesNothing) {
    buy_harness h{top_only_cfg()};
    h.quote(1000, 25);
    h.ack_all();
    const auto id = placed_at(h.exec, 1000);

    h.st.quote(1010, 25);
    h.reconcile();
    h.ignore_pending();
    h.reject_modify(id, reject_reason_t::too_late_to_act);
    EXPECT_FALSE(h.st.rejected_at(1010));
    ASSERT_CONSISTENT(h);

    const auto mark = h.exec.mark();
    h.reconcile();
    EXPECT_EQ(h.exec.count(mock_executor::kind::modify, mark), 0u) << "not modified again";
    EXPECT_EQ(h.exec.count(mock_executor::kind::cancel, mark), 1u);
    EXPECT_EQ(sent_at(h.exec, mock_executor::kind::place, 1010, mark), 1u);
    EXPECT_EQ(h.working(1010), 25);
    EXPECT_CONSISTENT(h);
}

// A terminal reject of a new order that was carrying a deferred reprice refuses
// the price the order was placed at, not the one it was about to be moved to.
// The stack has already left the placed price, so there is nothing to latch.
TEST(StackerRejects, TerminalNewRejectLatchesThePlacedPrice) {
    auto cfg = top_only_cfg();
    cfg.ack_required = true;
    buy_harness h{cfg};
    h.quote(1000, 25);
    const auto id = placed_at(h.exec, 1000);
    h.ignore_pending();

    h.quote(1010, 25);  // deferred onto the acknowledgement
    ASSERT_EQ(h.exec.count(mock_executor::kind::modify), 0u);
    ASSERT_EQ(h.working(1010), 25);

    h.reject_new(id, reject_reason_t::invalid_price);
    EXPECT_FALSE(h.st.rejected_at(1010));
    ASSERT_CONSISTENT(h);

    h.settle();
    EXPECT_EQ(h.working(1010), 25);
    EXPECT_EQ(sent_at(h.exec, mock_executor::kind::place, 1000), 1u);
    EXPECT_CONSISTENT(h);
}
