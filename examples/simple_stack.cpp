// Copyright 2026 Slick Quant
// SPDX-License-Identifier: MIT
//
// A complete, runnable stacker: an executor that prints what it is asked to do,
// a stack built behind a quote, a price move, a fill, and the recovery.
//
// Everything a real integration needs is here -- the only thing missing is a
// gateway behind `my_executor`.

#include <slick/stacker/stacker.hpp>

#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <vector>

using namespace slick::stacker;

namespace {

/// Stands in for an order gateway. Records what it was told so the example can
/// play the venue's responses back; a real one would write to a session.
class my_executor {
public:
    using order_id_t = std::uint64_t;
    static constexpr order_id_t invalid_order_id = 0;

    struct pending {
        char kind;  // 'P', 'M' or 'C'
        order_id_t id;
        price_t price;
        qty_t qty;
    };

    order_id_t place(side_t s, price_t price, qty_t qty, order_type_t type) {
        const order_id_t id = next_id_++;
        std::printf("  -> PLACE  %-4s %6lld x %-4lld %-5s (id %llu)\n", to_string(s),
                    static_cast<long long>(price), static_cast<long long>(qty), to_string(type),
                    static_cast<unsigned long long>(id));
        queue_.push_back({'P', id, price, qty});
        return id;
    }

    bool modify(const order_id_t& id, price_t price, qty_t qty) {
        std::printf("  -> MODIFY id %-3llu   %6lld x %-4lld\n", static_cast<unsigned long long>(id),
                    static_cast<long long>(price), static_cast<long long>(qty));
        queue_.push_back({'M', id, price, qty});
        return true;
    }

    bool cancel(const order_id_t& id) {
        std::printf("  -> CANCEL id %llu\n", static_cast<unsigned long long>(id));
        queue_.push_back({'C', id, 0, 0});
        return true;
    }

    /// Play every queued request back as an acknowledgement.
    template <class Stacker>
    void acknowledge(Stacker& st) {
        for (std::size_t i = 0; i < queue_.size(); ++i) {
            const pending p = queue_[i];  // by value: draining may enqueue more
            switch (p.kind) {
                case 'P':
                    live_[p.id] = {p.price, p.qty};
                    st.on_accepted(p.id, p.price, p.qty);
                    break;
                case 'M':
                    live_[p.id] = {p.price, p.qty};
                    st.on_replaced(p.id, p.price, p.qty);
                    break;
                default: {
                    const qty_t left = resting(p.id);
                    live_.erase(p.id);
                    st.on_canceled(p.id, left);
                    break;
                }
            }
        }
        queue_.clear();
    }

    /// Identifier of the most recent order sent to `price`. A real integration
    /// takes this from the execution report instead.
    [[nodiscard]] order_id_t id_at(price_t price) const {
        order_id_t found = invalid_order_id;
        for (const auto& [id, o] : live_) {
            if (o.price == price && (found == invalid_order_id || id > found)) {
                found = id;
            }
        }
        return found;
    }

    [[nodiscard]] qty_t resting(order_id_t id) const {
        auto it = live_.find(id);
        return it == live_.end() ? 0 : it->second.qty;
    }

private:
    struct venue_order {
        price_t price;
        qty_t qty;
    };

    std::vector<pending> queue_;
    std::unordered_map<order_id_t, venue_order> live_;
    order_id_t next_id_ = 1;
};

using bid_stacker = stacker<my_executor, side_t::buy>;

void show(const char* what, const bid_stacker& st, price_t from, price_t to, price_t tick) {
    std::printf("%s\n", what);
    for (price_t px = from; px >= to; px -= tick) {
        const qty_t w = st.working_at(px);
        if (w != 0) {
            std::printf("     %6lld  %lld\n", static_cast<long long>(px),
                        static_cast<long long>(w));
        }
    }
}

/// Reconcile and acknowledge until nothing further is outstanding. A live
/// strategy does this implicitly by reconciling once per event batch.
void settle(bid_stacker& st, my_executor& exec) {
    for (int i = 0; i < 8 && (st.dirty() || i == 0); ++i) {
        st.reconcile();
        exec.acknowledge(st);
    }
}

}  // namespace

int main() {
    stacker_config cfg;
    cfg.tick_size = 25;
    cfg.levels = 3;
    cfg.stack_qty = 10;
    cfg.ack_required = true;   // this venue wants an acknowledgement first
    cfg.prefer_modify = true;  // reprice rather than cancel and replace

    if (const auto err = cfg.validate(); err != config_error::ok) {
        std::printf("bad configuration: %s\n", to_string(err));
        return 1;
    }

    my_executor exec;
    bid_stacker bid{exec, cfg};

    std::printf("\n== quote 25 at 500000, three rungs of 10 behind it ==\n");
    bid.quote(500'000, 25);
    settle(bid, exec);
    show("   resting:", bid, 500'000, 499'900, 25);

    std::printf("\n== the market ticks up; the quote follows ==\n");
    bid.quote(500'025, 25);
    settle(bid, exec);
    show("   resting:", bid, 500'025, 499'925, 25);

    std::printf("\n== 10 of an order at the top level trades ==\n");
    const auto hit = exec.id_at(500'025);
    bid.on_filled(hit, 10, 500'025);
    settle(bid, exec);
    show("   resting:", bid, 500'025, 499'925, 25);
    std::printf(
        "   (the fill consumed the target: the stack does not re-arm by\n"
        "    itself, it waits to be told)\n");

    std::printf("\n== ask for the size back ==\n");
    bid.quote(500'025, 25);
    settle(bid, exec);
    show("   resting:", bid, 500'025, 499'925, 25);

    std::printf("\n== pull everything ==\n");
    bid.pull();
    settle(bid, exec);
    std::printf("   %u orders left\n\n", bid.live_order_count());

    return bid.validate() ? 0 : 1;
}
