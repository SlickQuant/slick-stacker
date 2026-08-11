// Copyright 2026 Slick Quant
// SPDX-License-Identifier: MIT

#pragma once

#include <slick/stacker/concepts.hpp>
#include <slick/stacker/config.hpp>
#include <slick/stacker/types.hpp>

#include <cstdint>
#include <functional>

SLICK_STACKER_NAMESPACE_BEGIN

/// An executor assembled from `std::function` callbacks.
///
/// Convenience only. Every order action becomes an indirect call through a
/// type-erased wrapper, which is precisely what the template `Executor`
/// parameter exists to avoid -- write a small struct with three methods
/// instead and the calls inline. This is here for wiring a prototype together,
/// binding from a scripting layer, or any path where the extra indirection is
/// beneath notice.
///
/// A callback left unset behaves as a refusal, so a partially wired adapter
/// fails safe: the stacker leaves the level short and retries rather than
/// believing in orders that were never sent.
template <OrderIdLike Id = std::uint64_t>
class function_executor {
public:
    using order_id_t = Id;
    static constexpr Id invalid_order_id = Id{};

    using place_fn = std::function<Id(side_t, price_t, qty_t, order_type_t)>;
    using modify_fn = std::function<bool(const Id&, price_t, qty_t)>;
    using cancel_fn = std::function<bool(const Id&)>;
    using flush_fn = std::function<void()>;

    function_executor() = default;
    function_executor(place_fn p, modify_fn m, cancel_fn c, flush_fn f = {})
        : on_place(std::move(p)),
          on_modify(std::move(m)),
          on_cancel(std::move(c)),
          on_flush(std::move(f)) {}

    Id place(side_t s, price_t price, qty_t qty, order_type_t type) {
        return on_place ? on_place(s, price, qty, type) : invalid_order_id;
    }

    bool modify(const Id& id, price_t price, qty_t qty) {
        return on_modify ? on_modify(id, price, qty) : false;
    }

    bool cancel(const Id& id) { return on_cancel ? on_cancel(id) : false; }

    void flush() {
        if (on_flush) {
            on_flush();
        }
    }

    place_fn on_place;
    modify_fn on_modify;
    cancel_fn on_cancel;
    flush_fn on_flush;
};

static_assert(OrderExecutor<function_executor<>>,
              "function_executor must satisfy the executor contract");

SLICK_STACKER_NAMESPACE_END
