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
list links, the mirrored footer, the payload, the canary, and anywhere in the
arena at all.

## What this is not

Being precise about the limits matters more than the defence sounding
impressive.

- **This is not radiation hardening.** Real hardening is ECC memory, redundant
  hardware, and physical shielding. Nothing in software can stop a bit
  flipping; this can only notice afterwards.
- **Repair is limited to metadata, and only while one copy survives.** A
  damaged header is rebuilt from its mirrored footer, and a damaged footer is
  republished from its header. Nothing reconstructs a payload: if the flip
  landed in user data, the payload checksum reports it and the data is gone.
  When both copies of the metadata are destroyed the block cannot be rebuilt
  at all, and it is surrendered.
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
| Header metadata | checksum over every field | every access, free, resize, and heap check |
| Footer | full mirror of the header | same |
| Payload | checksum over the whole payload | `mm_read`, `mm_verify` |
| End of payload | 8-byte canary | every access, free, resize |
| List topology | neighbour cross-checks, adjacency, exact tiling of the arena | `mm_check_heap` |

A neighbour that fails validation is never *written through* — a block's size
field decides where its footer lands, so sealing a corrupted neighbour would
scatter writes across the arena. Space surrendered is counted, so the
consistency check can tell deliberate loss from memory that has genuinely gone
missing.

## Rebuilding a damaged block

Blocks tile the arena in address order, which means a damaged block's **start**
is known from its predecessor even when its own header is unreadable. Only its
extent is missing — and the mirrored footer still records that.

The difficulty is that the footer's position is derived from the very size that
was lost, so it cannot simply be looked up. It is searched for instead: step a
candidate position outwards and accept one only when

- the magic and checksum both hold, **and**
- the size the candidate carries is exactly the size that would place it at
  that address, **and**
- whatever follows the block it describes is either the end of the arena or
  another intact block.

The second condition ties a candidate to its position, so that forty-eight
bytes which merely happen to checksum are not enough. The third uses the tiling
itself as redundancy. A test plants a perfectly-formed decoy footer nearer the
start than the real one — correct magic, correct checksum over its own fields —
and requires that it be refused. **A wrong repair is worse than no repair:** it
would hand back a block of the wrong extent overlapping a live neighbour.

When nothing can be rebuilt, the walk resynchronises on the next block that
stands up on its own and gives up only the span in between. Truncating the list
instead would cost the entire remainder of the arena — measured at 99.8% of it
for a single flipped bit, before this was addressed.

Quarantine poisons **both** copies of a block's metadata. Poisoning only the
header would leave the mirror intact for the recovery path to faithfully
rebuild, bringing back the very block that was just given up on.

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

## A free correctness oracle

Theory says what some of these numbers must be. A checksum over the header
detects any single-bit change in it, without exception — so single-bit header
corruption must come out at 100% detection with zero silent corruption. If it
does not, the harness has found a bug rather than a statistic.

Cells where theory and measurement can be compared are worth more than cells
where only measurement exists, and disagreement between them should always be
investigated as a defect first.
