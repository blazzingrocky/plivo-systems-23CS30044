# Design Options — Loss-Recovery Protocols for Sender/Receiver

Four candidate protocol designs, estimated against the measured/configured behavior of
`profiles/A.json` (loss 2%, delay 10–40ms, dup 0.5%) and `profiles/B.json` (loss 5%,
delay 20–80ms, dup 1%), calibrated against the real numbers captured in `RUNLOG.md` and
the closed-form models in `NETWORK_ANALYSIS.md`. **No code has been written for any of
these** — this is a proposal-and-estimate pass only, to pick a direction before
implementing.

All estimates use `n = 1500` frames/run (30s @ 20ms), i.i.d. per-packet loss `p` (2%/5%,
both profiles have no `burst_loss`), and the measured baseline calibration point: raw
overhead 1.02x, observed miss floor 2.27%/5.40% (matches configured `p` within sampling
noise — `RUNLOG.md`), mean one-hop delay 26ms/51.5ms, jitter ceiling ~41ms/82ms.

---

## 1. Pure retransmission (ARQ)

**Mechanism**: receiver detects a gap (missing seq after a timeout ≈ `delay_max_ms`, to
avoid firing on merely-slow packets), sends a NACK on the feedback lane (47003→47004),
sender re-sends that one frame on the media lane. No data is sent proactively — every
recovery costs a full round trip through the hostile relay.

**Miss rate**: a frame is still missing only if the original **and** the recovery both
fail. Recovery fails if either the NACK or the resend is lost:
`P(miss) = p · (1 − (1−p)²)`

| Profile | Estimate |
|---|---|
| A (p=0.02) | 0.02 × 0.0396 = **0.079%** |
| B (p=0.05) | 0.05 × 0.0975 = **0.49%** |

Both comfortably clear the 1% cap — **but only if the recovery round trip is given
enough time to land before the deadline.**

**Bandwidth overhead**: cheapest option by far — only the ~2–5% of frames that are
actually lost generate any extra traffic (a small NACK + one resend each). Extra bytes
≈ `n · p · (1−p) · 164` ≈ 4.8 KB (A) / 11.7 KB (B) on top of the ~246,000B raw stream.
**≈1.03× (A) / ≈1.06× (B)** — a large amount of the 2.0× budget goes completely unused.

**Minimum achievable delay**: the binding constraint. Detection wait (~`delay_max_ms`)
+ round trip (mean `delay_min+delay_max`, both lanes) from `NETWORK_ANALYSIS.md` §6:
**≈90ms mean (A) / ≈180ms mean (B)**, with worst-case up to 120ms/240ms. Below that
budget, a lost frame's recovery copy simply cannot arrive in time — the design
degrades toward the *baseline's* miss rate (2.27%/5.40%), not the 0.08%/0.49% figure
above. This is a hard floor, not a tunable knob: it's set by two independent hops
through the same relay, not by anything the protocol design can shrink.

**Verdict**: best bandwidth efficiency, worst delay — only viable if a fairly generous
`delay_ms` (~100–200ms) is acceptable, which is a poor position to be in on the
"lowest valid delay wins" scoring rule.

---

## 2. Duplicate transmission

**Mechanism**: send every frame twice, as two independent UDP datagrams, each taking
an independent path through the relay (independent loss/delay/dup draws per copy —
confirmed by `relay.py`'s per-packet RNG calls). No coding, no feedback.

**Miss rate**: a frame is lost only if **both** copies are lost — independent draws:
`P(miss) = p²`

| Profile | Estimate |
|---|---|
| A | 0.02² = **0.04%** |
| B | 0.05² = **0.25%** |

Best raw miss-rate numbers of any design here, with zero dependency on round-trip
timing.

**Bandwidth overhead**: this is where duplication breaks down. Sending the full
164-byte unit twice per frame: `2 × 164 / 160 = 2.05×` — **over the 2.0× cap** using
the harness's own framing overhead, and still landing at *exactly* the cap (`2×160/160
= 2.0×`) even in the impossible best case of a zero-byte header. There is no framing
choice that makes literal "send everything twice" fit under the cap with any margin at
all — the payload itself is 160 of the allowed 320 bytes/frame, before any header,
feedback, or the second copy's own header is counted.

**Minimum achievable delay**: excellent — both copies of frame `i` are ready to send
the instant frame `i` is available (no need to wait on other frames), so the floor is
the same one-hop jitter ceiling as the baseline: **≈40ms (A) / ≈80ms (B)**, no
round-trip tax.

**Verdict**: the best delay profile of all four options and a very strong miss rate,
but **not viable as literally specified** — it sits at or over the overhead cap with
no safety margin, and any real implementation (retry logic, feedback, protocol
metadata) would push it further over. A reduced form (duplicate only a fraction of
frames, or duplicate with a smaller payload) could fit, but that's a different, weaker
design than "duplicate everything."

---

## 3. XOR FEC (block parity)

**Mechanism**: group `k` consecutive data frames with one XOR parity packet
(`parity = frame_i ⊕ frame_{i+1} ⊕ ... ⊕ frame_{i+k-1}`), sent right after the last
frame in the group. Any single lost unit among the `k+1` (data or parity) is
reconstructible from the rest; two or more losses in the same group are not. No
feedback, no round trip.

**Miss rate** (per frame, derived in `NETWORK_ANALYSIS.md`-style closed form): frame
`X` is unrecoverable iff `X` itself is lost **and** at least one other unit in its
`(k+1)`-size group is also lost:
`P(miss) = p · (1 − (1−p)^k)`

| k | Overhead `(k+1)/k × 1.025` | A miss rate | B miss rate |
|---|---|---|---|
| 2 | 1.54× | 0.08% | 0.49% |
| 4 | 1.28× | 0.16% | **0.93% (marginal)** |
| 8 | 1.15× | 0.30% | 1.68% (**over cap**) |
| 16 | 1.09× | 0.55% | 2.80% (**over cap**) |

Larger groups are more bandwidth-efficient but rapidly lose their margin against B's
5% loss — `k=8` already breaches the 1% cap on B. `k=4` clears both profiles but with
almost no slack on B (0.93% against a 1.00% cap — a single unlucky run could tip over
it), which matters because **the grading profile is unseen** and may be worse than B.

**Bandwidth overhead**: the strongest of the three single-mechanism designs — even
`k=2` (1.54×) leaves comfortable headroom under 2.0×, and `k=4` (1.28×) leaves a lot.

**Minimum achievable delay**: FEC introduces a delay cost the other designs don't
have. The parity packet for a group can only be built (and sent) once its *last* frame
is available — a hard consequence of the harness delivering frame `i` no earlier than
`t0+i·20ms`. So the *first* frame in a group, if lost, cannot be reconstructed until
the parity arrives, `(k-1)·20ms` after that frame's own availability. Minimum delay ≈
jitter ceiling + group-completion wait:

| k | A (≈41ms + (k-1)·20ms) | B (≈82ms + (k-1)·20ms) |
|---|---|---|
| 2 | ≈61ms | ≈102ms |
| 4 | ≈101ms | ≈142ms |
| 8 | ≈181ms | ≈222ms |

**Verdict**: much better bandwidth economics than duplication, no round-trip tax like
ARQ — but the group-size choice is a genuine three-way trade among overhead, delay, and
margin against loss, and it has to be picked *without* knowing the actual grading
profile. `k=4` is a reasonable default but is uncomfortably close to the cap on a
B-like profile; `k=2` is safer but pays more delay and bytes for it. A smarter
sliding-window code (parity built from *past* frames only, not a future-looking block)
would remove the group-wait delay tax entirely — worth flagging as a refinement, not
designed here.

---

## 4. Hybrid ARQ + FEC

**Mechanism**: run XOR FEC (small `k`, e.g. `k=4`) as the primary, always-on defense —
it resolves the overwhelming majority of losses with zero round-trip cost. Only when a
*group* fails to reconstruct (≥2 losses in one block — the rare residual FEC can't
fix) does the receiver fall back to a NACK + resend, exactly as in design 1. Because
that fallback only fires for the already-small FEC-residual population, it's paying
ARQ's round-trip cost rarely instead of on every loss.

**Miss rate**: FEC residual, then one retry attempt on that residual:
`P(miss) = [p·(1−(1−p)^k)] · (1 − (1−p)²)`, k=4:

| Profile | FEC residual | × retry-fail | Final |
|---|---|---|---|
| A | 0.16% | × 0.0396 | **≈0.006%** |
| B | 0.93% | × 0.0975 | **≈0.09%** |

An order of magnitude better margin under the cap than any single mechanism above, on
both profiles — and specifically **more robust to a worse-than-B or bursty unseen
grading profile**, since ARQ is still there to mop up whatever FEC's fixed group size
couldn't (including correlated/burst losses that defeat a `k=4` XOR block outright).

**Bandwidth overhead**: FEC's `k=4` baseline (1.28×) plus the ARQ layer's traffic,
which now only fires on the FEC residual (≈0.16%/0.93% of frames) rather than the raw
loss rate (2%/5%) — a small addition, order `+0.02–0.05×`. **≈1.3× (A) / ≈1.35× (B)**,
still with a large margin under the 2.0× cap — room to strengthen the FEC layer
further if desired.

**Minimum achievable delay**: the key result is that this is **not** the sum of FEC's
and ARQ's delay costs. The ARQ backstop only needs to win its round trip for the
already-rare FEC-residual frames; since that residual is already far under the 1% cap
on its own, the delay budget doesn't have to be sized to guarantee the backstop always
lands in time — it can be sized for the FEC layer's normal-case requirement (same as
design 3), and the ARQ layer is upside (helps in worse-than-modeled conditions, e.g.
burst loss) rather than something the score has to pay for. **≈100ms (A) / ≈140ms
(B)** — same floor as pure FEC at `k=4`, not pure ARQ's ~90–180ms.

**Verdict**: best miss-rate margin of all four by a wide factor, overhead well inside
budget, delay floor matching FEC (not ARQ), and the only design with a real answer to
"what if the real grading profile is worse than B." Costs more implementation
complexity than any single mechanism (needs the feedback lane, group bookkeeping, *and*
retry/dedup logic, whereas the baseline uses none of that today).

---

## Comparison

| Design | A miss rate | B miss rate | A overhead | B overhead | A min delay | B min delay |
|---|---|---|---|---|---|---|
| Pure ARQ | 0.08%¹ | 0.49%¹ | 1.03× | 1.06× | ~90–100ms | ~180–200ms |
| Duplicate ×2 | 0.04% | 0.25% | **2.05× (over cap)** | **2.05× (over cap)** | ~40ms | ~80ms |
| XOR FEC (k=4) | 0.16% | 0.93% (marginal) | 1.28× | 1.28× | ~100ms | ~140ms |
| Hybrid FEC(k=4)+ARQ | **0.006%** | **0.09%** | 1.3× | 1.35× | ~100ms | ~140ms |

¹ only realized if `delay_ms` is generous enough (~90–100ms A, ~180–200ms B) for the
round trip to complete; below that, pure ARQ collapses toward the baseline's raw miss
rate.

## Recommendation

**Hybrid ARQ + FEC.** It dominates on the metric that matters most for validity
(miss rate has the widest margin under the 1% cap on both profiles, including the one
— B at `k=4` — where plain FEC is uncomfortably marginal), while matching pure FEC's
delay floor rather than paying ARQ's round-trip tax on every loss. Overhead has ample
headroom (~0.65–0.7× spare under the cap) to tighten the FEC group size further if
real measurements on an unseen grading profile show it's needed. Its main cost is
implementation complexity — it's the only design that needs both a working feedback
channel and FEC group bookkeeping — but that complexity buys the one property none of
the single-mechanism designs have: a real answer for "what if the actual network is
worse than what we tuned against."

Plain XOR FEC (`k=2`, the safer group size) is a reasonable fallback if the added
feedback-channel/ARQ complexity turns out not to be worth it — it's the second-best
margin (0.08%/0.49%), still well under the overhead cap (1.54×), at a delay cost
(~61–102ms) not much worse than the hybrid's.
