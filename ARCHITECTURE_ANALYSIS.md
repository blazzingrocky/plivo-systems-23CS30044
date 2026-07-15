# Architecture Analysis — The Flaky Network

Read-only analysis of the harness and baseline as they exist in this repo. No code was
changed to produce this document.

## 1. Overall architecture

Five processes, all UDP, all on `127.0.0.1`, orchestrated per-run by `run.py`:

```
harness source --47010--> YOUR SENDER --47001--> relay --47002--> YOUR RECEIVER --47020--> harness player
                                ^                                        |
                                |                                        v
                              47004 <----------------- relay <-------- 47003
                                        (feedback lane, optional, unused by baseline)
```

| Port  | Direction                          | Owner (binds)     | Format                    |
|-------|-------------------------------------|--------------------|---------------------------|
| 47010 | harness source → sender             | sender             | fixed (harness)           |
| 47001 | sender → relay (media, uplink)      | relay              | free (student-designed)   |
| 47002 | relay → receiver (media, downlink)  | receiver           | free (student-designed)   |
| 47003 | receiver → relay (feedback, up)     | relay              | free (student-designed)   |
| 47004 | relay → sender (feedback, down)     | sender             | free (student-designed)   |
| 47020 | receiver → harness player            | harness (player)   | fixed (harness)           |

`common.py` is the only file shared by every process (constants + the deterministic
payload generator); it is never imported by student code — the contract between the
harness and the student binaries is purely "bytes on these UDP ports," per
`endpoints.py:1-4`.

**Process lifecycle** (`run.py`):
1. Deletes any stale `playout_log.json` / `relay_stats.json` so a previous run can never
   be scored as the current one (`run.py:41-46`).
2. Generates a random 8-byte hex `stream_seed` (`secrets.token_hex(8)`) and sets
   `t0 = time.time() + 1.5` — 1.5 s of slack for process startup (`run.py:48-49`).
3. Launches, in order: `relay.py` (no env needed), the receiver, then the sender — each
   student process gets `T0`, `DURATION_S`, `DELAY_MS` as environment variables
   (`run.py:50-65`).
4. Sleeps 0.3 s and polls each process; if any has already exited it aborts with a hint
   about the port it probably failed to bind (`run.py:66-71`).
5. Launches `endpoints.py`, piping `stream_seed` over **stdin**, not argv/env — stdin is
   not readable by the sender/receiver processes, so the payload-generating seed never
   becomes visible outside the harness (`run.py:72-80`, `endpoints.py:73-77`). This is
   what lets `score.py` later verify payload correctness without the student ever being
   able to precompute or replay expected bytes.
6. Waits for `endpoints.py` to finish (it runs for `DURATION_S` plus ~0.5 s of drain
   time), then kills the student sender/receiver and waits for the relay to flush its
   stats file (`run.py:82-91`).
7. Invokes `score.py` with the `stream_seed` and duration to compute the final report
   from the two JSON artifacts written by the player thread and the relay
   (`run.py:97-98`).

The whole thing is single-shot: one `t0`, one seed, one delay budget per invocation of
`run.py`.

## 2. How `sender.c` and `receiver.c` currently work

Both are minimal, single-threaded, blocking UDP relayers — intentionally naive
(explicitly called out as "naive on purpose" in their header comments).

**`sender.c`** (`sender.c:24-49`):
- Binds one socket to `127.0.0.1:47010` to receive frames from the harness source.
- Opens a second unbound socket used only as a client to send toward the relay at
  `127.0.0.1:47001`.
- Loop: blocking `recvfrom` on the 47010 socket → immediately `sendto` the exact same
  bytes, unmodified, to the relay. No buffering, no sequence tracking, no retry state.
- It never binds or reads anything on port 47004 (the feedback-from-receiver lane)
  despite the header comment noting it's available — the baseline does not implement
  feedback at all.
- It never reads `T0`, `DURATION_S`, or `DELAY_MS` from the environment even though
  `run.py` provides them — the baseline is fully stateless and protocol-agnostic.
- Each frame is sent **exactly once**. If the relay drops that datagram, there is no
  second chance — the sender has already forgotten it (it isn't kept anywhere after the
  `sendto` call returns).

**`receiver.c`** (`receiver.c:23-49`):
- Binds one socket to `127.0.0.1:47002` to receive frames relayed from the sender.
- Opens a second unbound socket used only as a client to send toward the harness player
  at `127.0.0.1:47020`.
- Loop: blocking `recvfrom` on 47002 → immediately `sendto` the same bytes, unmodified,
  to the player. No reordering, no jitter buffer, no gap detection, no de-duplication
  logic of its own.
- It never sends anything on port 47003 (the feedback-to-sender lane) — like the sender,
  it ignores the feedback path entirely.
- Duplicates arriving from the relay are forwarded twice to the player, but this happens
  to be harmless only because the harness *player* (not the receiver) deduplicates by
  sequence number, keeping only the first arrival per `i` (`endpoints.py:52`,
  `first_arrival[i]` is set once and never overwritten).
- Both loops block indefinitely on `recvfrom` with no socket timeout. That is safe for
  the sender (the harness source paces it at exactly one frame per 20 ms) and safe for
  the receiver only because there's nothing else the receiver needs to be doing
  concurrently (no retransmit timers, no periodic feedback) — a receive loop that must
  also fire timers later cannot use this pattern unmodified.

In short: both binaries currently implement `cp stdin stdout` over UDP. All of the
"jitter buffer / reorder / recovery logic goes here" (`receiver.c:44`) and "your protocol
design goes here" (`sender.c:45`) is unwritten.

## 3. Exact packet formats

There are two distinct format domains:

**Harness-fixed format** — used on the two legs the harness itself terminates (47010
in, 47020 out). Defined once in `common.py:10` as `HEADER_FMT = "!I"` (network-order,
i.e. big-endian, unsigned 32-bit) plus a fixed `PAYLOAD_BYTES = 160`:

```
byte offset:   0        1        2        3        4 ................. 163
field:        [------ seq (uint32, big-endian) ------][------ payload (160 bytes) ------]
total size:    164 bytes
```

- `endpoints.py:33` builds this on the source side: `struct.pack("!I", i) + frame_payload(seed, i)`.
- `endpoints.py:51` parses it on the player side: `struct.unpack("!I", data[:4])`, with
  everything from offset 4 onward treated as opaque payload and hashed
  (`hashlib.sha256(data[4:])`) for later verification.
- The payload itself is not arbitrary — `common.py:21-29` derives it deterministically
  from `sha256(f"{seed}:{i}:{j}")` chunks, truncated to 160 bytes. Only the harness
  processes (`endpoints.py`, `score.py`) ever see `seed`, so nobody outside the harness
  can precompute or fake a valid payload for frame `i`.

**Wire format between sender and receiver** (47001/47002/47003/47004) — explicitly
"entirely your design" per the assignment PDF and the header comments in both `.c`
files. The current baseline does not design anything here: because it copies the raw
164-byte harness buffer through unmodified in both directions, the de-facto wire format
on 47001/47002 today is identical to the harness-fixed format above, and 47003/47004
carry nothing at all (never used). Any redesign of the sender/receiver protocol is free
to change the bytes on these four ports as long as the receiver still emits the
harness-fixed 164-byte format on 47020.

## 4. How `run.py`, `relay.py`, and `score.py` interact

```
run.py
 ├─ spawns relay.py     --profile P --seed S --duration D   (writes relay_stats.json at exit)
 ├─ spawns receiver_cmd  (env: T0, DURATION_S, DELAY_MS)
 ├─ spawns sender_cmd    (env: T0, DURATION_S, DELAY_MS)
 ├─ spawns endpoints.py --t0 --duration --delay_ms, seed piped via stdin
 │                                                            (writes playout_log.json at exit)
 ├─ waits for endpoints.py, then kills sender/receiver, then waits for relay
 └─ spawns score.py --stream_seed S --duration D
                                                              (reads both JSON files, prints report)
```

**`relay.py`** is the hostile network. It binds the two inbound lanes (`up_in` on
47001, `dn_in` on 47003) and, per received datagram:
1. Counts it toward `up_bytes`/`up_pkts` or `down_bytes`/`down_pkts` **before** deciding
   anything else (`relay.py:96-98`) — so a packet that gets dropped a moment later has
   already been charged to the overhead total.
2. Rolls a drop decision (`Impair.drop`, `relay.py:30-42`): a plain i.i.d. loss
   probability (`p["loss"]`), optionally combined with a Gilbert–Elliott burst-loss
   model if the profile defines a `burst_loss` block (neither `profiles/A.json` nor
   `profiles/B.json` sets this key, so both practice profiles use plain i.i.d. loss
   only — grading profiles are explicitly allowed to differ).
3. If not dropped, draws a delay uniformly from `[delay_min_ms, delay_max_ms]`, with an
   optional additive "spike" applied with some probability (`Impair.delay_s`,
   `relay.py:44-50`), and schedules delivery on a min-heap keyed by release time
   (`relay.py:102`).
4. Optionally duplicates the packet (`Impair.dup`, `relay.py:52-53`), pushing a *second*
   heap entry with an independently-drawn delay — so a duplicate can arrive before or
   after its original.
5. A separate loop drains the heap whenever `release_time <= now` and forwards to the
   opposite lane's destination (up → `RELAY_TO_RECV` 47002, down → `RELAY_TO_SEND`
   47004).

Each lane (`up`, `down`) gets its own `Impair` instance seeded from `--seed` and
`--seed + 1` respectively, so uplink and downlink impairments are independent but
reproducible for a fixed input packet sequence. The relay explicitly warns
(`relay.py:7-9`) that reproducibility only holds if the student sends the identical
packet sequence — any adaptive/retry logic changes what the RNG stream sees, which
shifts the realized loss/delay pattern even at a fixed `--seed`.

At exit, `relay.py:113-114` writes `relay_stats.json`:
`{up_bytes, down_bytes, up_pkts, down_pkts, dropped, duplicated}`.

**`score.py`** consumes exactly two artifacts:
- `playout_log.json` (written by the player thread in `endpoints.py:62-63`): `delay_ms`
  plus a `frames` array of `{i, present, sha}`.
- `relay_stats.json` (written by `relay.py`, as above).

It never talks to the network itself and never sees the sender/receiver — it is a pure
offline verifier over the two JSON files (see §6/§7 in the companion failure-analysis
document for what it actually computes and why the baseline fails it).

## 5. How deadlines are computed

There is a single anchor time `t0` per run, chosen by `run.py` as
`time.time() + 1.5` seconds — a fixed point in the near future shared by every process
via env var (student binaries) or CLI arg (harness processes).

**Frame availability** — the *earliest* moment frame `i` can possibly exist at the
sender — is fixed by the harness source thread:

```
available(i) = t0 + i * FRAME_MS / 1000        (FRAME_MS = 20)
```

`endpoints.py:30-34` sleeps until this wall-clock instant, then sends frame `i` to
47010. This is a hard physical constraint: the source thread cannot and does not send a
frame early, so no sender-side design can beat this floor.

**Frame deadline** — the *latest* moment a correct copy of frame `i` may arrive at the
harness player (port 47020) to still count — is fixed by the player thread using the
same `t0` plus the run's chosen playout-delay budget:

```
deadline(i) = t0 + delay_ms / 1000 + i * FRAME_MS / 1000
            = available(i) + delay_ms / 1000
```

So every frame gets an identical, fixed `delay_ms` **end-to-end budget** — measured from
the instant the harness source theoretically could have sent it, to the instant it must
be sitting (correctly) on the player's socket. That budget must cover: time between
frame-available and the sender's actual `sendto`, uplink transit + relay impairment
delay, sender→receiver processing, downlink transit + relay impairment delay, and
receiver→player `sendto` + kernel/socket delivery.

**What counts as "on time"** (`endpoints.py:37-61`): the player thread listens
continuously and records, per sequence number, only the **first** arrival
(`first_arrival[i]`, never overwritten by a later duplicate or a stale retransmission).
After the run window closes, each frame `i` is scored `present = True` iff a first
arrival was recorded **and** its wall-clock arrival time was `<= deadline(i)`. Content
correctness is checked separately, later, offline by `score.py` (see the companion
document) — a frame that arrives on time but with the wrong bytes is still ultimately
counted as a miss once `score.py` runs, even though the player log alone would have
marked it "present."

`delay_ms` is the single independent variable the student controls per run
(`--delay_ms` to `run.py`) and it is also the score that gets minimized once a run is
valid — tightening it directly tightens every frame's deadline uniformly, and it is the
only parameter of the entire scoring formula that isn't a measured outcome.
