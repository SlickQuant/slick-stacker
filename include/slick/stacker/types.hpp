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
/// enough that a slot's hot fields stay inside one cache line.
using slot_index_t = std::uint16_t;

/// Sentinel for "no slot".
inline constexpr slot_index_t k_null_slot = std::numeric_limits<slot_index_t>::max();

/// Sentinel meaning "no price".
inline constexpr price_t k_null_price = std::numeric_limits<price_t>::min();

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

/// Why the venue rejected a request. `retryable` and `throttled` leave the
/// stacker's target untouched so the next reconcile tries again; everything
/// else is treated as terminal for the order in question.
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
