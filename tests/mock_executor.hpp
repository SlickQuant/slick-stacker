// Copyright 2026 Slick Quant
// SPDX-License-Identifier: MIT

#pragma once

#include <slick/stacker/stacker.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace testing_support {

using slick::stacker::order_type_t;
using slick::stacker::price_t;
using slick::stacker::qty_t;
using slick::stacker::side_t;

/// Records every message the stacker sends and keeps a venue-side view of what
/// is working, so tests can assert on both the resulting book *and* what it
/// cost in messages to get there. For a stacker the second half matters as much
/// as the first: the same book reached with twice the traffic is a regression.
class mock_executor {
public:
    using order_id_t = std::uint64_t;
    static constexpr order_id_t invalid_order_id = 0;

    enum class kind : std::uint8_t { place, modify, cancel };

    struct message {
        kind type;
        order_id_t id;
        side_t side;
        price_t price;
        qty_t qty;
        order_type_t order_type = order_type_t::limit;  ///< only set on `place`
    };

    struct venue_order {
        order_id_t id = 0;
        side_t side = side_t::buy;
        price_t price = 0;
        qty_t qty = 0;      ///< total order quantity as last acknowledged/requested
        qty_t filled = 0;
        bool live = true;
        order_type_t order_type = order_type_t::limit;
    };

    // -- executor interface --------------------------------------------------

    order_id_t place(side_t s, price_t px, qty_t q, order_type_t type) {
        if (fail_place) {
            ++refused;
            return invalid_order_id;
        }
        const order_id_t id = next_id_++;
        log.push_back({kind::place, id, s, px, q, type});
        orders[id] = venue_order{id, s, px, q, 0, true, type};
        return id;
    }

    bool modify(const order_id_t& id, price_t px, qty_t q) {
        if (fail_modify || id == refuse_modify_id) {
            ++refused;
            return false;
        }
        log.push_back({kind::modify, id, side_of(id), px, q});
        pending_modify_[id] = {px, q};
        return true;
    }

    bool cancel(const order_id_t& id) {
        if (fail_cancel) {
            ++refused;
            return false;
        }
        log.push_back({kind::cancel, id, side_of(id), price_of(id), 0});
        return true;
    }

    void flush() { ++flushes; }

    // -- test-side helpers ---------------------------------------------------

    /// Apply a modify the venue has accepted, so `orders` reflects it.
    void confirm_modify(order_id_t id) {
        auto it = pending_modify_.find(id);
        if (it == pending_modify_.end()) {
            return;
        }
        auto& o = orders[id];
        o.price = it->second.first;
        o.qty = it->second.second;
        pending_modify_.erase(it);
    }

    [[nodiscard]] bool has_pending_modify(order_id_t id) const {
        return pending_modify_.count(id) != 0;
    }

    [[nodiscard]] std::pair<price_t, qty_t> pending_modify(order_id_t id) const {
        auto it = pending_modify_.find(id);
        return it == pending_modify_.end() ? std::pair<price_t, qty_t>{0, 0} : it->second;
    }

    void drop_pending_modify(order_id_t id) { pending_modify_.erase(id); }

    [[nodiscard]] side_t side_of(order_id_t id) const {
        auto it = orders.find(id);
        return it == orders.end() ? side_t::buy : it->second.side;
    }

    [[nodiscard]] price_t price_of(order_id_t id) const {
        auto it = orders.find(id);
        return it == orders.end() ? 0 : it->second.price;
    }

    /// Index of the next message a test has not looked at yet.
    [[nodiscard]] std::size_t mark() const { return log.size(); }

    [[nodiscard]] std::size_t count(kind k, std::size_t from = 0) const {
        std::size_t n = 0;
        for (std::size_t i = from; i < log.size(); ++i) {
            if (log[i].type == k) {
                ++n;
            }
        }
        return n;
    }

    [[nodiscard]] std::size_t total(std::size_t from = 0) const { return log.size() - from; }

    /// Total quantity the venue believes is working at `px`, from its own
    /// records rather than the stacker's.
    [[nodiscard]] qty_t venue_qty_at(price_t px) const {
        qty_t total = 0;
        for (const auto& [id, o] : orders) {
            if (o.live && o.price == px) {
                total += o.qty - o.filled;
            }
        }
        return total;
    }

    [[nodiscard]] std::size_t live_orders() const {
        std::size_t n = 0;
        for (const auto& [id, o] : orders) {
            if (o.live) {
                ++n;
            }
        }
        return n;
    }

    std::vector<message> log;
    std::unordered_map<order_id_t, venue_order> orders;
    bool fail_place = false;
    bool fail_modify = false;
    bool fail_cancel = false;
    order_id_t refuse_modify_id = invalid_order_id;  ///< refuse modifies of this order only
    std::size_t refused = 0;
    std::size_t flushes = 0;

private:
    order_id_t next_id_ = 1;
    std::unordered_map<order_id_t, std::pair<price_t, qty_t>> pending_modify_;
};

/// Same behaviour, plus the optional user-data slot. Selecting this executor
/// switches the stacker from a hash lookup to a direct index on every event,
/// and the tests run the whole tracking suite against both to prove the two
/// paths agree.
class mock_executor_ud : public mock_executor {
public:
    void set_order_user_data(const order_id_t& id, std::uint32_t v) { user_data_[id] = v; }

    [[nodiscard]] std::uint32_t get_order_user_data(const order_id_t& id) const {
        auto it = user_data_.find(id);
        return it == user_data_.end() ? 0xFFFFFFFFu : it->second;
    }

private:
    std::unordered_map<order_id_t, std::uint32_t> user_data_;
};

}  // namespace testing_support
