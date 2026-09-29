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
    /// plus the largest price jump you expect between reconciles. A ladder that
    /// does not fit at all is rejected by `stacker_config::validate`.
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
    max_inflight_modifies_zero,
    ladder_exceeds_capacity,
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
        case config_error::max_inflight_modifies_zero:
            return "max_inflight_modifies must be > 0";
        case config_error::ladder_exceeds_capacity:
            return "(levels + slack_levels) * level_gap_ticks must be < Traits::level_capacity";
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

    /// What kind of order to send. Passed straight through to
    /// `Executor::place`. Both kinds rest, so the two behave identically as far
    /// as the stacker is concerned -- how long the venue keeps them is the
    /// venue's business.
    order_type_t order_type = order_type_t::limit;

    /// True when the venue will not accept a modify or cancel for an order it
    /// has not yet acknowledged. The stacker then defers the action onto the
    /// order slot and fires it from the acknowledgement.
    bool ack_required = true;

    /// How many modify requests may be outstanding against one order at once.
    ///
    /// 1 -- the default -- means a further change waits for the one in flight
    /// to be answered. That is a round trip of latency between the strategy
    /// deciding and the venue hearing about it, which on a fast instrument is
    /// exactly the wrong place to spend time.
    ///
    /// Venues that chain replaces on the client order id (CME among them)
    /// accept a modify against an order whose previous modify has not been
    /// answered yet. Raising this lets the stacker keep up with the market
    /// instead of with the session.
    ///
    /// The cost is exposure, not correctness: until the chain drains, the venue
    /// is working a quantity the stacker no longer intends, and a reject part
    /// way along unwinds to the last state the venue actually confirmed. Keep
    /// it small -- a chain deeper than the round trip is latency you are not
    /// getting back.
    ///
    /// Independent of `ack_required`, which governs the first request against
    /// an order rather than subsequent ones.
    std::uint8_t max_inflight_modifies = 1;

    /// Prefer repricing a surplus order into a level that needs quantity over
    /// cancelling it and sending a new one. Saves messages and order slots, at
    /// the cost of losing queue position at the destination either way.
    bool prefer_modify = true;

    /// When false (the default), a fill reduces the level's target as well as
    /// its working quantity, so the stacker will not silently re-arm size the
    /// caller has not re-authorised. Set true to have the stack automatically
    /// replenish itself back to target after every fill.
    bool refill_on_fill = false;

    /// Most rungs whose ladder, slack included, still fits in the price ring of
    /// `Traits`: `(levels + slack_levels) * level_gap_ticks` ticks must stay
    /// under `level_capacity`. A wider ladder aliases rungs onto one ring slot
    /// and leaves the live band wider than the ring, so every quote move would
    /// rebuild the grid and cancel the whole stack.
    template <class Traits = default_traits>
    [[nodiscard]] constexpr std::uint16_t max_fitting_levels() const noexcept {
        if (level_gap_ticks == 0) {
            return 0;
        }
        const std::uint32_t rungs = (Traits::level_capacity - 1u) / level_gap_ticks;
        return rungs > slack_levels ? static_cast<std::uint16_t>(rungs - slack_levels) : 0;
    }

    /// `n` bounded by `Traits::max_levels` and by `max_fitting_levels`.
    template <class Traits = default_traits>
    [[nodiscard]] constexpr std::uint16_t clamp_levels(std::uint16_t n) const noexcept {
        const std::uint16_t fit = max_fitting_levels<Traits>();
        const std::uint16_t cap = fit < Traits::max_levels ? fit : Traits::max_levels;
        return n < cap ? n : cap;
    }

    /// A copy with every field `validate` would reject replaced by the nearest
    /// usable value, so nothing the stacker computes from it can divide by zero
    /// or run off the end of the ring. This is what the stacker runs on; it is
    /// a guard against a bad config doing harm, not a substitute for checking
    /// it -- a zero `tick_size` becomes 1, which is rarely the venue's grid.
    ///
    ///   - `tick_size <= 0` becomes 1, `level_gap_ticks == 0` becomes 1.
    ///   - `levels` is clamped by `clamp_levels`.
    ///   - A `qty_profile` sized for the requested `levels` keeps its leading
    ///     `levels` entries after the clamp; one of any other size is dropped,
    ///     leaving `stack_qty` in charge.
    ///   - Negative quantities become 0, `qty_increment <= 0` becomes 1, and a
    ///     `max_order_qty` below `min_order_qty` is raised to it.
    ///   - `max_orders_per_level` and `max_inflight_modifies` of 0 become 1.
    ///
    /// Negative entries inside `qty_profile` cannot be fixed in a borrowed
    /// span; the stacker clamps them to 0 as it copies the profile.
    template <class Traits = default_traits>
    [[nodiscard]] constexpr stacker_config sanitized() const noexcept {
        auto non_negative = [](qty_t q) { return q < 0 ? qty_t{0} : q; };
        stacker_config c = *this;
        if (c.tick_size <= 0) {
            c.tick_size = 1;
        }
        if (c.level_gap_ticks == 0) {
            c.level_gap_ticks = 1;
        }
        c.levels = c.clamp_levels<Traits>(levels);
        c.qty_profile =
            qty_profile.size() == levels ? qty_profile.first(c.levels) : std::span<const qty_t>{};
        c.stack_qty = non_negative(c.stack_qty);
        c.max_level_qty = non_negative(c.max_level_qty);
        c.min_order_qty = non_negative(c.min_order_qty);
        c.max_order_qty = non_negative(c.max_order_qty);
        c.qty_hysteresis = non_negative(c.qty_hysteresis);
        c.queue_gap = non_negative(c.queue_gap);
        if (c.qty_increment <= 0) {
            c.qty_increment = 1;
        }
        if (c.max_order_qty < c.min_order_qty) {
            c.max_order_qty = c.min_order_qty;
        }
        if (c.max_orders_per_level == 0) {
            c.max_orders_per_level = 1;
        }
        if (c.max_inflight_modifies == 0) {
            c.max_inflight_modifies = 1;
        }
        return c;
    }

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
        if (levels > max_fitting_levels<Traits>()) {
            return config_error::ladder_exceeds_capacity;
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
        if (max_inflight_modifies == 0) {
            return config_error::max_inflight_modifies_zero;
        }
        return config_error::ok;
    }
};

SLICK_STACKER_NAMESPACE_END
