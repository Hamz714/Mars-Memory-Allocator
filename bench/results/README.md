# Recorded measurements

Every number quoted anywhere in this repository comes from a file in here. A
figure with no CSV behind it is a figure nobody can check, reproduce or
disagree with, so it does not get written down.

Each file records the machine, kernel, compiler, profile and commit it was
taken on, in `#` comment lines at the top. Comparing rows from different files
means reading those first.

| File | What it is | How to reproduce |
|---|---|---|
| `phase05-freelists.csv` | Throughput and utilisation, mars against glibc, on the seven frozen workloads | `bench --ops 20000 --reps 5 --arena 33554432 --out FILE` |
| `scrub-sweep.csv` | Detection rate and detection latency against the scrub interval | `for s in 1 256 1024 4096 off; do faultinject --trials 200 --bits 1,2 --scrub-interval $s --csv FILE; done` |

## Reading the scrub sweep

`latency_mean_ops` is the mean number of allocator calls between a bit flip and
the allocator first reporting it, over ordinary traffic that never touches the
damaged block. It is **censored** at the 4096-call measurement window:
`latency_n` says how many trials were detected inside it, and when that is
short of `trials` the mean is a lower bound rather than an estimate.

Two groups of targets behave completely differently, and that is the point.
`free_hdr` and `links` sit on the allocation path, so validate-on-touch finds
damage to them in ten to fifteen calls whatever the patrol is set to. Damage to
an allocated block's header, canary or payload is found only by the patrol, and
with the patrol off it is not found at all by traffic alone — `latency_n` of 0.
That is the cost of O(1) allocation, priced.
