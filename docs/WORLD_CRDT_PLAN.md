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
1. **Wire protocol v4**: frames are `[owner, hw, digest, [records]]` where
   `digest` is an order-independent 64-bit FNV-1a sum over the sender's LIVE
   entries. Digest comparison replaces the v3 scalar high-water-mark check,
   which could only detect "missing the newest record" and silently missed
   dropped updates to non-newest fields.
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

### Runtime
4. **World-only gossip**: self-stream broadcasts removed.
5. **FX repair handshake** (`'F','X'` v2): digest mismatch → index request →
   local diff → value request → normal gossip frame with just the missing
   records. Requests carry the requester's node id so replies are UNICAST to
   one peer instead of fanned out (measured ~7x control-traffic reduction).
6. **Sticky repair flag**: with all peers aggregated under one target, a
   single "current" report used to flicker the flag off before any handshake
   fired (~170 gap reports produced ~12 attempts). Now cleared only by a
   sustained streak of current reports.
7. **Deferred TX queue**: replies composed inside the transport's inbound-
   dispatch callback were vanishing when sent re-entrantly; they are queued
   and flushed from the main loop.
8. **Transport-agnostic hooks**: `fleece_runtime_on_gossip_frame()`,
   `fleece_runtime_on_control_frame()`, `fleece_runtime_note_behind_shared()`
   let any transport feed the runtime without going through FleeceComms.

### Reticulum module (`fleece_reticulum`)
9. Inbound gossip now merges via `import_ex()` and reports divergence through
   `fleece_reticulum_set_gap_callback()` — previously gap detection was dead
   code under the mesh transport.
10. FX frames ride the gossip aspect (module demultiplexes by `'F','X'`
    prefix) because the separate control destination's path establishment
    lags far behind gossip on real transports.
11. `fleece_reticulum_send_to_node()`: per-peer unicast by node id.

### Tests & benchmarks
12. `tests/test_crdt.c`: register convergence (order-independent), non-newest
    gap detection (the v3 blind spot), delete durability through compaction,
    delete/write race regression, 20-seed randomized fuzz (loss + partitions
    + heal). `test_gossip.c` updated to v4 hand-built frames.
13. `benchmarks/bench_gossip.c`: headless protocol benchmark with MTU
    fragmentation + baud pacing model.
14. `benchmarks/bench_mesh.c`: INTEGRATION benchmark over the real stack —
    N processes, real microReticulum, UDP-multicast radio harness with
    byte-exact wire counters, drop injection at the radio boundary, baud
    pacing, convergence aggregation.

## Measured results

| Scenario | Result |
|---|---|
| Headless fuzz, 20 seeds (loss+partitions+heal) | all converge |
| Real mesh N=3, 0% / 10% / 30% loss | CONVERGED, identical views |
| Control traffic after unicast replies | ~7x reduction |
| Channel utilization at N=3, 30% loss | ~70-80%, converges anyway |

**Key operational finding**: microReticulum's default 80 KiB heap pool ran at
<10% free and shed most sends ("DEPRIORITIZING gossip send"). Raised to
128 KiB — a defensible slice of the ESP32-P4's 768 KiB internal SRAM — which
eliminated shedding entirely.

## Next steps

1. **Scaling**: N=6 @ 10% loss did not converge within 200 ticks. Suspects:
   the aggregated "mesh" repair target lacks per-peer attribution; announce/
   discovery pacing at boot. Introduce per-peer targets once senders can be
   identified, or multi-responder round-robin.
2. **Repair trigger hysteresis**: piggyback a compact view digest on regular
   gossip (already there) but suppress handshake storms during churn with
   bounded leak-through instead of pure streak-clearing.
3. **Tombstone GC**: named tombstones currently cost their slot forever. Add
   ack-based collection: a tombstone may drop when every tracked peer's index
   shows no older live copy of the key.
4. **Proof-based FX delivery**: use RNS delivery proofs on tiny control
   frames instead of blind app-level retries (RNS has resend/proof support;
   no FEC — LoRa CR handles that at modulation level).
5. **Unicast index requests** once divergence can be attributed to a specific
   peer (needs per-peer gap bookkeeping, see 1).
6. **Bench calibration against live LoRa**: MTU 460 vs 500, CR-FEC effect on
   effective packet loss, real airtime scheduling.
7. **Cleanup**: gate/remove the temporary `FX_DEBUG` tracing in the runtime;
   decide whether `bench_swarm` (legacy two-stream model) stays or goes.
