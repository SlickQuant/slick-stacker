// Copyright 2026 Slick Quant
// SPDX-License-Identifier: MIT

#pragma once

#include <slick/stacker/config.hpp>
#include <slick/stacker/types.hpp>

#include <cstdint>
#include <span>

SLICK_STACKER_NAMESPACE_BEGIN

/// Compile-time sizing for a `stacker`. Everything the stacker owns is a fixed
/// array sized from these, so a stacker never allocates after construction.
struct default_traits {
    /// Number of price levels the ring can address at once. Must be a power of
    /// two. The ring spans +/- `level_capacity/2` ticks of quote travel while
    /// orders remain live, so it needs to comfortably exceed the deepest stack
    /// plus the largest price jump you expect between reconciles.
    static constexpr std::uint16_t level_capacity = 256;

    /// Maximum number of simultaneously tracked orders across all levels.
    static constexpr std::uint16_t max_orders = 256;

    /// Largest value `stacker_config::levels` may take.
    static constexpr std::uint16_t max_levels = 64;
};

/// Why a `stacker_config` was rejected.
enum class config_error : std::uint8_t {
    ok = 0,
    tick_size_not_positive,
    level_gap_not_positive,
    too_many_levels,
    qty_profile_size_mismatch,
    negative_qty,
    qty_increment_not_positive,
    max_order_qty_below_min,
    max_orders_per_level_zero,
};

[[nodiscard]] constexpr const char* to_string(config_error e) noexcept {
    switch (e) {
        case config_error::ok:
            return "ok";
        case config_error::tick_size_not_positive:
            return "tick_size must be > 0";
        case config_error::level_gap_not_positive:
            return "level_gap_ticks must be > 0";
        case config_error::too_many_levels:
            return "levels exceeds Traits::max_levels";
        case config_error::qty_profile_size_mismatch:
            return "qty_profile size must equal levels";
        case config_error::negative_qty:
            return "quantities must be >= 0";
        case config_error::qty_increment_not_positive:
            return "qty_increment must be > 0";
        case config_error::max_order_qty_below_min:
            return "max_order_qty must be >= min_order_qty";
        case config_error::max_orders_per_level_zero:
            return "max_orders_per_level must be > 0";
    }
    return "unknown";
}

/// Describes the stack the caller wants built beneath each quote, plus the
/// venue-specific rules the stacker must respect while building it.
///
/// A call to `stacker::quote(price, qty)` produces:
///
///     price                                  -> qty            (the top level)
///     price -/+ 1*level_gap_ticks*tick_size  -> shape qty [0]
///     price -/+ 2*level_gap_ticks*tick_size  -> shape qty [1]
///     ...                                                       (`levels` of them)
///
/// where "shape qty [i]" is `qty_profile[i]` when a profile is supplied and
/// `stack_qty` otherwise, and the sign of the offset is away from the market
/// (down for a buy stack, up for a sell stack).
struct stacker_config {
    // -- shape ---------------------------------------------------------------

    /// Minimum price increment. All stack prices are multiples of this away
    /// from the quoted price.
    price_t tick_size = 1;

    /// Number of levels to stack *behind* the quoted price. The quoted price
    /// itself is always level zero and is not counted here, so a stack of
    /// `levels = 3` results in up to four working price levels.
    std::uint16_t levels = 0;

    /// Tick spacing between consecutive stack levels. 1 gives a contiguous
    /// stack; 2 quotes every other tick, and so on.
    std::uint16_t level_gap_ticks = 1;

    /// Target quantity for every stack level, used when `qty_profile` is empty.
    qty_t stack_qty = 0;

    /// Per-level target quantities, outermost index = deepest level. When
    /// non-empty its size must equal `levels`, and it overrides `stack_qty`.
    /// The span is copied into the stacker; it does not need to outlive the
    /// call that consumes it.
    std::span<const qty_t> qty_profile{};

    /// Upper bound on the total resting quantity the stacker will hold at any
    /// single price level.
    qty_t max_level_qty = k_no_qty_limit;

    // -- order sizing --------------------------------------------------------

    /// Largest single order the stacker will send. A level needing more than
    /// this is filled with several orders (subject to `max_orders_per_level`).
    qty_t max_order_qty = k_no_qty_limit;

    /// Smallest single order the stacker will send. A residual smaller than
    /// this is left unquoted rather than sent as an odd lot.
    qty_t min_order_qty = 0;

    /// Order quantities are rounded down to a multiple of this.
    qty_t qty_increment = 1;

    /// Cap on concurrently working orders at one price level.
    std::uint16_t max_orders_per_level = 8;

    // -- churn control -------------------------------------------------------

    /// Levels beyond the bottom of the stack that are kept rather than
    /// cancelled when the quote moves in. Absorbs oscillation around a price
    /// so a one-tick flicker does not cancel and re-place the tail of the
    /// stack.
    std::uint16_t slack_levels = 0;

    /// Do not send anything to top a level up when the shortfall is at or below
    /// this. Reductions are always acted on -- this only suppresses increases.
    qty_t qty_hysteresis = 0;

    /// Require at least this much market quantity resting behind our last order
    /// at a level before adding another order there. Zero disables the gate,
    /// which is the right setting unless you feed `on_queue_position`.
    qty_t queue_gap = 0;

    // -- venue behaviour -----------------------------------------------------

    /// True when the venue will not accept a modify or cancel for an order it
    /// has not yet acknowledged. The stacker then defers the action onto the
    /// order slot and fires it from the acknowledgement.
    bool ack_required = true;

    /// Prefer repricing a surplus order into a level that needs quantity over
    /// cancelling it and sending a new one. Saves messages and order slots, at
    /// the cost of losing queue position at the destination either way.
    bool prefer_modify = true;

    /// When false (the default), a fill reduces the level's target as well as
    /// its working quantity, so the stacker will not silently re-arm size the
    /// caller has not re-authorised. Set true to have the stack automatically
    /// replenish itself back to target after every fill.
    bool refill_on_fill = false;

    /// Validate against the sizing limits of `Traits`.
    template <class Traits = default_traits>
    [[nodiscard]] constexpr config_error validate() const noexcept {
        if (tick_size <= 0) {
            return config_error::tick_size_not_positive;
        }
        if (level_gap_ticks == 0) {
            return config_error::level_gap_not_positive;
        }
        if (levels > Traits::max_levels) {
            return config_error::too_many_levels;
        }
        if (!qty_profile.empty() && qty_profile.size() != levels) {
            return config_error::qty_profile_size_mismatch;
        }
        if (stack_qty < 0 || min_order_qty < 0 || max_order_qty < 0 || max_level_qty < 0 ||
            qty_hysteresis < 0 || queue_gap < 0) {
            return config_error::negative_qty;
        }
        for (qty_t q : qty_profile) {
            if (q < 0) {
                return config_error::negative_qty;
            }
        }
        if (qty_increment <= 0) {
            return config_error::qty_increment_not_positive;
        }
        if (max_order_qty < min_order_qty) {
            return config_error::max_order_qty_below_min;
        }
        if (max_orders_per_level == 0) {
            return config_error::max_orders_per_level_zero;
        }
        return config_error::ok;
    }
};

SLICK_STACKER_NAMESPACE_END
