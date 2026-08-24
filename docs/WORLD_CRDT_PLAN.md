# World-Object CRDT & Mesh Gossip — Plan, Status, Next Steps

## Goal

Make `world` — the single swarm-replicated JSON object — globally correct and
bandwidth-efficient over lossy mesh links:

- `world` keys are LWW-registers ordered by `(logical_timestamp, origin_node_id)`
  — a deterministic total order, so every node converges on the same winner
  for every key regardless of arrival order. This is json-crdt's
  object/register layer (à la json-joy); RGA/YATA sequence machinery is
  deliberately out of scope because a JSON object has no positions.
- `self` is node-local storage: never broadcast. Nodes publish selected fields
  into `world` explicitly.
- Steady-state traffic = per-key deltas only; divergence triggers targeted,
  peer-addressed repair instead of full-state floods.

## Done

### State manager (`fleece_state_manager`)
1. **Wire protocol v5** (was v4): frames are
   `[sender_node_id, owner, hw, digest, [records]]`. The v4 digest check
   remains (order-independent 64-bit FNV-1a over the sender's LIVE entries),
   but the header now also names the node that TRANSMITTED the frame - the
   prerequisite for per-peer repair attribution (see Runtime, below). A v4
   frame's owner field is always FLEECE_SHARED_OWNER_ID for world gossip, so
   receivers could tell WHAT they were behind on but never WHO to ask.
2. **Register semantics hardened**:
   - `remove_shared()` now re-stamps BOTH version components (fresh tick +
     deleter identity). It previously inherited the old value's author,
     producing two indistinguishable versions that deadlocked the tie-break
     across the swarm (found by fuzzing, regression-tested).
   - Raw `remove()` stamps its tombstone with a fresh tick (delete never
     propagated before).
   - `compact()` retains NAMED tombstones: they are delete-markers; dropping
     them lets stale replicas resurrect deleted fields. Cost: one slot each,
     no payload.
3. **Targeted anti-entropy API**: `export_shared_index()` (compact
   `[(key_hash, ts)]` index), `export_shared_by_hash()` (normal gossip frame
   filtered to requested keys), `shared_at_least()` (local diff check),
   `stream_digest()`, `ticks_since_last_write()`.
4. **Bounded exports** (`export_shared_delta_bounded`, byte-capped
   `export_shared_by_hash`): repair and delta frames stay under the
   transport's single-packet ceiling. Oversized frames used to fall back to
   Resource/Link transfer, whose pending transfers pin fixed-pool memory -
   measured live: one wedged node shed its ENTIRE gossip fan-out for a whole
   run while its unicast control path kept working.

### Runtime
5. **World-only gossip**: self-stream broadcasts removed.
6. **FX repair handshake** (`'F','X'` v2): digest mismatch → index request →
    local diff → value request → normal gossip frame with just the missing
    records. Requests carry the requester's node id so replies are UNICAST to
    one peer instead of fanned out.
7. **Per-peer resync targets**: each peer gets its own target keyed by node id
    (v5 sender header); index requests are UNICAST to the specific divergent
    peer. The old single anonymous "mesh" target made every peer's reports
    fight each other (sticky-flag flicker) and made attribution impossible.
    Stickiness is gone because it is now sound: a digest match from peer X
    certifies "we hold everything X holds", so X's flag clears immediately -
    cross-peer contamination was the only reason stickiness existed. An empty
    diff in an index reply likewise disarms THAT responder's flag.
8. **Re-entrancy fix for VALUE_REQs**: frames sent from inside the transport's
    inbound-dispatch callback vanish without failing (known transport
    property). Index replies already deferred their replies; the VALUE_REQ
    fired from the index-reply handler did NOT - measured: value requests
    never reached any responder, stalling every repair round forever. All
    callback-context sends now go through the deferred TX queue.
9. **Gossip cadence + empty-delta suppression + digest beacons**: deltas are
    batched every 10 ticks and bounded to 320 B; empty sends skipped; a small
    header-only beacon (staggered phase) keeps advertising the view digest so
    peers detect gaps even when nobody has fresh data. Per-tick fan-out
    gossip oversubscribed the channel by itself at N=6 (ticks stretched ~20x).
10. **Bounded leak-through**: at most 2 new handshake rounds start per tick;
    repairs wait for a short local-write quiet period (quiesce gate restored:
    churn-era repair storms were self-defeating at N>=6). In-flight rounds
    and retries are exempt.
11. **Transport-agnostic hooks**: `fleece_runtime_on_gossip_frame()`,
    `fleece_runtime_on_control_frame()`, `fleece_runtime_note_behind_from()`
    (now carries the reporting peer's node id).

### Reticulum module (`fleece_reticulum`)
12. Gap callback carries the sender node id from the v5 header
    (`FleeceReticulumGapFn(behind, sender, ud)`), so mesh-side gap reports are
    attributable and unicast-addressable.
13. FX frames ride the gossip aspect (module demultiplexes by `'F','X'`
    prefix) because the separate control destination's path establishment
    lags far behind gossip on real transports.
14. `fleece_reticulum_send_to_node()`: per-peer unicast by node id.
15. microReticulum TLSF pool raised 80 KiB → 128 KiB (the "operational
    finding" below; the bump had not actually landed in CMakeLists until
    now).

### Tests & benchmarks
16. `tests/test_crdt.c`: register convergence (order-independent), non-newest
    gap detection, delete durability through compaction, delete/write race
    regression, 20-seed randomized fuzz (loss + partitions + heal).
    `test_gossip.c`: hand-built v5 frames (arity/sender/type confusions,
    loopback and reserved-sender rejection), sender-attribution test, and a
    repair-frame roundtrip regression (the served VALUE frame lagged the v5
    header change and every repair payload was silently rejected on import -
    found only when the mesh benchmark refused to converge at 0% loss).
17. `benchmarks/bench_gossip.c`: headless protocol benchmark with MTU
    fragmentation + baud pacing model.
18. `benchmarks/bench_mesh.c`: INTEGRATION benchmark over the real stack -
    N processes, real microReticulum, UDP-multicast radio harness with
    byte-exact wire counters, drop injection at the radio boundary, baud
    pacing, convergence aggregation, and per-key LWW state dumps so the
    parent can name exactly WHICH entries disagree when digests diverge.

## Measured results

| Scenario | Result |
|---|---|
| Headless fuzz, 20 seeds (loss+partitions+heal) | all converge |
| Real mesh N=6 @ 10% loss, 200 ticks | CONVERGED, identical views (previously NOT CONVERGED) |
| Real mesh N=6 @ 0% loss | CONVERGED |
| Real mesh N=4 @ 20% loss (default scenario) | CONVERGED |
| Unicast delivery after per-peer targeting | uni_fail ≈ 0 |

**Key operational findings (this round)**:
- The N=6 failure had FOUR stacked causes, uncovered bottom-up:
  1. the served VALUE frame still had the v4 wire shape, so every repaired
     record was silently rejected on import (repairs could never work);
  2. VALUE_REQs were sent re-entrantly from the transport dispatch callback
     and vanished (so even correct replies were never requested);
  3. oversized boot-time deltas (~800 B) fell into the Resource/Link path,
     whose stuck transfers pinned the TLSF pool under its 10% red line, and
     the pool backstop then shed the node's entire gossip fan-out indefinitely
     (a silent, one-way partitioned node whose unicast control still worked);
  4. with nobody sending empty frames anymore, post-churn silence left gap
     detection blind (fixed by digest beacons).
- Per-tick gossip fan-out alone exceeds a fair-shared LoRa channel at N=6;
  batching + bounding + suppression cut steady-state load ~10x. Channel
  utilization in the bench is still above nominal fair share once RNS
  per-packet overhead x N-peer fan-out is counted - fine for the UDP harness,
  the next lever for real radios is cadence/cap tuning (constants are named:
  FLEECE_GOSSIP_EVERY_TICKS, FLEECE_GOSSIP_FRAME_BYTES, FLEECE_BEACON_EVERY_TICKS).
- Bench node logs must be line-buffered: children exit via _exit(), which
  skips stdio flush and previously erased the shedding evidence.

## Next steps

1. **Channel-load tuning for real LoRa**: utilization is ~2x nominal fair
   share at N=6 with current constants; tune gossip cadence/frame cap/beacon
   interval against the MTU-460 CR-FEC radio (see #6 below), or make the
   cadence adaptive to observed airtime.
2. **Tombstone GC**: named tombstones currently cost their slot forever. Add
   ack-based collection: a tombstone may drop when every tracked peer's index
   shows no older live copy of the key.
3. **Proof-based FX delivery**: use RNS delivery proofs on tiny control
   frames instead of blind app-level retries (RNS has resend/proof support;
   no FEC - LoRa CR handles that at modulation level).
4. **Bench calibration against live LoRa**: MTU 460 vs 500, CR-FEC effect on
   effective packet loss, real airtime scheduling.
5. **Cleanup**: decide whether `bench_swarm` (legacy two-stream model) stays
   or goes; several `[fx]` trace points are env-gated and worth an audit.
