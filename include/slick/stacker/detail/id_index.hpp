// Copyright 2026 Slick Quant
// SPDX-License-Identifier: MIT

#pragma once

#include <slick/stacker/concepts.hpp>
#include <slick/stacker/config.hpp>
#include <slick/stacker/types.hpp>

#include <bit>
#include <cstdint>
#include <type_traits>

SLICK_STACKER_NAMESPACE_BEGIN
namespace detail {

/// Round up to the next power of two, minimum 2.
[[nodiscard]] constexpr std::uint32_t next_pow2(std::uint32_t v) noexcept {
    std::uint32_t r = 2;
    while (r < v) {
        r <<= 1;
    }
    return r;
}

/// Fibonacci hash of an order identifier; the quality is in the *top* bits, so
/// callers shift rather than mask. Identifiers are frequently sequential, and a
/// raw sequential key against a masked table produces long probe runs the
/// moment ids are recycled unevenly. Multiplying by 2^64/phi spreads any run
/// of consecutive or evenly strided keys near-uniformly across the top bits,
/// for one multiply on a path that costs a handful of nanoseconds in all.
/// Folding the high word in first keeps keys whose entropy sits only in their
/// upper bits -- a venue prefix, a pointer's page -- from collapsing together.
template <class Id>
[[nodiscard]] inline std::uint64_t id_hash(const Id& id) noexcept {
    std::uint64_t x;
    if constexpr (std::is_pointer_v<Id>) {
        x = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(id));
    } else {
        x = static_cast<std::uint64_t>(static_cast<std::make_unsigned_t<Id>>(id));
    }
    x ^= x >> 32;
    return x * 0x9E3779B97F4A7C15ULL;
}

/// Fixed-capacity open-addressing map from order id to slot index.
///
/// Linear probing with backward-shift deletion, so there are no tombstones and
/// probe runs cannot degrade over a long session of orders being created and
/// retired. `Capacity` must be a power of two and is sized at roughly twice the
/// order pool, keeping the load factor at or below 0.5 -- the point where
/// linear probing's expected probe count is under 1.5.
///
/// Not used at all when the executor offers a user-data slot; see
/// `null_id_index`.
template <OrderIdLike Id, std::uint32_t Capacity>
class id_index {
    static_assert(Capacity >= 2, "capacity must be at least 2");
    static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");

    static constexpr std::uint32_t k_mask = Capacity - 1;
    static constexpr int k_shift = 64 - std::countr_zero(Capacity);  // keep log2(Capacity) top bits

    struct entry {
        Id id{};
        slot_index_t slot = k_null_slot;
        std::uint8_t used = 0;
    };

public:
    static constexpr bool is_active = true;

    /// Insert or overwrite. Returns false only if the table is full, which
    /// cannot happen while the caller respects the order pool's own capacity.
    bool insert(const Id& id, slot_index_t slot) noexcept {
        std::uint32_t i = home(id);
        for (std::uint32_t probe = 0; probe < Capacity; ++probe) {
            entry& e = entries_[i];
            if (!e.used) {
                e.id = id;
                e.slot = slot;
                e.used = 1;
                ++size_;
                return true;
            }
            if (e.id == id) {
                e.slot = slot;  // re-bind a recycled id
                return true;
            }
            i = (i + 1) & k_mask;
        }
        return false;
    }

    /// Returns `k_null_slot` when the id is unknown.
    [[nodiscard]] slot_index_t find(const Id& id) const noexcept {
        std::uint32_t i = home(id);
        for (std::uint32_t probe = 0; probe < Capacity; ++probe) {
            const entry& e = entries_[i];
            if (!e.used) {
                return k_null_slot;
            }
            if (e.id == id) {
                return e.slot;
            }
            i = (i + 1) & k_mask;
        }
        return k_null_slot;
    }

    /// Returns true if the id was present and has been removed.
    bool erase(const Id& id) noexcept {
        std::uint32_t i = home(id);
        std::uint32_t probe = 0;
        for (; probe < Capacity; ++probe) {
            entry& e = entries_[i];
            if (!e.used) {
                return false;
            }
            if (e.id == id) {
                break;
            }
            i = (i + 1) & k_mask;
        }
        if (probe == Capacity) {
            return false;
        }

        // Backward-shift deletion: walk the rest of the probe run and pull
        // forward any entry whose home slot is at or before the hole, so every
        // remaining key stays reachable without a tombstone.
        entries_[i].used = 0;
        --size_;

        std::uint32_t hole = i;
        std::uint32_t j = (i + 1) & k_mask;
        while (entries_[j].used) {
            const std::uint32_t h = home(entries_[j].id);
            // Is `h` cyclically outside the open interval (hole, j]? If so the
            // entry may legally move into the hole.
            const std::uint32_t hole_to_j = (j - hole) & k_mask;
            const std::uint32_t hole_to_h = (h - hole) & k_mask;
            if (hole_to_h == 0 || hole_to_h > hole_to_j) {
                entries_[hole] = entries_[j];
                entries_[j].used = 0;
                hole = j;
            }
            j = (j + 1) & k_mask;
        }
        return true;
    }

    void clear() noexcept {
        for (auto& e : entries_) {
            e.used = 0;
        }
        size_ = 0;
    }

    [[nodiscard]] std::uint32_t size() const noexcept { return size_; }
    [[nodiscard]] static constexpr std::uint32_t capacity() noexcept { return Capacity; }

private:
    [[nodiscard]] static std::uint32_t home(const Id& id) noexcept {
        return static_cast<std::uint32_t>(id_hash(id) >> k_shift);
    }

    entry entries_[Capacity]{};
    std::uint32_t size_ = 0;
};

/// Stand-in used when the executor stores the slot index for us. Has the same
/// shape as `id_index` so call sites need no extra branching, occupies no
/// space, and is never actually consulted.
template <OrderIdLike Id>
struct null_id_index {
    static constexpr bool is_active = false;

    bool insert(const Id&, slot_index_t) noexcept { return true; }
    [[nodiscard]] slot_index_t find(const Id&) const noexcept { return k_null_slot; }
    bool erase(const Id&) noexcept { return true; }
    void clear() noexcept {}
    [[nodiscard]] std::uint32_t size() const noexcept { return 0; }
    [[nodiscard]] static constexpr std::uint32_t capacity() noexcept { return 0; }
};

}  // namespace detail
SLICK_STACKER_NAMESPACE_END
