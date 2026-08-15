# Fault model

What this allocator defends against, what it does not, and how the defence is
measured.

## The fault being modelled

A **single-event upset**: ionising radiation flips one or more bits in DRAM.
The memory is otherwise working; a value simply changes without anything having
written it. This is a real failure mode for spacecraft, and a convenient one to
study, because it can be reproduced exactly by XOR-ing a bit.

`tools/faultinject` flips *k* bits inside a chosen structure and records what
the allocator then does. Targets are the allocated header, a free header, the
free-list links, a free block's boundary tag, the payload, the canary, and
anywhere in the arena at all.

There is no allocated-block footer to aim at any more. Since the layout change
a footer exists only inside a free block, which is why the old `footer` target
became `free_footer`; see [BLOCK_LAYOUT.md](BLOCK_LAYOUT.md).

**Everything below depends on the integrity profile.** `hardened` is described
here because it is the default; `fast` carries no checksum and no canary and
detects correspondingly less, while `paranoid` adds a tail mirror and can
repair what the others surrender. The profile is printed at the top of every
run and recorded in the first column of the CSV, because a detection rate
without it is not comparable with anything.

## What this is not

Being precise about the limits matters more than the defence sounding
impressive.

- **This is not radiation hardening.** Real hardening is ECC memory, redundant
  hardware, and physical shielding. Nothing in software can stop a bit
  flipping; this can only notice afterwards.
- **Repair is limited to metadata, only under `paranoid`, and only while the
  mirror survives.** Nothing reconstructs a payload: if the flip landed in user
  data, the payload checksum reports it and the data is gone. Under `hardened`
  and `fast` there is no second copy of the metadata either, so a damaged
  header means a surrendered block — detection without repair is the trade
  those profiles make, and it is what buys back 48 bytes per allocation.
- **Read-after-write verification would not be brownout protection.** An
  earlier version of this allocator wrote each metadata field, read it back,
  and retried up to three times, describing this as protection against power
  dips. Against DRAM it protects against nothing: the read is satisfied by the
  same storage the write went to, and a compiler is free to fold the comparison
  away entirely, which is what happened. The mechanism was removed rather than
  kept as decoration. A hardware write-verify against a device that can fail a
  write is a real technique; this was not that.
- **Payload integrity only holds while access goes through `mm_read` and
  `mm_write`.** Once a raw pointer is handed out, the caller can write through
  it without telling the allocator, and any payload checksum is immediately
  stale. This is a property of returning pointers at all, not an oversight.

## What is checked, and when

| Structure | Protection | Verified on |
|---|---|---|
| Header metadata | CRC32C over the control word, the payload checksum, the block's index and the arena secret | every access, free, resize, and heap check |
| Tail mirror (`paranoid`) | full copy of the header, at a fixed offset from the block's end | recovery |
| Payload | CRC32C over the whole payload | `mm_read`, `mm_verify` |
| End of payload | 8-byte canary, bound to the block's index | every access, free, resize |
| Free-list links | structural cross-checks in a fixed order, XOR-masked with the arena secret | every traversal, `mm_check_heap` |
| Boundary tag | corroborated against the header it claims to describe | every backward step, `mm_check_heap`, the patrol |
| Topology | `PREV_IN_USE` agreement, no adjacent free blocks, exact tiling, bin membership against the tiling | `mm_check_heap` |
| Anything untouched | the bounded patrol, resuming where it stopped | every `N` allocator calls |

A neighbour that fails validation is never *written through* — a block's
control word decides where its own trailer lands, so sealing a corrupted
neighbour would scatter writes across the arena. Space surrendered is counted,
so the consistency check can tell deliberate loss from memory that has
genuinely gone missing.

## Rebuilding a damaged block

Blocks tile the arena in address order, which means a damaged block's **start**
is known from its predecessor even when its own header is unreadable. What is
missing is its extent — and the extent is exactly what decides where anything
else belonging to the block can be found.

So the walk resynchronises first: it scans forward for the next header that
stands up on its own *and* tiles with whatever follows it, which is the only
way back in step once an extent is gone. A real block is at least
`MM_MIN_BLOCK` long, so the scan starts there rather than at the next alignment
unit.

Under `paranoid` that foothold is what makes the mirror usable. The mirror sits
at a fixed offset from the block's **end**, and the resynchronisation point is
that end, so the repair is one bounded check rather than a search over every
candidate extent. It is accepted only when

- the extent it carries is at least `MM_MIN_BLOCK` and reaches back no further
  than the damaged block's known start, **and**
- its checksum holds *for the index of the position it would restore*.

The second condition is what refuses a mirror belonging to a different block.
A test copies one block's mirror verbatim over another's — correct extent,
correct flags, correct checksum over its own fields — and requires it to be
rejected. **A wrong repair is worse than no repair:** it would hand back a
block of the wrong extent overlapping a live neighbour.

When nothing can be rebuilt, the span between the damage and the
resynchronisation point is surrendered, and only that span. Truncating the walk
instead would cost the entire remainder of the arena — measured at 99.8% of it
for a single flipped bit, before this was addressed.

Quarantine no longer needs to poison anything. A block given up on stays in the
tiling as permanently-allocated space marked `QUARANTINED`: it can never be
merged, never be handed out, and never be freed back into circulation, and the
arena still tiles exactly. That is what turns "space was lost" from an
inference into an arithmetic identity `mm_check_heap` can check.

## Outcome taxonomy

Every trial lands in exactly one bucket:

| Outcome | Meaning |
|---|---|
| `detected_no_loss` | flagged; the block was rebuilt or the damage cost nothing, and the allocator gave up no memory |
| `detected_quarantined` | flagged; a block was isolated and its space surrendered |
| `detected_fatal` | flagged, but the allocator could no longer serve requests |
| `undetected_benign` | not flagged, and nothing was wrong — the flip landed in slack, padding, or free space |
| `undetected_silent` | not flagged, **and the data came back wrong** |
| `crash` | the trial died on a signal or ran out of time |

**Separating the two undetected buckets is the point of the whole exercise.**
Most flips into a large arena land somewhere that never mattered. Counting
those as failures would understate the allocator badly; counting them as
successes would overstate it just as badly. Only `undetected_silent` — where a
read reported success and returned different bytes than were written — is a
real failure to detect.

This is why every trial carries a shadow model of what each live block is
supposed to contain. Without something to compare against, "silent" cannot be
distinguished from "harmless", and a detection rate means nothing.

## Method

- Each trial runs in a **forked child under an alarm**. A good proportion are
  expected to crash or hang; that is a result to record, not a reason to stop
  the run. This is why the tool is Unix-only.
- Trials are **seeded** from a base seed plus the trial index, so any cell of
  the table can be reproduced exactly.
- Proportions are reported with **95% Wilson score intervals**, which stay
  inside [0, 1] near the extremes where the textbook normal approximation does
  not. At 1,000 trials the interval is roughly ±2 percentage points, which is
  why a detection rate should never be quoted without its trial count.
- **Detection coverage** is `detected / (detected + silent)` — of the flips
  that actually mattered, how many were caught. Benign flips are excluded,
  because there was nothing there to catch.

## Detection latency

Whether damage is caught stopped being the only question once allocation became
O(1). The linear search it replaced revalidated every free block on every call,
so damage was found almost as soon as anything happened; with bins, a block
nobody is using is a block nobody looks at, and a bounded patrol decides how
long it stays that way. **How long** is therefore now a measured quantity
rather than an implicit one.

Each trial runs up to 4096 ordinary allocations and frees after the flip,
deliberately never touching the damaged block, and records the call on which
the allocator first reported something wrong. `--scrub-interval` sweeps the
setting; the interval is a mandatory CSV column, because two runs at different
settings are not comparable and nothing else in the row would say so.

Read `latency_mean_ops` together with `latency_n`. The measurement window is
4096 calls, so the mean is **censored**: when `latency_n` falls short of the
trial count the mean is a lower bound, not an estimate. An interval at or above
the window produces exactly that.

`bench/results/scrub-sweep.csv` holds the curve. What it shows is two different
populations. `free_hdr` and `links` sit on the allocation path and are found in
ten to fifteen calls at every setting, patrol included or not — validate-on-
touch is what catches them. An allocated block's header, canary and payload
track the interval almost exactly, and with the patrol off are not found by
traffic at all. That second row is the honest price of O(1) allocation, and the
patrol is what buys it back.

## A free correctness oracle

Theory says what some of these numbers must be. A 32-bit CRC over the header
detects any single-bit change in it, without exception — so under any profile
that carries one, single-bit header corruption must come out at 100% detection
with zero silent corruption. If it does not, the harness has found a bug rather
than a statistic. CI gates exactly that cell for `hardened` and `paranoid`.

`fast` carries no checksum, so no such prediction exists for it and the cell is
recorded rather than gated. What *is* gated for all three is the crash count:
every profile promises never to read or write outside the arena, whatever a
corrupted control word says, and that promise does not depend on being able to
detect the corruption.

Cells where theory and measurement can be compared are worth more than cells
where only measurement exists, and disagreement between them should always be
investigated as a defect first.
