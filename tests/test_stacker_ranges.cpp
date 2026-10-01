// Copyright 2026 Slick Quant
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <array>
#include <limits>

#include "harness.hpp"

using namespace testing_support;
using slick::stacker::config_error;
using slick::stacker::k_max_price;
using slick::stacker::k_max_qty;
using slick::stacker::k_min_price;
using slick::stacker::k_null_price;
using slick::stacker::side_t;

namespace {

constexpr price_t k_i64_max = std::numeric_limits<price_t>::max();
constexpr price_t k_i64_min = std::numeric_limits<price_t>::min();

stacker_config base_cfg() {
    stacker_config cfg;
    cfg.tick_size = 10;
    cfg.levels = 3;
    cfg.stack_qty = 10;
    cfg.ack_required = false;
    return cfg;
}

using buy_harness = harness<side_t::buy>;
using sell_harness = harness<side_t::sell>;

}  // namespace

// ---------------------------------------------------------------------------
// Prices
// ---------------------------------------------------------------------------

// A price 2^32 ticks from the anchor used to truncate to depth 0 and read --
// or, from a fill, consume -- the quote's own level.
TEST(StackerRanges, DistantPriceDoesNotAliasOntoALiveLevel) {
    auto cfg = base_cfg();
    cfg.tick_size = 1;
    cfg.levels = 0;
    buy_harness h{cfg};
    h.quote(1000, 25);
    h.ack_all();
    ASSERT_EQ(h.working(1000), 25);

    const price_t alias = 1000 - (price_t{1} << 32);
    EXPECT_EQ(h.working(alias), 0);
    EXPECT_EQ(h.target(alias), 0);

    h.st.on_filled(h.last_placed(), 5, alias);
    EXPECT_EQ(h.target(1000), 25) << "a fill at a price with no level consumes nothing";
    EXPECT_CONSISTENT(h);
}

TEST(StackerRanges, LookupsAtTheEdgesOfThePriceTypeMatchNothing) {
    buy_harness h{base_cfg()};
    h.quote(-1000, 25);
    h.ack_all();

    for (const price_t px : {k_i64_min, k_i64_max, k_min_price - 1, k_max_price + 1}) {
        EXPECT_EQ(h.working(px), 0) << px;
        h.st.on_book_level(px, 100);
    }
    EXPECT_EQ(h.working(-1000), 25);
    EXPECT_CONSISTENT(h);
}

TEST(StackerRanges, QuoteOutsideThePriceRangeQuotesNothing) {
    buy_harness h{base_cfg()};
    h.quote(k_max_price + 1, 10);
    EXPECT_EQ(h.exec.total(), 0u);

    h.quote(1000, 25);
    h.ack_all();
    ASSERT_EQ(h.working(1000), 25);

    // Out of range is treated as no quote at all: the stack comes out.
    h.quote(k_i64_min, 25);
    h.settle();
    EXPECT_EQ(h.exec.live_orders(), 0u);
    EXPECT_FALSE(h.st.dirty());
    EXPECT_CONSISTENT(h);
}

// The rung prices are computed, so a quote near the edge must not let them
// run past it -- for a sell, all the way to the `k_null_price` sentinel.
TEST(StackerRanges, SellLadderPastThePriceRangeQuotesNothing) {
    sell_harness h{base_cfg()};
    h.quote(k_i64_max - 30, 10);
    EXPECT_EQ(h.exec.total(), 0u) << "would have priced a level at k_null_price";

    h.quote(k_max_price - 20, 10);
    EXPECT_EQ(h.exec.total(), 0u) << "deepest rung one tick past the range";

    h.quote(k_max_price - 30, 10);
    EXPECT_EQ(h.exec.count(mock_executor::kind::place), 4u) << "deepest rung on the edge";
    EXPECT_EQ(h.working(k_max_price), 10);
    EXPECT_EQ(h.st.nearest_live_price(), k_max_price - 30);
    EXPECT_CONSISTENT(h);
}

TEST(StackerRanges, BuyLadderPastThePriceRangeQuotesNothing) {
    buy_harness h{base_cfg()};
    h.quote(k_min_price + 20, 10);
    EXPECT_EQ(h.exec.total(), 0u);

    h.quote(k_min_price + 30, 10);
    EXPECT_EQ(h.exec.count(mock_executor::kind::place), 4u);
    EXPECT_EQ(h.working(k_min_price), 10);
    EXPECT_CONSISTENT(h);
}

// A tick wide enough that the ladder's span overflows if multiplied out.
TEST(StackerRanges, HugeTickDoesNotOverflowTheLadderCheck) {
    auto cfg = base_cfg();
    cfg.tick_size = k_max_price;
    sell_harness h{cfg};
    h.quote(0, 10);
    EXPECT_EQ(h.exec.total(), 0u);

    cfg.levels = 1;
    h.st.configure(cfg);
    h.quote(0, 10);
    EXPECT_EQ(h.working(0), 10);
    EXPECT_EQ(h.working(k_max_price), 10);
    EXPECT_CONSISTENT(h);
}

// With the quote withdrawn by passing the null price, a tick change used to
// rebuild the grid around that sentinel, after which every depth overflowed.
TEST(StackerRanges, TickChangeWithNoQuoteKeepsTheGridInRange) {
    buy_harness h{base_cfg()};
    h.quote(1000, 25);
    h.ack_all();
    h.quote(k_null_price, 25);
    h.ack_all();
    ASSERT_EQ(h.exec.live_orders(), 0u);

    auto cfg = base_cfg();
    cfg.tick_size = 5;
    h.st.configure(cfg);
    h.quote(-1000, 25);
    h.ack_all();
    EXPECT_EQ(h.working(-1000), 25);
    EXPECT_EQ(h.working(-1005), 10);
    EXPECT_CONSISTENT(h);
}

// The venue's word on where an order rests is the one price the stacker does
// not choose. One the grid cannot address belongs to no level: the order is
// cancelled and the level rebuilt, rather than booked at a garbage depth.
TEST(StackerRanges, VenueBookingTheGridCannotAddressIsTakenOut) {
    for (const price_t booked : {k_i64_min, k_max_price + 1, price_t{1005}}) {
        auto cfg = base_cfg();
        cfg.levels = 0;
        buy_harness h{cfg};
        h.st.quote(1000, 25);
        h.reconcile();
        const auto id = h.last_placed();
        const auto mark = h.exec.mark();

        h.ack_all_adjusting(id, booked, 25);
        EXPECT_CONSISTENT(h);
        EXPECT_EQ(h.acked(1000), 0) << booked;
        EXPECT_EQ(h.exec.count(mock_executor::kind::cancel, mark), 1u) << booked;

        h.settle();
        EXPECT_FALSE(h.exec.orders[id].live) << booked;
        EXPECT_EQ(h.working(1000), 25) << booked;
        EXPECT_EQ(h.exec.venue_qty_at(1000), 25) << booked;
        EXPECT_CONSISTENT(h);
    }
}

// ---------------------------------------------------------------------------
// Quantities
// ---------------------------------------------------------------------------

TEST(StackerRanges, QuoteQuantityIsClamped) {
    auto cfg = base_cfg();
    cfg.levels = 0;
    buy_harness h{cfg};
    h.quote(1000, std::numeric_limits<qty_t>::max());
    ASSERT_EQ(h.exec.log.size(), 1u);
    EXPECT_EQ(h.exec.log[0].qty, k_max_qty);
    EXPECT_EQ(h.target(1000), k_max_qty);
    EXPECT_EQ(h.st.quote_qty(), k_max_qty);
    h.ack_all();
    EXPECT_CONSISTENT(h);
}

TEST(StackerRanges, VenueQuantitiesAreClamped) {
    auto cfg = base_cfg();
    cfg.levels = 0;
    cfg.refill_on_fill = true;
    buy_harness h{cfg};
    h.st.quote(1000, 25);
    h.reconcile();
    const auto id = h.last_placed();

    h.st.on_accepted(id, 1000, std::numeric_limits<qty_t>::max());
    EXPECT_EQ(h.acked(1000), k_max_qty);
    EXPECT_CONSISTENT(h);

    // Fills and cancels this large would overflow the order's counters if
    // they were added up unchecked. Saturated, the order is simply done.
    h.st.on_filled(id, std::numeric_limits<qty_t>::max(), 1000);
    h.st.on_filled(id, std::numeric_limits<qty_t>::max(), 1000);
    EXPECT_CONSISTENT(h);
    EXPECT_EQ(h.acked(1000), 0);
    EXPECT_EQ(h.st.live_order_count(), 0u);

    h.reconcile();
    const auto id2 = h.last_placed();
    ASSERT_NE(id2, id);
    h.st.on_accepted(id2, 1000, 25);
    h.st.on_canceled(id2, std::numeric_limits<qty_t>::max());
    EXPECT_CONSISTENT(h);
    EXPECT_EQ(h.st.live_order_count(), 0u);
}

// `market_qty - qty_in_front` overflowed for a book update of INT64_MIN, and
// wrapped round to a gap wide enough to open the gate.
TEST(StackerRanges, BookAndQueueFeedsAreClamped) {
    auto cfg = base_cfg();
    cfg.levels = 1;
    cfg.stack_qty = 30;
    cfg.max_order_qty = 10;
    cfg.max_orders_per_level = 3;
    cfg.queue_gap = 5;
    buy_harness h{cfg};
    h.quote(1000, 10);
    h.ack_all();
    ASSERT_EQ(h.orders_at(990), 1) << "the gate holds the rung to one order";

    const auto last = h.exec.log.back().id;
    ASSERT_EQ(h.exec.log.back().price, 990);
    h.st.on_queue_position(last, std::numeric_limits<qty_t>::min());
    h.st.on_queue_position(last, 1);
    h.st.on_book_level(990, std::numeric_limits<qty_t>::min());
    h.settle();
    EXPECT_EQ(h.orders_at(990), 1) << "an empty book opens no gate";

    h.st.on_book_level(990, std::numeric_limits<qty_t>::max());
    EXPECT_TRUE(h.st.dirty()) << "a full book behind the order opens it";
    h.reconcile();
    EXPECT_EQ(h.orders_at(990), 2);
    EXPECT_CONSISTENT(h);
}

TEST(StackerRanges, ConfigQuantitiesAboveTheRangeAreRejectedAndClamped) {
    auto cfg = base_cfg();
    EXPECT_EQ(cfg.validate<test_traits>(), config_error::ok);

    cfg.max_level_qty = k_max_qty;
    EXPECT_EQ(cfg.validate<test_traits>(), config_error::ok);
    cfg.max_level_qty = k_no_qty_limit;
    EXPECT_EQ(cfg.validate<test_traits>(), config_error::ok) << "the sentinel is not a limit";

    cfg.max_level_qty = k_max_qty + 1;
    EXPECT_EQ(cfg.validate<test_traits>(), config_error::qty_out_of_range);
    EXPECT_EQ(cfg.sanitized<test_traits>().max_level_qty, k_max_qty);

    cfg = base_cfg();
    cfg.stack_qty = k_max_qty + 1;
    EXPECT_EQ(cfg.validate<test_traits>(), config_error::qty_out_of_range);
    EXPECT_EQ(cfg.sanitized<test_traits>().stack_qty, k_max_qty);
    EXPECT_EQ(cfg.sanitized<test_traits>().validate<test_traits>(), config_error::ok);

    static constexpr std::array<qty_t, 3> profile{1, std::numeric_limits<qty_t>::max(), 3};
    cfg = base_cfg();
    cfg.qty_profile = profile;
    EXPECT_EQ(cfg.validate<test_traits>(), config_error::qty_out_of_range);

    // The stacker clamps the profile as it copies it.
    buy_harness h{cfg};
    EXPECT_EQ(h.st.config().validate<test_traits>(), config_error::ok);
    ASSERT_EQ(h.st.config().qty_profile.size(), 3u);
    EXPECT_EQ(h.st.config().qty_profile[1], k_max_qty);

    h.st.set_stack_qty(std::numeric_limits<qty_t>::max());
    EXPECT_EQ(h.st.config().stack_qty, k_max_qty);
}
