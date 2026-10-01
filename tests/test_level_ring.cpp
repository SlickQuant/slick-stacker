// Copyright 2026 Slick Quant
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>
#include <slick/stacker/detail/level_ring.hpp>

#include <cstdint>
#include <limits>

using slick::stacker::k_max_price;
using slick::stacker::k_min_price;
using slick::stacker::price_t;
using slick::stacker::side_t;
using slick::stacker::detail::k_invalid_depth;
using slick::stacker::detail::k_max_depth;
using slick::stacker::detail::level;
using slick::stacker::detail::level_ring;

namespace {

using buy_ring = level_ring<side_t::buy, 16>;
using sell_ring = level_ring<side_t::sell, 16>;

}  // namespace

// Depth counts away from the market, so a buy stack's depth grows as price
// falls and a sell stack's as price rises. Every comparison downstream relies
// on this, so it is worth pinning both directions explicitly.
TEST(LevelRing, BuyDepthGrowsDownward) {
    buy_ring ring;
    ring.reset(/*anchor=*/10000, /*tick_size=*/10);

    EXPECT_EQ(ring.depth_of(10000), 0);
    EXPECT_EQ(ring.depth_of(9990), 1);
    EXPECT_EQ(ring.depth_of(9900), 10);
    EXPECT_EQ(ring.depth_of(10010), -1);  // price improved past the anchor

    EXPECT_EQ(ring.price_at(0), 10000);
    EXPECT_EQ(ring.price_at(3), 9970);
    EXPECT_EQ(ring.price_at(-2), 10020);
}

TEST(LevelRing, SellDepthGrowsUpward) {
    sell_ring ring;
    ring.reset(10000, 10);

    EXPECT_EQ(ring.depth_of(10000), 0);
    EXPECT_EQ(ring.depth_of(10010), 1);
    EXPECT_EQ(ring.depth_of(10100), 10);
    EXPECT_EQ(ring.depth_of(9990), -1);

    EXPECT_EQ(ring.price_at(0), 10000);
    EXPECT_EQ(ring.price_at(3), 10030);
    EXPECT_EQ(ring.price_at(-2), 9980);
}

TEST(LevelRing, DepthPriceRoundTrip) {
    buy_ring ring;
    ring.reset(50000, 25);
    for (std::int32_t d = -20; d <= 20; ++d) {
        EXPECT_EQ(ring.depth_of(ring.price_at(d)), d) << "depth " << d;
    }
}

TEST(LevelRing, OffGridPriceIsRejected) {
    buy_ring ring;
    ring.reset(10000, 10);
    EXPECT_EQ(ring.depth_of(9995), k_invalid_depth);
    EXPECT_EQ(ring.depth_of(10003), k_invalid_depth);
    EXPECT_NE(ring.depth_of(9990), k_invalid_depth);
}

// A depth that does not fit in 32 bits used to be truncated onto a live one,
// and a price at the far end of the type overflowed the subtraction.
TEST(LevelRing, PriceOutOfReachIsRejected) {
    buy_ring ring;
    ring.reset(1000, 1);
    EXPECT_EQ(ring.depth_of(1000 - (price_t{1} << 32)), k_invalid_depth) << "would alias depth 0";
    EXPECT_EQ(ring.depth_of(1000 - k_max_depth), k_max_depth);
    EXPECT_EQ(ring.depth_of(1000 + k_max_depth), -k_max_depth);
    EXPECT_EQ(ring.depth_of(1000 - k_max_depth - 1), k_invalid_depth);
    EXPECT_EQ(ring.depth_of(1000 + k_max_depth + 1), k_invalid_depth);

    for (const price_t px :
         {std::numeric_limits<price_t>::min(), std::numeric_limits<price_t>::max(), k_min_price - 1,
          k_max_price + 1, k_min_price, k_max_price}) {
        EXPECT_EQ(ring.depth_of(px), k_invalid_depth) << px;
    }

    // Both ends of the range are addressable from an anchor at the other.
    sell_ring wide;
    wide.reset(k_min_price, k_max_price);
    EXPECT_EQ(wide.depth_of(k_min_price), 0);
    EXPECT_EQ(wide.depth_of(k_max_price), 2);
    EXPECT_EQ(wide.price_at(2), k_max_price);
}

TEST(LevelRing, IsBetterOrdersByCloserToMarket) {
    EXPECT_TRUE(buy_ring::is_better(0, 1));
    EXPECT_FALSE(buy_ring::is_better(1, 0));
    EXPECT_TRUE(sell_ring::is_better(-1, 3));
}

TEST(LevelRing, TryAtBindsLazily) {
    buy_ring ring;
    ring.reset(10000, 10);

    EXPECT_EQ(ring.peek(2), nullptr);
    level* l = ring.try_at(2);
    ASSERT_NE(l, nullptr);
    EXPECT_TRUE(l->bound);
    EXPECT_EQ(l->depth, 2);
    EXPECT_EQ(l->price, 9980);
    EXPECT_TRUE(l->idle());

    EXPECT_EQ(ring.peek(2), l);
    EXPECT_EQ(ring.try_at(2), l);  // stable across calls
}

TEST(LevelRing, BandTracksBoundLevels) {
    buy_ring ring;
    ring.reset(10000, 10);
    EXPECT_TRUE(ring.band_empty());

    ring.try_at(3)->target = 5;
    EXPECT_FALSE(ring.band_empty());
    EXPECT_EQ(ring.min_depth(), 3);
    EXPECT_EQ(ring.max_depth(), 3);

    ring.try_at(7)->target = 5;
    ring.try_at(-1)->target = 5;
    EXPECT_EQ(ring.min_depth(), -1);
    EXPECT_EQ(ring.max_depth(), 7);
}

TEST(LevelRing, ShrinkBandDropsIdleEdgesButKeepsInteriorHoles) {
    buy_ring ring;
    ring.reset(10000, 10);

    ASSERT_NE(ring.try_at(0), nullptr);  // idle
    ring.try_at(1)->target = 5;
    ASSERT_NE(ring.try_at(2), nullptr);  // idle interior hole
    ring.try_at(3)->target = 5;
    ASSERT_NE(ring.try_at(4), nullptr);  // idle
    EXPECT_EQ(ring.min_depth(), 0);
    EXPECT_EQ(ring.max_depth(), 4);

    ring.shrink_band();
    EXPECT_EQ(ring.min_depth(), 1);
    EXPECT_EQ(ring.max_depth(), 3);

    // The interior hole is still addressable; it just carries nothing.
    ASSERT_NE(ring.peek(2), nullptr);
    EXPECT_TRUE(ring.peek(2)->idle());

    // The released edges are unbound and rebind clean.
    EXPECT_EQ(ring.peek(0), nullptr);
    EXPECT_EQ(ring.peek(4), nullptr);
}

TEST(LevelRing, ShrinkBandToEmpty) {
    buy_ring ring;
    ring.reset(10000, 10);
    ASSERT_NE(ring.try_at(1), nullptr);
    ASSERT_NE(ring.try_at(2), nullptr);
    ring.shrink_band();
    EXPECT_TRUE(ring.band_empty());
}

// A ring slot is shared by every depth congruent modulo the capacity. Reusing
// one that still carries orders would silently merge two unrelated prices, so
// try_at must refuse instead.
TEST(LevelRing, AliasedDepthWithLiveLevelIsRefused) {
    buy_ring ring;  // capacity 16
    ring.reset(10000, 10);

    level* l = ring.try_at(1);
    ASSERT_NE(l, nullptr);
    l->target = 100;
    l->acked = 100;
    l->order_count = 1;

    EXPECT_EQ(ring.try_at(17), nullptr);  // 17 & 15 == 1
    EXPECT_EQ(ring.try_at(-15), nullptr);

    // Once the level goes idle the slot is free to be rebound.
    l->target = 0;
    l->acked = 0;
    l->order_count = 0;
    level* re = ring.try_at(17);
    ASSERT_NE(re, nullptr);
    EXPECT_EQ(re->depth, 17);
    EXPECT_EQ(re->price, ring.price_at(17));
}

TEST(LevelRing, WouldOverflowDetectsBandWiderThanRing) {
    buy_ring ring;  // capacity 16
    ring.reset(10000, 10);
    EXPECT_FALSE(ring.would_overflow(1000));  // empty band: anything is fine

    ring.try_at(0)->target = 1;
    EXPECT_FALSE(ring.would_overflow(15));
    EXPECT_TRUE(ring.would_overflow(16));
    EXPECT_TRUE(ring.would_overflow(-16));
}

TEST(LevelRing, ResetClearsEverything) {
    buy_ring ring;
    ring.reset(10000, 10);
    ring.try_at(1)->target = 42;
    ring.try_at(5)->acked = 7;

    ring.reset(20000, 5);
    EXPECT_TRUE(ring.band_empty());
    EXPECT_EQ(ring.anchor(), 20000);
    EXPECT_EQ(ring.tick_size(), 5);
    EXPECT_EQ(ring.peek(1), nullptr);
    EXPECT_EQ(ring.peek(5), nullptr);
    EXPECT_EQ(ring.price_at(2), 19990);
}

TEST(Level, DeltaAndIdle) {
    level l;
    EXPECT_TRUE(l.idle());

    l.target = 100;
    EXPECT_EQ(l.working(), 0);
    EXPECT_EQ(l.delta(), 100);
    EXPECT_FALSE(l.idle());

    l.inflight = 100;
    EXPECT_EQ(l.working(), 100);
    EXPECT_EQ(l.delta(), 0);

    l.inflight = 0;
    l.acked = 100;
    EXPECT_EQ(l.delta(), 0);

    // A reduction in flight shows up as negative inflight.
    l.inflight = -40;
    EXPECT_EQ(l.working(), 60);
    l.target = 60;
    EXPECT_EQ(l.delta(), 0);
}
