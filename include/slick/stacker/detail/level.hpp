// Copyright 2026 Slick Quant
// SPDX-License-Identifier: MIT

#pragma once

#include <slick/stacker/config.hpp>
#include <slick/stacker/types.hpp>

#include <cstdint>

SLICK_STACKER_NAMESPACE_BEGIN
namespace detail {

/// State of one price level.
///
/// Three quantity counters carry the whole picture:
///
///   target   what the caller's quote says should rest here
///   acked    what the venue has confirmed is resting here
///   inflight net change we have requested but not yet had confirmed; signed,
///            and negative while a reduction or a reprice-away is outstanding
///
/// `working()` is what we will have once the venue catches up, and `delta()` is
/// what reconcile still owes this level. Everything the stacker does is driven
/// by the sign of `delta()`, which is why fills adjust `acked` and `target`
/// together rather than being tracked in a fourth counter.
struct level {
    /// Price of this level. Redundant with `depth` but kept materialised so the
    /// reconcile loop never divides.
    price_t price = 0;

    qty_t target = 0;
    qty_t acked = 0;
    qty_t inflight = 0;

    /// Quantity resting in the public book at this price, from
    /// `stacker::on_book_level`. Only consulted by the queue-gap gate.
    qty_t market_qty = 0;

    /// Ticks away from the ring anchor, increasing away from the market. The
    /// ring slot this level occupies is `depth & mask`, so `depth` doubles as
    /// the tag proving the slot currently represents this price.
    std::int32_t depth = 0;

    /// Orders resting here, in queue order: `head` is the one most likely to
    /// fill next, `tail` the one we would give up first.
    slot_index_t head = k_null_slot;
    slot_index_t tail = k_null_slot;
    std::uint16_t order_count = 0;

    /// False until the slot has been claimed for `depth`.
    bool bound = false;

    /// The venue terminally refused quantity at this price -- a new order, or a
    /// modify moving one here. Reconcile adds nothing more until the caller
    /// changes `target` or clears the latch, so a price the venue will not
    /// take cannot turn into a reject-and-resend loop.
    bool rejected = false;

    [[nodiscard]] constexpr qty_t working() const noexcept { return acked + inflight; }

    /// Positive when the level is short of its target, negative when it is
    /// carrying more than the caller asked for.
    [[nodiscard]] constexpr qty_t delta() const noexcept { return target - working(); }

    /// Nothing wanted here and nothing outstanding, so the ring slot may be
    /// recycled for a different price.
    [[nodiscard]] constexpr bool idle() const noexcept {
        return target == 0 && acked == 0 && inflight == 0 && order_count == 0;
    }

    /// True when the level holds, or is about to hold, orders.
    [[nodiscard]] constexpr bool has_orders() const noexcept { return order_count != 0; }
};

}  // namespace detail
SLICK_STACKER_NAMESPACE_END
