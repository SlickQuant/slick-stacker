// Copyright 2026 Slick Quant
// SPDX-License-Identifier: MIT

#pragma once

#include <slick/stacker/concepts.hpp>
#include <slick/stacker/config.hpp>
#include <slick/stacker/detail/order_slot.hpp>
#include <slick/stacker/types.hpp>

#include <cstdint>

SLICK_STACKER_NAMESPACE_BEGIN
namespace detail {

/// Fixed-capacity pool of order slots with an intrusive free list.
///
/// Slots are addressed by `slot_index_t`, never by pointer, so the whole
/// structure is relocatable and every intrusive link costs two bytes. Exhaustion
/// is reported rather than thrown: the stacker treats a full pool the same way
/// it treats a rejected `place`, by leaving the level short and retrying on the
/// next reconcile.
template <OrderIdLike Id, std::uint16_t Capacity>
class slot_pool {
    static_assert(Capacity > 0, "capacity must be non-zero");
    static_assert(Capacity < k_null_slot, "capacity must leave room for the null sentinel");

public:
    using slot_type = order_slot<Id>;

    slot_pool() noexcept { reset(); }

    /// Returns `k_null_slot` when the pool is exhausted. The returned slot is
    /// value-initialised apart from its state, which is left `free` for the
    /// caller to set.
    [[nodiscard]] slot_index_t acquire() noexcept {
        if (free_head_ == k_null_slot) [[unlikely]] {
            return k_null_slot;
        }
        const slot_index_t idx = free_head_;
        free_head_ = slots_[idx].next;
        slots_[idx] = slot_type{};
        ++in_use_;
        return idx;
    }

    void release(slot_index_t idx) noexcept {
        SLICK_STACKER_ASSERT(idx < Capacity);
        SLICK_STACKER_ASSERT(in_use_ > 0);
        slot_type& s = slots_[idx];
        s = slot_type{};
        s.next = free_head_;
        free_head_ = idx;
        --in_use_;
    }

    [[nodiscard]] slot_type& operator[](slot_index_t idx) noexcept {
        SLICK_STACKER_ASSERT(idx < Capacity);
        return slots_[idx];
    }
    [[nodiscard]] const slot_type& operator[](slot_index_t idx) const noexcept {
        SLICK_STACKER_ASSERT(idx < Capacity);
        return slots_[idx];
    }

    [[nodiscard]] std::uint16_t in_use() const noexcept { return in_use_; }
    [[nodiscard]] static constexpr std::uint16_t capacity() noexcept { return Capacity; }
    [[nodiscard]] bool empty() const noexcept { return in_use_ == 0; }
    [[nodiscard]] bool full() const noexcept { return free_head_ == k_null_slot; }

    void reset() noexcept {
        for (std::uint16_t i = 0; i < Capacity; ++i) {
            slots_[i] = slot_type{};
            slots_[i].next = static_cast<slot_index_t>(i + 1);
        }
        slots_[Capacity - 1].next = k_null_slot;
        free_head_ = 0;
        in_use_ = 0;
    }

private:
    slot_type slots_[Capacity];
    slot_index_t free_head_ = k_null_slot;
    std::uint16_t in_use_ = 0;
};

}  // namespace detail
SLICK_STACKER_NAMESPACE_END
