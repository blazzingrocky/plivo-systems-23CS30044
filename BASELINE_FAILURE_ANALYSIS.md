# Baseline Failure Analysis — The Flaky Network

Read-only analysis of why `sender.c` + `receiver.c`, as they exist in this repo today,
cannot produce a VALID run. No code was changed to produce this document. See
`ARCHITECTURE_ANALYSIS.md` for the mechanics referenced below.

## 6. Why the baseline implementation fails under packet loss

### The core defect: zero loss recovery, anywhere

The baseline sender transmits each frame **exactly once** and retains no copy of it
after the `sendto` call (`sender.c:42-47`). The baseline receiver has no notion of a
missing sequence number — it only ever reacts to packets that *do* arrive
(`receiver.c:41-47`). Neither process reads the feedback ports (47003/47004) that the
architecture reserves for exactly this purpose. UDP itself provides no retransmission.
The consequence: **any datagram the relay drops on the uplink is gone forever** — there
is no ACK, no NACK, no timeout-and-resend, no forward error correction, no redundant
copy sent proactively. A dropped frame is not a frame that arrives late; it is a frame
that never arrives at all, and `present` can never become `True` for it
(`endpoints.py:58-61`).

### This alone already exceeds the miss-rate cap, independent of `delay_ms`

The relay's loss model is i.i.d. per packet on the uplink (`Impair.drop`,
`relay.py:30-42`, since neither practice profile sets `burst_loss`). For a 30 s run,
`n = duration*1000 // FRAME_MS = 1500` frames (`endpoints.py:78`). Each uplink datagram
independently survives with probability `1 - loss`:

| Profile | `loss` | Expected drops over 1500 frames | Resulting miss rate | Cap  |
|---------|--------|----------------------------------|----------------------|------|
| A (`A_mild`)     | 0.02 | ~30 | ~2.0% | 1.0% |
| B (`B_moderate`) | 0.05 | ~75 | ~5.0% | 1.0% |

Both already breach the 1% cap from loss alone — **before accounting for any frames
that arrive correctly but late**. This is true at *any* `delay_ms`, including a very
generous one, because no amount of extra playout budget helps a packet that was never
delivered. This matches the assignment's own framing: "It will be INVALID — every
dropped packet is a glitch." The baseline's failure is fundamentally a **loss problem,
not a timing problem** — increasing `delay_ms` alone cannot fix it.

### Timing headroom is actually generous, which isolates the real cause

Because the baseline receiver adds essentially zero processing delay of its own (it
forwards immediately on arrival, `receiver.c:43-46`), the entire `delay_ms` budget is
spent purely on relay transit time. The relay's own delay range tops out at
`delay_max_ms` (40 ms for profile A, 80 ms for profile B) plus a possible spike. At the
assignment's suggested starting point (`--delay_ms 40` against profile A), most
correctly-delivered packets that experience typical (non-spiked) delay will still land
inside the deadline. This confirms that the dominant miss cause is *loss*, not
*lateness*: a packet that isn't dropped is arriving in time to matter, which is exactly
why fixing timing without fixing loss would not move the score.

### Duplicates and reordering are already handled, but incidentally

The relay both duplicates (`dup` probability, ~0.5–1% in the practice profiles) and
reorders (independent random per-packet delay naturally reorders packets sent close
together). Neither currently breaks the baseline, but not because the baseline handles
them — because the harness *player* absorbs both for free: it keys arrivals by sequence
number and only records the first (`endpoints.py:52`), so duplicates are inert, and it
judges each sequence number against its own independent deadline, so out-of-order
arrival of *different* frames doesn't inherently cost anything. This is a property of
the harness, not evidence that the receiver's pass-through design is sound — a receiver
performing its own reordering/gap-detection logic would still need to replicate
equivalent care once it stops being a pure pass-through.

### The bandwidth budget is almost entirely unused

`score.py:39-40` computes overhead as
`(relay up_bytes + relay down_bytes) / (n * PAYLOAD_BYTES)`, and `relay.py:97` counts
bytes **on ingress, before the drop decision** — so every byte the baseline sender
transmits counts toward overhead whether or not the relay later drops it. For the
baseline:

```
up_bytes  ≈ n * 164   (each frame forwarded once, unmodified, 4-byte seq + 160-byte payload)
down_bytes ≈ 0         (feedback path never used)
raw        = n * 160
overhead   ≈ (n*164) / (n*160) = 164/160 ≈ 1.025×
```

That's roughly **1.03×**, far under the 2.0× cap — the entire baseline failure is
concentrated in the miss-rate cap, while ~1× of the 2× bandwidth budget sits completely
unspent. That headroom exists specifically to be spent on loss recovery (retransmission,
redundancy/FEC, or some other proactive strategy) — the assignment's "things worth
thinking about" prompts (cost of a lost packet, cost of a request+reply round trip
through the same hostile relay, what could be sent *ahead of time* instead) are all
pointing at how to spend that unused budget, not something this baseline currently
attempts in any form.

### Why a naive "ask again" fix is not free

Any retransmission-based repair has to cross the hostile relay twice more per recovered
frame: a request from receiver→sender over the feedback lane (up to `delay_max_ms` on
that lane, itself lossy and duplicable) and a resend from sender→receiver back over the
media lane (another independent draw from the same delay/loss distribution). At a 20 ms
frame cadence, a round trip through two more hostile hops can easily exceed a tight
`delay_ms` budget, and the request or the resend can itself be dropped, requiring
further recovery. This is a real cost, not a reason recovery is impossible — it's the
central tradeoff the assignment is built around — but it explains why the baseline's
complete absence of *any* redundancy (not just absence of retransmission specifically)
is the more fundamental gap: relying solely on request/resend recovery is expensive
relative to the 20 ms frame interval, which is exactly why the unused overhead budget
matters.

## 7. What metrics the grader computes

Everything below is computed once, offline, by `score.py`, from the two artifacts a run
produces: `playout_log.json` (player thread) and `relay_stats.json` (relay).

**1. Deadline-miss rate** (`score.py:28-37`) — for each of the `n` frames:
- If the player never recorded an on-time first arrival (`present == False`), it's a
  miss.
- Otherwise, `score.py` independently recomputes the *expected* payload —
  `sha256(frame_payload(stream_seed, i))` — using the seed that was piped only to
  `endpoints.py`, and compares it against the `sha` the player recorded from the
  arrived bytes. A mismatch (wrong or corrupted payload) is **also** counted as a miss,
  even though the player log alone had marked it present. This is the check that
  guards against a receiver that plays out *something* on time but forwards the wrong
  bytes (e.g. a reused/garbled buffer or a sequence mixup in a custom protocol).
- `miss_rate = misses / n`. **Cap: `miss_rate <= 0.01` (1%).**

**2. Bandwidth overhead** (`score.py:39-40`) —
`overhead = (relay_stats.up_bytes + relay_stats.down_bytes) / (n * PAYLOAD_BYTES)`.
This is every byte the relay saw in *either* direction — media uplink and feedback
downlink combined — including bytes belonging to packets the relay subsequently dropped
(counted on ingress, `relay.py:96-98`), divided by the raw, uncoded stream size
(`n * 160`). Any header the student protocol adds, any retransmission, any duplicate
proactive send, and any feedback traffic all inflate this number directly.
**Cap: `overhead <= 2.0` (2.0×).**

**3. Playout delay** — simply the `delay_ms` value the run was invoked with, echoed
back from `playout_log.json` (`score.py:45`). Not computed from measurements — it's the
independent variable the student chose for that run.

**4. Validity gate and ranking** (`score.py:48-51`) —
`valid = (miss_rate <= 0.01) and (overhead <= 2.0)`. A run that fails either cap prints
`INVALID` regardless of how low its `delay_ms` was. Among **valid** runs, the grading
rule (stated in the assignment, enforced by whoever compares run logs — `score.py`
itself only scores one run at a time) is: **lowest `delay_ms` wins; overhead breaks
ties.** A run's own printed report always shows all four numbers plus the `VALID` /
`INVALID` verdict, with a hint to fix misses and overhead before chasing a lower delay
when the run is invalid.

Applied to the current baseline: profile A yields `overhead ≈ 1.03×` (comfortably under
cap) and `miss_rate ≈ 2%` (over cap) → `INVALID`; profile B is worse on miss rate
(`≈5%`) for the same near-1× overhead. Both fail on exactly one axis, and it's the same
axis in both cases.
