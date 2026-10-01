// Copyright 2026 Slick Quant
// SPDX-License-Identifier: MIT

#pragma once

#include <slick/stacker/config.hpp>

#include <cstdint>
#include <limits>

SLICK_STACKER_NAMESPACE_BEGIN

/// Price in ticks of the instrument's minimum increment, or any fixed-point
/// scaling of your choosing. The stacker only ever adds, subtracts and divides
/// by `tick_size`, so any integral representation works as long as it is
/// consistent. Signed, because a level's depth arithmetic goes negative when
/// the quote improves past the ring anchor.
using price_t = std::int64_t;

/// Quantity. Signed on purpose: `level::inflight` carries net in-flight change
/// and is negative while a reduction is outstanding.
using qty_t = std::int64_t;

/// Index into the order slot pool. `uint16_t` keeps the intrusive links small
/// enough that a slot's hot fields -- see `detail::order_slot` -- fit in the
/// first 32 bytes.
using slot_index_t = std::uint16_t;

/// Sentinel for "no slot".
inline constexpr slot_index_t k_null_slot = std::numeric_limits<slot_index_t>::max();

/// Sentinel meaning "no price". Deliberately the same shape as
/// `k_no_qty_limit`: the top of the type's range. Every use of it is an
/// equality test -- see `crossing_ok()` and `stacker_pair::tighter()`, both of
/// which check for it before any ordering comparison -- so the value carries no
/// directional meaning and must never itself be compared with `<` or `>`.
inline constexpr price_t k_null_price = std::numeric_limits<price_t>::max();

/// Sentinel meaning "unbounded" for the quantity limits in `stacker_config`.
inline constexpr qty_t k_no_qty_limit = std::numeric_limits<qty_t>::max();

/// Side of the book the stacker quotes on. Values are usable as array indices.
enum class side_t : std::uint8_t {
    buy = 0,
    sell = 1,
};

[[nodiscard]] constexpr side_t opposite(side_t s) noexcept {
    return s == side_t::buy ? side_t::sell : side_t::buy;
}

[[nodiscard]] constexpr const char* to_string(side_t s) noexcept {
    return s == side_t::buy ? "buy" : "sell";
}

/// What kind of order the stacker sends.
///
/// Every order it sends carries a price, so both of these are limit orders in
/// the FIX sense; what differs is how long the venue keeps them. The names
/// follow the usual trading-desk shorthand rather than splitting order type
/// from time in force.
///
/// Both rest, which is the only property the stacker itself depends on: a level
/// of a ladder is a resting order by definition. Immediate-or-cancel kinds have
/// no place here -- an order the venue kills on arrival cannot hold a level, and
/// a stacker that keeps replacing one is a machine gun. Send those directly.
enum class order_type_t : std::uint8_t {
    /// Rests until the end of the session. The default.
    limit = 0,

    /// Rests across sessions.
    gtc,
};

[[nodiscard]] constexpr const char* to_string(order_type_t t) noexcept {
    switch (t) {
        case order_type_t::limit:
            return "limit";
        case order_type_t::gtc:
            return "gtc";
    }
    return "unknown";
}

/// Lifecycle of a single working order.
enum class order_state_t : std::uint8_t {
    free = 0,        ///< slot is on the free list
    pending_new,     ///< sent, no acknowledgement yet
    live,            ///< acknowledged and resting
    pending_modify,  ///< a modify is outstanding
    pending_cancel,  ///< a cancel is outstanding
};

/// An action the stacker wants to take but cannot yet, because the venue will
/// not accept it until the order has been acknowledged. Stamped on the slot and
/// fired from the acknowledgement handler.
enum class pending_action_t : std::uint8_t {
    none = 0,
    cancel,
    modify,
};

/// Why the venue rejected a request.
///
/// `throttled` and `retryable` -- see `is_retryable` -- change nothing, so the
/// next reconcile sends the same request again. Every other reason, `unknown`
/// included, is terminal for what was asked:
///
///   * A rejected new order, or a rejected modify that moved an order to a new
///     price, latches that price's level: nothing that would add quantity there
///     is sent again until the caller changes the level's target or calls
///     `stacker::clear_rejects()`. The target itself is left alone, so
///     `target_at` still reports what the caller asked for.
///   * A rejected modify also marks the order as one the venue will not amend.
///     It is never modified again; when it has to shrink or move it is
///     cancelled, and the add pass replaces whatever is still wanted.
///
/// `too_late_to_act` on a modify means the order is already finishing, not that
/// the price was wrong, so it marks the order but latches no level.
///
/// A rejected cancel is retried whatever the reason, because a cancel only ever
/// takes exposure off -- except `too_late_to_act`, which waits for the fill or
/// cancel that is already on its way.
enum class reject_reason_t : std::uint8_t {
    unknown = 0,
    throttled,        ///< rate limited; safe to retry
    retryable,        ///< transient venue/gateway condition
    too_late_to_act,  ///< the order already filled or was already done
    risk_limit,
    invalid_price,
    invalid_qty,
    self_match_prevention,
    market_closed,
    terminal,  ///< do not retry
};

[[nodiscard]] constexpr bool is_retryable(reject_reason_t r) noexcept {
    return r == reject_reason_t::throttled || r == reject_reason_t::retryable;
}

/// Why an order was cancelled. `unsolicited` covers venue-initiated removals
/// (self-match prevention, session drop, mass cancel) that the stacker did not
/// request and must therefore account for on arrival.
enum class cancel_reason_t : std::uint8_t {
    requested = 0,
    unsolicited,
    self_match_prevention,
    expired,
};

SLICK_STACKER_NAMESPACE_END
