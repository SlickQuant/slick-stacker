// Copyright 2026 Slick Quant
// SPDX-License-Identifier: MIT

#pragma once

#include <slick/stacker/config.hpp>
#include <slick/stacker/detail/level.hpp>
#include <slick/stacker/types.hpp>

#include <cstdint>
#include <limits>

SLICK_STACKER_NAMESPACE_BEGIN
namespace detail {

/// Returned by `level_ring::depth_of` for a price the ring cannot address.
inline constexpr std::int32_t k_invalid_depth = std::numeric_limits<std::int32_t>::min();

/// Furthest depth `depth_of` reports, either side of the anchor. The headroom
/// above it lets the stacker add a ladder span (under 2^16 ticks) and step
/// across the band in `int32_t` without overflowing. A price further from the
/// anchor than this is far outside any ring, so it rebuilds the grid like any
/// other price the ring cannot reach.
inline constexpr std::int32_t k_max_depth = std::int32_t{1} << 30;

/// Fixed ring of price levels addressed by *depth* -- ticks away from an
/// anchor price, counting in the direction that moves away from the market.
///
/// Depth rather than raw price is what makes the side disappear: a buy stack
/// grows downward in price and a sell stack upward, but both grow toward
/// increasing depth, so every comparison in the reconcile loop is the same
/// integer comparison for both sides. `Side` is a template parameter, so the
/// one place the direction matters compiles down to no branch at all.
///
/// A slot is claimed for a depth on first use and released when the level goes
/// idle, so the ring addresses an unbounded price range with fixed memory as
/// long as the *live* range stays under `Capacity` ticks. When it does not --
/// a gap open, a limit move, a bad feed -- `try_at` reports the collision
/// rather than corrupting a level, and the stacker recovers by flattening and
/// re-anchoring.
template <side_t Side, std::uint16_t Capacity>
class level_ring {
    static_assert(Capacity >= 8, "capacity must be at least 8");
    static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");

    static constexpr std::uint32_t k_mask = Capacity - 1;

public:
    /// Drop every level and re-establish the price grid. Callers are
    /// responsible for having dealt with any orders first. `anchor` must be
    /// within `price_in_range`, which is what keeps `depth_of` from
    /// overflowing.
    void reset(price_t anchor, price_t tick_size) noexcept {
        SLICK_STACKER_ASSERT(price_in_range(anchor) && tick_size > 0);
        anchor_ = anchor;
        tick_size_ = tick_size;
        for (auto& l : levels_) {
            l = level{};
        }
        min_depth_ = 0;
        max_depth_ = -1;  // empty band
    }

    [[nodiscard]] price_t anchor() const noexcept { return anchor_; }
    [[nodiscard]] price_t tick_size() const noexcept { return tick_size_; }
    [[nodiscard]] static constexpr std::uint16_t capacity() noexcept { return Capacity; }

    /// Ticks from the anchor to `px`, increasing away from the market.
    /// `k_invalid_depth` if `px` is not on the tick grid, is outside
    /// `price_in_range`, or is more than `k_max_depth` ticks from the anchor.
    ///
    /// Any price may be passed. Both it and the anchor being in range is what
    /// keeps the subtraction from overflowing, and the depth bound is what
    /// keeps the narrowing exact -- a truncated depth would alias a distant
    /// price onto a live level.
    [[nodiscard]] std::int32_t depth_of(price_t px) const noexcept {
        if (!price_in_range(px)) [[unlikely]] {
            return k_invalid_depth;
        }
        const price_t diff = (Side == side_t::buy) ? (anchor_ - px) : (px - anchor_);
        if (diff % tick_size_ != 0) [[unlikely]] {
            return k_invalid_depth;
        }
        const price_t depth = diff / tick_size_;
        // `-k_max_depth <= depth <= k_max_depth`, as one unsigned compare.
        constexpr auto k_reach = static_cast<std::uint64_t>(k_max_depth);
        if (static_cast<std::uint64_t>(depth) + k_reach > 2 * k_reach) [[unlikely]] {
            return k_invalid_depth;
        }
        return static_cast<std::int32_t>(depth);
    }

    /// `depth_of` for a price already known to be addressable: one `depth_of`
    /// has accepted against the current anchor. None of the checks are
    /// repeated, which is what keeps level accounting -- the hottest caller --
    /// down to a single division.
    [[nodiscard]] std::int32_t depth_of_known(price_t px) const noexcept {
        const price_t diff = (Side == side_t::buy) ? (anchor_ - px) : (px - anchor_);
        const auto depth = static_cast<std::int32_t>(diff / tick_size_);
        SLICK_STACKER_ASSERT(depth_of(px) == depth);
        return depth;
    }

    /// Price of `depth`. Exact, and cannot overflow, for any depth whose price
    /// is in range -- every depth `depth_of` returns, and every rung of a
    /// ladder the stacker has checked fits.
    [[nodiscard]] price_t price_at(std::int32_t depth) const noexcept {
        const price_t off = static_cast<price_t>(depth) * tick_size_;
        return (Side == side_t::buy) ? (anchor_ - off) : (anchor_ + off);
    }

    /// True when `a` is closer to the market than `b`.
    [[nodiscard]] static constexpr bool is_better(std::int32_t a, std::int32_t b) noexcept {
        return a < b;
    }

    /// Level for `depth`, claiming a ring slot if this is its first use.
    ///
    /// Returns nullptr when the slot is already occupied by a live level at a
    /// different depth, which means the live price range has outgrown the ring.
    [[nodiscard]] level* try_at(std::int32_t depth) noexcept {
        level& l = levels_[static_cast<std::uint32_t>(depth) & k_mask];
        if (l.bound && l.depth == depth) [[likely]] {
            return &l;
        }
        if (l.bound && !l.idle()) [[unlikely]] {
            return nullptr;
        }
        l = level{};
        l.depth = depth;
        l.price = price_at(depth);
        l.bound = true;
        extend_band(depth);
        return &l;
    }

    /// Level for `depth` only if it is already claimed; never binds.
    [[nodiscard]] level* peek(std::int32_t depth) noexcept {
        level& l = levels_[static_cast<std::uint32_t>(depth) & k_mask];
        return (l.bound && l.depth == depth) ? &l : nullptr;
    }
    [[nodiscard]] const level* peek(std::int32_t depth) const noexcept {
        const level& l = levels_[static_cast<std::uint32_t>(depth) & k_mask];
        return (l.bound && l.depth == depth) ? &l : nullptr;
    }

    /// Unchecked access for a depth known to be bound.
    [[nodiscard]] level& at(std::int32_t depth) noexcept {
        level& l = levels_[static_cast<std::uint32_t>(depth) & k_mask];
        SLICK_STACKER_ASSERT(l.bound && l.depth == depth);
        return l;
    }

    /// Inclusive depth band that may contain claimed levels. Empty when
    /// `band_empty()`. Scanning it is how reconcile enumerates live levels:
    /// the band is a handful of entries wide and contiguous in the ring, so a
    /// straight walk beats any auxiliary index.
    [[nodiscard]] std::int32_t min_depth() const noexcept { return min_depth_; }
    [[nodiscard]] std::int32_t max_depth() const noexcept { return max_depth_; }
    [[nodiscard]] bool band_empty() const noexcept { return max_depth_ < min_depth_; }

    /// Widen the band to include `depth`. Called automatically by `try_at`;
    /// exposed for callers that bind levels through `at`.
    void extend_band(std::int32_t depth) noexcept {
        if (band_empty()) {
            min_depth_ = depth;
            max_depth_ = depth;
            return;
        }
        if (depth < min_depth_) {
            min_depth_ = depth;
        }
        if (depth > max_depth_) {
            max_depth_ = depth;
        }
    }

    /// Release idle levels at both edges and pull the band in around what is
    /// left. Cheap enough to run at the end of every reconcile.
    void shrink_band() noexcept {
        while (!band_empty()) {
            level* l = peek(min_depth_);
            if (l != nullptr && !l->idle()) {
                break;
            }
            if (l != nullptr) {
                l->bound = false;
            }
            ++min_depth_;
        }
        while (!band_empty()) {
            level* l = peek(max_depth_);
            if (l != nullptr && !l->idle()) {
                break;
            }
            if (l != nullptr) {
                l->bound = false;
            }
            --max_depth_;
        }
        if (band_empty()) {
            min_depth_ = 0;
            max_depth_ = -1;
        }
    }

    /// True when `depth` would push the live band wider than the ring.
    [[nodiscard]] bool would_overflow(std::int32_t depth) const noexcept {
        if (band_empty()) {
            return false;
        }
        const std::int32_t lo = depth < min_depth_ ? depth : min_depth_;
        const std::int32_t hi = depth > max_depth_ ? depth : max_depth_;
        return static_cast<std::int64_t>(hi) - static_cast<std::int64_t>(lo) >=
               static_cast<std::int64_t>(Capacity);
    }

private:
    level levels_[Capacity]{};
    price_t anchor_ = 0;
    price_t tick_size_ = 1;
    std::int32_t min_depth_ = 0;
    std::int32_t max_depth_ = -1;
};

}  // namespace detail
SLICK_STACKER_NAMESPACE_END
