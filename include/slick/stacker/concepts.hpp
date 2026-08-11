// Copyright 2026 Slick Quant
// SPDX-License-Identifier: MIT

#pragma once

#include <slick/stacker/config.hpp>
#include <slick/stacker/types.hpp>

#include <concepts>
#include <cstdint>
#include <type_traits>

SLICK_STACKER_NAMESPACE_BEGIN

/// Order identifiers must be cheap to copy, compare and hash. Integers and
/// pointers both qualify, so an executor can hand back either a venue order id
/// or a pointer straight into its own order table.
template <class T>
concept OrderIdLike =
    std::equality_comparable<T> && std::is_trivially_copyable_v<T> &&
    (std::integral<T> || std::is_pointer_v<T>) && !std::same_as<std::remove_cv_t<T>, bool>;

/// The order-entry interface the stacker drives.
///
/// `place` returns the identifier the venue or gateway assigned, or
/// `invalid_order_id` if the request could not be sent at all (a gate, a
/// throttle, an exhausted order pool). Returning `invalid_order_id` is not an
/// error the stacker propagates -- it simply stops adding at that level and
/// tries again on the next reconcile.
///
/// `modify` and `cancel` return false if the request could not be sent. The
/// stacker leaves its accounting untouched in that case, so a later reconcile
/// retries.
///
/// None of the three may call back into the stacker.
/// `place` receives the configured `order_type_t`; a replace does not, because
/// changing an order's kind mid-life is not something the stacker ever asks for.
template <class E>
concept OrderExecutor = requires(E& e, side_t s, price_t px, qty_t q, order_type_t ot,
                                 const typename E::order_id_t& id) {
    typename E::order_id_t;
    requires OrderIdLike<typename E::order_id_t>;
    { E::invalid_order_id } -> std::convertible_to<typename E::order_id_t>;
    { e.place(s, px, q, ot) } -> std::same_as<typename E::order_id_t>;
    { e.modify(id, px, q) } -> std::same_as<bool>;
    { e.cancel(id) } -> std::same_as<bool>;
};

/// Optional. When present, the stacker calls it once per reconcile after the
/// last message, so an executor that batches into a single gateway write can
/// flush at exactly the right point.
template <class E>
concept HasFlush = requires(E& e) { e.flush(); };

/// Optional, and the single biggest win available to an integrator.
///
/// Because the executor owns the order identifier, routing an inbound event
/// back to the stacker's slot normally costs a hash probe. If the executor can
/// store one 32-bit word alongside its own order record, the stacker keeps its
/// slot index there instead and every event handler becomes a single indexed
/// load with no hashing and no extra cache line touched.
template <class E>
concept HasOrderUserData =
    requires(E& e, const typename E::order_id_t& id, std::uint32_t v) {
        e.set_order_user_data(id, v);
        { e.get_order_user_data(id) } -> std::convertible_to<std::uint32_t>;
    };

/// Optional. Lets the executor veto acting on an order the stacker believes is
/// actionable -- for example one the gateway knows is already being cancelled.
/// Absent, the stacker relies solely on its own order state.
template <class E>
concept HasCanAct =
    requires(E& e, const typename E::order_id_t& id) {
        { e.can_act(id) } -> std::convertible_to<bool>;
    };

SLICK_STACKER_NAMESPACE_END
