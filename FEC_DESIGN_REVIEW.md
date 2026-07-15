# FEC Design Review — Is the XOR Delay Estimate Pessimistic?

Critical re-examination of the FEC delay estimates in
[DESIGN_OPTIONS.md](DESIGN_OPTIONS.md), prompted by the hypothesis that parity built
from a **backward-looking sliding window** of past frames can recover losses with far
less added delay than the future-looking block scheme those estimates assumed. **No code
written** — analysis only. Numbers are calibrated to the measured captures in
[RUNLOG.md](RUNLOG.md): one-way delay ceiling ≈42ms (A) / ≈82ms (B), mean 26 / 51.5ms,
direct-frame "no longer late" threshold ≈55–60ms (A) / ≈95–100ms (B), raw overhead 1.02×.

## Verdict up front

**The suspicion is correct.** DESIGN_OPTIONS.md's FEC delay numbers are pessimistic, and
the reason is subtle: the delay penalty it attributed to "FEC" is really a penalty of
**future-looking construction plus a needlessly large parity group**, not of erasure
coding as such. The fix is two independent levers, both of which we can afford:

1. **Orientation** — build parity from *past* frames so it is ready to send the instant
   its newest member is available (no waiting for future frames).
2. **Parity rate** — since the 2.0× overhead cap leaves us ~1× of unused budget
   (baseline is 1.02×), emit parity *frequently* (1 per 2 data frames, not 1 per 4+),
   which is what actually collapses the recovery latency.

Both levers point at the same construction: a **backward-looking sliding-window XOR at
rate 1:2**. Estimated minimum delay **≈60ms (A) / ≈100ms (B)** at ≈1.54× overhead —
versus block-`k=4`'s ≈101ms / ≈142ms. That is the single biggest delay win available,
and it is the metric the assignment scores on.

## The core correction: what actually sets recovery latency

Define **L (frames)** = how many 20ms slots after a lost frame `j`'s own availability the
*earliest parity that can repair it* is transmitted. The delay budget a recovered frame
needs is:

```
delay_ms  ≥  L · 20ms  +  (parity one-way transit)
```

For a **directly-arriving** (non-lost) frame, L = 0 — it just needs `delay_ms ≥ transit`.
So the FEC scheme only has to pay `L·20ms` on the *lost* fraction (2–5%), and only for
frames whose covering parity comes late. This reframing is the whole game:

| Where the delay comes from | Block XOR (future) | Backward sliding |
|---|---|---|
| Parity ready-to-send time | after the **last** frame of the group (`+(k-1)·20ms` vs the group's first frame) | immediately after its **newest** member (`+0`) |
| Earliest parity repairing lost frame `j` | the group's parity, up to `k-1` slots later | the next emitted parity ≥ `j`, up to `r-1` slots later |
| Worst-case L | `k-1` | `r-1` |
| L for the *last* frame in each group | 0 | 0 |

The critical observation DESIGN_OPTIONS.md missed: **at equal parity rate, aligned block
and aligned backward-sliding have the *same* worst-case L** (`k-1` = `r-1`). The delay
tax is not caused by "block vs sliding" — it is caused by choosing a large group. What
backward orientation buys is that a *small* rate `r` is achievable **without** any
future-waiting, and small `r` is affordable because overhead is abundant. DESIGN_OPTIONS.md
picked `k=4` to save bytes we did not need to save, and paid ~40–60ms of delay for it.

### Why this matters at a *fixed* low delay budget (the real scoring lens)

Take `delay_ms = 60` on profile A and ask how each scheme does. A lost frame is repaired
on time only if `L·20 + parity_transit ≤ 60`, i.e. (median transit 26ms) `L ≤ ~1.7`:

- **Block `k=4`**: lost frames are uniformly the 1st/2nd/3rd/4th of their group
  (L = 3/2/1/0). Only the last-two-of-four (L ≤ 1) repair in time → **half** of losses
  still miss → miss rate ≈ p/2 ≈ **1.0%** (A), ≈2.5% (B): fails or is marginal.
- **Backward sliding `r=2`**: every lost frame has L ≤ 1 → essentially all singles
  repair in time → miss ≈ double-loss residual ≈ **0.08%** (A).

Same delay, same overhead order, ~12× better miss rate. Equivalently: block `k=4` needs
`delay_ms ≈ 100–142ms` to reach the miss rate that sliding `r=2` reaches at ≈60–100ms.
That gap is the pessimism the review set out to find.

---

## Construction-by-construction

Overhead model: one parity per `r` data frames ⇒ `overhead ≈ (1 + 1/r) × 1.03`
(the 1.03 carries the per-packet 4-byte-ish header on both data and parity). Recovery
math uses i.i.d. loss `p` (2% / 5%; neither practice profile is bursty, but the unseen
grading profile may be — flagged per construction).

### 1. Block XOR (future-looking) — the baseline FEC assumption

`k` consecutive data frames + 1 parity emitted after the group's last frame.

- **Recoverable patterns**: any **single** erasure among the `k+1` units. **Two or more
  losses in one group are unrecoverable** — and because the group is `k` *consecutive*
  frames, any burst of length ≥2 that lands inside a group defeats it. Worst case for
  bursts.
- **Overhead**: `k=2` → 1.54×, `k=4` → 1.28×, `k=8` → 1.15×. Best bytes-per-protection.
- **Complexity**: **lowest.** Ring buffer of `k`, one XOR accumulator, emit on group
  close; receiver reconstructs when a group has exactly one missing unit. ~30 lines.
- **Min delay**: worst-case `L = k-1` ⇒ `≈ (k-1)·20 + transit_ceiling`. `k=2`: ≈60ms
  (A) / ≈102ms (B). `k=4`: ≈101 / ≈142ms. **The delay grows linearly with the group
  size you chose for byte-efficiency** — a direct overhead↔delay conflict.

### 2. Sliding-window XOR (backward-looking) — the proposed fix

Parity emitted every `r` frames, each covering a backward window of the last `w` frames
(`w ≥ r`): e.g. `parity_i = f_i ⊕ f_{i-1} ⊕ … ⊕ f_{i-w+1}`, ready the instant `f_i` is.

- **Recoverable patterns**: single erasure per covering window, **plus chained
  consecutive multi-losses when windows overlap (`w > r`)**. Example `r=2, w=3`: a
  consecutive double loss `f_{2m-1}, f_{2m}` is repaired by first solving `f_{2m}` from
  the *next* parity `P_{2m+2}` (which still sees it as a one-unknown older member), then
  back-substituting into `P_{2m}` — recovery block XOR at the same overhead cannot do.
  So sliding is **strictly more robust than block at equal rate**, not just lower delay.
- **Overhead**: identical to block at equal `r` — `r=2` → 1.54×, `r=3` → 1.37×.
- **Complexity**: **moderate.** Sender keeps a `w`-deep ring and emits an XOR every `r`
  frames — trivially more than block. Receiver holds recent frames + parities and, on
  each parity or each newly-filled gap, repairs any covering equation with exactly one
  unknown (a small fixed-point loop for chaining). ~60–80 lines.
- **Min delay**: worst-case `L = r-1`. Because overhead lets us pick `r=2`, `L ≤ 1` ⇒
  **≈60ms (A) / ≈100ms (B)** — and *half* of all lost frames (the even, window-closing
  ones) have `L = 0`, needing only `transit_ceiling`. This is the lowest delay of any
  construction here that stays under the overhead cap. (Rate `r=1`, `L=0` everywhere,
  would be ideal but is 2.05× — over the cap, same wall duplication hits.)

### 3. Interleaved XOR

Parity over frames spaced by a stride `D` instead of consecutive ones. **Must be built
backward** to stay low-delay (`parity_i = f_i ⊕ f_{i-D} ⊕ f_{i-2D} ⊕ …`); a *forward*
interleave is a delay disaster because the group cannot close until its temporally-last,
widely-spaced member is available.

- **Recoverable patterns**: single loss per interleave group, but because members are
  `D` apart, **a burst of up to `D` consecutive losses spreads across `D` different
  groups → one loss each → all recoverable.** This is the classic burst→erasure
  converter, and the *only* single-parity construction with real burst immunity.
- **Overhead**: same as its rate (e.g. 1.54× at `r=2`).
- **Complexity**: **moderate-plus.** Sliding-window bookkeeping with strided indices;
  the receiver's group membership arithmetic is fiddlier and it must buffer `D·w` frames
  deep (more memory, and a larger backward reach — though *not* more delay, since it is
  backward-looking).
- **Min delay**: with backward orientation, same `L = r-1` as construction 2 ⇒
  **≈60ms (A) / ≈100ms (B)**. Interleaving adds burst protection at **no delay cost** —
  its only prices are complexity and buffer depth. Under the current non-bursty A/B
  profiles it is pure insurance with no measurable benefit; under an unseen bursty
  grading profile it could be the difference between valid and invalid.

### 4. Multiple parity streams

Two (or more) parities per window with different member combinations (e.g. one over the
whole window, one over a sub/strided set — a small XOR-based `2`-erasure code, RAID-6-ish).

- **Recoverable patterns**: **up to 2 (or #streams) erasures per window**, including
  *clustered* losses a single parity can't touch — the strongest robustness here, short
  of a full Reed–Solomon code (which the "standard library only / no frameworks" rule and
  the time budget make unattractive).
- **Overhead**: highest — 2 parities per `k` data. `2:4` → 1.54×, `2:6` → 1.37×. Still
  well under 2.0×.
- **Complexity**: **highest.** The decoder is no longer "fill the one blank" — with two
  unknowns it must solve a 2×2 XOR system (Gaussian elimination over GF(2)). Correct
  handling of the "which equations are currently solvable" state is the main bug surface.
  ~120+ lines and the most test-sensitive.
- **Min delay**: backward-oriented, again `L = r-1` ⇒ **≈60ms (A) / ≈100ms (B)**. Same
  delay floor as 2 and 3; it spends *overhead and complexity*, not delay, to buy
  multi-loss recovery.

---

## Comparison (all backward-oriented except block)

| Construction | Recoverable loss | Overhead (at chosen rate) | Complexity | Min delay A / B |
|---|---|---|---|---|
| 1. Block XOR (k=4) | 1 per group; **bursts fatal** | 1.28× | Low | ~101 / ~142ms |
| 1. Block XOR (k=2) | 1 per group; bursts fatal | 1.54× | Low | ~60 / ~102ms |
| 2. Sliding backward (r=2, w=3) | 1 per window **+ chained doubles** | 1.54× | Moderate | **~60 / ~100ms** |
| 3. Interleaved backward (r=2, D>burst) | 1 per group; **bursts ≤ D** | 1.54× | Moderate+ | ~60 / ~100ms |
| 4. Multi-parity backward (2:4) | **≥2 per window / clusters** | 1.54× | High | ~60 / ~100ms |

Key reading: **once you commit to backward orientation, all of 2/3/4 share the same delay
floor** (set by the parity rate `r=2` → `L=1`), and they differ only in how much
*robustness* they buy with extra overhead/complexity. Only block, being future-looking,
pays delay for its group size. So **for minimizing delay, orientation and rate are
everything; the choice among 2/3/4 is a robustness/complexity decision, not a delay one.**

## Recommendation for minimizing delay under the scoring rule

**Backward-looking sliding-window XOR at rate 1:2, window w=3 (construction 2).**

- It achieves the **joint-minimum delay** (~60ms A / ~100ms B) — bounded below only by
  the parity-rate limit the 2.0× cap imposes (`r=1`/`L=0` is unreachable under the cap),
  so no cap-compliant construction here beats it on delay.
- The overlapping window (`w=3 > r=2`) gives it **chained double-loss recovery for free**,
  so it is already meaningfully burst-tolerant without paying interleaving's buffer depth
  or multi-parity's GF(2) solver.
- Overhead ~1.54× leaves ~0.45× of headroom — enough to *layer* an ARQ backstop (the
  hybrid from DESIGN_OPTIONS.md) for the rare residual, at the **same delay floor**, since
  the backstop only fires on the <0.1% of frames FEC misses and need not be sized into the
  budget.
- Complexity is moderate and squarely within "standard library, sockets/threads/time."

**Reserve interleaving (3) or a second parity stream (4) as a drop-in upgrade** if
measurement on the unseen grading profile shows burst loss defeating the `w=3` chaining —
both raise robustness at the *same* delay, trading only overhead/complexity, so the delay
target does not move if we escalate.

## Correction to DESIGN_OPTIONS.md

DESIGN_OPTIONS.md is not wrong to recommend a **hybrid FEC+ARQ**, but its FEC layer should
be **backward sliding `r=2`**, not **block `k=4`**. That single substitution drops the
recommended design's minimum delay from ~140ms to ~100ms on profile B (and ~100→~60ms on
A) at essentially the same overhead and miss margin — a direct improvement on the *scored*
quantity. The document's own parenthetical ("a smarter sliding-window code … would remove
the group-wait delay tax entirely — worth flagging as a refinement") understated its own
point: it is not a refinement, it is the design.
