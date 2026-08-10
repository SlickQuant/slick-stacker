# Architecture

How slick-stacker is put together, and why.

The whole library is one idea repeated: keep the state that describes *what we
want* separate from the state that describes *what the venue has confirmed*, and
derive everything else from the difference.

## The three counters

Each price level carries three quantities:

```
target     what the caller's quote says should rest here
acked      what the venue has confirmed is resting here
inflight   net change we have requested but not had confirmed; signed, and
           negative while a reduction or a reprice-away is outstanding
```

From those:

```
working() = acked + inflight     what we will have once the venue catches up
delta()   = target - working()   what reconcile still owes this level
```

Everything the stacker does is driven by the sign of `delta()`. Positive means
add, negative means shed, zero means leave alone. There is no fourth counter for
fills, no separate in-flight-out counter, and no per-order history of pending
states — each of those turns out to be derivable.

A fill, for instance, decrements `acked` because the quantity is genuinely no
longer working, and (by default) decrements `target` too, because the caller
authorised that size once and it has now been used. `delta()` therefore stays at
zero and the stack settles at what is left rather than silently re-arming.

A reprice from level A to level B posts `-q` to `A.inflight` and `+q` to
`B.inflight`; the acknowledgement posts the mirror to `acked` on both. The two
net to zero without either level needing to know the other exists.

## One definition of the accounting

Every mutation of an order is bracketed:

```cpp
detach(slot);      // subtract this order's contribution from its levels
// ...change the order...
attach(slot);      // add it back
```

`detail::contribution_of` is the single function that says how an order maps
onto its levels' counters. Deriving a delta by hand at each of the two dozen
call sites is exactly where this kind of code rots, so instead the contribution
is recomputed from one definition and applied twice with opposite signs. It
costs a handful of adds on paths that are already touching those cache lines.

`stacker::validate()` rebuilds every level through the same function and
compares against the incrementally maintained values. The test suite calls it
after every step, which is what makes the incremental path trustworthy: if the
two ever disagree, a test fails at the operation that caused it rather than
twenty steps later.

## Levels: a ring indexed by depth

Levels are addressed by **depth** — ticks away from an anchor price, counting in
the direction that moves away from the market.

```cpp
buy:   depth = (anchor - price) / tick_size
sell:  depth = (price - anchor) / tick_size
```

Depth is what makes the side disappear. A buy ladder grows downward in price and
a sell ladder upward, but both grow toward increasing depth, so every comparison
in the reconcile loop is the same integer comparison on both sides. `Side` is a
template parameter, so the one place the direction actually matters compiles to
no branch at all.

Levels live in a fixed array of `Traits::level_capacity` entries — a power of
two — indexed by `depth & mask`. A slot is claimed for a depth on first use and
released when the level goes quiet, so the ring addresses an unbounded price
range with fixed memory as long as the *live* range stays inside the capacity.

That distinction matters: a quote can walk a thousand ticks without ever
outgrowing a 256-entry ring, because only three or four levels are live at a
time. What does not fit is a live range wider than the capacity, and when that
happens — or when a price arrives off the tick grid, meaning the grid itself is
wrong — the ring reports the collision rather than silently merging two prices.
The recovery is a cold path: cancel everything, re-anchor, start again.

### Why not a map, or a linked list

A `std::map` keyed on price gives ordered iteration and O(log n) lookup, at the
cost of a node allocation per level and a pointer chase per step. A doubly
linked list of preallocated levels avoids the allocation but not the chase, and
needs the list to be materialised over the whole reachable price range up front.

The ring gives O(1) lookup by price with no allocation, no indirection, and a
reconcile pass that walks contiguous memory.

### The band

Rather than maintaining an auxiliary index of active levels, the ring tracks the
inclusive depth range that may contain claimed levels. Reconcile walks it
directly. The band is a handful of entries wide — the ladder, plus slack, plus
whatever the quote has recently moved across — and contiguous in the ring, so a
straight scan beats any structure that would have to be kept sorted. Idle levels
at the edges are released at the end of every pass.

## Orders: a slot pool, indexed not pointed

Orders live in a fixed array with an intrusive free list. Every reference is a
`uint16_t` index, never a pointer, so the whole structure is relocatable and a
level's order queue costs four bytes per link.

Each slot tracks two prices and two quantities:

```
price / order_qty            what we most recently asked for
acked_price / acked_qty      what the venue last confirmed
```

While a request is outstanding the two disagree. A modify reject rolls the first
pair back onto the second and re-homes the order to the level it actually rests
on — which is why the pair is kept, rather than one set of fields overwritten
optimistically.

Exhaustion is reported, never thrown: a full pool is handled exactly like a
refused `place`, by leaving the level short and retrying.

## Identifier routing

The executor assigns the order identifier, so an inbound event has to be mapped
back to a slot. By default that is an open-addressing table sized at twice the
order pool — linear probing, backward-shift deletion, no tombstones, so probe
runs cannot degrade over a long session of orders being created and retired.

If the executor can store a 32-bit word alongside its own order record, the
stacker keeps its slot index there instead and the table compiles away to an
empty struct. Detection is a concept, so nothing is paid for the option when it
is not taken.

## At most one request per order

An order never has more than one outstanding request. Anything the stacker wants
to do while a request is in flight is recorded on the slot and sent from the
acknowledgement.

This is the single largest simplification in the design. Allowing chained
modifies means keeping a per-order history of in-flight states, because a fill
or a reject arriving mid-chain has to be unwound against the right link. One
request per order means an order's state is a single enum and a deferred-action
marker.

The cost is latency on venues that require an acknowledgement before the next
action — but on those venues the chain was never legal anyway.

Because `price` and `order_qty` already hold what we want, the deferred action
needs no payload of its own. It is purely a "still owe the venue a message"
marker, and `desired()` reads through it: an order with a deferred cancel
contributes zero to its level the moment the intent is formed, not when the
message finally goes out.

## The four passes

`reconcile()` runs over the band:

**1. Reprice.** Move whole orders out of levels that are over target and into
levels that are under it. One message instead of two, and one order slot instead
of two.

The moved order is **resized to what the destination wants**, growing as readily
as shrinking. A reprice sends the order to the back of the destination's queue
whatever its size, so carrying extra quantity across in the same message is
free — and it removes the separate new order that topping the level up
afterwards would otherwise need. A ladder walking one tick therefore costs two
messages and places nothing, at any depth.

Two cursors walk inward from opposite edges of the band, and they are allowed to
cross. A quote that improved leaves the surplus deep and the deficit shallow; a
quote that backed away leaves it the other way round. Whichever cursor cannot
make progress advances, so the pass is linear in the width of the band either
way, and cannot spin: every successful move consumes an order, and there are
finitely many.

An order carrying more than its level can spare is left alone — moving it would
take the source level below its own target. The reduce pass trims it in place
instead, which also keeps its queue position. Growth is bounded by
`max_order_qty` and by the destination's remaining room, so a level that slices
still slices.

**2. Reduce.** Shed what is still over target, back of the queue first. An order
straddling the boundary is shrunk with a modify rather than cancelled, so the
order with the best place in the queue is the one that keeps it. If trimming
would leave an order below `min_order_qty`, the whole order goes instead.

**3. Add.** Top up what is still short. Before sending anything, deferred cancels
at the level are reclaimed — reinstating one costs no message at all, which
makes it strictly the cheapest quantity available. What remains is sliced
against `max_order_qty`, `min_order_qty`, `qty_increment`, `max_level_qty` and
`max_orders_per_level`, and gated on the queue-gap rule and crossing protection.

**4. Release.** Pull the band in around what is left.

## Guards

**Overfill.** A reduction the executor refuses to send is real exposure: the
venue is holding more than we want and we could not tell it otherwise. The order
is flagged, a counter incremented, and the add pass suppressed entirely until it
clears. Growing anywhere else while over-exposed would compound the problem.

Note the distinction from a *deferred* reduction, which is not blocked: the
intent is recorded and will fire on the acknowledgement, so the accounting is
consistent and the exposure window is one round trip. Only an unexpressed
reduction counts.

**Crossing.** No order is placed at or through `on_opposite_top`. `stacker_pair`
extends this to our own other side, feeding each stack a limit that is the
tighter of the market's opposite top and the nearest price the other side may
still be holding.

**Queue gap.** Stacking several of our own orders back to back at one price puts
them all behind the same queue. When `queue_gap` is set, a level will not take
another order until enough market quantity has arrived behind the last one. The
top level is never gated — that is the quote itself.

**Retry.** A refused message leaves work undone with no event coming to wake the
stacker up again, so the dirty flag is re-armed and the next pass tries once
more.

## Slack and hysteresis

Both exist to stop the stack churning on noise.

**Slack** keeps levels that have just fallen off the bottom of the ladder,
rather than cancelling them, so a one-tick round trip leaves the tail
undisturbed. A retained level is capped at the ladder's own deepest rung size:
without the cap, a level that used to be the quote would sit several ticks back
still carrying quote size. Trimming costs the same one message that cancelling
would have, and keeps the order and its queue position. For a genuine tail
level — already at that size — the cap costs nothing.

**Hysteresis** suppresses topping a level up for a trivial shortfall. It applies
only to levels that are already working: an empty level is always established,
however small the target. Reductions are never suppressed — carrying more than
intended is a risk position, where carrying slightly less is only a missed
opportunity.

## What is deliberately absent

- **Throttling and rate limiting.** The caller's concern. The stacker respects a
  refused message and retries; it does not decide when to hold back.
- **Position and risk limits.** Same. `max_level_qty` and `max_order_qty` are
  shape constraints, not a risk layer.
- **Venue dialects.** `ack_required` and `prefer_modify` cover the behavioural
  differences that change what the stacker may do. Anything else belongs in the
  executor.
- **Threading.** Single-threaded by construction. Cross-thread delivery is a
  queue in front, not a lock inside.
