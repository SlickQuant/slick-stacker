// Copyright 2026 Slick Quant
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>
#include <slick/stacker/detail/id_index.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

using slick::stacker::k_null_slot;
using slick::stacker::slot_index_t;
using slick::stacker::detail::id_index;
using slick::stacker::detail::next_pow2;
using slick::stacker::detail::null_id_index;

namespace {

using index_t = id_index<std::uint64_t, 64>;

}  // namespace

TEST(IdIndex, NextPow2) {
    EXPECT_EQ(next_pow2(1u), 2u);
    EXPECT_EQ(next_pow2(2u), 2u);
    EXPECT_EQ(next_pow2(3u), 4u);
    EXPECT_EQ(next_pow2(255u), 256u);
    EXPECT_EQ(next_pow2(256u), 256u);
    EXPECT_EQ(next_pow2(257u), 512u);
}

TEST(IdIndex, EmptyFindMisses) {
    index_t idx;
    EXPECT_EQ(idx.size(), 0u);
    EXPECT_EQ(idx.find(1), k_null_slot);
    EXPECT_FALSE(idx.erase(1));
}

TEST(IdIndex, InsertFindErase) {
    index_t idx;
    ASSERT_TRUE(idx.insert(1000, 7));
    ASSERT_TRUE(idx.insert(2000, 9));
    EXPECT_EQ(idx.size(), 2u);
    EXPECT_EQ(idx.find(1000), 7);
    EXPECT_EQ(idx.find(2000), 9);
    EXPECT_EQ(idx.find(3000), k_null_slot);

    EXPECT_TRUE(idx.erase(1000));
    EXPECT_EQ(idx.size(), 1u);
    EXPECT_EQ(idx.find(1000), k_null_slot);
    EXPECT_EQ(idx.find(2000), 9);
}

TEST(IdIndex, ReinsertRebinds) {
    index_t idx;
    ASSERT_TRUE(idx.insert(42, 1));
    ASSERT_TRUE(idx.insert(42, 2));
    EXPECT_EQ(idx.size(), 1u);
    EXPECT_EQ(idx.find(42), 2);
}

TEST(IdIndex, Clear) {
    index_t idx;
    for (std::uint64_t i = 0; i < 16; ++i) {
        ASSERT_TRUE(idx.insert(i, static_cast<slot_index_t>(i)));
    }
    idx.clear();
    EXPECT_EQ(idx.size(), 0u);
    for (std::uint64_t i = 0; i < 16; ++i) {
        EXPECT_EQ(idx.find(i), k_null_slot);
    }
}

// Every key must stay reachable after an arbitrary erase pattern. Backward
// shift deletion is the part of an open-addressing table that is easy to get
// subtly wrong, and the failure mode is a lookup miss long after the erase.
TEST(IdIndex, BackwardShiftKeepsEveryKeyReachable) {
    index_t idx;
    std::unordered_map<std::uint64_t, slot_index_t> shadow;

    // Fill to the intended load factor.
    for (std::uint64_t i = 0; i < 32; ++i) {
        const auto slot = static_cast<slot_index_t>(i);
        ASSERT_TRUE(idx.insert(i * 7 + 1, slot));
        shadow[i * 7 + 1] = slot;
    }

    // Erase every third key, checking all survivors after each removal.
    std::vector<std::uint64_t> to_erase;
    for (std::uint64_t i = 0; i < 32; i += 3) {
        to_erase.push_back(i * 7 + 1);
    }
    for (std::uint64_t key : to_erase) {
        ASSERT_TRUE(idx.erase(key));
        shadow.erase(key);
        for (const auto& [k, v] : shadow) {
            EXPECT_EQ(idx.find(k), v) << "key " << k << " lost after erasing " << key;
        }
        EXPECT_EQ(idx.find(key), k_null_slot);
    }
    EXPECT_EQ(idx.size(), shadow.size());
}

// Keys that all hash into the same region force one long probe run, which is
// where a wrapping bug shows up.
TEST(IdIndex, CollidingKeysAndWraparound) {
    id_index<std::uint64_t, 8> idx;
    // Only four entries at capacity 8 -- but insert/erase them repeatedly so
    // probe runs wrap the end of the table many times over.
    for (int round = 0; round < 50; ++round) {
        for (std::uint64_t i = 0; i < 4; ++i) {
            const std::uint64_t key = static_cast<std::uint64_t>(round) * 4 + i;
            ASSERT_TRUE(idx.insert(key, static_cast<slot_index_t>(i)));
        }
        for (std::uint64_t i = 0; i < 4; ++i) {
            const std::uint64_t key = static_cast<std::uint64_t>(round) * 4 + i;
            ASSERT_EQ(idx.find(key), static_cast<slot_index_t>(i)) << "round " << round;
        }
        for (std::uint64_t i = 0; i < 4; ++i) {
            ASSERT_TRUE(idx.erase(static_cast<std::uint64_t>(round) * 4 + i));
        }
        ASSERT_EQ(idx.size(), 0u);
    }
}

TEST(IdIndex, FullTableRejectsNewKey) {
    id_index<std::uint64_t, 8> idx;
    for (std::uint64_t i = 0; i < 8; ++i) {
        ASSERT_TRUE(idx.insert(i + 1, static_cast<slot_index_t>(i)));
    }
    EXPECT_EQ(idx.size(), 8u);
    EXPECT_FALSE(idx.insert(9999, 0));
    // Existing keys are still all reachable.
    for (std::uint64_t i = 0; i < 8; ++i) {
        EXPECT_EQ(idx.find(i + 1), static_cast<slot_index_t>(i));
    }
}

TEST(IdIndex, PointerKeys) {
    int storage[4]{};
    id_index<int*, 16> idx;
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(idx.insert(&storage[i], static_cast<slot_index_t>(i)));
    }
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(idx.find(&storage[i]), static_cast<slot_index_t>(i));
    }
    EXPECT_TRUE(idx.erase(&storage[2]));
    EXPECT_EQ(idx.find(&storage[2]), k_null_slot);
    EXPECT_EQ(idx.find(&storage[3]), 3);
}

TEST(IdIndex, NullIndexIsEmptyAndInert) {
    static_assert(!null_id_index<std::uint64_t>::is_active);
    static_assert(index_t::is_active);
    EXPECT_LE(sizeof(null_id_index<std::uint64_t>), sizeof(char));

    null_id_index<std::uint64_t> idx;
    EXPECT_TRUE(idx.insert(1, 2));
    EXPECT_EQ(idx.find(1), k_null_slot);
    EXPECT_EQ(idx.size(), 0u);
}
