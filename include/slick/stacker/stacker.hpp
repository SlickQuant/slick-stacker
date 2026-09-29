// Copyright 2026 Slick Quant
// SPDX-License-Identifier: MIT

#pragma once

#include <slick/stacker/concepts.hpp>
#include <slick/stacker/config.hpp>
#include <slick/stacker/detail/id_index.hpp>
#include <slick/stacker/detail/level.hpp>
#include <slick/stacker/detail/level_ring.hpp>
#include <slick/stacker/detail/order_slot.hpp>
#include <slick/stacker/detail/slot_pool.hpp>
#include <slick/stacker/stacker_config.hpp>
#include <slick/stacker/types.hpp>

#include <algorithm>
#include <cstdint>
#include <span>
#include <type_traits>

SLICK_STACKER_NAMESPACE_BEGIN

/// Maintains a stack of resting orders on one side of one instrument.
///
/// The caller supplies a single price and quantity; the stacker builds that
/// quote plus a configured ladder of levels behind it, and then keeps the whole
/// thing in agreement with the venue as acknowledgements, fills, cancels and
/// rejects arrive.
///
/// ### Usage
///
///     my_executor exec;
///     stacker_config cfg;
///     cfg.tick_size = 25;
///     cfg.levels = 3;
///     cfg.stack_qty = 10;
///     stacker<my_executor, side_t::buy> s{exec, cfg};
///
///     s.quote(500'000, 25);   // 25 at the top, 10 on each of the 3 below
///     s.reconcile();          // sends the orders
///
///     // ... later, from the order-entry callback thread ...
///     s.on_accepted(id, px, qty);
///     s.on_filled(id, 5, px);
///     s.reconcile();          // one pass for the whole event batch
///
/// ### Event handlers never send
///
/// Every `on_*` handler updates state and marks the stacker dirty; none of them
/// send a message. Work happens in `reconcile()`, which the caller runs once
/// after draining a batch of events. A burst of twenty fills therefore costs
/// one recalculation rather than twenty, and no intermediate state ever reaches
/// the wire.
///
/// ### Threading
///
/// Single-threaded and lock-free: no mutexes, no atomics, no allocation after
/// construction. Deliver market data and order events on the same thread that
/// calls `quote` and `reconcile`. If they arrive on another thread, hand them
/// across with an SPSC queue and drain it before reconciling.
template <OrderExecutor Executor, side_t Side, class Traits = default_traits>
class stacker {
public:
    using executor_type = Executor;
    using order_id_t = typename Executor::order_id_t;
    using traits_type = Traits;

    static constexpr side_t side = Side;

private:
    using ring_type = detail::level_ring<Side, Traits::level_capacity>;
    using pool_type = detail::slot_pool<order_id_t, Traits::max_orders>;
    using slot_type = detail::order_slot<order_id_t>;
    using level_type = detail::level;

    /// Sized at twice the order pool so the table never runs above half load,
    /// which is where linear probing stays at roughly one probe per lookup.
    static constexpr std::uint32_t k_index_capacity =
        detail::next_pow2(static_cast<std::uint32_t>(Traits::max_orders) * 2u);

    using index_type = std::conditional_t<HasOrderUserData<Executor>,
                                          detail::null_id_index<order_id_t>,
                                          detail::id_index<order_id_t, k_index_capacity>>;

public:
    stacker(Executor& exec, const stacker_config& cfg) noexcept : exec_(&exec) { configure(cfg); }

    stacker(const stacker&) = delete;
    stacker& operator=(const stacker&) = delete;

    // -----------------------------------------------------------------------
    // Quoting
    // -----------------------------------------------------------------------

    /// Set the top of the stack. The levels behind it follow from the config.
    ///
    /// A `qty` of zero pulls the top level only and leaves the ladder behind it
    /// working -- useful for stepping away from the touch without giving up
    /// depth. Use `pull()` to take everything out.
    void quote(price_t price, qty_t qty) noexcept {
        if (qty < 0) [[unlikely]] {
            qty = 0;
        }
        // Re-asserting the quote already in force is the most common call there
        // is. It cannot change the shape, so it must not cost a pass over the
        // band -- and it must not mark the stacker dirty, or every quiet tick
        // would drag a reconcile behind it.
        //
        // Unless a fill has eaten into a target since the last pass: then the
        // identical quote is the caller asking for that size back, and the
        // shape has to be laid down again.
        if (price == quote_price_ && qty == quote_qty_ && !pulled_ &&
            !target_consumed_) [[likely]] {
            return;
        }
        quote_price_ = price;
        quote_qty_ = qty;
        pulled_ = false;
        shape_dirty_ = true;
        dirty_ = true;
    }

    /// Zero every target. The next `reconcile()` cancels the whole stack.
    void pull() noexcept {
        pulled_ = true;
        shape_dirty_ = true;
        dirty_ = true;
    }

    /// True when there is work outstanding for `reconcile()`.
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }

    /// Bring the venue into agreement with the current targets.
    ///
    /// Runs four passes over the live band of levels: reprice surplus orders
    /// into levels that need quantity, shrink what is still over target, top up
    /// what is still short, and release the levels that have gone quiet. A
    /// change of `order_type` prepends a fifth, which takes out the orders left
    /// carrying the old kind.
    void reconcile() noexcept {
        if (!dirty_) {
            return;
        }
        dirty_ = false;

        if (shape_dirty_) {
            shape_dirty_ = false;
            apply_shape();
        }
        if (!anchored_) [[unlikely]] {
            return;
        }

        needs_retry_ = false;
        // Before anything else: an order of the wrong kind is not a candidate
        // for repricing or trimming, it is a candidate for removal. Taking it
        // out first also frees the quantity for the add pass to put back.
        if (retype_pending_) [[unlikely]] {
            retype_pass();
        }
        if (cfg_.prefer_modify) {
            reprice_pass();
        }
        reduce_pass();
        add_pass();

        ring_.shrink_band();

        // A refused message leaves real work undone with no event coming to
        // wake us up again, so stay dirty and try once more on the next pass.
        dirty_ = needs_retry_;

        if constexpr (HasFlush<Executor>) {
            exec_->flush();
        }
    }

    // -----------------------------------------------------------------------
    // Market data
    // -----------------------------------------------------------------------

    /// Publish the quantity resting in the public book at one of our prices.
    /// Only the queue-gap gate consults it, so this can be skipped entirely
    /// when `queue_gap` is zero. Prices outside the live band are ignored.
    ///
    /// The value is always recorded, but the stacker is only marked dirty when
    /// it opens the gate at a level that is waiting on it. A busy book feed
    /// therefore never turns into reconciles that have nothing to do.
    void on_book_level(price_t price, qty_t qty) noexcept {
        if (!anchored_) {
            return;
        }
        const std::int32_t d = ring_.depth_of(price);
        if (d == detail::k_invalid_depth) {
            return;
        }
        level_type* l = ring_.peek(d);
        if (l == nullptr || l->market_qty == qty) {
            return;
        }
        const qty_t before = l->market_qty;
        l->market_qty = qty;
        // Only an update that opens the gate is worth a reconcile. At a busy
        // level the gate stays shut far more often than not, so its new state
        // is tested first, and its old state and the rest of what holds the
        // level back only on the rare update that passes.
        if (dirty_ || cfg_.queue_gap <= 0 || l->order_count == 0) {
            return;
        }
        const slot_type& last = pool_[l->tail];
        if (gap_met(last, qty) && !gap_met(last, before) && subject_to_queue_gap(*l)) {
            dirty_ = true;
        }
    }

    /// Publish the best price on the other side of the book. The stacker will
    /// not place an order that would cross it. Pass `k_null_price` to disable
    /// the check.
    void on_opposite_top(price_t price) noexcept {
        if (opposite_top_ != price) {
            opposite_top_ = price;
            dirty_ = true;
        }
    }

    /// Publish how much market quantity is ahead of one of our orders. Feeds
    /// the queue-gap gate; without it, a non-zero `queue_gap` will hold the
    /// stacker to one order per level.
    ///
    /// Like `on_book_level`, this only marks the stacker dirty when the update
    /// opens the gate: the order is the last one at a level that is waiting on
    /// it, which is the only order the gate looks at.
    void on_queue_position(const order_id_t& id, qty_t qty_in_front) noexcept {
        const slot_index_t si = lookup(id);
        if (si == k_null_slot) {
            return;
        }
        slot_type& s = pool_[si];
        if (cfg_.queue_gap <= 0 || dirty_) {
            // Nothing to decide: the gate is off, or a reconcile is due anyway.
            s.qty_in_front = qty_in_front;
            s.set_flag(slot_type::flag_qp_valid);
            return;
        }
        const qty_t before = s.qty_in_front;
        const bool had = s.has_flag(slot_type::flag_qp_valid);
        if (had && before == qty_in_front) {
            return;
        }
        s.qty_in_front = qty_in_front;
        s.set_flag(slot_type::flag_qp_valid);
        // Tested in the same order as `on_book_level`: the gate only looks at
        // a level's last order, and only once it is live; then whether it is
        // open now, whether it was shut before, and whether the level is
        // waiting on it at all.
        if (s.state != order_state_t::live || !s.has_flag(slot_type::flag_linked)) {
            return;
        }
        const level_type* l = ring_.peek(s.linked_depth);
        if (l == nullptr || l->tail != si || !gap_behind(l->market_qty, qty_in_front)) {
            return;
        }
        if (!(had && gap_behind(l->market_qty, before)) && subject_to_queue_gap(*l)) {
            dirty_ = true;
        }
    }

    // -----------------------------------------------------------------------
    // Order events
    // -----------------------------------------------------------------------

    /// A new order was accepted. `price` and `qty` are what the venue booked,
    /// which is normally but not necessarily what we asked for.
    void on_accepted(const order_id_t& id, price_t price, qty_t qty) noexcept {
        on_confirmed(id, price, qty, /*is_replace=*/false);
    }

    /// A modify was accepted. `qty` is the new total order quantity, on the
    /// same basis as the quantity passed to `Executor::modify`.
    void on_replaced(const order_id_t& id, price_t price, qty_t qty) noexcept {
        on_confirmed(id, price, qty, /*is_replace=*/true);
    }

    /// An execution. `fill_qty` is this fill alone, not a running total.
    void on_filled(const order_id_t& id, qty_t fill_qty, price_t fill_price) noexcept {
        const slot_index_t si = lookup(id);
        if (si == k_null_slot || fill_qty <= 0) {
            return;
        }
        slot_type& s = pool_[si];

        detach(s);
        s.filled += fill_qty;
        // A fill can beat the acknowledgement out of the venue. Whatever it
        // has already done is by definition working, so pull `acked_qty` up
        // rather than let `resting()` go negative.
        s.acked_qty = std::max(s.acked_qty, s.filled + s.canceled);
        attach(s);

        if (!cfg_.refill_on_fill) {
            // The caller's quote authorised this size once. Consume it, so the
            // stack does not silently re-arm quantity nobody asked for a second
            // time; the next quote() decides whether to put it back.
            consume_target(fill_price != k_null_price ? fill_price : s.acked_price, fill_qty);
        }

        if (is_done(s)) {
            retire(si);
        }
        dirty_ = true;
    }

    /// An order was cancelled, by us or by the venue. `canceled_qty` is the
    /// quantity removed; pass 0 to mean "whatever was left".
    void on_canceled(const order_id_t& id, qty_t canceled_qty,
                     cancel_reason_t reason = cancel_reason_t::requested) noexcept {
        (void)reason;
        const slot_index_t si = lookup(id);
        if (si == k_null_slot) {
            return;
        }
        slot_type& s = pool_[si];

        detach(s);
        s.canceled += canceled_qty > 0 ? canceled_qty : s.resting();
        s.acked_qty = std::max(s.acked_qty, s.filled + s.canceled);
        s.pending = pending_action_t::none;
        s.state = order_state_t::live;
        if (s.resting() > 0) {
            // A partial removal -- the venue trimmed the order but left it
            // working. Adopt what is left as our intent and let reconcile
            // decide whether to make the quantity up elsewhere.
            s.order_qty = s.acked_qty;
        } else {
            // Gone for good, whatever we last asked for.
            s.order_qty = s.filled + s.canceled;
        }
        attach(s);

        if (is_done(s)) {
            retire(si);
        }
        dirty_ = true;
    }

    /// A new order was rejected: it never existed at the venue.
    void on_rejected(const order_id_t& id, reject_reason_t reason) noexcept {
        (void)reason;
        const slot_index_t si = lookup(id);
        if (si == k_null_slot) {
            return;
        }
        retire(si);
        dirty_ = true;
    }

    /// A modify was rejected. The order is still working at whatever the venue
    /// last confirmed, so intent is rolled back onto that and the slot is
    /// re-homed to the level it actually rests on.
    void on_modify_rejected(const order_id_t& id, reject_reason_t reason) noexcept {
        (void)reason;
        const slot_index_t si = lookup(id);
        if (si == k_null_slot) {
            return;
        }
        slot_type& s = pool_[si];
        if (s.inflight_modifies > 0) {
            --s.inflight_modifies;
        }
        if (s.inflight_modifies > 0) {
            // A later request in the chain is still outstanding and carries the
            // real intent. Rolling back now would clobber it; whichever
            // response lands last decides what the venue is actually holding.
            dirty_ = true;
            return;
        }

        rollback_to_acked(si);
        if (is_done(pool_[si])) {
            // The reject was "too late to act" in disguise: the order had
            // already finished. Nothing to roll back to.
            retire(si);
        } else {
            drain_pending(si);
        }
        dirty_ = true;
    }

    /// A cancel was rejected: the order is still working.
    void on_cancel_rejected(const order_id_t& id, reject_reason_t reason) noexcept {
        const slot_index_t si = lookup(id);
        if (si == k_null_slot) {
            return;
        }
        if (reason == reject_reason_t::too_late_to_act) {
            // The order is already finishing; its fill or cancel is on the way.
            // Leaving the slot in `pending_cancel` keeps reconcile from
            // chasing it in the meantime.
            return;
        }
        slot_type& s = pool_[si];
        detach(s);
        s.state = order_state_t::live;
        s.pending = pending_action_t::none;
        attach(s);
        if (is_done(s)) {
            retire(si);
        }
        dirty_ = true;
    }

    // -----------------------------------------------------------------------
    // Runtime configuration
    // -----------------------------------------------------------------------

    [[nodiscard]] const stacker_config& config() const noexcept { return cfg_; }

    /// Replace the whole configuration. `tick_size` changes force the price
    /// grid to be rebuilt, which cancels everything currently working, and
    /// `order_type` changes cancel and replace it.
    ///
    /// The stacker runs on `cfg.sanitized<Traits>()`, so an unvalidated config
    /// cannot corrupt it; `config()` returns what it is actually running on.
    void configure(const stacker_config& cfg) noexcept {
        const stacker_config next = cfg.template sanitized<Traits>();
        const bool grid_changed = anchored_ && next.tick_size != cfg_.tick_size;
        const bool type_changed = next.order_type != cfg_.order_type;
        cfg_ = next;
        cfg_.qty_profile = {};  // the span is copied below; do not retain it
        set_qty_profile(next.qty_profile);
        if (grid_changed) {
            rebase(quote_price_);
        }
        if (type_changed) {
            arm_retype();
        }
        shape_dirty_ = true;
        dirty_ = true;
    }

    /// Change the kind of order the stacker sends.
    ///
    /// A modify carries price and quantity only, so orders already at the venue
    /// cannot be amended into the new kind. The next `reconcile()` cancels them
    /// and the add pass re-establishes each level with the new one -- the same
    /// cancel-and-replace the stacker uses anywhere else an order cannot be
    /// changed in place, and it costs two messages per working order.
    void set_order_type(order_type_t type) noexcept {
        if (type == cfg_.order_type) {
            return;
        }
        cfg_.order_type = type;
        arm_retype();
        dirty_ = true;
    }

    void set_levels(std::uint16_t levels) noexcept {
        cfg_.levels = cfg_.template clamp_levels<Traits>(levels);
        if (profile_len_ == levels) {
            // Sized for the ladder asked for: keep the rungs that still fit.
            profile_len_ = cfg_.levels;
        } else if (profile_len_ != cfg_.levels) {
            profile_len_ = 0;  // a stale profile no longer describes the ladder
        }
        shape_dirty_ = true;
        dirty_ = true;
    }

    void set_stack_qty(qty_t qty) noexcept {
        cfg_.stack_qty = qty < 0 ? 0 : qty;
        shape_dirty_ = true;
        dirty_ = true;
    }

    /// Per-level target quantities, index 0 being the level nearest the quote.
    /// An empty span reverts to the uniform `stack_qty`. The span is copied.
    void set_qty_profile(std::span<const qty_t> profile) noexcept {
        profile_len_ = static_cast<std::uint16_t>(
            std::min<std::size_t>(profile.size(), Traits::max_levels));
        for (std::uint16_t i = 0; i < profile_len_; ++i) {
            profile_[i] = profile[i] < 0 ? 0 : profile[i];
        }
        shape_dirty_ = true;
        dirty_ = true;
    }

    // -----------------------------------------------------------------------
    // Introspection
    // -----------------------------------------------------------------------

    [[nodiscard]] qty_t target_at(price_t price) const noexcept {
        const level_type* l = level_or_null(price);
        return l != nullptr ? l->target : 0;
    }

    /// Quantity that will be resting at `price` once every outstanding request
    /// has been confirmed.
    [[nodiscard]] qty_t working_at(price_t price) const noexcept {
        const level_type* l = level_or_null(price);
        return l != nullptr ? l->working() : 0;
    }

    /// Quantity the venue has confirmed at `price`.
    [[nodiscard]] qty_t acked_at(price_t price) const noexcept {
        const level_type* l = level_or_null(price);
        return l != nullptr ? l->acked : 0;
    }

    [[nodiscard]] std::uint16_t order_count_at(price_t price) const noexcept {
        const level_type* l = level_or_null(price);
        return l != nullptr ? l->order_count : 0;
    }

    [[nodiscard]] std::uint16_t live_order_count() const noexcept { return pool_.in_use(); }

    /// Number of orders the stacker wanted to shrink but could not, because the
    /// executor refused the message. Adds are suppressed while this is
    /// non-zero, so the stack cannot grow while it is already over-exposed.
    [[nodiscard]] std::uint16_t blocked_count() const noexcept { return blocked_count_; }

    /// How many times the price grid has been rebuilt from scratch. Should stay
    /// at one in steady state; anything more means the live price range keeps
    /// outgrowing `Traits::level_capacity`, or the tick size is wrong.
    [[nodiscard]] std::uint32_t rebase_count() const noexcept { return rebase_count_; }

    [[nodiscard]] price_t quote_price() const noexcept { return quote_price_; }
    [[nodiscard]] qty_t quote_qty() const noexcept { return quote_qty_; }

    /// Price of the level closest to the market that the venue may still be
    /// holding an order at, or `k_null_price` if there is nothing out. Counts
    /// acknowledged quantity as well as intended, so a level with a cancel in
    /// flight still reports -- until that cancel is confirmed the order can
    /// still trade.
    [[nodiscard]] price_t nearest_live_price() const noexcept {
        if (!anchored_ || ring_.band_empty()) {
            return k_null_price;
        }
        for (std::int32_t d = ring_.min_depth(); d <= ring_.max_depth(); ++d) {
            const level_type* l = ring_.peek(d);
            if (l != nullptr && (l->acked > 0 || l->working() > 0)) {
                return l->price;
            }
        }
        return k_null_price;
    }

    /// Recompute every level's counters from the orders that feed them and
    /// compare against the incrementally maintained values, then check the
    /// intrusive lists. Returns false on any disagreement.
    ///
    /// Not cheap and not needed in production -- it exists so tests can assert
    /// after every step that the fast path has stayed honest.
    [[nodiscard]] bool validate() const noexcept;

private:
    // -----------------------------------------------------------------------
    // Level accounting
    //
    // Every mutation of a slot is bracketed by detach()/attach(). Rather than
    // hand-deriving a delta at each of the two dozen call sites -- which is
    // exactly where this kind of code rots -- the slot's contribution is
    // recomputed from one definition and applied twice with opposite signs.
    // It costs a handful of adds and it is what makes validate() meaningful.
    // -----------------------------------------------------------------------

    void detach(const slot_type& s) noexcept { apply_contribution(s, -1); }
    void attach(const slot_type& s) noexcept { apply_contribution(s, +1); }

    void apply_contribution(const slot_type& s, qty_t sign) noexcept {
        if (s.has_flag(slot_type::flag_orphaned)) {
            return;
        }
        const auto c = detail::contribution_of(s);
        if (c.acked_delta != 0) {
            if (level_type* l = level_for(c.acked_price)) {
                l->acked += sign * c.acked_delta;
            }
        }
        if (c.out_delta != 0) {
            if (level_type* l = level_for(c.out_price)) {
                l->inflight += sign * c.out_delta;
            }
        }
        if (c.in_delta != 0) {
            if (level_type* l = level_for(c.in_price)) {
                l->inflight += sign * c.in_delta;
            }
        }
    }

    [[nodiscard]] level_type* level_for(price_t price) noexcept {
        if (!anchored_) [[unlikely]] {
            return nullptr;
        }
        const std::int32_t d = ring_.depth_of(price);
        if (d == detail::k_invalid_depth) [[unlikely]] {
            return nullptr;
        }
        return ring_.try_at(d);
    }

    [[nodiscard]] const level_type* level_or_null(price_t price) const noexcept {
        if (!anchored_) {
            return nullptr;
        }
        const std::int32_t d = ring_.depth_of(price);
        if (d == detail::k_invalid_depth) {
            return nullptr;
        }
        return ring_.peek(d);
    }

    void consume_target(price_t price, qty_t qty) noexcept {
        if (level_type* l = level_for(price)) {
            l->target = l->target > qty ? l->target - qty : 0;
            target_consumed_ = true;
        }
    }

    // -----------------------------------------------------------------------
    // Level order lists
    // -----------------------------------------------------------------------

    void link_back(level_type& l, slot_index_t si) noexcept {
        slot_type& s = pool_[si];
        s.next = k_null_slot;
        s.prev = l.tail;
        if (l.tail != k_null_slot) {
            pool_[l.tail].next = si;
        } else {
            l.head = si;
        }
        l.tail = si;
        ++l.order_count;
        s.linked_depth = l.depth;
        s.set_flag(slot_type::flag_linked);
        ring_.extend_band(l.depth);
    }

    void unlink(slot_index_t si) noexcept {
        slot_type& s = pool_[si];
        if (!s.has_flag(slot_type::flag_linked)) {
            return;
        }
        level_type* l = ring_.peek(s.linked_depth);
        if (l != nullptr) {
            if (s.prev != k_null_slot) {
                pool_[s.prev].next = s.next;
            } else {
                l->head = s.next;
            }
            if (s.next != k_null_slot) {
                pool_[s.next].prev = s.prev;
            } else {
                l->tail = s.prev;
            }
            --l->order_count;
        }
        s.next = k_null_slot;
        s.prev = k_null_slot;
        s.clear_flag(slot_type::flag_linked);
    }

    void relink(level_type& to, slot_index_t si) noexcept {
        if (pool_[si].has_flag(slot_type::flag_linked) && pool_[si].linked_depth == to.depth) {
            return;
        }
        unlink(si);
        link_back(to, si);
    }

    // -----------------------------------------------------------------------
    // Identifier lookup
    // -----------------------------------------------------------------------

    [[nodiscard]] slot_index_t lookup(const order_id_t& id) noexcept {
        if constexpr (HasOrderUserData<Executor>) {
            const std::uint32_t ud = exec_->get_order_user_data(id);
            if (ud >= Traits::max_orders) {
                return k_null_slot;
            }
            const auto si = static_cast<slot_index_t>(ud);
            return (pool_[si].active() && pool_[si].id == id) ? si : k_null_slot;
        } else {
            return index_.find(id);
        }
    }

    void bind_index(const order_id_t& id, slot_index_t si) noexcept {
        if constexpr (HasOrderUserData<Executor>) {
            exec_->set_order_user_data(id, static_cast<std::uint32_t>(si));
        } else {
            index_.insert(id, si);
        }
    }

    void unbind_index(const order_id_t& id) noexcept {
        if constexpr (!HasOrderUserData<Executor>) {
            index_.erase(id);
        }
    }

    // -----------------------------------------------------------------------
    // Slot lifecycle
    // -----------------------------------------------------------------------

    [[nodiscard]] static bool is_done(const slot_type& s) noexcept {
        if (s.pending != pending_action_t::none || s.inflight_modifies != 0) {
            return false;
        }
        // A new order or a modify may still be confirmed for quantity we do not
        // know about yet. An outstanding cancel cannot: it can only ever remove
        // quantity, so an order the venue has already finished is finished even
        // if a cancel for it is still in flight. The cancel reject that follows
        // will find no slot, which is exactly right.
        if (s.state == order_state_t::pending_new || s.state == order_state_t::pending_modify) {
            return false;
        }
        const qty_t gone = s.filled + s.canceled;
        return gone >= s.acked_qty && gone >= s.order_qty;
    }

    /// Note that a message the stacker wanted to send did not go out, so the
    /// next reconcile must run even if nothing else happens in between.
    void note_retry() noexcept { needs_retry_ = true; }

    void mark_blocked(slot_index_t si) noexcept {
        note_retry();
        slot_type& s = pool_[si];
        if (!s.has_flag(slot_type::flag_blocked_decrement)) {
            s.set_flag(slot_type::flag_blocked_decrement);
            ++blocked_count_;
        }
    }

    void clear_blocked(slot_index_t si) noexcept {
        slot_type& s = pool_[si];
        if (s.has_flag(slot_type::flag_blocked_decrement)) {
            s.clear_flag(slot_type::flag_blocked_decrement);
            --blocked_count_;
        }
    }

    void retire(slot_index_t si) noexcept {
        clear_blocked(si);
        slot_type& s = pool_[si];
        detach(s);
        unlink(si);
        unbind_index(s.id);
        pool_.release(si);
    }

    // -----------------------------------------------------------------------
    // Sending
    // -----------------------------------------------------------------------

    /// True when the venue will accept a modify or cancel for this order right
    /// now. At most one request per order is ever outstanding: chained modifies
    /// would need a per-order history of in-flight states, and the whole point
    /// of this design is not to keep one.
    [[nodiscard]] bool can_send_now(const slot_type& s) const noexcept {
        switch (s.state) {
            case order_state_t::live:
                break;
            case order_state_t::pending_new:
                if (cfg_.ack_required) {
                    return false;
                }
                break;
            case order_state_t::pending_modify:
                // Venues that chain replaces on the client order id accept a
                // further modify while the previous one is unanswered. Waiting
                // for the acknowledgement instead would put a round trip
                // between the strategy deciding and the venue hearing it.
                if (s.inflight_modifies >= cfg_.max_inflight_modifies) {
                    return false;
                }
                break;
            default:
                return false;
        }
        if constexpr (HasCanAct<Executor>) {
            return exec_->can_act(s.id);
        } else {
            return true;
        }
    }

    /// True when we can express an intent against this order at all, whether by
    /// sending now or by deferring onto its acknowledgement.
    [[nodiscard]] static bool can_act(const slot_type& s) noexcept {
        return s.state != order_state_t::pending_cancel;
    }

    [[nodiscard]] bool crossing_ok(price_t price) const noexcept {
        if (opposite_top_ == k_null_price) {
            return true;
        }
        if constexpr (Side == side_t::buy) {
            return price < opposite_top_;
        } else {
            return price > opposite_top_;
        }
    }

    bool issue_cancel(slot_index_t si) noexcept {
        slot_type& s = pool_[si];
        const auto prev_state = s.state;
        const auto prev_pending = s.pending;

        detach(s);
        s.pending = pending_action_t::none;
        s.state = order_state_t::pending_cancel;
        attach(s);

        if (!exec_->cancel(s.id)) [[unlikely]] {
            detach(s);
            s.state = prev_state;
            s.pending = prev_pending;
            attach(s);
            note_retry();
            return false;
        }
        clear_blocked(si);
        return true;
    }

    bool issue_modify(slot_index_t si, level_type& to, qty_t new_leaves) noexcept {
        slot_type& s = pool_[si];
        const price_t prev_price = s.price;
        const qty_t prev_qty = s.order_qty;
        const auto prev_state = s.state;
        const auto prev_pending = s.pending;
        const qty_t new_order_qty = new_leaves + s.filled + s.canceled;

        detach(s);
        s.price = to.price;
        s.order_qty = new_order_qty;
        s.pending = pending_action_t::none;
        s.state = order_state_t::pending_modify;
        attach(s);

        if (!exec_->modify(s.id, to.price, new_order_qty)) [[unlikely]] {
            detach(s);
            s.price = prev_price;
            s.order_qty = prev_qty;
            s.state = prev_state;
            s.pending = prev_pending;
            attach(s);
            note_retry();
            return false;
        }

        ++s.inflight_modifies;
        relink(to, si);
        if (prev_price != to.price || new_order_qty > prev_qty) {
            // Moving price or increasing size loses queue position, so the
            // stale estimate must not gate the next add at this level.
            s.clear_flag(slot_type::flag_qp_valid);
            s.qty_in_front = 0;
        }
        clear_blocked(si);
        return true;
    }

    /// Cancel now if the venue allows it, otherwise record the intent and send
    /// it from the acknowledgement. Either way the level's accounting drops the
    /// quantity immediately, so reconcile does not keep re-deciding.
    bool request_cancel(slot_index_t si) noexcept {
        slot_type& s = pool_[si];
        if (can_send_now(s)) {
            if (issue_cancel(si)) {
                return true;
            }
            mark_blocked(si);
            return false;
        }
        if (s.pending != pending_action_t::cancel) {
            detach(s);
            s.pending = pending_action_t::cancel;
            attach(s);
        }
        return true;
    }

    bool request_modify(slot_index_t si, level_type& to, qty_t new_leaves) noexcept {
        slot_type& s = pool_[si];
        if (can_send_now(s)) {
            if (issue_modify(si, to, new_leaves)) {
                return true;
            }
            if (new_leaves < s.desired()) {
                mark_blocked(si);
            }
            return false;
        }
        const qty_t new_order_qty = new_leaves + s.filled + s.canceled;
        detach(s);
        s.price = to.price;
        s.order_qty = new_order_qty;
        s.pending = pending_action_t::modify;
        attach(s);
        relink(to, si);
        return true;
    }

    /// Send whatever was deferred onto this order's acknowledgement.
    void drain_pending(slot_index_t si) noexcept {
        slot_type& s = pool_[si];
        switch (s.pending) {
            case pending_action_t::none:
                return;
            case pending_action_t::cancel:
                if (!issue_cancel(si)) {
                    mark_blocked(si);
                }
                return;
            case pending_action_t::modify: {
                level_type* to = level_for(s.price);
                if (to == nullptr) {
                    s.pending = pending_action_t::none;
                    return;
                }
                if (!can_send_now(s)) {
                    // Still no room in the chain. Leave the marker in place;
                    // the outstanding requests will bring us back here.
                    return;
                }
                if (!issue_modify(si, *to, s.order_qty - s.filled - s.canceled)) {
                    mark_blocked(si);
                }
                return;
            }
        }
    }

    bool send_new(level_type& l, qty_t leaves) noexcept {
        const slot_index_t si = pool_.acquire();
        if (si == k_null_slot) [[unlikely]] {
            note_retry();
            return false;
        }
        const order_id_t id = exec_->place(Side, l.price, leaves, cfg_.order_type);
        if (id == Executor::invalid_order_id) [[unlikely]] {
            pool_.release(si);
            note_retry();
            return false;
        }

        slot_type& s = pool_[si];
        s.id = id;
        s.price = l.price;
        s.acked_price = l.price;
        s.order_qty = leaves;
        s.acked_qty = 0;
        s.state = order_state_t::pending_new;
        s.order_type = cfg_.order_type;

        link_back(l, si);
        attach(s);
        bind_index(id, si);
        return true;
    }

    // -----------------------------------------------------------------------
    // Shape
    // -----------------------------------------------------------------------

    [[nodiscard]] qty_t shape_qty(std::uint16_t index) const noexcept {
        const qty_t q = (profile_len_ == cfg_.levels && profile_len_ != 0) ? profile_[index]
                                                                          : cfg_.stack_qty;
        return cfg_.max_level_qty == k_no_qty_limit ? q : std::min(q, cfg_.max_level_qty);
    }

    /// Target the shape asks for at `depth`, ignoring slack retention.
    [[nodiscard]] qty_t shape_target_at(std::int32_t depth) const noexcept {
        if (depth < top_depth_) {
            return 0;  // never leave anything in front of the quote
        }
        const std::int32_t off = depth - top_depth_;
        if (off == 0) {
            return std::min(quote_qty_, cfg_.max_level_qty);
        }
        const auto gap = static_cast<std::int32_t>(cfg_.level_gap_ticks);
        if (off % gap != 0) {
            return 0;
        }
        const std::int32_t index = off / gap;
        if (index > static_cast<std::int32_t>(cfg_.levels)) {
            return 0;
        }
        return shape_qty(static_cast<std::uint16_t>(index - 1));
    }

    /// Most a retained slack level may carry: the size of the ladder's own
    /// deepest rung.
    ///
    /// Without the cap, a level that used to be the quote would sit in slack
    /// still carrying quote size, several ticks behind the market and larger
    /// than any rung the configuration ever asks for. Trimming it costs the
    /// same one message that cancelling would have, and keeps the order and
    /// its place in the queue. A genuine tail level is already at this size,
    /// so for the case slack exists to serve the cap costs nothing.
    [[nodiscard]] qty_t slack_retain_qty() const noexcept {
        return cfg_.levels > 0 ? shape_qty(static_cast<std::uint16_t>(cfg_.levels - 1))
                               : cfg_.stack_qty;
    }

    /// True when `depth` sits in the slack region: past the bottom of the
    /// ladder but close enough that a level already working there is kept
    /// rather than cancelled, so a one-tick flicker does not churn the tail.
    [[nodiscard]] bool in_slack(std::int32_t depth) const noexcept {
        if (cfg_.slack_levels == 0 || depth <= top_depth_) {
            return false;
        }
        const auto gap = static_cast<std::int32_t>(cfg_.level_gap_ticks);
        const std::int32_t off = depth - top_depth_;
        if (off % gap != 0) {
            return false;
        }
        const std::int32_t index = off / gap;
        return index > static_cast<std::int32_t>(cfg_.levels) &&
               index <= static_cast<std::int32_t>(cfg_.levels) +
                            static_cast<std::int32_t>(cfg_.slack_levels);
    }

    void apply_shape() noexcept {
        target_consumed_ = false;
        if (pulled_ || quote_price_ == k_null_price) {
            zero_all_targets();
            return;
        }
        if (!prepare_window()) {
            return;
        }

        const auto span = static_cast<std::int32_t>(cfg_.levels) *
                          static_cast<std::int32_t>(cfg_.level_gap_ticks);
        std::int32_t lo = top_depth_;
        std::int32_t hi = top_depth_ + span;
        if (!ring_.band_empty()) {
            lo = std::min(lo, ring_.min_depth());
            hi = std::max(hi, ring_.max_depth());
        }

        for (std::int32_t d = lo; d <= hi; ++d) {
            const qty_t want = shape_target_at(d);
            level_type* l = (want != 0) ? ring_.try_at(d) : ring_.peek(d);
            if (l == nullptr) {
                continue;
            }
            if (want == 0 && in_slack(d) && l->working() > 0) {
                l->target = std::min(l->target, slack_retain_qty());
                continue;
            }
            l->target = want;
        }
    }

    void zero_all_targets() noexcept {
        if (!anchored_ || ring_.band_empty()) {
            return;
        }
        for (std::int32_t d = ring_.min_depth(); d <= ring_.max_depth(); ++d) {
            if (level_type* l = ring_.peek(d)) {
                l->target = 0;
            }
        }
    }

    /// Make sure the ring can address the quote and the whole ladder behind it,
    /// rebuilding the price grid if it cannot.
    bool prepare_window() noexcept {
        if (!anchored_) [[unlikely]] {
            ring_.reset(quote_price_, cfg_.tick_size);
            anchored_ = true;
            ++rebase_count_;
        }

        std::int32_t d = ring_.depth_of(quote_price_);
        const auto span = static_cast<std::int32_t>(cfg_.levels) *
                          static_cast<std::int32_t>(cfg_.level_gap_ticks);

        if (d == detail::k_invalid_depth || ring_.would_overflow(d) ||
            ring_.would_overflow(d + span)) [[unlikely]] {
            rebase(quote_price_);
            d = 0;
        }
        top_depth_ = d;
        return true;
    }

    /// Rebuild the price grid around `anchor_price`.
    ///
    /// Reached when the live price range outgrows the ring or the tick grid
    /// shifts underneath us. Everything working is cancelled and detached from
    /// level accounting; the orders stay tracked, orphaned, only so their
    /// terminal events can be matched and their slots reclaimed.
    SLICK_STACKER_COLD SLICK_STACKER_NEVER_INLINE void rebase(price_t anchor_price) noexcept {
        for (slot_index_t si = 0; si < Traits::max_orders; ++si) {
            slot_type& s = pool_[si];
            if (!s.active() || s.has_flag(slot_type::flag_orphaned)) {
                continue;
            }
            detach(s);
            unlink(si);
            clear_blocked(si);
            s.pending = pending_action_t::none;
            s.set_flag(slot_type::flag_orphaned);
            if (s.state != order_state_t::pending_cancel) {
                if (exec_->cancel(s.id)) {
                    s.state = order_state_t::pending_cancel;
                }
            }
        }
        ring_.reset(anchor_price, cfg_.tick_size);
        anchored_ = true;
        top_depth_ = 0;
        ++rebase_count_;
    }

    // -----------------------------------------------------------------------
    // Reconcile passes
    // -----------------------------------------------------------------------

    /// Note that the configured order type no longer matches what the working
    /// orders were sent as, so the next reconcile has to take them out.
    void arm_retype() noexcept { retype_pending_ = pool_.in_use() != 0; }

    /// Cancel every working order the venue is holding as the wrong kind.
    ///
    /// The level's accounting drops each one the moment the cancel is formed,
    /// exactly as it does for a cancel-and-replace anywhere else, so the add
    /// pass later in the same reconcile puts the quantity back as the new kind.
    /// Until those cancels are confirmed the venue is briefly holding both --
    /// the price of a kind that cannot be modified in place.
    ///
    /// The pass is driven off the slots rather than a list of what to do, so it
    /// is idempotent: it stays armed while any stale order is still standing,
    /// and a refused cancel is simply retried on the next pass.
    SLICK_STACKER_COLD SLICK_STACKER_NEVER_INLINE void retype_pass() noexcept {
        bool remaining = false;
        for (slot_index_t si = 0; si < Traits::max_orders; ++si) {
            slot_type& s = pool_[si];
            if (!s.active() || s.has_flag(slot_type::flag_orphaned) ||
                s.order_type == cfg_.order_type) {
                continue;
            }
            // Nothing of it is still wanted -- the cancel has gone out, or is
            // booked against the acknowledgement. Either way it is on its way
            // off the level and needs no second request.
            if (s.desired() <= 0) {
                continue;
            }
            if (!request_cancel(si)) {
                remaining = true;
            }
        }
        retype_pending_ = remaining;
    }

    [[nodiscard]] bool wants_more(const level_type& l) const noexcept {
        const qty_t d = l.delta();
        if (d <= 0) {
            return false;
        }
        // Hysteresis only suppresses topping up a level that is already
        // working; a level with nothing on it is always established.
        return l.working() <= 0 || d > cfg_.qty_hysteresis;
    }

    /// Move whole orders out of levels that are over target and into levels
    /// that are under it, rather than cancelling and re-sending. One message
    /// instead of two, and one order slot instead of two.
    ///
    /// The two cursors walk inward from opposite edges of the band and are
    /// allowed to cross: a quote that improved leaves the surplus deep and the
    /// deficit shallow, and a quote that backed away leaves it the other way
    /// round. Whichever cursor cannot make progress advances, so the pass is
    /// linear in the width of the band no matter which case it is in.
    ///
    /// The same holds within a source level: `cursor` walks its queue back to
    /// front once per pass rather than restarting at the tail after every move.
    void reprice_pass() noexcept {
        if (ring_.band_empty()) {
            return;
        }
        const std::int32_t lo = ring_.min_depth();
        const std::int32_t hi = ring_.max_depth();
        std::int32_t to_d = lo;
        std::int32_t from_d = hi;
        std::int32_t cursor_d = hi + 1;  // the level `cursor` walks; none yet
        slot_index_t cursor = k_null_slot;
        // What `to_d` can take, worked out once per destination and again only
        // after a move changes it -- not on every step of the source cursor.
        std::int32_t take_d = lo - 1;
        level_type* to = nullptr;
        qty_t take = 0;

        while (to_d <= hi && from_d >= lo) {
            if (take_d != to_d) {
                take_d = to_d;
                to = ring_.peek(to_d);
                take = (to != nullptr && wants_more(*to)) ? reprice_take(*to) : 0;
            }
            if (take == 0) {
                ++to_d;
                continue;
            }
            level_type* from = ring_.peek(from_d);
            if (from == nullptr || from == to || from->delta() >= 0 || from->order_count == 0) {
                --from_d;
                continue;
            }
            if (cursor_d != from_d) {
                cursor_d = from_d;
                cursor = from->tail;
            }
            if (move_one(*from, *to, take, cursor)) {
                take = wants_more(*to) ? reprice_take(*to) : 0;
            } else {
                --from_d;
            }
        }
    }

    /// Size of the order a reprice into `to` would carry, or zero when `to`
    /// cannot take one at all -- which is the destination cursor's problem, so
    /// the sources are not given up on because of it.
    ///
    /// The order is resized to what the destination wants, growing as readily
    /// as shrinking. A reprice sends the order to the back of the destination's
    /// queue whatever its size, so carrying extra quantity across in the same
    /// message is free -- and it saves the separate new order that topping the
    /// level up afterwards would need.
    [[nodiscard]] qty_t reprice_take(const level_type& to) const noexcept {
        if (!crossing_ok(to.price) || to.order_count >= cfg_.max_orders_per_level) {
            return 0;
        }
        const qty_t need = to.delta();
        const qty_t room = level_room(to);
        if (need <= 0 || room <= 0) {
            return 0;
        }
        const qty_t take = std::min({need, room, cfg_.max_order_qty});
        return take < cfg_.min_order_qty ? 0 : take;
    }

    /// Reprice one order of `from` into `to`, resized to `take`, resuming the
    /// walk of `from`'s queue at `cursor`. Returns false once nothing left in
    /// `from` can move.
    ///
    /// Take from the back of the queue: those are the orders we would give up
    /// first anyway, so repricing them costs the least queue position. An order
    /// carrying more than `from` can spare is left alone -- moving it would take
    /// the source level below its own target.
    ///
    /// Whatever the cursor steps past stays unmovable for the rest of the pass:
    /// the surplus only shrinks as orders leave, an order being cancelled stays
    /// so, and one the executor just refused would only be refused again.
    ///
    /// `request_modify` rather than `issue_modify`, so an order the venue has
    /// not acknowledged yet is still worth moving: booking the reprice now and
    /// sending it from the acknowledgement costs one message, where giving up
    /// and starting again costs a cancel and a new order.
    bool move_one(level_type& from, level_type& to, qty_t take, slot_index_t& cursor) noexcept {
        const qty_t surplus = -from.delta();
        while (cursor != k_null_slot) {
            const slot_index_t si = cursor;
            slot_type& s = pool_[si];
            cursor = s.prev;  // read before a move relinks the order
            const qty_t have = s.desired();
            if (have > 0 && have <= surplus && can_act(s) && request_modify(si, to, take)) {
                return true;
            }
        }
        return false;
    }

    void reduce_pass() noexcept {
        if (ring_.band_empty()) {
            return;
        }
        for (std::int32_t d = ring_.min_depth(); d <= ring_.max_depth(); ++d) {
            level_type* l = ring_.peek(d);
            if (l == nullptr) {
                continue;
            }
            const qty_t excess = -l->delta();
            if (excess > 0 && l->order_count != 0) {
                reduce_level(*l, excess);
            }
        }
    }

    /// Shed `excess` from `l`, back of the queue first. An order that straddles
    /// the boundary is shrunk with a modify rather than cancelled, which keeps
    /// its place in the queue.
    void reduce_level(level_type& l, qty_t excess) noexcept {
        for (slot_index_t si = l.tail; si != k_null_slot && excess > 0;) {
            slot_type& s = pool_[si];
            const slot_index_t prev = s.prev;
            const qty_t have = s.desired();
            if (have <= 0 || !can_act(s)) {
                si = prev;
                continue;
            }
            if (have <= excess) {
                if (request_cancel(si)) {
                    excess -= have;
                }
            } else {
                const qty_t keep = have - excess;
                if (keep >= cfg_.min_order_qty) {
                    if (request_modify(si, l, keep)) {
                        excess = 0;
                    }
                } else if (request_cancel(si)) {
                    // The remainder would be below the minimum order size, so
                    // there is no order that can hold it: take the whole thing
                    // out and let the add pass re-establish the level if the
                    // target still justifies one.
                    excess -= have;
                }
            }
            si = prev;
        }
    }

    void add_pass() noexcept {
        if (ring_.band_empty()) {
            return;
        }
        if (blocked_count_ != 0) [[unlikely]] {
            // We are already carrying more than we want somewhere and could not
            // shed it. Growing anywhere else would compound the exposure.
            return;
        }
        for (std::int32_t d = ring_.min_depth(); d <= ring_.max_depth(); ++d) {
            level_type* l = ring_.peek(d);
            if (l == nullptr || !wants_more(*l)) {
                continue;
            }
            add_to_level(*l);
        }
    }

    /// Headroom left at a level before `max_level_qty` bites.
    [[nodiscard]] qty_t level_room(const level_type& l) const noexcept {
        if (cfg_.max_level_qty == k_no_qty_limit) {
            return k_no_qty_limit;
        }
        return cfg_.max_level_qty - l.working();
    }

    void add_to_level(level_type& l) noexcept {
        // Cheapest quantity available is quantity we already have here but had
        // decided to cancel. Reinstating a deferred cancel costs no message at
        // all, so it is always tried before sending anything.
        reclaim_deferred_cancels(l);

        qty_t need = std::min(l.delta(), level_room(l));
        if (need <= 0 || !crossing_ok(l.price)) {
            return;
        }

        while (need > 0 && l.order_count < cfg_.max_orders_per_level) {
            if (!queue_gap_ok(l)) {
                return;
            }
            qty_t slice = std::min(need, cfg_.max_order_qty);
            slice = round_down(slice, cfg_.qty_increment);
            if (slice <= 0 || slice < cfg_.min_order_qty) {
                return;
            }
            if (!send_new(l, slice)) {
                return;
            }
            need -= slice;
        }
    }

    /// Un-cancel orders at `l` whose removal has not gone out yet, up to what
    /// the level now wants back.
    void reclaim_deferred_cancels(level_type& l) noexcept {
        qty_t need = l.delta();
        if (need <= 0) {
            return;
        }
        for (slot_index_t si = l.head; si != k_null_slot && need > 0;) {
            slot_type& s = pool_[si];
            const slot_index_t next = s.next;
            // An order held back for a retype is not free quantity: it is the
            // wrong kind, and reinstating it would undo the very cancel the
            // retype pass just formed.
            if (s.pending == pending_action_t::cancel && s.order_type == cfg_.order_type) {
                const qty_t back = s.order_qty - s.filled - s.canceled;
                if (back > 0 && back <= need) {
                    detach(s);
                    s.pending = pending_action_t::none;
                    attach(s);
                    need -= back;
                }
            }
            si = next;
        }
    }

    /// Do not stack another order behind our own unless enough market quantity
    /// has queued up behind the last one. Without this a level fills with our
    /// own orders sitting back to back, all of them behind the same queue.
    [[nodiscard]] bool queue_gap_ok(const level_type& l) const noexcept {
        if (cfg_.queue_gap <= 0 || l.order_count == 0 || l.depth == top_depth_) {
            return true;
        }
        return gap_met(pool_[l.tail], l.market_qty);
    }

    /// True when `market_qty` in the book leaves at least `queue_gap` behind
    /// an order with `in_front` ahead of it.
    [[nodiscard]] bool gap_behind(qty_t market_qty, qty_t in_front) const noexcept {
        return market_qty - std::max<qty_t>(in_front, 0) >= cfg_.queue_gap;
    }

    /// True when the gate lets another order in behind `last`, a level's last
    /// order, with `market_qty` in the book at that price.
    [[nodiscard]] bool gap_met(const slot_type& last, qty_t market_qty) const noexcept {
        return last.state == order_state_t::live && last.has_flag(slot_type::flag_qp_valid) &&
               gap_behind(market_qty, last.qty_in_front);
    }

    /// True when `l` wants to grow and the queue-gap gate decides whether it
    /// may, whatever state the gate is in.
    ///
    /// The feeds mark the stacker dirty only when an update takes the gate
    /// from shut to open at such a level. One that leaves it shut -- the
    /// common case at a busy level -- changes nothing the next reconcile
    /// would do, and nor does one at a level whose gate was already open,
    /// since whatever is holding that level back is not the queue.
    [[nodiscard]] bool subject_to_queue_gap(const level_type& l) const noexcept {
        return cfg_.queue_gap > 0 && l.order_count != 0 &&
               l.order_count < cfg_.max_orders_per_level && l.depth != top_depth_ &&
               wants_more(l);
    }

    [[nodiscard]] static constexpr qty_t round_down(qty_t v, qty_t increment) noexcept {
        return increment <= 1 ? v : (v / increment) * increment;
    }

    // -----------------------------------------------------------------------
    // Event helpers
    // -----------------------------------------------------------------------

    void on_confirmed(const order_id_t& id, price_t price, qty_t qty, bool is_replace) noexcept {
        const slot_index_t si = lookup(id);
        if (si == k_null_slot) {
            return;
        }
        slot_type& s = pool_[si];

        detach(s);
        s.acked_price = price;
        s.acked_qty = qty;
        if (is_replace && s.inflight_modifies > 0) {
            --s.inflight_modifies;
        }
        // With a chain outstanding this is only an intermediate confirmation --
        // the venue still owes us answers, so the order is not settled yet.
        if (s.inflight_modifies == 0 && (s.state == order_state_t::pending_new ||
                                         s.state == order_state_t::pending_modify)) {
            s.state = order_state_t::live;
        }
        attach(s);

        if (s.has_flag(slot_type::flag_orphaned)) {
            // Confirmation for an order the grid rebuild left behind: it is
            // already cancelled or about to be, and belongs to no level.
            dirty_ = true;
            return;
        }

        if (is_done(s)) {
            retire(si);
            dirty_ = true;
            return;
        }

        drain_pending(si);
        dirty_ = true;
    }

    void rollback_to_acked(slot_index_t si) noexcept {
        slot_type& s = pool_[si];

        if (s.has_flag(slot_type::flag_orphaned)) {
            // The grid was rebuilt underneath this order. It belongs to no
            // level and must not be booked into one -- even when its old price
            // happens to still exist on the new grid. It is only still tracked
            // so its terminal event can be matched.
            s.state = order_state_t::pending_cancel;
            return;
        }

        level_type* home = level_for(s.acked_price);

        detach(s);
        s.state = order_state_t::live;
        s.price = s.acked_price;
        s.order_qty = s.acked_qty;
        attach(s);

        if (home != nullptr) {
            relink(*home, si);
        } else if (!s.has_flag(slot_type::flag_orphaned)) {
            // The level it rests on is no longer addressable. Nothing sensible
            // is left to do with the order except take it out.
            unlink(si);
            s.set_flag(slot_type::flag_orphaned);
            detach(s);
            if (exec_->cancel(s.id)) {
                s.state = order_state_t::pending_cancel;
            }
        }
    }

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------

    Executor* exec_;
    stacker_config cfg_{};
    qty_t profile_[Traits::max_levels]{};
    std::uint16_t profile_len_ = 0;

    ring_type ring_{};
    pool_type pool_{};
    [[no_unique_address]] index_type index_{};

    price_t quote_price_ = k_null_price;
    qty_t quote_qty_ = 0;
    price_t opposite_top_ = k_null_price;
    std::int32_t top_depth_ = 0;

    std::uint16_t blocked_count_ = 0;
    std::uint32_t rebase_count_ = 0;

    bool anchored_ = false;
    bool dirty_ = false;
    bool shape_dirty_ = false;
    bool pulled_ = false;
    bool needs_retry_ = false;
    bool target_consumed_ = false;
    bool retype_pending_ = false;
};

// ---------------------------------------------------------------------------

template <OrderExecutor Executor, side_t Side, class Traits>
bool stacker<Executor, Side, Traits>::validate() const noexcept {
    if (!anchored_) {
        return pool_.in_use() == 0;
    }

    struct accumulator {
        std::int32_t depth = 0;
        qty_t acked = 0;
        qty_t inflight = 0;
        bool used = false;
    };
    accumulator acc[Traits::level_capacity]{};
    constexpr std::uint32_t mask = Traits::level_capacity - 1;

    auto bucket = [&](price_t price, std::int32_t& out_depth) -> accumulator* {
        const std::int32_t d = ring_.depth_of(price);
        if (d == detail::k_invalid_depth) {
            return nullptr;
        }
        out_depth = d;
        accumulator& a = acc[static_cast<std::uint32_t>(d) & mask];
        if (a.used && a.depth != d) {
            return nullptr;  // two live depths aliased onto one ring slot
        }
        a.used = true;
        a.depth = d;
        return &a;
    };

    // Rebuild every level's counters from the orders that feed them.
    for (slot_index_t si = 0; si < Traits::max_orders; ++si) {
        const slot_type& s = pool_[si];
        if (!s.active() || s.has_flag(slot_type::flag_orphaned)) {
            continue;
        }
        const auto c = detail::contribution_of(s);
        std::int32_t d = 0;
        if (c.acked_delta != 0) {
            accumulator* a = bucket(c.acked_price, d);
            if (a == nullptr) {
                return false;
            }
            a->acked += c.acked_delta;
        }
        if (c.out_delta != 0) {
            accumulator* a = bucket(c.out_price, d);
            if (a == nullptr) {
                return false;
            }
            a->inflight += c.out_delta;
        }
        if (c.in_delta != 0) {
            accumulator* a = bucket(c.in_price, d);
            if (a == nullptr) {
                return false;
            }
            a->inflight += c.in_delta;
        }
    }

    // Compare against what the incremental path has been maintaining, and check
    // the intrusive lists while we are here.
    std::uint16_t linked_total = 0;
    if (!ring_.band_empty()) {
        for (std::int32_t d = ring_.min_depth(); d <= ring_.max_depth(); ++d) {
            const level_type* l = ring_.peek(d);
            if (l == nullptr) {
                continue;
            }
            const accumulator& a = acc[static_cast<std::uint32_t>(d) & mask];
            const qty_t expect_acked = (a.used && a.depth == d) ? a.acked : 0;
            const qty_t expect_inflight = (a.used && a.depth == d) ? a.inflight : 0;
            if (l->acked != expect_acked || l->inflight != expect_inflight) {
                return false;
            }
            if (l->price != ring_.price_at(d)) {
                return false;
            }

            std::uint16_t n = 0;
            slot_index_t prev = k_null_slot;
            for (slot_index_t si = l->head; si != k_null_slot;) {
                if (si >= Traits::max_orders) {
                    return false;
                }
                const slot_type& s = pool_[si];
                if (!s.active() || !s.has_flag(slot_type::flag_linked) || s.linked_depth != d) {
                    return false;
                }
                if (s.prev != prev) {
                    return false;
                }
                if (++n > Traits::max_orders) {
                    return false;  // cycle
                }
                prev = si;
                si = s.next;
            }
            if (n != l->order_count || prev != l->tail) {
                return false;
            }
            linked_total = static_cast<std::uint16_t>(linked_total + n);
        }
    }

    // Every non-orphaned order must be reachable from exactly one level.
    std::uint16_t expect_linked = 0;
    std::uint16_t blocked = 0;
    for (slot_index_t si = 0; si < Traits::max_orders; ++si) {
        const slot_type& s = pool_[si];
        if (!s.active()) {
            continue;
        }
        if (s.has_flag(slot_type::flag_blocked_decrement)) {
            ++blocked;
        }
        if (s.inflight_modifies > cfg_.max_inflight_modifies) {
            return false;
        }
        // An order with modifies outstanding cannot have been settled, so it
        // must still be in one of the states that says the venue owes us an
        // answer.
        if (s.inflight_modifies > 0 && s.state != order_state_t::pending_modify &&
            s.state != order_state_t::pending_cancel) {
            return false;
        }
        if (s.has_flag(slot_type::flag_orphaned)) {
            if (s.has_flag(slot_type::flag_linked)) {
                return false;
            }
            continue;
        }
        if (!s.has_flag(slot_type::flag_linked)) {
            return false;
        }
        if (s.filled + s.canceled > std::max(s.order_qty, s.acked_qty)) {
            return false;
        }
        ++expect_linked;
    }
    return linked_total == expect_linked && blocked == blocked_count_;
}

SLICK_STACKER_NAMESPACE_END
