// Copyright 2026 Slick Quant
// SPDX-License-Identifier: MIT

#pragma once

#include <slick/stacker/concepts.hpp>
#include <slick/stacker/config.hpp>
#include <slick/stacker/types.hpp>

#include <cstdint>

SLICK_STACKER_NAMESPACE_BEGIN
namespace detail {

/// Per-order state. One of these exists for every order the stacker believes
/// is, or may shortly be, working at the venue.
///
/// Two prices and two quantities are tracked rather than one of each, and the
/// distinction is load-bearing:
///
///   * `acked_price` / `acked_qty` are what the venue has last confirmed.
///   * `price` / `order_qty` are what we most recently asked for.
///
/// While a request is outstanding the two disagree, and a modify reject rolls
/// the second pair back onto the first. Level accounting is derived from both;
/// see `slot_contribution`.
///
/// Field order is a cache decision. Everything that routing an event and the
/// queue/book feeds read -- `id` through `flags`, 29 bytes with a 64-bit id --
/// comes first, so those paths touch one cache line instead of two; the prices
/// and quantity counters follow, read by paths that touch the whole slot anyway.
template <OrderIdLike Id>
struct order_slot {
    /// Identifier the executor returned from `place`.
    Id id{};

    /// Market quantity ahead of this order in the queue, from
    /// `stacker::on_queue_position`. Only meaningful when `flag_qp_valid` is
    /// set.
    qty_t qty_in_front = 0;

    /// Depth of the level whose order list this slot is linked into. Only
    /// meaningful while `flag_linked` is set.
    std::int32_t linked_depth = 0;

    /// Intrusive links. Within a level these order the queue front to back;
    /// while the slot is free, `next` chains the free list.
    slot_index_t next = k_null_slot;
    slot_index_t prev = k_null_slot;

    order_state_t state = order_state_t::free;

    /// Modify requests sent against this order and not yet answered.
    ///
    /// With a chain outstanding, `state` alone can no longer say whether the
    /// venue still owes us something: an intermediate acknowledgement arrives
    /// while later requests are still in the air. The count is what makes a
    /// reject unambiguous -- rolling intent back to the last confirmed state is
    /// only correct once nothing else is outstanding to overrule it.
    std::uint8_t inflight_modifies = 0;

    /// An action we want but could not send yet, because the venue will not
    /// accept it until the order is acknowledged. `price` and `order_qty`
    /// already hold what we want, so the deferred action needs no payload of
    /// its own -- it is purely a "still owe the venue a message" marker.
    pending_action_t pending = pending_action_t::none;

    /// The kind this order was sent to the venue as. A modify carries price and
    /// quantity only, so an order whose kind no longer matches the configuration
    /// cannot be amended into agreement -- it has to be cancelled and replaced.
    order_type_t order_type = order_type_t::limit;

    std::uint8_t flags = 0;

    /// This order could not be reduced when the stack needed it to be, so the
    /// stacker is exposed to more quantity than it wants and must not add any
    /// more anywhere until the situation clears.
    static constexpr std::uint8_t flag_blocked_decrement = 1u << 0;

    /// `qty_in_front` has been populated at least once since the order last
    /// moved. Queue-gap gating refuses to act on a stale queue position.
    static constexpr std::uint8_t flag_qp_valid = 1u << 1;

    /// The order outlived the price grid it was booked against -- the stacker
    /// re-anchored underneath it. It has been cancelled and contributes to no
    /// level; it exists only so its terminal event can be matched and retired.
    static constexpr std::uint8_t flag_orphaned = 1u << 2;

    /// The slot is present in a level's order list.
    static constexpr std::uint8_t flag_linked = 1u << 3;

    /// The venue terminally rejected a modify of this order. Resending it would
    /// only be rejected again, so the order is never modified from here on:
    /// it is cancelled where it would have been shrunk or repriced.
    static constexpr std::uint8_t flag_no_modify = 1u << 4;

    /// Price we want this order resting at -- the level it is booked under.
    price_t price = 0;

    /// Price the venue last confirmed.
    price_t acked_price = 0;

    /// Quantity as of our most recent request.
    qty_t order_qty = 0;

    /// Quantity the venue last confirmed.
    qty_t acked_qty = 0;

    /// Cumulative executed quantity.
    qty_t filled = 0;

    /// Cumulative cancelled quantity, including partial reductions the venue
    /// reports as cancels.
    qty_t canceled = 0;

    [[nodiscard]] constexpr bool has_flag(std::uint8_t f) const noexcept {
        return (flags & f) != 0;
    }
    constexpr void set_flag(std::uint8_t f) noexcept { flags |= f; }
    constexpr void clear_flag(std::uint8_t f) noexcept {
        flags = static_cast<std::uint8_t>(flags & ~f);
    }

    /// Quantity the venue currently has working, per its last confirmation.
    [[nodiscard]] constexpr qty_t resting() const noexcept { return acked_qty - filled - canceled; }

    /// Quantity we want working once every outstanding request has landed. A
    /// cancel -- sent or merely deferred -- means we want none of it, so the
    /// level's accounting reflects the intent the moment it is formed rather
    /// than when the message finally goes out.
    [[nodiscard]] constexpr qty_t desired() const noexcept {
        if (state == order_state_t::pending_cancel || pending == pending_action_t::cancel) {
            return 0;
        }
        return order_qty - filled - canceled;
    }

    /// True while the venue owes us a response.
    [[nodiscard]] constexpr bool in_flight() const noexcept {
        return state == order_state_t::pending_new || state == order_state_t::pending_modify ||
               state == order_state_t::pending_cancel;
    }

    /// True when everything the stacker asked for has been confirmed and there
    /// is nothing left to send.
    [[nodiscard]] constexpr bool settled() const noexcept {
        return state == order_state_t::live && pending == pending_action_t::none &&
               price == acked_price && order_qty == acked_qty;
    }

    /// True when the order is tracked at all.
    [[nodiscard]] constexpr bool active() const noexcept { return state != order_state_t::free; }
};

/// How a slot contributes to its levels' running totals.
///
/// This is the single definition of the accounting rule. The stacker maintains
/// `level::acked` and `level::inflight` incrementally on the hot path -- it
/// never recomputes them -- but `stacker::validate()` rebuilds them through
/// this function and compares, which is what keeps the incremental updates
/// honest across the whole test suite.
struct slot_contribution {
    price_t acked_price = 0;  ///< level that receives `acked_delta`
    qty_t acked_delta = 0;
    price_t out_price = 0;  ///< level that receives `out_delta` (never positive)
    qty_t out_delta = 0;
    price_t in_price = 0;  ///< level that receives `in_delta` (never negative)
    qty_t in_delta = 0;
};

template <OrderIdLike Id>
[[nodiscard]] constexpr slot_contribution contribution_of(const order_slot<Id>& s) noexcept {
    slot_contribution c;
    const qty_t resting = s.resting();

    c.acked_price = s.acked_price;
    c.acked_delta = resting;

    if (!s.settled()) {
        // The confirmed quantity is about to leave the level it rests on, and
        // the requested quantity is about to arrive at the level we want. For a
        // same-level resize both prices coincide and the two net out to the
        // difference; for a reprice they are genuinely different levels.
        c.out_price = s.acked_price;
        c.out_delta = -resting;
        c.in_price = s.price;
        c.in_delta = s.desired();
    }
    return c;
}

}  // namespace detail
SLICK_STACKER_NAMESPACE_END
