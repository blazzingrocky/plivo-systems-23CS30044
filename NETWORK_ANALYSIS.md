# Network Analysis — profiles/A.json and profiles/B.json

Read-only analysis of the two practice impairment profiles and what they imply for
design, grounded in `relay.py`'s actual impairment model (`Impair.drop`,
`Impair.delay_s`, `Impair.dup`, `relay.py:24-53`). No code was changed to produce this
document. Numeric examples assume the harness defaults: `duration = 30 s`,
`FRAME_MS = 20`, so `n = 1500` frames per run (`endpoints.py:78`).

Raw profile contents:

```json
// profiles/A.json
{"name": "A_mild",     "loss": 0.02, "delay_min_ms": 10, "delay_max_ms": 40, "dup": 0.005}
// profiles/B.json
{"name": "B_moderate", "loss": 0.05, "delay_min_ms": 20, "delay_max_ms": 80, "dup": 0.01}
```

Neither profile sets `burst_loss` or `spike` — both are optional keys the relay checks
with `.get(...)` and only acts on if present (`relay.py:32,47`). That matters
throughout this analysis: A and B are the *easy* case (memoryless loss, no delay
spikes), and the grading harness explicitly may use unseen profiles that turn either
one on.

## 1. Loss percentages

`Impair.drop()` (`relay.py:30-42`) applies `random.random() < p["loss"]` as an
independent Bernoulli trial **per packet, per lane**. Each lane (uplink 47001→47002,
downlink 47003→47004) gets its own `Impair` instance seeded from `--seed` and
`--seed + 1` respectively (`relay.py:74-77`), but both instances read the *same*
`loss` value from the profile — uplink (media) and downlink (feedback) suffer
identical loss probability, just independently realized.

| Profile | `loss` | Expected drops / 1500 frames (media, up lane) | Frame-level miss rate from loss alone |
|---|---|---|---|
| A | 2% | ~30 | ~2.0% (already over the 1% cap) |
| B | 5% | ~75 | ~5.0% (already over the 1% cap) |

Two independent draws in a row (both directions of a single retry) succeed with
probability `(1-loss)^2`: **96.04%** for A, **90.25%** for B — i.e. roughly **1 in 25**
round trips fails outright under A, and roughly **1 in 10** fails under B, before
counting any further retry.

## 2. Jitter characteristics

`Impair.delay_s()` (`relay.py:44-50`) draws delay **uniformly** per packet from
`[delay_min_ms, delay_max_ms]`, independently for every packet (no smoothing, no
autocorrelation between consecutive packets), then optionally adds a `spike` (absent
in both profiles).

| Profile | Range | Mean | Std. dev. (`range/√12`) | Range width (`max-min`) |
|---|---|---|---|---|
| A | 10–40 ms | 25 ms | 8.66 ms | 30 ms |
| B | 20–80 ms | 50 ms | 17.32 ms | 60 ms |

Media frames cross the relay exactly once (uplink only), so this is the **entire**
network-induced transit-time variance a frame experiences — there is no smoothing
between hops because there's only one hop. Because draws are independent per packet
(not a slowly-varying "current network state"), two frames sent 20 ms apart can have
transit-time draws anywhere within the full range on each side — the jitter is
effectively adversarial from one packet to the next, not a smooth trend a simple
moving-average predictor could track.

## 3. Reordering probabilities

The relay has no explicit "reorder" parameter — reordering is a *derived* effect of
independent per-packet jitter draws combined with the fixed 20 ms send cadence.
Frame `i` is sent at `available(i) = t0 + i·20ms`; frame `i+k` is sent `k·20ms` later.
Frame `i+k` overtakes frame `i` in arrival order iff its transit-delay advantage
exceeds the `k·20ms` head start `i` had, i.e. `d_i − d_{i+k} > k·20ms`.

For `d_i, d_{i+k}` drawn i.i.d. `Uniform(0, R)` (R = jitter range = `max−min`; the
`delay_min_ms` offset cancels out of the difference), `D = d_i − d_{i+k}` follows a
triangular distribution on `[−R, R]`, giving:

```
P(D > x) = (R − x)² / (2R²)   for 0 ≤ x ≤ R,   0 for x > R
```

| Profile | R | Adjacent-pair reorder P (`x=20`) | Max index distance reordering is even possible (`k < R/20`) | 2-apart reorder P (`x=40`), only relevant if feasible |
|---|---|---|---|---|
| A | 30 ms | **5.6%** | `k=1` only (30/20 = 1.5 → next-nearest, k=2, needs `x=40>R`, impossible) | n/a (infeasible) |
| B | 60 ms | **22.2%** | `k=1,2` (60/20 = 3 → k=2 feasible at `x=40<60`; k=3 sits exactly at the boundary, effectively 0%) | **5.6%** |

So under A, only immediately-adjacent frames can ever swap order (~5.6% of adjacent
pairs do); under B, both adjacent frames (~22% of pairs — better than 1 in 5) *and*
next-nearest frames (~5.6%) can swap, i.e. reordering under B spans a noticeably wider
window than under A, not just a higher rate.

## 4. Duplicate rates

`Impair.dup()` (`relay.py:52-53`) is checked **only for packets that already survived
the drop roll** (`relay.py:99-105`), so expected duplicates ≈ `n · (1−loss) · dup`.
Each duplicate is scheduled with its *own* independent fresh delay draw
(`relay.py:104-105`), so a duplicate can land before or after its original, or
interleaved with unrelated frames.

| Profile | `dup` | Expected duplicate packets / 1500 frames (media, up lane) |
|---|---|---|
| A | 0.5% | ~7.4 |
| B | 1.0% | ~14.25 |

Both are low-probability but not negligible, and — importantly for design — relay
byte-counting for the overhead metric happens **on ingress from the student process**
(`relay.py:96-98`), before drop/dup logic runs. A relay-generated duplicate egressing
toward the receiver is therefore **not** counted a second time against the sender's
overhead budget; only bytes the sender itself transmits count.

## 5. Burst-loss behaviour

`Impair.drop()`'s Gilbert–Elliott state machine (`relay.py:32-40`, keyed on
`in_burst`, `p_enter`, `p_exit`, `p_loss_in_burst`) only activates when the profile
defines a `burst_loss` block. **Neither `profiles/A.json` nor `profiles/B.json` defines
one** — both practice profiles use pure i.i.d. loss with no autocorrelation: knowing
that packet `i` was dropped tells you nothing about packet `i+1`'s chance of being
dropped.

This is a meaningful gap between practice and grading conditions. The assignment states
grading uses unseen profiles with "different loss patterns," and the relay code
explicitly supports correlated burst loss as a first-class mechanism — a design that is
only validated against A/B's memoryless loss has never been exercised against clustered
consecutive drops, which is a materially harder failure mode for any recovery strategy
that assumes losses are isolated (see §FEC estimate below).

## 6. Approximate RTT implications

There is no ping/pong primitive in this harness; "RTT" here means the cost of a
feedback-driven exchange — e.g. a receiver signalling a gap on the downlink (47003) and
the sender responding on the uplink (47001) — which crosses the hostile relay **twice**,
once per direction, using the *same* profile parameters on both lanes (`relay.py:73-78`
— both `Impair` instances read the same `prof` dict).

| Profile | Mean 1-hop delay | Mean round trip (down+up) | Worst-case round trip (both at `delay_max`) | P(both hops survive loss) |
|---|---|---|---|---|
| A | 25 ms | **~50 ms** | 80 ms | 96.04% |
| B | 50 ms | **~100 ms** | 160 ms | 90.25% |

A full detect-and-recover cycle is larger still: a receiver has to wait roughly
`delay_max_ms` after a frame's expected arrival window before it can safely conclude
"lost" rather than "still in flight," or it risks firing spurious recovery requests for
merely-slow packets. Adding that wait to the round trip above:

| Profile | Detection wait (≈`delay_max_ms`) | + round trip | ≈ Total mean recovery latency |
|---|---|---|---|
| A | 40 ms | 50 ms | **~90 ms** |
| B | 80 ms | 100 ms | **~180 ms** |

---

## Estimates

### Minimum jitter buffer required

The receiver only controls how long it holds an arrived packet before releasing it;
the harness controls the deadline. For a packet with transit delay `d ∈ [delay_min_ms,
delay_max_ms]`, slack before its personal deadline is `delay_ms − d`. For **every**
packet to have non-negative slack purely from jitter (i.e. before any loss recovery is
considered), the chosen playout budget must satisfy:

```
delay_ms ≥ delay_max_ms
```

| Profile | Minimum viable `delay_ms` from jitter alone | Buffer holding window (`delay_max−delay_min`) | Buffer depth in frames (`≈ window / 20ms`, rounded up) |
|---|---|---|---|
| A | **≥ 40 ms** | 30 ms | **2 frames** |
| B | **≥ 80 ms** | 60 ms | **3 frames** |

(The 40 ms figure for profile A exactly matches the assignment's own suggested
starting point, `python3 run.py --profile profiles/A.json --delay_ms 40` — that value
is the jitter floor, not a coincidence.) The frame-count figures match the reordering
window derived in §3 (max reorder distance `k=1` for A, `k=2` for B → buffer must hold
`k_max+1` frames to place a same-index-window early arrival correctly), so time-domain
and frame-domain sizing agree. This is a floor for absorbing jitter only — it assumes
zero packet loss and adds nothing for recovery latency.

### Whether retransmission can meet deadlines

Not as a sole mechanism, at least not at a competitive `delay_ms`. §6 puts mean
detect-plus-recover latency at **~90 ms (A)** and **~180 ms (B)** — roughly **2.25×–3×**
the jitter-only floor computed above (40 ms / 80 ms). A design relying purely on
reactive retransmission would need to budget `delay_ms` in that ~90–180 ms range just
to make *first-attempt* recovery land on time, and that estimate assumes the recovery
round trip itself succeeds — at A's 96.04% and B's 90.25% per-attempt success rate,
a nontrivial share of recovery attempts (≈4% A, ≈10% B) need a *second* round trip,
which likely blows any deadline budgeted for a single retry. Since among valid runs the
score is the playout delay itself, a retransmission-only strategy is structurally
biased toward a worse (higher) score than one that avoids paying round-trip latency for
the common case. Retransmission is not useless — it can plausibly serve as a
backstop for rare, otherwise-unrecoverable losses at a more generous delay budget — but
it cannot be the primary mechanism if the goal is to minimize `delay_ms`.

### Whether FEC is necessary

The evidence points strongly toward some form of proactive redundancy (FEC, or
sending compact recovery information ahead of a loss occurring, rather than reacting
to it after the fact) being close to necessary for a competitive score, for four
compounding reasons:

1. **Loss is memoryless in the practice profiles** (§5) — modest, independent
   per-packet loss (2%/5%) is exactly the regime simple erasure coding handles well
   (e.g. a systematic code that can repair one erasure per small group of frames turns
   a `p`-probability single loss into a `~p²`-probability *uncorrectable* group
   failure — for `p=0.02` that's roughly two orders of magnitude below the 1% cap on
   groups small enough to keep latency low; for `p=0.05` the margin is smaller but
   still favorable versus leaving losses unrecovered).
2. **Reactive recovery is too slow relative to the cadence** — a 20 ms frame interval
   against a ~90–180 ms mean round-trip recovery cost (previous estimate) means
   waiting to ask is fundamentally not a low-latency answer; data that's already
   redundant on first arrival doesn't pay that cost at all.
3. **The overhead budget has room for it** — the harness's own pass-through baseline
   already sits at ≈1.03× overhead (4-byte header on a 160-byte payload, see
   `BASELINE_FAILURE_ANALYSIS.md` §6, "The bandwidth budget is almost entirely
   unused"), leaving up to roughly another 0.9–1× of the 2.0× cap available specifically for redundancy or
   feedback traffic — a light-weight code (adding on the order of 20–35% overhead
   rather than 100% full duplication) fits comfortably inside that margin for both A
   and B's loss rates.
4. **It is one of only two remedy families available at all** — given UDP provides no
   transport-level recovery and the two endpoints only ever exchange bytes through the
   hostile relay, the only options are "ask again after the fact" (retransmission,
   shown above to be latency-expensive) or "send enough redundant information the
   first time that asking again usually isn't necessary" (FEC / proactive
   redundancy). Since (2) rules out reactive-only as sufficient for a competitive
   delay, FEC-class redundancy is close to necessary, not merely optional.

The one caveat (tying back to §5): a code sized only for A/B's isolated single losses
(small groups, one parity per group) is validated against memoryless loss only. If a
grading profile enables `burst_loss`, consecutive correlated drops can exceed a small
group's one-erasure recovery capacity all at once — so group size / interleaving
choices that look sufficient against A and B are not guaranteed to generalize, and that
risk is inherent to the practice profiles being the easier case, not a flaw in the
reasoning above.
