// Copyright 2026 Slick Quant
// SPDX-License-Identifier: MIT

#pragma once

#include <slick/stacker/config.hpp>
#include <slick/stacker/stacker.hpp>
#include <slick/stacker/types.hpp>

#include <algorithm>

SLICK_STACKER_NAMESPACE_BEGIN

/// Two stackers, one per side, plus the guard that keeps them out of each
/// other's way.
///
/// Quoting both sides of the same instrument introduces a hazard a single-sided
/// stacker cannot see: the bid stack walking up into prices where our own offer
/// stack is still working. Depending on the venue that is a self-trade, a
/// rejected order, or a self-match-prevention cancel that silently removes the
/// resting side. The pair prevents it by feeding each side a placement limit
/// that is the tighter of the market's opposite top and the nearest price the
/// other side may still be holding.
///
/// Because the resting order has to actually be withdrawn before the other side
/// may use that price, resolving a crossed quote takes a round trip. The pair
/// reports `dirty()` until it has settled, so a caller that reconciles on every
/// event batch converges without needing to know why.
template <OrderExecutor Executor, class Traits = default_traits>
class stacker_pair {
public:
    using executor_type = Executor;
    using order_id_t = typename Executor::order_id_t;
    using buy_stacker = stacker<Executor, side_t::buy, Traits>;
    using sell_stacker = stacker<Executor, side_t::sell, Traits>;

    stacker_pair(Executor& exec, const stacker_config& buy_cfg,
                 const stacker_config& sell_cfg) noexcept
        : buy_(exec, buy_cfg), sell_(exec, sell_cfg) {}

    stacker_pair(Executor& exec, const stacker_config& cfg) noexcept
        : stacker_pair(exec, cfg, cfg) {}

    stacker_pair(const stacker_pair&) = delete;
    stacker_pair& operator=(const stacker_pair&) = delete;

    [[nodiscard]] buy_stacker& buy() noexcept { return buy_; }
    [[nodiscard]] sell_stacker& sell() noexcept { return sell_; }
    [[nodiscard]] const buy_stacker& buy() const noexcept { return buy_; }
    [[nodiscard]] const sell_stacker& sell() const noexcept { return sell_; }

    /// Quote both sides at once. Pass a quantity of zero to step off one side
    /// while leaving its ladder working, exactly as on a single stacker.
    void quote(price_t bid, qty_t bid_qty, price_t ask, qty_t ask_qty) noexcept {
        buy_.quote(bid, bid_qty);
        sell_.quote(ask, ask_qty);
    }

    void pull() noexcept {
        buy_.pull();
        sell_.pull();
    }

    /// Lift every reject latch on both sides. See `stacker::clear_rejects()`.
    void clear_rejects() noexcept {
        buy_.clear_rejects();
        sell_.clear_rejects();
    }

    /// Publish the market's top of book. Either price may be `k_null_price`
    /// when that side of the book is empty.
    void on_top_of_book(price_t bid, price_t ask) noexcept {
        market_bid_ = bid;
        market_ask_ = ask;
    }

    [[nodiscard]] bool dirty() const noexcept { return buy_.dirty() || sell_.dirty(); }

    /// Each side's limit is taken from the other side's state immediately
    /// before it reconciles, so the bid (which goes first) wins a contested
    /// price and the offer is kept clear of whatever the bid just sent.
    void reconcile() noexcept {
        guard_buy();
        buy_.reconcile();
        guard_sell();
        sell_.reconcile();
        // Withdrawing from a contested price frees it for the other side, but
        // only once the venue confirms. Re-arm the bid's guard against the
        // offer as it now stands so the next pass picks the ground up; the
        // offer's is already current, since the bid has not moved since.
        guard_buy();
    }

    // -----------------------------------------------------------------------
    // Event routing
    //
    // The side is a parameter rather than something the pair works out for
    // itself: the caller already knows it, and guessing would mean probing both
    // sides' identifier tables on every event.
    // -----------------------------------------------------------------------

    void on_accepted(side_t s, const order_id_t& id, price_t price, qty_t qty) noexcept {
        dispatch(s, [&](auto& st) { st.on_accepted(id, price, qty); });
    }

    void on_replaced(side_t s, const order_id_t& id, price_t price, qty_t qty) noexcept {
        dispatch(s, [&](auto& st) { st.on_replaced(id, price, qty); });
    }

    void on_filled(side_t s, const order_id_t& id, qty_t fill_qty, price_t fill_price) noexcept {
        dispatch(s, [&](auto& st) { st.on_filled(id, fill_qty, fill_price); });
    }

    void on_canceled(side_t s, const order_id_t& id, qty_t canceled_qty,
                     cancel_reason_t reason = cancel_reason_t::requested) noexcept {
        dispatch(s, [&](auto& st) { st.on_canceled(id, canceled_qty, reason); });
    }

    void on_rejected(side_t s, const order_id_t& id, reject_reason_t reason) noexcept {
        dispatch(s, [&](auto& st) { st.on_rejected(id, reason); });
    }

    void on_modify_rejected(side_t s, const order_id_t& id, reject_reason_t reason) noexcept {
        dispatch(s, [&](auto& st) { st.on_modify_rejected(id, reason); });
    }

    void on_cancel_rejected(side_t s, const order_id_t& id, reject_reason_t reason) noexcept {
        dispatch(s, [&](auto& st) { st.on_cancel_rejected(id, reason); });
    }

    void on_book_level(side_t s, price_t price, qty_t qty) noexcept {
        dispatch(s, [&](auto& st) { st.on_book_level(price, qty); });
    }

    void on_queue_position(side_t s, const order_id_t& id, qty_t qty_in_front) noexcept {
        dispatch(s, [&](auto& st) { st.on_queue_position(id, qty_in_front); });
    }

    [[nodiscard]] bool validate() const noexcept { return buy_.validate() && sell_.validate(); }

private:
    template <class F>
    void dispatch(side_t s, F&& f) noexcept {
        if (s == side_t::buy) {
            f(buy_);
        } else {
            f(sell_);
        }
    }

    /// Give a side a placement limit that is the tighter of the market's
    /// opposite top and the nearest price the other side may still hold.
    void guard_buy() noexcept {
        buy_.on_opposite_top(tighter<side_t::buy>(market_ask_, sell_.nearest_live_price()));
    }

    void guard_sell() noexcept {
        sell_.on_opposite_top(tighter<side_t::sell>(market_bid_, buy_.nearest_live_price()));
    }

    /// For a buy the more restrictive limit is the lower price; for a sell, the
    /// higher. `k_null_price` means "no limit from this source".
    template <side_t S>
    [[nodiscard]] static price_t tighter(price_t a, price_t b) noexcept {
        if (a == k_null_price) {
            return b;
        }
        if (b == k_null_price) {
            return a;
        }
        if constexpr (S == side_t::buy) {
            return std::min(a, b);
        } else {
            return std::max(a, b);
        }
    }

    buy_stacker buy_;
    sell_stacker sell_;
    price_t market_bid_ = k_null_price;
    price_t market_ask_ = k_null_price;
};

SLICK_STACKER_NAMESPACE_END
