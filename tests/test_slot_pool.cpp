// Copyright 2026 Slick Quant
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>
#include <slick/stacker/detail/slot_pool.hpp>

#include <cstddef>
#include <cstdint>
#include <set>
#include <type_traits>
#include <vector>

using slick::stacker::k_null_slot;
using slick::stacker::order_state_t;
using slick::stacker::slot_index_t;
using slick::stacker::detail::contribution_of;
using slick::stacker::detail::order_slot;
using slick::stacker::detail::slot_pool;

namespace {

using pool_t = slot_pool<std::uint64_t, 8>;

}  // namespace

// Routing an event and the queue/book feeds read only the leading fields of a
// slot. Keeping them in the first 32 bytes is what lets those paths touch one
// cache line; a reorder that pushes one of them back must fail here.
TEST(SlotPool, HotFieldsLeadTheSlot) {
    using slot = order_slot<std::uint64_t>;
    static_assert(std::is_standard_layout_v<slot>);
    static_assert(offsetof(slot, id) == 0);
    static_assert(offsetof(slot, qty_in_front) + sizeof(slot::qty_in_front) <= 32);
    static_assert(offsetof(slot, linked_depth) + sizeof(slot::linked_depth) <= 32);
    static_assert(offsetof(slot, next) + sizeof(slot::next) <= 32);
    static_assert(offsetof(slot, prev) + sizeof(slot::prev) <= 32);
    static_assert(offsetof(slot, state) + sizeof(slot::state) <= 32);
    static_assert(offsetof(slot, inflight_modifies) + sizeof(slot::inflight_modifies) <= 32);
    static_assert(offsetof(slot, pending) + sizeof(slot::pending) <= 32);
    static_assert(offsetof(slot, order_type) + sizeof(slot::order_type) <= 32);
    static_assert(offsetof(slot, flags) + sizeof(slot::flags) <= 32);
    static_assert(sizeof(slot) == 80, "the reorder must not grow the slot");
    SUCCEED();
}

TEST(SlotPool, StartsEmpty) {
    pool_t pool;
    EXPECT_TRUE(pool.empty());
    EXPECT_FALSE(pool.full());
    EXPECT_EQ(pool.in_use(), 0u);
    EXPECT_EQ(pool_t::capacity(), 8u);
}

TEST(SlotPool, AcquireReleaseRoundTrip) {
    pool_t pool;
    const slot_index_t a = pool.acquire();
    ASSERT_NE(a, k_null_slot);
    EXPECT_EQ(pool.in_use(), 1u);

    pool[a].id = 1234;
    pool[a].state = order_state_t::live;

    pool.release(a);
    EXPECT_EQ(pool.in_use(), 0u);
    EXPECT_TRUE(pool.empty());
    // Release only marks the slot free; that alone must make it unmatchable,
    // since routing by user data compares the id only for an active slot.
    EXPECT_FALSE(pool[a].active());

    // A recycled slot must come back clean -- stale state here would let a
    // freed order's quantity leak into a new level's accounting.
    const slot_index_t b = pool.acquire();
    ASSERT_NE(b, k_null_slot);
    EXPECT_EQ(pool[b].id, 0u);
    EXPECT_EQ(pool[b].state, order_state_t::free);
    EXPECT_EQ(pool[b].order_qty, 0);
}

TEST(SlotPool, ExhaustionReportsNullSlot) {
    pool_t pool;
    std::vector<slot_index_t> held;
    for (int i = 0; i < 8; ++i) {
        const slot_index_t s = pool.acquire();
        ASSERT_NE(s, k_null_slot) << "at " << i;
        held.push_back(s);
    }
    EXPECT_TRUE(pool.full());
    EXPECT_EQ(pool.acquire(), k_null_slot);
    EXPECT_EQ(pool.in_use(), 8u);

    pool.release(held[3]);
    EXPECT_FALSE(pool.full());
    EXPECT_NE(pool.acquire(), k_null_slot);
}

TEST(SlotPool, AllIndicesAreDistinct) {
    pool_t pool;
    std::set<slot_index_t> seen;
    for (int i = 0; i < 8; ++i) {
        const slot_index_t s = pool.acquire();
        ASSERT_NE(s, k_null_slot);
        EXPECT_TRUE(seen.insert(s).second) << "duplicate index " << s;
        EXPECT_LT(s, pool_t::capacity());
    }
}

TEST(SlotPool, ResetReclaimsEverything) {
    pool_t pool;
    for (int i = 0; i < 5; ++i) {
        ASSERT_NE(pool.acquire(), k_null_slot);
    }
    pool.reset();
    EXPECT_EQ(pool.in_use(), 0u);
    for (int i = 0; i < 8; ++i) {
        EXPECT_NE(pool.acquire(), k_null_slot);
    }
}

TEST(SlotPool, ChurnDoesNotLoseSlots) {
    pool_t pool;
    std::vector<slot_index_t> held;
    for (int round = 0; round < 100; ++round) {
        while (!pool.full()) {
            held.push_back(pool.acquire());
        }
        EXPECT_EQ(pool.in_use(), 8u);
        for (slot_index_t s : held) {
            pool.release(s);
        }
        held.clear();
        EXPECT_EQ(pool.in_use(), 0u);
    }
}

// --- accounting rule -------------------------------------------------------
//
// contribution_of is the single definition of how a slot maps onto its levels'
// counters, and stacker::validate() rebuilds every level through it. If these
// cases are wrong, validate() agrees with a wrong implementation and the whole
// suite stops meaning anything.

TEST(OrderSlot, ContributionPendingNew) {
    order_slot<std::uint64_t> s;
    s.state = order_state_t::pending_new;
    s.price = 100;
    s.acked_price = 100;
    s.order_qty = 10;

    const auto c = contribution_of(s);
    EXPECT_EQ(c.acked_delta, 0);  // nothing confirmed yet
    EXPECT_EQ(c.out_delta, 0);
    EXPECT_EQ(c.in_price, 100);
    EXPECT_EQ(c.in_delta, 10);
}

TEST(OrderSlot, ContributionLive) {
    order_slot<std::uint64_t> s;
    s.state = order_state_t::live;
    s.price = 100;
    s.acked_price = 100;
    s.order_qty = 10;
    s.acked_qty = 10;

    const auto c = contribution_of(s);
    EXPECT_EQ(c.acked_price, 100);
    EXPECT_EQ(c.acked_delta, 10);
    EXPECT_EQ(c.out_delta, 0);  // nothing outstanding
    EXPECT_EQ(c.in_delta, 0);
}

TEST(OrderSlot, ContributionSameLevelReduction) {
    order_slot<std::uint64_t> s;
    s.state = order_state_t::pending_modify;
    s.price = 100;
    s.acked_price = 100;
    s.acked_qty = 10;
    s.order_qty = 4;

    const auto c = contribution_of(s);
    // Confirmed 10 resting, requested 4: the level nets to 4.
    EXPECT_EQ(c.acked_delta, 10);
    EXPECT_EQ(c.out_delta, -10);
    EXPECT_EQ(c.in_delta, 4);
    EXPECT_EQ(c.acked_delta + c.out_delta + c.in_delta, 4);
}

TEST(OrderSlot, ContributionReprice) {
    order_slot<std::uint64_t> s;
    s.state = order_state_t::pending_modify;
    s.acked_price = 100;
    s.price = 99;
    s.acked_qty = 10;
    s.order_qty = 10;

    const auto c = contribution_of(s);
    // Level 100 nets to zero, level 99 gains the whole order.
    EXPECT_EQ(c.acked_price, 100);
    EXPECT_EQ(c.acked_delta, 10);
    EXPECT_EQ(c.out_price, 100);
    EXPECT_EQ(c.out_delta, -10);
    EXPECT_EQ(c.in_price, 99);
    EXPECT_EQ(c.in_delta, 10);
}

TEST(OrderSlot, ContributionPendingCancel) {
    order_slot<std::uint64_t> s;
    s.state = order_state_t::pending_cancel;
    s.price = 100;
    s.acked_price = 100;
    s.acked_qty = 10;
    s.order_qty = 10;

    const auto c = contribution_of(s);
    EXPECT_EQ(s.desired(), 0);
    EXPECT_EQ(c.acked_delta + c.out_delta + c.in_delta, 0);
}

TEST(OrderSlot, ContributionPartiallyFilledLive) {
    order_slot<std::uint64_t> s;
    s.state = order_state_t::live;
    s.price = 100;
    s.acked_price = 100;
    s.order_qty = 10;
    s.acked_qty = 10;
    s.filled = 3;

    EXPECT_EQ(s.resting(), 7);
    const auto c = contribution_of(s);
    EXPECT_EQ(c.acked_delta, 7);
    EXPECT_EQ(c.out_delta, 0);
    EXPECT_EQ(c.in_delta, 0);
}

TEST(OrderSlot, Flags) {
    order_slot<std::uint64_t> s;
    EXPECT_FALSE(s.has_flag(order_slot<std::uint64_t>::flag_qp_valid));
    s.set_flag(order_slot<std::uint64_t>::flag_qp_valid);
    s.set_flag(order_slot<std::uint64_t>::flag_blocked_decrement);
    EXPECT_TRUE(s.has_flag(order_slot<std::uint64_t>::flag_qp_valid));
    EXPECT_TRUE(s.has_flag(order_slot<std::uint64_t>::flag_blocked_decrement));
    s.clear_flag(order_slot<std::uint64_t>::flag_qp_valid);
    EXPECT_FALSE(s.has_flag(order_slot<std::uint64_t>::flag_qp_valid));
    EXPECT_TRUE(s.has_flag(order_slot<std::uint64_t>::flag_blocked_decrement));
}
