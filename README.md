# slick-stacker

A standalone, header-only order stacker for low-latency trading.

Give it one price and one quantity. It places that quote, builds a configured
ladder of orders at the price levels behind it, and then keeps the whole
structure in agreement with the venue as acknowledgements, fills, cancels and
rejects come back.

It depends on nothing but the standard library. Order entry is a template
parameter, so the stacker drops into any strategy framework — or none.

```cpp
#include <slick/stacker/stacker.hpp>

using namespace slick::stacker;

struct my_executor {
    using order_id_t = std::uint64_t;
    static constexpr order_id_t invalid_order_id = 0;

    order_id_t place(side_t side, price_t price, qty_t qty, order_type_t type);
    bool       modify(const order_id_t& id, price_t price, qty_t qty);
    bool       cancel(const order_id_t& id);
};

stacker_config cfg;
cfg.tick_size = 25;
cfg.levels    = 3;      // three rungs behind the quote
cfg.stack_qty = 10;     // ten on each of them

my_executor exec;
stacker<my_executor, side_t::buy> bid{exec, cfg};

bid.quote(500'000, 25);
bid.reconcile();
```

That asks for:

| price     | quantity |
| --------- | -------- |
| 500'000   | 25       |
| 499'975   | 10       |
| 499'950   | 10       |
| 499'925   | 10       |

and sends exactly the four orders needed to establish it.

## Requirements

C++20, header-only, no dependencies. Tested on GCC, Clang and MSVC.

```cmake
find_package(slick-stacker CONFIG REQUIRED)
target_link_libraries(my_strategy PRIVATE slick::stacker)
```

or fetched at configure time:

```cmake
include(FetchContent)
FetchContent_Declare(slick-stacker
    GIT_REPOSITORY https://github.com/SlickQuant/slick-stacker.git
    GIT_TAG        v0.1.1
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(slick-stacker)

target_link_libraries(my_strategy PRIVATE slick::stacker)
```

or vendored:

```cmake
add_subdirectory(external/slick-stacker)
target_link_libraries(my_strategy PRIVATE slick::stacker)
```

Tests build only when slick-stacker is the top-level project, and benchmarks and
examples are off unless asked for, so consuming it any of these three ways costs
nothing but the headers.

## The shape

`quote(price, qty)` sets the top of the stack. Everything behind it comes from
the configuration:

```cpp
cfg.levels          = 4;    // rungs behind the quote
cfg.level_gap_ticks = 2;    // every other tick
cfg.stack_qty       = 10;   // uniform rung size

static constexpr std::array<qty_t, 4> profile{30, 20, 20, 10};
cfg.qty_profile = profile;  // ...or a size per rung, nearest first
```

The ladder always grows away from the market: down in price for a buy stack, up
for a sell stack.

Two ways to step back:

- `quote(price, 0)` pulls the top level and **leaves the ladder working**. Use
  it to come off the touch without giving up depth.
- `pull()` zeroes everything.

## Events, and why they never send

Every `on_*` handler updates state and marks the stacker dirty. None of them
sends a message. The work happens in `reconcile()`, which you call once after
draining a batch of events:

```cpp
for (const auto& ev : incoming) {
    switch (ev.type) {
        case accepted: bid.on_accepted(ev.id, ev.price, ev.qty);   break;
        case filled:   bid.on_filled(ev.id, ev.qty, ev.price);     break;
        case canceled: bid.on_canceled(ev.id, ev.qty);             break;
        case rejected: bid.on_rejected(ev.id, ev.reason);          break;
        // ...
    }
}
bid.reconcile();   // one pass for the whole batch
```

A burst of twenty fills costs one recalculation rather than twenty, and no
intermediate state ever reaches the wire. Re-asserting a quote that has not
changed costs a few nanoseconds and sends nothing at all.

## Configuration

| Field | Meaning |
| --- | --- |
| `tick_size` | Minimum price increment. All ladder prices are multiples of this. |
| `levels` | Rungs behind the quote. The quote itself is not counted. |
| `level_gap_ticks` | Tick spacing between rungs. `1` is a contiguous ladder. `(levels + slack_levels) * level_gap_ticks` must stay under `Traits::level_capacity`. |
| `stack_qty` | Uniform rung size, used when `qty_profile` is empty. |
| `qty_profile` | Per-rung sizes, nearest rung first. Copied, not retained. |
| `max_level_qty` | Cap on total resting quantity at any one price. |
| `max_order_qty` | Largest single order. A rung needing more is split. |
| `min_order_qty` | Smallest single order. Residuals below this go unquoted. |
| `qty_increment` | Order quantities round down to a multiple of this. |
| `max_orders_per_level` | Cap on concurrent orders at one price. |
| `slack_levels` | Rungs past the bottom that are kept rather than cancelled. |
| `qty_hysteresis` | Do not top a working level up for less than this. |
| `queue_gap` | Market quantity required behind our last order before adding another at that price. |
| `order_type` | `limit` (default) or `gtc`. Passed to `place`. |
| `ack_required` | The venue will not act on an order it has not acknowledged. |
| `max_inflight_modifies` | How many modifies may be outstanding against one order. 1 waits for each to be answered. |
| `prefer_modify` | Reprice surplus orders instead of cancel-and-replace. |
| `refill_on_fill` | Whether a fill re-arms the level automatically. |

`cfg.validate<Traits>()` returns a `config_error` describing the first problem,
or `config_error::ok`. The stacker does not require a validated config; it
clamps `levels` to `Traits::max_levels` and to what fits in the price ring at
the configured gap and slack (`cfg.max_fitting_levels<Traits>()`), which
`validate` reports as `ladder_exceeds_capacity`.

### Things worth knowing

**`order_type`** — `limit` by default. Both kinds carry a price, so both are
limit orders in the FIX sense; what differs is how long the venue keeps them,
and that is the venue's business rather than the stacker's.

There is deliberately no immediate-or-cancel option. A level of a ladder is a
resting order by definition, and an order the venue kills on arrival cannot hold
one — the level would look short again straight away and the next reconcile
would send another, forever. Send those directly rather than through a stacker.

Changing it at runtime — `set_order_type(t)`, or a `configure()` carrying a
different one — cancels every working order and replaces it with the new kind on
the next `reconcile()`. A modify carries price and quantity only, so there is no
amending an order into a different kind; two messages per working order is what
the change costs. Between the cancel and its confirmation the venue is briefly
holding both, exactly as it is for any other cancel-and-replace. Set it once at
startup unless you mean to pay that.

**`ack_required`** — set it true unless you know your venue accepts a modify or
cancel against an order it has not yet acknowledged. When true, the stacker
records the action against the order and sends it from the acknowledgement. The
level's accounting drops the quantity immediately either way, so nothing
double-counts in the meantime.

**`max_inflight_modifies`** — 1 by default, meaning a change waits for the
modify already in flight to be answered. That is a round trip between the
strategy deciding and the venue hearing about it. Venues that chain replaces on
the client order id (CME among them) accept a modify against an order whose
previous one is unanswered; raise this to use that. Intent is never lost either
way — at 1 it is merely delayed — so this is a latency knob, and the price of
raising it is that until the chain drains the venue is working a quantity you no
longer intend. Independent of `ack_required`, which governs the *first* request
against an order rather than subsequent ones.

**`refill_on_fill`** — false by default. A fill reduces both the level's working
quantity and its target, so the stack settles at what is left and waits. The
alternative is a stacker that quietly re-arms size nobody re-authorised. Set it
true if automatic replenishment is what you want.

**`queue_gap`** — needs `on_queue_position` to be fed. Without it a non-zero
`queue_gap` holds every level to a single order. Leave it at zero if you have no
queue-position feed. `on_book_level` and `on_queue_position` are cheap to call
either way: they record the value but only mark the stacker dirty when the gate
is on and the update could release a level it is holding back.

## Both sides

`stacker_pair` runs a bid and an offer stack and stops them stepping on each
other:

```cpp
#include <slick/stacker/stacker_pair.hpp>

stacker_pair<my_executor> book{exec, bid_cfg, ask_cfg};
book.on_top_of_book(market_bid, market_ask);
book.quote(bid_px, bid_qty, ask_px, ask_qty);
book.reconcile();

book.on_filled(side_t::buy, id, qty, price);   // side is explicit: no guessing
```

Each side is given a placement limit that is the tighter of the market's
opposite top and the nearest price the other side may still be holding, so the
bid ladder can never walk into a live offer of our own. Because the resting
order has to actually be withdrawn first, resolving a crossed quote takes a
round trip; `dirty()` stays true until it has settled.

## Threading

Single-threaded and lock-free: no mutexes, no atomics, no allocation after
construction. Deliver market data and order events on the same thread that calls
`quote` and `reconcile`. If they arrive on another thread, hand them across with
an SPSC queue — [slick-queue](https://github.com/SlickQuant/slick-queue) is
built for it — and drain it before reconciling.

## Sizing

Everything the stacker owns is a fixed array sized from a traits type:

```cpp
struct my_traits {
    static constexpr std::uint16_t level_capacity = 512;  // power of two
    static constexpr std::uint16_t max_orders     = 512;
    static constexpr std::uint16_t max_levels     = 64;
};

stacker<my_executor, side_t::buy, my_traits> bid{exec, cfg};
```

`level_capacity` bounds the *live* price range, not the absolute one: a quote
can walk arbitrarily far as long as it does not leave orders strewn across more
than that many ticks. If it ever does, or the tick grid shifts underneath the
stacker, the grid is rebuilt — everything working is cancelled and the stack
starts again around the new price. `rebase_count()` reports how often that has
happened; in steady state it should stay at one.

## Making event routing free

Your executor owns the order identifier, so routing an event back to the
stacker's slot normally costs a hash probe. If the executor can store one 32-bit
word alongside its own order record, add two methods and the stacker will keep
its slot index there instead:

```cpp
void          set_order_user_data(const order_id_t& id, std::uint32_t v);
std::uint32_t get_order_user_data(const order_id_t& id) const;
```

The hash table then compiles away entirely and every event handler becomes a
single indexed load. Worth about 15-25% of the routing cost.

Two other optional methods are picked up the same way:

```cpp
void flush();                       // called once per reconcile, after the last message
bool can_act(const order_id_t& id); // veto acting on an order the gateway knows is finished
```

## Performance

MSVC 19.44 Release (`/O2`), one pinned core of a Ryzen 9 5900HX that was not
idle, so read these as upper bounds. See `docs/PERFORMANCE.md` for the full set,
the exact environment, and how to reproduce.

| Operation | 4 rungs |
| --- | --- |
| Re-assert an unchanged quote | ~6 ns |
| Change the top level size | ~220 ns |
| Walk the quote one tick | ~480 ns, 2 messages, nothing placed |
| Route an order event | ~9 ns hashed, ~7 ns with user data |
| Apply a fill | ~48 ns |

## Building and testing

```sh
cmake -B build -DSLICK_STACKER_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Benchmarks and examples are off by default:

```sh
cmake -B build -DSLICK_STACKER_BUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target run_benchmarks

cmake -B build -DSLICK_STACKER_BUILD_EXAMPLES=ON
cmake --build build --target simple_stack && ./build/examples/simple_stack
```

`examples/simple_stack.cpp` is a complete integration in one file — an executor,
a stack, a price move, a fill and the recovery — and prints every message it
sends.

The test suite defines `SLICK_STACKER_VALIDATE` and calls `validate()` after
every step. That function rebuilds each level's counters from the orders feeding
them and compares against the values the fast path has been maintaining
incrementally — it is what keeps the incremental accounting honest. It is not
cheap; leave it out of production builds.

## Design

`ARCHITECTURE.md` covers how the levels are stored, how the accounting works,
and what each of the four reconcile passes is for.

## Licence

MIT. See `LICENSE`.
