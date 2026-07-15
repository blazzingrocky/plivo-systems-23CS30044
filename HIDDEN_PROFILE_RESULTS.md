# Hidden Grading Profile Results

Six synthetic profiles built to exercise **every** impairment mechanism `relay.py`
supports (`burst_loss` Gilbert–Elliott state machine, `spike` delay, base `loss`/`dup`,
and jitter-width-driven reordering), then benchmarked against the current design
(backward sliding XOR FEC r=1:2 w=3 + dormant ARQ backstop). All runs: 30 s = 1500
frames, seed 1, overhead constant **1.55×** (never the binding constraint).

## The profiles and why each is a realistic hidden test

| # | File | Key parameters | Why it's a realistic hidden test |
|---|---|---|---|
| 1 | [C_burst_mild](profiles/C_burst_mild.json) | loss 1%, burst{enter .015, exit .5, in-burst .5}, 10–40 ms | Real links lose packets in short clusters (brief Wi-Fi interference), not the i.i.d. loss of practice A/B. Mean burst ≈2. |
| 2 | [D_burst_moderate](profiles/D_burst_moderate.json) | loss 1.5%, burst{enter .04, exit .33, in-burst .65}, 20–60 ms | Congestion-era loss: bursts mean ≈3, longer tails. The regime that separates "isolated loss" codes from burst-aware ones. |
| 3 | [E_burst_severe](profiles/E_burst_severe.json) | loss 2%, burst{enter .06, exit .25, in-burst .8}, 20–80 ms | Cellular handover / deep fade: long correlated outages (mean ≈4, tail to 9). Stress ceiling. |
| 4 | [F_spikes](profiles/F_spikes.json) | loss 2%, 10–40 ms, spike{prob 5%, +150 ms} | Bufferbloat / scheduler stalls: most packets fast, a few catastrophically delayed. Tests the jitter buffer and deadline logic, not loss. |
| 5 | [G_burst_spikes](profiles/G_burst_spikes.json) | loss 2%, burst{enter .04, exit .3, in-burst .7}, 20–60 ms, spike{prob 4%, +120 ms} | The realistic nightmare: correlated loss *and* latency spikes together (a congested mobile link). |
| 6 | [H_reorder](profiles/H_reorder.json) | loss 0.8%, 5–100 ms, dup 2% | Multipath / ECMP reordering: very wide jitter (95 ms span) reorders packets several positions and duplicates often. Tests reorder/dedup robustness. |

## Benchmark — burst profiles C/D/E (full delay sweep)

miss % (cap 1.00%); overhead 1.55× throughout; ARQ never fired (0 NACKs):

| delay_ms | C_burst_mild | D_burst_moderate | E_burst_severe |
|---:|---:|---:|---:|
| 50  | 1.07% INVALID | 23.07% INVALID | 54.53% INVALID |
| 60  | **0.40% VALID** | 7.27% INVALID | 38.93% INVALID |
| 70  | 0.27% VALID | 4.80% INVALID | 27.47% INVALID |
| 80  | 0.20% VALID | 3.67% INVALID | 17.87% INVALID |
| 90  | 0.20% VALID | 3.20% INVALID | 16.40% INVALID |
| 100 | 0.13% VALID | 2.80% INVALID | 14.93% INVALID |
| 110 | 0.13% VALID | 2.60% INVALID | 13.93% INVALID |
| 120 | 0.07% VALID | 2.47% INVALID | 13.13% INVALID |

The D/E curves **flatten well above 1%** — adding delay stops helping because the misses
are *unrecoverable residual*, not lateness. Diagnostic at delay 120:

| profile | residual (never delivered) | late | stuck parity groups | undelivered burst segments (mean / max len) |
|---|---:|---:|---:|---|
| C mild | 1 (0.07%) | 1 | 0 | max len **1** (chaining fixed every multi-loss) |
| D moderate | **35 (2.33%)** | 2 | 9 | mean 1.94 / **max 5** |
| E severe | **188 (12.5%)** | 9 | 38 | mean 2.77 / **max 9** |

## Benchmark — F/G/H at the recommended delay (110 ms)

| profile | miss % | RESULT | residual | late | max reorder depth | note |
|---|---:|---|---:|---:|---:|---|
| F_spikes | 0.67% | **VALID** | 2 (0.13%) | 8 (0.53%) | 8 | FEC parity rescues most spike-delayed frames |
| G_burst_spikes | 6.60% | **INVALID** | 75 (5.0%) | 24 (1.6%) | 7 | burst residual + spike lateness stack |
| H_reorder | 0.07% | **VALID** | 0 | 1 | 4 | 571 reorder events absorbed cleanly |

## What the data says

- **Reordering is a non-issue.** H produced 571 reorder events (mean depth 1.8, max 4)
  and still hit 0.07% — the sequence-indexed buffer + dedup handle arbitrary reordering
  and 2% duplication with zero misses. F reordered to depth 8 (spikes) and still passed.
- **Spikes alone are survivable.** F passes at 110 ms despite 5% of packets arriving at
  160–190 ms. The reason is emergent and worth noting: each frame's **parity travels an
  independent relay path**, so when a frame's DATA is spike-delayed, its (un-spiked)
  parity often reconstructs it *before* the deadline. FEC doubles as spike insurance.
- **Burst loss is the single failure mode.** The moment bursts routinely exceed one
  consecutive loss inside a 3-frame window (D onward), single-parity XOR — even with its
  overlapping-window chaining — leaves a hard residual (2.3% D, 12.5% E) that no delay
  removes. The `stuck parity groups` and `max burst len` columns are the fingerprint.

---

## Answers

**1. Does the current sliding XOR remain valid?**
Partially. It stays **VALID under mild burst (C, ≥60 ms), pure delay spikes (F, 110 ms),
and heavy reordering (H, 110 ms)** — the isolated-loss and jitter/reorder regimes. It is
**INVALID under moderate burst (D), severe burst (E), and combined burst+spikes (G)** at
every delay tested. So it is robust to jitter, reordering, duplication, and *isolated*
loss, but **not to correlated burst loss**.

**2. Which impairment causes failure first?**
**Burst loss** — specifically once the mean burst length pushes two or more losses into a
single 3-frame parity window. D_burst_moderate is the first to fail (2.5% residual floor)
and the failure is *residual*, not lateness (late stays <0.3% even at 120 ms). Spikes do
**not** cause failure first (F passes), because parity provides an independent-path
rescue. Burst is both the earliest and the dominant killer; in the combined profile G its
5% residual, not the 1.6% spike lateness, is what busts the cap.

**3. Would interleaving help?**
**Yes — decisively, and it targets exactly the failure mode.** Interleaving spreads a
burst of consecutive losses across *different* parity groups, converting a fatal
"≥2 losses in one window" into isolated single losses that XOR already recovers. With
stride ≥ the max burst length (≈5 for D, ≈9 for E), the D/E/G residual would largely
collapse. It adds nothing to F/H (already valid) and costs no extra overhead — just a
strided index map. This is the highest-value upgrade.

**4. Would a second parity stream help?**
**Yes, complementary but weaker alone.** A second independent parity per window raises
per-window recovery from 1 to 2 erasures, fixing the common adjacent-double case (much of
D). But a burst of 3+ consecutive losses in one window still defeats two parities, so it
does **not** fully solve E/G by itself. Best combination: **interleaving (to break up long
bursts) + a second parity stream (to raise per-group capacity)** — together they cover
both clustered and spread losses. If forced to pick one for burst robustness, interleaving
generalizes better to long bursts. Overhead room exists for both: two parities per 2
data frames is ~2.0× (at the cap); interleaving alone stays at 1.55×.

**5. What grading delay should we finally submit?**
**110 ms.** The design is **validity-limited by burst loss, not by delay** — the D/E/G
failures are flat across 50–120 ms, so no delay choice rescues them (only a mechanism
change would). Among the profiles the current mechanism *can* pass (practice A/B plus
synthetic C/F/H), the binding minimum valid delays are 100 ms (B), 60 ms (C), ~110 ms (F,
spikes push lateness), 100 ms (H, = its delay_max). **110 ms is the lowest delay valid on
all of them with margin**, and since grading uses unseen profiles that may resemble
F-style spikes, 110 ms is the safe robust submission. Going to 100 ms risks spike-heavy
profiles; going higher wastes score without buying burst robustness. If burst-aware FEC
(answer 3/4) is added later, the submittable delay could return toward the ~60–100 ms
jitter floor *with* burst validity — but for the design as it stands today, **submit
110 ms**.
