# RUNLOG

Concise experiment log. Four major versions, each building on the last. Full experiment
detail (per-run breakdowns, hidden-profile stress tests, design derivations) lives in the
companion docs referenced inline; this file records what was tested, what changed, and
why, at each version boundary.

---

## V0 — Baseline forwarding

**What**: the original naive `sender.c`/`receiver.c` — receive, forward unchanged, once.
No sequencing, no recovery, no state.

**Why tested**: establish the failure floor before changing anything.

| Profile | delay_ms | miss % | overhead | Result |
|---|---|---|---|---|
| A (loss 2%) | 40–120 | 2.27–7.07% (floors at 2.27% once delay ≥60) | 1.02× | INVALID at every delay |
| B (loss 5%) | 40–120 | 5.40–71.07% (floors at 5.40% once delay ≥100) | 1.02× | INVALID at every delay |

**Finding**: raw i.i.d. packet loss alone exceeds the 1% miss cap regardless of
`delay_ms` — no delay choice can fix a design with zero loss recovery. Overhead sits at
1.02× (just the 4-byte seq header), meaning ~1× of the 2.0× budget is completely unused.

---

## V1 — Sequence tracking + jitter/reordering handling

**What**: added per-sequence-number receiver state, duplicate suppression (each seq
forwarded to the player at most once), reorder-tolerant slot-filling, and a
release-immediately playout policy (a frame is forwarded the instant a correct copy
exists — never delayed artificially, since the harness player only credits the first
on-time arrival).

**Why**: this is prerequisite infrastructure for any recovery mechanism — FEC/ARQ both
need per-seq bookkeeping and dedup to be safe to layer on top. It does **not** by itself
recover lost packets, so it was implemented together with V2 rather than benchmarked in
isolation; standalone it would still fail the miss-rate cap for the same reason V0 does
(no redundancy). Its value is structural: correctness of buffering/dedup/reorder is a
precondition, verified as part of the V2 results below (0 correctness failures across
every run — `score.py`'s payload hash check never flagged a wrong-payload delivery).

---

## V2 — Backward sliding-window XOR FEC

**What**: sender emits a parity packet every 2 data frames, built backward from the last
3 frames already sent (`parity_i = frame_i ⊕ frame_{i-1} ⊕ frame_{i-2}`) — ready to send
the instant its newest member exists, no waiting on future frames. Receiver runs a
fixpoint XOR solver: any parity equation with exactly one unknown member is resolved,
repeating until nothing more can be recovered (chained recovery across overlapping
windows).

**Why backward, not forward-block**: a future-looking block code can't emit its parity
until the *last* frame of the group arrives, costing early group members up to
`(k-1)×20ms` of avoidable delay. Backward orientation removes that tax entirely, as
confirmed by the results below.

| Profile | delay_ms sweep | Min valid delay | Miss % at min | Overhead |
|---|---|---|---|---|
| A | 40,50,60,70,80,90,100,110,120 | **50 ms** | 0.80% | 1.55× |
| B | 40,50,60,70,80,90,100,110,120 | **100 ms** | 0.67% | 1.55× |

**Finding**: FEC recovers the overwhelming majority of losses (33/34 true losses on A,
82/85 on B) at a constant 1.55× overhead, comfortably under the 2.0× cap. A small residual
remains on both profiles (1 frame A, 3 frames B) — double losses inside one 3-frame
window, which single-parity XOR cannot repair by construction.

---

## V3 — ARQ backstop evaluation

**What**: added a retransmission backstop, FEC left unchanged. Receiver NACKs a frame
only when (a) FEC could no longer recover it (past its last covering parity) **and**
(b) a full round trip still fits before the deadline. Sender caches recent payloads and
resends on NACK; receiver suppresses duplicate retransmissions.

**Why**: target the V2 residual directly — the double-loss-in-window frames FEC can't
fix are exactly what a backstop retry should mop up.

| Profile | delay_ms sweep | NACKs fired (40–110ms) | Min valid delay | Overhead |
|---|---|---|---|---|
| A | 40,50,60,70,80,90,100,110 | 0 | 50 ms (unchanged) | 1.55× |
| B | 40,50,60,70,80,90,100,110 | 0 | 100 ms (unchanged) | 1.55× |

**Finding**: ARQ **never fired** anywhere in the 40–110ms grading range — the round trip
(NACK + resend, ≈2× one-way delay) costs more time than any competitive delay budget
allows, so the trigger condition (b) is never satisfied. A separate demonstration at
B/delay=350ms confirmed the mechanism itself works when given room (6 NACKs, 4
recoveries, residual driven 3→0), but that delay is far outside the scored range. **ARQ
adds no value at competitive delays and does not change the minimum valid delay.**

---

## Final conclusions

- **Profile A valid at 50 ms.**
- **Profile B valid at 100 ms.**
- **Final recommended grading delay: 110 ms.**
- **Overhead ≈1.55×** (constant across every run, well under the 2.0× cap).
- Stress-testing against six synthetic hidden profiles (`HIDDEN_PROFILE_RESULTS.md`)
  showed the design is robust to jitter, reordering, duplication, and isolated loss, but
  **remaining failures are caused by burst-correlated multi-loss events** — once a burst
  exceeds the 3-frame parity window, the residual is unrecoverable by this design.
- **Increasing delay beyond 110 ms does not materially help**: on the profiles that fail
  (moderate/severe burst loss), the miss rate is a flat residual floor across the entire
  50–120ms range, not a lateness problem — more delay cannot deliver a packet that was
  never received and never reconstructed. 110 ms is chosen for margin against
  unseen spike-heavy profiles, not because burst-loss profiles need it (they can't be
  fixed by delay at all; that requires burst-aware FEC — see `HIDDEN_PROFILE_RESULTS.md`
  for the interleaving/second-parity-stream analysis).
