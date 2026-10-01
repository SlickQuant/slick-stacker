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

// --- chained modifies -------------------------------------------------------
//
// Venues that chain replaces on the client order id accept a modify against an
// order whose previous one is unanswered. Holding back would put a round trip
// between the strategy deciding and the venue hearing about it.

namespace {

stacker_config chain_cfg(std::uint8_t depth) {
    auto cfg = strict_cfg();
    cfg.ack_required = false;
    cfg.max_inflight_modifies = depth;
    return cfg;
}

/// Drive the top level to `qty` and reconcile, without acknowledging anything.
void retarget(buy_harness& h, qty_t qty) {
    h.st.quote(1000, qty);
    h.reconcile();
}

}  // namespace

TEST(StackerChainedModify, SecondModifyGoesOutImmediately) {
    buy_harness h{chain_cfg(4)};
    h.quote(1000, 25);
    h.ack_all();
    const auto id = placed_at(h.exec, 1000);

    const auto mark = h.exec.mark();
    retarget(h, 20);
    retarget(h, 15);

    EXPECT_EQ(h.exec.count(mock_executor::kind::modify, mark), 2u)
        << "the second change must not wait for the first to be answered";
    EXPECT_EQ(h.exec.log.back().qty, 15);
    EXPECT_EQ(h.working(1000), 15);
    EXPECT_EQ(h.acked(1000), 25) << "the venue has confirmed none of it yet";
    ASSERT_CONSISTENT(h);

    // Both answers arrive in order; the order settles at the last intent.
    h.st.on_replaced(id, 1000, 20);
    ASSERT_CONSISTENT(h);
    EXPECT_EQ(h.working(1000), 15) << "an intermediate ack must not settle the order";
    h.st.on_replaced(id, 1000, 15);
    EXPECT_EQ(h.working(1000), 15);
    EXPECT_EQ(h.acked(1000), 15);
    EXPECT_CONSISTENT(h);
}

TEST(StackerChainedModify, DepthOneIsTheDefaultAndStillDefers) {
    buy_harness h{chain_cfg(1)};
    h.quote(1000, 25);
    h.ack_all();
    ASSERT_EQ(h.st.config().max_inflight_modifies, 1);

    const auto mark = h.exec.mark();
    retarget(h, 20);
    retarget(h, 15);
    EXPECT_EQ(h.exec.count(mock_executor::kind::modify, mark), 1u);
    EXPECT_EQ(h.working(1000), 15) << "intent still lands immediately, only the message waits";
    EXPECT_CONSISTENT(h);
}

TEST(StackerChainedModify, ChainDepthIsCapped) {
    buy_harness h{chain_cfg(2)};
    h.quote(1000, 25);
    h.ack_all();
    const auto id = placed_at(h.exec, 1000);

    const auto mark = h.exec.mark();
    retarget(h, 20);
    retarget(h, 15);
    retarget(h, 10);
    EXPECT_EQ(h.exec.count(mock_executor::kind::modify, mark), 2u) << "the third waits";
    EXPECT_EQ(h.working(1000), 10);
    ASSERT_CONSISTENT(h);

    // Answering one frees a slot in the chain, and the deferred change goes.
    h.st.on_replaced(id, 1000, 20);
    EXPECT_EQ(h.exec.count(mock_executor::kind::modify, mark), 3u);
    EXPECT_EQ(h.exec.log.back().qty, 10);
    EXPECT_CONSISTENT(h);
}

// A reject part way along the chain must not clobber the intent carried by the
// requests still in the air behind it.
TEST(StackerChainedModify, RejectMidChainDoesNotDiscardLaterIntent) {
    buy_harness h{chain_cfg(4)};
    h.quote(1000, 25);
    h.ack_all();
    const auto id = placed_at(h.exec, 1000);

    retarget(h, 20);
    retarget(h, 15);
    ASSERT_EQ(h.working(1000), 15);

    h.st.on_modify_rejected(id, reject_reason_t::terminal);
    EXPECT_EQ(h.working(1000), 15) << "the second modify is still outstanding and still wanted";
    EXPECT_EQ(h.acked(1000), 25);
    ASSERT_CONSISTENT(h);

    h.st.on_replaced(id, 1000, 15);
    EXPECT_EQ(h.working(1000), 15);
    EXPECT_EQ(h.acked(1000), 15);
    EXPECT_CONSISTENT(h);
}

TEST(StackerChainedModify, RejectOfTheLastLinkRollsBackToTheLastAck) {
    buy_harness h{chain_cfg(4)};
    h.quote(1000, 25);
    h.ack_all();
    const auto id = placed_at(h.exec, 1000);

    retarget(h, 20);
    retarget(h, 15);

    h.st.on_replaced(id, 1000, 20);  // first link confirmed
    ASSERT_CONSISTENT(h);
    h.st.on_modify_rejected(id, reject_reason_t::retryable);

    EXPECT_EQ(h.acked(1000), 20);
    EXPECT_EQ(h.working(1000), 20) << "reality is what the last acknowledgement said";
    ASSERT_CONSISTENT(h);

    // The target still wants 15, so reconcile tries again.
    const auto mark = h.exec.mark();
    h.settle();
    EXPECT_EQ(h.exec.count(mock_executor::kind::modify, mark), 1u);
    EXPECT_EQ(h.working(1000), 15);
    EXPECT_CONSISTENT(h);
}

TEST(StackerChainedModify, WholeChainRejectedUnwindsToTheOriginal) {
    buy_harness h{chain_cfg(4)};
    h.quote(1000, 25);
    h.ack_all();
    const auto id = placed_at(h.exec, 1000);

    retarget(h, 20);
    retarget(h, 15);

    h.st.on_modify_rejected(id, reject_reason_t::terminal);
    ASSERT_CONSISTENT(h);
    h.st.on_modify_rejected(id, reject_reason_t::terminal);

    EXPECT_EQ(h.acked(1000), 25);
    EXPECT_EQ(h.working(1000), 25) << "nothing was applied, so the order is as it started";
    EXPECT_EQ(h.orders_at(1000), 1u);
    EXPECT_CONSISTENT(h);
}

TEST(StackerChainedModify, FillAgainstAChainedOrder) {
    buy_harness h{chain_cfg(4)};
    h.quote(1000, 25);
    h.ack_all();
    const auto id = placed_at(h.exec, 1000);

    retarget(h, 20);
    retarget(h, 15);

    // The venue is still working 25 while our intent is 15; a fill can take
    // more than we currently intend, which is the exposure a chain buys.
    h.st.on_filled(id, 25, 1000);
    EXPECT_CONSISTENT(h);
    EXPECT_EQ(h.acked(1000), 0);

    // The outstanding answers still have to be absorbed cleanly.
    h.st.on_modify_rejected(id, reject_reason_t::too_late_to_act);
    ASSERT_CONSISTENT(h);
    h.st.on_modify_rejected(id, reject_reason_t::too_late_to_act);
    EXPECT_EQ(h.st.live_order_count(), 0u);
    EXPECT_CONSISTENT(h);
}

TEST(StackerChainedModify, WalkingTheQuoteWithoutWaiting) {
    auto cfg = chain_cfg(8);
    cfg.levels = 2;
    cfg.stack_qty = 10;
    buy_harness h{cfg};
    h.quote(1000, 20);
    h.ack_all();

    // Three price moves back to back with nothing acknowledged in between.
    h.st.quote(1010, 20);
    h.reconcile();
    h.st.quote(1020, 20);
    h.reconcile();
    h.st.quote(1030, 20);
    h.reconcile();
    ASSERT_CONSISTENT(h);

    h.settle(16);
    EXPECT_EQ(h.working(1030), 20);
    EXPECT_EQ(h.working(1020), 10);
    EXPECT_EQ(h.working(1010), 10);
    EXPECT_EQ(h.working(1000), 0);
    EXPECT_EQ(h.exec.venue_qty_at(1000), 0);
    EXPECT_CONSISTENT(h);
}

// A grid rebuild orphans every working order. A reject arriving afterwards for
// one of them must not book it back into a level it no longer belongs to.
TEST(StackerChainedModify, ModifyRejectAfterAGridRebuild) {
    auto cfg = chain_cfg(4);
    buy_harness h{cfg};
    h.quote(1000, 25);
    h.ack_all();
    const auto id = placed_at(h.exec, 1000);

    retarget(h, 20);
    ASSERT_EQ(h.exec.count(mock_executor::kind::modify), 1u);

    // Rebuild the grid on a finer tick. 1000 is still on the new grid, so the
    // rolled-back order would find a level to be re-homed into -- and must
    // refuse it anyway, because it no longer belongs to this book.
    auto finer = cfg;
    finer.tick_size = 5;
    h.st.configure(finer);
    h.reconcile();
    ASSERT_GT(h.st.rebase_count(), 1u);
    ASSERT_CONSISTENT(h);

    // The rebuild re-quoted 1000 with a fresh order. The rolled-back orphan
    // must not join it there -- if it did, the level would count quantity the
    // stacker has already cancelled.
    ASSERT_EQ(h.orders_at(1000), 1u);
    const qty_t before = h.working(1000);

    h.st.on_modify_rejected(id, reject_reason_t::terminal);
    EXPECT_CONSISTENT(h) << "an orphaned order must not be re-homed";
    EXPECT_EQ(h.orders_at(1000), 1u) << "still just the new order";
    EXPECT_EQ(h.working(1000), before);

    h.st.on_canceled(id, 25);
    EXPECT_CONSISTENT(h);
    h.settle(16);
    EXPECT_EQ(h.working(1000), 20);
    EXPECT_EQ(h.st.live_order_count(), 1u) << "the orphan is retired, the new quote is working";
    EXPECT_EQ(h.exec.live_orders(), 1u);
    EXPECT_CONSISTENT(h);
}

// --- orphans ----------------------------------------------------------------
//
// A grid rebuild takes every working order out of the book and cancels it. The
// orphan belongs to no level, so no reconcile pass would ever come back to it:
// unless its cancel is seen through to the end, it stays working at the venue
// while the stack quotes fresh orders on top of it.

namespace {

std::size_t cancels_of(const mock_executor& exec, mock_executor::order_id_t id) {
    std::size_t n = 0;
    for (const auto& m : exec.log) {
        if (m.type == mock_executor::kind::cancel && m.id == id) {
            ++n;
        }
    }
    return n;
}

/// Rebuild the grid underneath the stack by moving to a finer tick.
void rebuild_grid(buy_harness& h) {
    auto finer = h.st.config();
    finer.tick_size = 5;
    const auto rebases = h.st.rebase_count();
    h.st.configure(finer);
    h.reconcile();
    ASSERT_GT(h.st.rebase_count(), rebases);
}

}  // namespace

TEST(StackerOrphan, RefusedCancelIsRetried) {
    buy_harness h{chain_cfg(1)};
    h.quote(1000, 25);
    h.ack_all();
    const auto old = placed_at(h.exec, 1000);

    h.exec.fail_cancel = true;
    const auto mark = h.exec.mark();
    rebuild_grid(h);
    ASSERT_CONSISTENT(h);
    EXPECT_GT(h.exec.refused, 0u);
    EXPECT_EQ(h.st.blocked_count(), 1u) << "the orphan is still working at the venue";
    EXPECT_EQ(h.exec.count(mock_executor::kind::place, mark), 0u)
        << "no fresh quote may go out on top of exposure we could not take off";
    EXPECT_TRUE(h.st.dirty()) << "the refused cancel must be retried";

    h.exec.fail_cancel = false;
    h.reconcile();
    ASSERT_CONSISTENT(h);
    EXPECT_EQ(cancels_of(h.exec, old), 1u) << "the orphan's cancel goes out on the retry";
    EXPECT_EQ(h.st.blocked_count(), 0u);

    h.settle(16);
    EXPECT_FALSE(h.exec.orders[old].live);
    EXPECT_EQ(h.st.live_order_count(), 1u) << "the orphan is retired, the new quote is working";
    EXPECT_EQ(h.exec.live_orders(), 1u);
    EXPECT_EQ(h.working(1000), 25);
    EXPECT_CONSISTENT(h);
}

TEST(StackerOrphan, CancelOfAnUnackedOrderWaitsForTheAck) {
    buy_harness h{strict_cfg()};
    h.quote(1000, 25);
    const auto old = placed_at(h.exec, 1000);

    rebuild_grid(h);
    ASSERT_CONSISTENT(h);
    EXPECT_EQ(cancels_of(h.exec, old), 0u) << "ack_required: nothing may go out unacknowledged";
    EXPECT_EQ(h.st.blocked_count(), 0u) << "a deferred cancel is not a refused one";

    h.ack_all();
    ASSERT_CONSISTENT(h);
    EXPECT_EQ(cancels_of(h.exec, old), 1u) << "the acknowledgement releases the cancel";

    h.settle(16);
    EXPECT_FALSE(h.exec.orders[old].live);
    EXPECT_EQ(h.st.live_order_count(), 1u);
    EXPECT_EQ(h.exec.live_orders(), 1u);
    EXPECT_EQ(h.working(1000), 25);
    EXPECT_CONSISTENT(h);
}

TEST(StackerOrphan, RejectedCancelIsRetried) {
    buy_harness h{chain_cfg(1)};
    h.quote(1000, 25);
    h.ack_all();
    const auto old = placed_at(h.exec, 1000);

    rebuild_grid(h);
    ASSERT_EQ(cancels_of(h.exec, old), 1u);
    h.ignore_pending();

    h.reject_cancel(old);
    ASSERT_CONSISTENT(h);
    EXPECT_EQ(h.st.blocked_count(), 1u);
    EXPECT_TRUE(h.st.dirty());

    h.reconcile();
    EXPECT_EQ(cancels_of(h.exec, old), 2u) << "the rejected cancel is sent again";
    EXPECT_EQ(h.st.blocked_count(), 0u);

    h.settle(16);
    EXPECT_FALSE(h.exec.orders[old].live);
    EXPECT_EQ(h.st.live_order_count(), 1u);
    EXPECT_EQ(h.exec.live_orders(), 1u);
    EXPECT_CONSISTENT(h);
}

// An orphan whose deferred cancel is overtaken by a full fill has nothing left
// to cancel once acknowledged; it must be retired rather than chased.
TEST(StackerOrphan, FilledBeforeTheAckIsRetiredWithoutACancel) {
    buy_harness h{strict_cfg()};
    h.quote(1000, 25);
    const auto old = placed_at(h.exec, 1000);

    rebuild_grid(h);
    h.fill(old, 25);
    ASSERT_CONSISTENT(h);

    h.ack_all();
    ASSERT_CONSISTENT(h);
    EXPECT_EQ(cancels_of(h.exec, old), 0u);
    EXPECT_EQ(h.st.live_order_count(), 1u) << "the filled orphan is retired";
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
