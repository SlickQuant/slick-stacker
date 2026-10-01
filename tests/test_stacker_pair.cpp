// Copyright 2026 Slick Quant
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>
#include <slick/stacker/executor_adapter.hpp>
#include <slick/stacker/stacker_pair.hpp>

#include <vector>

#include "harness.hpp"

using namespace testing_support;
using slick::stacker::function_executor;
using slick::stacker::k_null_price;
using slick::stacker::side_t;
using slick::stacker::stacker_pair;

namespace {

using pair_type = stacker_pair<mock_executor, test_traits>;

stacker_config base_cfg() {
    stacker_config cfg;
    cfg.tick_size = 10;
    cfg.levels = 1;
    cfg.stack_qty = 10;
    cfg.ack_required = false;
    return cfg;
}

/// Drives a pair the way `harness` drives one side: reconcile, then play the
/// venue's responses back in the order they were requested.
struct pair_harness {
    explicit pair_harness(const stacker_config& cfg) : pair(exec, cfg) {}

    void ack_all() {
        while (cursor < exec.log.size()) {
            const auto m = exec.log[cursor++];
            const side_t s = exec.side_of(m.id);
            switch (m.type) {
                case mock_executor::kind::place:
                    pair.on_accepted(s, m.id, m.price, m.qty);
                    break;
                case mock_executor::kind::modify:
                    exec.confirm_modify(m.id);
                    pair.on_replaced(s, m.id, m.price, m.qty);
                    break;
                case mock_executor::kind::cancel: {
                    auto& o = exec.orders[m.id];
                    const qty_t left = o.qty - o.filled;
                    o.live = false;
                    pair.on_canceled(s, m.id, left);
                    break;
                }
            }
        }
    }

    void settle(int rounds = 8) {
        for (int i = 0; i < rounds; ++i) {
            const std::size_t before = exec.log.size();
            pair.reconcile();
            ack_all();
            if (exec.log.size() == before && !pair.dirty()) {
                return;
            }
        }
        pair.reconcile();
        ack_all();
    }

    mock_executor exec;
    pair_type pair;
    std::size_t cursor = 0;
};

/// True when the venue could be holding one of our bids at or above one of our
/// offers. An order with a modify in flight counts at both its old and its new
/// price, since either may be the one resting when the other side arrives.
bool venue_crossed(const mock_executor& exec) {
    price_t best_bid = k_null_price;
    price_t best_ask = k_null_price;
    auto note = [&](side_t s, price_t px) {
        price_t& best = s == side_t::buy ? best_bid : best_ask;
        if (best == k_null_price || (s == side_t::buy ? px > best : px < best)) {
            best = px;
        }
    };
    for (const auto& [id, o] : exec.orders) {
        if (!o.live) {
            continue;
        }
        note(o.side, o.price);
        if (exec.has_pending_modify(id)) {
            note(o.side, exec.pending_modify(id).first);
        }
    }
    return best_bid != k_null_price && best_ask != k_null_price && best_bid >= best_ask;
}

}  // namespace

TEST(StackerPair, BothSidesQuoteIndependently) {
    pair_harness h{base_cfg()};
    h.pair.on_top_of_book(1000, 1010);
    h.pair.quote(1000, 25, 1010, 25);
    h.settle();

    EXPECT_EQ(h.pair.buy().working_at(1000), 25);
    EXPECT_EQ(h.pair.buy().working_at(990), 10);
    EXPECT_EQ(h.pair.sell().working_at(1010), 25);
    EXPECT_EQ(h.pair.sell().working_at(1020), 10);
    EXPECT_TRUE(h.pair.validate());
}

// The bid stack trying to move up into a price the offer stack is still
// working at is the hazard the pair exists to prevent.
TEST(StackerPair, BidWillNotStepOntoALiveOffer) {
    pair_harness h{base_cfg()};
    h.pair.on_top_of_book(1000, 1010);
    h.pair.quote(1000, 25, 1010, 25);
    h.settle();
    ASSERT_EQ(h.pair.sell().working_at(1010), 25);

    // Ask the bid for 1010 while the offer is still resting there.
    h.pair.on_top_of_book(1010, 1020);
    h.pair.buy().quote(1010, 25);
    h.pair.reconcile();
    h.ack_all();

    EXPECT_EQ(h.pair.buy().working_at(1010), 0) << "must not quote into our own offer";
    EXPECT_TRUE(h.pair.validate());
}

TEST(StackerPair, BidTakesThePriceOnceTheOfferIsWithdrawn) {
    pair_harness h{base_cfg()};
    h.pair.on_top_of_book(1000, 1010);
    h.pair.quote(1000, 25, 1010, 25);
    h.settle();

    // Pull the offer and lift the bid to where it was, in one batch.
    h.pair.on_top_of_book(1010, 1030);
    h.pair.sell().quote(1030, 25);
    h.pair.buy().quote(1010, 25);
    h.settle(16);

    EXPECT_EQ(h.pair.buy().working_at(1010), 25);
    EXPECT_EQ(h.pair.buy().working_at(1000), 10);
    EXPECT_EQ(h.pair.sell().working_at(1030), 25);
    EXPECT_EQ(h.pair.sell().working_at(1040), 10);
    EXPECT_EQ(h.exec.venue_qty_at(1020), 0);
    EXPECT_TRUE(h.pair.validate());
}

TEST(StackerPair, OfferWillNotStepOntoALiveBid) {
    pair_harness h{base_cfg()};
    h.pair.on_top_of_book(1000, 1010);
    h.pair.quote(1000, 25, 1010, 25);
    h.settle();
    ASSERT_EQ(h.pair.buy().working_at(1000), 25);

    h.pair.on_top_of_book(990, 1000);
    h.pair.sell().quote(1000, 25);
    h.pair.reconcile();
    h.ack_all();

    EXPECT_EQ(h.pair.sell().working_at(1000), 0);
    EXPECT_TRUE(h.pair.validate());
}

// A caller that only reconciles when `dirty()` must still see the market
// moving off a price the bid was held back from.
TEST(StackerPair, MarketMoveThatUnblocksALevelMarksThePairDirty) {
    pair_harness h{base_cfg()};
    h.pair.on_top_of_book(1000, 1010);
    h.pair.quote(1010, 25, 1030, 25);
    h.settle();
    ASSERT_EQ(h.pair.buy().working_at(1010), 0) << "blocked by the market ask";
    ASSERT_FALSE(h.pair.dirty());

    h.pair.on_top_of_book(1000, 1020);
    EXPECT_TRUE(h.pair.dirty());
    if (h.pair.dirty()) {
        h.settle();
    }
    EXPECT_EQ(h.pair.buy().working_at(1010), 25);
    EXPECT_TRUE(h.pair.validate());
}

TEST(StackerPair, MarketMoveThatUnblocksAnOfferMarksThePairDirty) {
    pair_harness h{base_cfg()};
    h.pair.on_top_of_book(1010, 1020);
    h.pair.quote(990, 25, 1010, 25);
    h.settle();
    ASSERT_EQ(h.pair.sell().working_at(1010), 0) << "blocked by the market bid";
    ASSERT_FALSE(h.pair.dirty());

    h.pair.on_top_of_book(1000, 1020);
    EXPECT_TRUE(h.pair.dirty());
    if (h.pair.dirty()) {
        h.settle();
    }
    EXPECT_EQ(h.pair.sell().working_at(1010), 25);
    EXPECT_TRUE(h.pair.validate());
}

// The quiet-tick fast path: a top that does not change either side's limit
// must not drag a reconcile behind it.
TEST(StackerPair, TopOfBookThatLeavesTheLimitsAloneStaysClean) {
    pair_harness h{base_cfg()};
    h.pair.on_top_of_book(1000, 1010);
    h.pair.quote(1000, 25, 1010, 25);
    h.settle();
    ASSERT_FALSE(h.pair.dirty());

    h.pair.on_top_of_book(1000, 1010);
    EXPECT_FALSE(h.pair.dirty()) << "unchanged top";

    // Our own offer at 1010 is still the tighter limit for the bid, and our
    // bid at 1000 for the offer, so the market backing off changes nothing.
    h.pair.on_top_of_book(990, 1020);
    EXPECT_FALSE(h.pair.dirty()) << "own orders still bound both limits";
}

// The bid ladder reaching down is fine; it is the offer ladder reaching down
// into it that has to be stopped. Neither ladder may overlap the other.
TEST(StackerPair, LaddersMayNotOverlap) {
    auto cfg = base_cfg();
    cfg.levels = 3;
    pair_harness h{cfg};

    h.pair.on_top_of_book(1000, 1010);
    h.pair.quote(1000, 25, 1010, 25);
    h.settle();

    // Bid ladder occupies 1000/990/980/970, offer 1010/1020/1030/1040.
    for (price_t px = 970; px <= 1000; px += 10) {
        EXPECT_EQ(h.pair.sell().working_at(px), 0) << "offer found at " << px;
    }
    for (price_t px = 1010; px <= 1040; px += 10) {
        EXPECT_EQ(h.pair.buy().working_at(px), 0) << "bid found at " << px;
    }
    EXPECT_TRUE(h.pair.validate());
}

// With no market top there is nothing but our own orders to stop a crossed
// quote. The bid reconciles first, so the offer's limit has to reflect what the
// bid just sent, not what it held before this pass.
TEST(StackerPair, CrossedInitialQuoteOnDormantBookDoesNotSelfCross) {
    pair_harness h{base_cfg()};
    h.pair.quote(1010, 25, 1000, 25);
    h.pair.reconcile();
    EXPECT_FALSE(venue_crossed(h.exec)) << "a single pass sent both sides into each other";

    h.ack_all();
    h.settle();
    EXPECT_FALSE(venue_crossed(h.exec));
    EXPECT_EQ(h.pair.buy().working_at(1010), 25);
    EXPECT_EQ(h.pair.sell().working_at(1000), 0);
    EXPECT_EQ(h.pair.sell().working_at(1010), 0);
    EXPECT_TRUE(h.pair.validate());
}

TEST(StackerPair, OverlappingLaddersOnDormantBookDoNotSelfCross) {
    auto cfg = base_cfg();
    cfg.levels = 3;
    pair_harness h{cfg};

    // Bid ladder wants 1000..970, offer ladder 980..1010: three prices shared.
    h.pair.quote(1000, 25, 980, 25);
    h.pair.reconcile();
    EXPECT_FALSE(venue_crossed(h.exec));

    h.ack_all();
    h.settle();
    EXPECT_FALSE(venue_crossed(h.exec));
    for (price_t px = 980; px <= 1000; px += 10) {
        EXPECT_EQ(h.pair.sell().working_at(px), 0) << "offer found at " << px;
    }
    EXPECT_EQ(h.pair.buy().working_at(1000), 25);
    EXPECT_TRUE(h.pair.validate());
}

TEST(StackerPair, PullClearsBothSides) {
    pair_harness h{base_cfg()};
    h.pair.on_top_of_book(1000, 1010);
    h.pair.quote(1000, 25, 1010, 25);
    h.settle();
    ASSERT_GT(h.exec.live_orders(), 0u);

    h.pair.pull();
    h.settle(16);
    EXPECT_EQ(h.exec.live_orders(), 0u);
    EXPECT_EQ(h.pair.buy().live_order_count(), 0u);
    EXPECT_EQ(h.pair.sell().live_order_count(), 0u);
    EXPECT_TRUE(h.pair.validate());
}

TEST(StackerPair, FillsAreRoutedToTheRightSide) {
    pair_harness h{base_cfg()};
    h.pair.on_top_of_book(1000, 1010);
    h.pair.quote(1000, 25, 1010, 25);
    h.settle();

    mock_executor::order_id_t bid_id = 0;
    mock_executor::order_id_t ask_id = 0;
    for (const auto& m : h.exec.log) {
        if (m.type != mock_executor::kind::place) {
            continue;
        }
        if (m.price == 1000) {
            bid_id = m.id;
        }
        if (m.price == 1010) {
            ask_id = m.id;
        }
    }
    ASSERT_NE(bid_id, 0u);
    ASSERT_NE(ask_id, 0u);

    h.pair.on_filled(side_t::buy, bid_id, 10, 1000);
    h.pair.on_filled(side_t::sell, ask_id, 5, 1010);

    EXPECT_EQ(h.pair.buy().acked_at(1000), 15);
    EXPECT_EQ(h.pair.sell().acked_at(1010), 20);
    EXPECT_TRUE(h.pair.validate());
}

TEST(StackerPair, SeparateConfigurationPerSide) {
    auto bid_cfg = base_cfg();
    bid_cfg.levels = 1;
    bid_cfg.stack_qty = 5;
    auto ask_cfg = base_cfg();
    ask_cfg.levels = 3;
    ask_cfg.stack_qty = 20;

    mock_executor exec;
    pair_type pair{exec, bid_cfg, ask_cfg};
    pair.on_top_of_book(1000, 1010);
    pair.quote(1000, 25, 1010, 25);
    pair.reconcile();

    EXPECT_EQ(pair.buy().working_at(990), 5);
    EXPECT_EQ(pair.buy().working_at(980), 0);
    EXPECT_EQ(pair.sell().working_at(1020), 20);
    EXPECT_EQ(pair.sell().working_at(1040), 20);
    EXPECT_TRUE(pair.validate());
}

// --- the std::function adapter ---------------------------------------------

TEST(FunctionExecutor, DrivesAStackerLikeAnyOther) {
    struct message {
        char kind;
        price_t price;
        qty_t qty;
    };
    std::vector<message> log;
    std::uint64_t next = 1;

    function_executor<> exec{
        [&](side_t, price_t px, qty_t q, slick::stacker::order_type_t) {
            log.push_back({'P', px, q});
            return next++;
        },
        [&](const std::uint64_t&, price_t px, qty_t q) {
            log.push_back({'M', px, q});
            return true;
        },
        [&](const std::uint64_t&) {
            log.push_back({'C', 0, 0});
            return true;
        }};

    auto cfg = base_cfg();
    slick::stacker::stacker<function_executor<>, side_t::buy, test_traits> st{exec, cfg};
    st.quote(1000, 25);
    st.reconcile();

    ASSERT_EQ(log.size(), 2u);
    EXPECT_EQ(log[0].kind, 'P');
    EXPECT_EQ(log[0].price, 1000);
    EXPECT_EQ(log[0].qty, 25);
    EXPECT_EQ(log[1].price, 990);
    EXPECT_EQ(log[1].qty, 10);
    EXPECT_TRUE(st.validate());
}

// A half-wired adapter must fail safe: the stacker leaves the level short and
// retries rather than believing in orders that were never sent.
TEST(FunctionExecutor, UnsetCallbacksBehaveAsRefusals) {
    function_executor<> exec;  // nothing bound
    auto cfg = base_cfg();
    slick::stacker::stacker<function_executor<>, side_t::buy, test_traits> st{exec, cfg};

    st.quote(1000, 25);
    st.reconcile();
    EXPECT_EQ(st.live_order_count(), 0u);
    EXPECT_EQ(st.working_at(1000), 0);
    EXPECT_TRUE(st.dirty()) << "a refused send must leave work outstanding";
    EXPECT_TRUE(st.validate());
}
