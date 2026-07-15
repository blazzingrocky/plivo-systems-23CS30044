# NOTES

The sender forwards each harness frame once (tagged with a type byte) and additionally
emits a backward-looking XOR parity every 2 frames over the last 3 frames
(`parity_i = frame_i ⊕ frame_{i-1} ⊕ frame_{i-2}`). Parity is built only from frames
that already exist, so it is ready to send the instant its newest member arrives — this
backward orientation was chosen specifically to avoid the `(k-1)×20ms` "wait for a
future frame" delay tax a forward-looking block code pays. The receiver keeps
per-sequence-number state, suppresses duplicates, tolerates arbitrary reordering by
slot-filling, and forwards each frame to the player the instant a correct copy exists —
never delaying artificially, since the player only credits the first on-time arrival. A
fixpoint XOR solver resolves any parity equation with exactly one unknown and repeats,
giving chained recovery across overlapping windows. An ARQ backstop (NACK only after FEC
is exhausted and only if a round trip still fits the deadline) was added but never fired
in the 40–120ms grading range, because the two-hop round trip costs more time than any
competitive delay budget allows — it degrades cleanly to FEC-only rather than helping.
Profile A is valid from 50ms and profile B from 100ms, at a constant ≈1.55× bandwidth
overhead, well under the 2× cap. Stress-testing six synthetic hidden profiles showed the
design tolerates jitter, reordering, duplication, and isolated loss, but breaks under
correlated burst loss once a burst exceeds the 3-frame parity window — that residual is
what actually caps the score, and more delay does not shrink it. **Recommended grading
delay: 110ms** — the lowest delay valid with margin across every survivable profile,
chosen for robustness against unseen spike-heavy profiles rather than shaving the last
milliseconds off profile B's bare 100ms floor. The next improvement is burst-aware FEC:
interleaving parity across a stride wider than the expected burst length turns clustered
losses into isolated, solver-recoverable ones, and a second parity stream would
complementarily raise per-group capacity to two erasures.
