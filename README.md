# Fleece - Lightweight Swarm Coordination Runtime

[![CI](https://github.com/MonadicLabs/fleece/actions/workflows/ci.yml/badge.svg)](https://github.com/MonadicLabs/fleece/actions/workflows/ci.yml)

fleece is a **lightweight, decentralized swarm coordination runtime** designed for **microcontrollers**.

## Key Features

### 🔄 Virtual Stigmergy
- **Decentralized, eventually consistent key-value store**
- **LWW (Last-Write-Wins) gossip protocol** for conflict resolution
- **Field-level timestamps** and **node ID tie-breaking**
- Optimized for **low RAM usage** in microcontroller environments

### 💻 Embedded JavaScript
- **Real QuickJS-ng integration** (vendored as a git submodule) for high-level scripting
- **Buzz-inspired lifecycle** ([buzz-lang/Buzz](https://github.com/buzz-lang/Buzz)): scripts define `init()`/`step()`/`reset()`/`destroy()`
- **`self`** - a read/write object for this node's own published state
- **`swarm`** - a read-only view of every other *live* known node's published state, kept in sync by gossip (dead/silent peers drop out automatically - see below)
- **`world`** - a durable, any-node-writable collection for swarm-shared data (e.g. discovered targets, shared tasks) that isn't tied to any single node's liveness
- **`platform`** - a pure name→function registry for hardware bindings (fleece defines none itself - see below)
- Sandboxed execution for security

### 🎯 GOAP Planning
- **Goal-Oriented Action Planning** (planner + runtime "brain"): plan → select action → execute action → re-plan when done
- Action preconditions/effects, action bodies (`exec`), goal expressions, and costs are authored as **JS functions**, evaluated by QuickJS; orchestration stays in C
- **Utility-curve goal selection**: priorities weighted by a curve over a blackboard field, so the unit picks the most pressing unsatisfied goal
- CBOR **plan blob** serialization for offline authoring / radio distribution of the whole action library
- See `src/planner/fleece_planner.c`, `src/embedded/fleece_goap_js.c`, and `examples/example4_goap.c`

### 📦 Gossip State
- **Real CBOR (RFC 8949) framing** for bandwidth-limited links - a 3-byte magic/version prefix followed by a CBOR-encoded record array, see `src/state/fleece_state_manager.c`
- **Delta gossip**: each tick sends only fields changed since the last send, with a periodic full-state resync so a peer can recover from a dropped packet
- **Two independent streams**: `self` (owned by each node) and shared/`world` (owned by no single node, relayed by whoever holds a copy)
- **Full replication of `world`**: every node re-broadcasts everything it currently holds, not just what it personally discovered, so a `world` entry propagates node-to-node until every reachable unit has it (multi-hop, once comms has a real transport); the periodic full resync also catches any node that missed a relay or joined late
- **Deterministic convergence on `world` conflicts**: every shared record carries its true author (`origin_node_id`), so if two nodes write the same field at the exact same logical timestamp, all nodes converge on the identical winner (higher origin id) instead of each side keeping its own write forever
- **`worldCompareAndSet`**: a local compare-and-set primitive for building safe claim protocols on top of `world` (e.g. "claim this target only if unclaimed") - see the World section below
- **Peer liveness / heartbeat**: every gossip frame (even an empty delta) counts as a heartbeat; a peer not heard from within a configurable TTL disappears from `swarm` (its data isn't deleted, just hidden - it reappears immediately once heard from again)
- Efficient field-level versioning and conflict resolution

## System Architecture

### Time-Step Loop
The core execution model follows a synchronized **4-phase coordination cycle**, run once per tick after `init()` is called:

1. **Phase 1: Input (Sensors/Radio)**
   - Process incoming radio packets
   - Handle wireless communication events

2. **Phase 2: Script Execution (QuickJS VM)**
   - Calls the script's `step()` function
   - Reads/writes `self`, reads `swarm`, reads/writes `world`, calls `platform.<name>(...)`
   - Runs *before* gossip so any `self.xxx`/`world.xxx` change this tick is broadcast this tick, not next

3. **Phase 3: Gossip (State Synchronization)**
   - Advances the peer-liveness tick counter, then exports and broadcasts what changed since the last send (delta), full state periodically (resync) - as two separate frames, one for `self`, one for the shared `world` stream
   - Peer frames arrive via the comms receive callback and merge into `swarm`/`world` with LWW semantics; every frame received also counts as a heartbeat from its sender
   - Maintains eventual consistency across the swarm, replicates `world` to every reachable unit, and prunes peers from `swarm` that haven't been heard from within the configured TTL

4. **Phase 4: Output (Actuators/Mesh Broadcast)**
   - Send mesh broadcasts, disseminate updates to peer nodes

`destroy()` is called once when the loop exits (e.g. on SIGINT/SIGTERM). `reset()` is callable but has no automatic trigger yet - see Future Work.

## Technical Decisions

### Consistency
- **Eventually consistent** via monotonic timestamps
- **Deterministic node-ID tie-breaking** for conflict resolution: `self`/`swarm` fields are safe by construction (a given (name, owner) pair only ever has one real writer); `world` fields can genuinely be written by two different nodes at the exact same logical timestamp, so those records carry their true author's id and break exact ties by higher id, consistently on every node
- **Conflict-free Replicated Data Types (CRDT)** principles
- No central authority or master nodes

### Serialization
- **Real CBOR (RFC 8949)** gossip frames for all state (see `fleece_state_manager_export`/`_import`/`_export_delta`) - a minimal encoder/decoder supporting just the CBOR major types actually needed (uint, byte string, text string, array, bool), not a general-purpose codec
- **Minimal overhead** for bandwidth-constrained links
- **Field-level versioning** via per-field timestamp + owner node id (shared/`world` records additionally carry the true author's id, `origin_node_id`, distinct from the storage owner - see Consistency below)
- **Delta export** by default (only fields changed since a watermark timestamp), with a full export every `FLEECE_GOSSIP_FULL_RESYNC_TICKS` ticks as anti-entropy

### Constraints
- **Memory-optimized data structures**
- **Avoid heavy CRDT metadata** in favor of shallow LWW maps
- **Resource-aware scheduling** for microcontroller timing
- **Graceful degradation** under network stress

## Development Principles

### Resource Awareness
- Always prioritize **memory footprint** over abstraction depth
- **Avoid heavy object allocation** in the main loop
- **Static memory pools** instead of dynamic allocation where possible
- **Lazy loading** and **on-demand computation**

### Fail-Fast Networking
- Assume the network is **volatile** and **packets are dropping**
- **Handles out-of-order delivery** gracefully
- **Redundant state synchronization** for robustness
- **Fast conflict resolution** for network partitions

### Decentralization
- **No nodes are "masters"** - every node is a full participant
- **Full mesh participation** in the gossip protocol
- **Equal status** for all swarm members
- **No single points of failure**

## API Overview

### Core Components

#### FleeceRuntime
```c
FleeceRuntime* runtime = fleece_runtime_create();
// or, to give this process a specific/stable node id (e.g. running more than
// one real node in the same process, or deriving an id from a CLI argument -
// see examples/example2_search_and_deliver.c):
// FleeceRuntime* runtime = fleece_runtime_create_with_node_id(node_id);
fleece_runtime_load_script(runtime, script_source); // defines init/step/reset/destroy
fleece_runtime_start(runtime);                      // calls init(), then the 4-phase loop, then destroy()
fleece_runtime_destroy(runtime);
```

#### FleeceStateManager
```c
// Set a field value (LWW semantics)
fleece_state_manager_set(manager, key, data, size);

// Get a field value
fleece_state_manager_get(manager, key, &data, &size);

// Check existence
fleece_state_manager_exists(manager, key);

// Remove a field
fleece_state_manager_remove(manager, key);
```

#### FleeceComms
The comms module itself is a transport-agnostic simulation - fleece ships no real radio/socket backend. Real transports are wired in entirely from outside via three callbacks, all optional:
```c
FleeceComms* comms = fleece_comms_create();
fleece_comms_initialize(comms);

// Called after a successful fleece_comms_send() - do the real send here.
fleece_comms_set_send_callback(comms, my_send, my_transport);

// Called once per fleece_comms_process_input() (i.e. once per runtime tick) -
// do your own periodic I/O here (e.g. poll a non-blocking socket). Fleece has
// no opinion on what happens inside it.
fleece_comms_set_poll_callback(comms, my_poll, my_transport);

// (fleece_comms_set_receive_callback also exists but nothing in the runtime
// currently drives it - see examples/example2_search_and_deliver.c for the
// pattern actually used: feed received bytes straight into
// fleece_state_manager_import() from inside the poll callback instead.)

fleece_comms_process_input(comms);
fleece_comms_process_output(comms);
```
See `examples/example2_search_and_deliver.c` for a complete real backend (UDP multicast) built entirely on these two callbacks, with zero transport-specific code in the library itself.

#### FleeceEmbedded
```c
FleeceEmbedded* embedded = fleece_embedded_create();
fleece_embedded_set_state_manager(embedded, manager);   // backs self/swarm/world
fleece_embedded_set_platform(embedded, platform);        // optional - backs platform.<name>()
fleece_embedded_set_peer_ttl_ticks(embedded, 30);         // optional - defaults to FLEECE_DEFAULT_PEER_TTL_TICKS
fleece_embedded_register_c_functions(embedded);           // installs console/self/swarm/platform/world

fleece_embedded_load_script(embedded,
    "function step() {\n"
    "  self.status = 'ok';\n"
    "  console.log('id', self.id, 'peers', Object.keys(swarm).length, 'world entries', Object.keys(world).length);\n"
    "  worldCompareAndSet('lock', undefined, self.id);  // claim-if-absent; see the World section below\n"
    "}\n", "<script>");
fleece_embedded_call_step(embedded);
fleece_embedded_destroy(embedded);
```
(`fleece_runtime_*` does all of this wiring for you - the snippet above is what it does internally.)

#### FleecePlatform
A pure name→function registry - fleece defines **no** functions of its own (no `arm`, no `moveTo`, nothing robot-specific). Register whatever verbs fit your actual hardware, and scripts call them as `platform.<name>(...)`. With nothing registered, `platform` is simply empty.
```c
static int my_stop(const uint8_t* args_json, uint32_t args_size,
                    uint8_t** result_json, uint32_t* result_size, void* user_data) {
    // interpret args_json however this hardware needs to, then act on it
    return 0; // or -1 to surface as a thrown JS exception
}

FleecePlatform* platform = fleece_runtime_get_platform(runtime);
fleece_platform_register(platform, "stop", my_stop, NULL);
// script: platform.stop();
```
See `examples/example3_embodied_swarm.c` for a complete real binding (`getPosition`/`moveToward`, backed by a small per-process physics struct via `user_data`) - see the Embodied Example section below.

## Directory Structure

```
fleece/
├── include/
│   ├── runtime/           # Runtime interface headers
│   ├── state/             # State manager interface
│   ├── comms/            # Comms interface
│   ├── embedded/         # Embedded JS interface (self/swarm/world/platform bindings)
│   ├── planner/          # GOAP planner interface
│   ├── platform/         # Platform function registry interface
│   └── core/             # Dead/unused parallel runtime - not wired into anything
├── src/
│   ├── runtime/          # Runtime implementation
│   ├── state/            # State manager implementation
│   ├── comms/           # Comms implementation (single-process simulation, no real transport)
│   ├── embedded/         # QuickJS-ng integration and self/swarm/world/platform bindings
│   ├── planner/          # GOAP planner (search, goal selection, CBOR plan blob)
│   ├── platform/        # Platform function registry implementation
│   └── core/            # Dead/unused - see include/core
├── third_party/
│   └── quickjs/          # QuickJS-ng, git submodule
├── examples/             # Example applications (one executable per .c file)
│   ├── example1.c / .js               # Basic swarm coordination example (single process, simulated peer loopback)
│   ├── example2_search_and_deliver.c / .js  # Search + CBAA task allocation + delivery over real UDP multicast (multi-process)
│   ├── run_swarm.sh                   # Launches N example2 agents as real processes with prefixed, interleaved output
│   ├── example3_embodied_swarm.c / .js      # Same, but with real 2D position/speed/arrival via a "platform" binding (multi-process)
│   ├── run_swarm3.sh                  # Launches N example3 agents, same idea as run_swarm.sh
│   ├── example4_goap.c               # GOAP-driven unit: plan -> execute -> replan, pure C (no script)
│   ├── example5_uav_swarm.c          # GOAP UAV swarm: GPS nav, brain-side claiming, mesh gossip, web dashboard
│   ├── http_server.c/.h              # Tiny threaded HTTP helper backing example5's dashboard (example-only, not part of the library)
│   ├── webui/                        # example5's web dashboard (map + per-unit GOAP/bandwidth telemetry)
│   └── example_common.h               # Shared helper: loads each example's companion .js file from disk (see below)
├── tools/
│   └── goap_editor/                   # Browser-based GOAP plan authoring studio (see "GOAP Plan Editor" below)
├── tests/                # Unit tests (one executable per file, run via ctest)
│   ├── test_state_manager.c # Raw LWW store tests
│   ├── test_gossip.c        # Export/import/merge/delta wire format tests
│   ├── test_embedded.c      # self/swarm/world QuickJS binding tests
│   ├── test_planner.c       # GOAP planner + CBOR plan blob tests
│   ├── test_goap_js.c       # QuickJS eval bridge + behavior-loop brain tests
│   └── test_platform.c      # Platform registry + JS binding tests
├── CMakeLists.txt        # Build system
├── README.md            # Project documentation
└── CLAUDE.md            # Development guidelines
```

## Building and Running

### Prerequisites
- GCC or compatible C compiler
- CMake 3.10 or higher
- Standard POSIX library (for `clock_gettime` and `nanosleep`)
- Linux or macOS with multicast-capable networking, to run `example2_search_and_deliver` or `example3_embodied_swarm` (both use BSD/glibc UDP multicast sockets - not needed to build/test the library itself)
- A browser to watch `example5_uav_swarm`'s dashboard (see below)

### Building
```bash
git submodule update --init --recursive   # fetches QuickJS-ng (third_party/quickjs)
mkdir build && cd build
cmake ..
make -j4
```

### Running Tests
```bash
cd build && ctest --output-on-failure
# or run an individual binary directly, e.g. ./build/test_embedded
```

### Running Examples
Each example's script lives in a companion `.js` file next to its `.c` file (e.g. `examples/example1.js`) rather than embedded in the binary, and is loaded from disk at startup (see `examples/example_common.h`). The one exception is `example4_goap` - all of its behavior runs through the GOAP brain, so it has no script at all. Run the binaries from `build/` as shown below - each one locates its own script relative to its own location, so this works regardless of your current directory.
```bash
# Basic swarm coordination example (single process)
./build/example1

# Search + CBAA task allocation + delivery, over real UDP multicast.
# Run as two (or more) SEPARATE processes to see real multi-process gossip -
# each argument is a small distinct agent number, not a full node id:
./build/example2_search_and_deliver 1 &
./build/example2_search_and_deliver 2 &

# ...or use the launcher, which spawns N real agent processes for you and
# interleaves their output with an [agent N] prefix - one command, no manual
# terminal juggling:
examples/run_swarm.sh          # 3 agents, until Ctrl+C
examples/run_swarm.sh 5        # 5 agents, until Ctrl+C
examples/run_swarm.sh 3 10     # 3 agents, stops automatically after 10s

# Same scenario, but agents have real position/speed and physically travel to
# a target before delivering it (see the Embodied Swarm section below):
./build/example3_embodied_swarm 1 &
./build/example3_embodied_swarm 2 &
# ...or, again, the launcher (give it more time than run_swarm.sh - movement
# takes real ticks, not an instant claim):
examples/run_swarm3.sh 3 30

# GOAP-driven unit - the planner runs the unit's behavior loop, in one process:
# plan -> select action -> execute action -> replan when done
# (runs ~70 ticks then exits):
./build/example4_goap

# GOAP UAV swarm with a live web dashboard: three UAVs autonomously patrol,
# claim targets in the shared world, intercept, carry them home, and recharge.
# The HTTP server + dashboard live entirely in examples/ (not the library).
# Point a browser at http://localhost:8080 to watch it:
./build/example5_uav_swarm 8080 examples/webui
```

## Usage Examples

### Basic State Management
```c
// Set a sensor reading
uint8_t temperature_data[] = {0x64}; // 100 in temperature units
fleece_state_manager_set(state_manager, 0x0001, temperature_data, sizeof(temperature_data));

// Get the latest reading
uint8_t* data;
uint32_t size;
if (fleece_state_manager_get(state_manager, 0x0001, &data, &size) == 0) {
    // Process sensor data
    free(data);
}
```

### Script Execution
```c
// self/swarm/platform/world are real Proxy-backed globals once
// fleece_embedded_register_c_functions() (or fleece_runtime_create()) has run.
// 'cool' is only callable if something called fleece_platform_register(platform, "cool", ...).
fleece_embedded_execute(embedded,
    "if (self.temperature > threshold && 'cool' in platform) platform.cool();");
```

### World (shared, swarm-replicated) State
```c
// Any node can publish, claim, or update a "world" entry - it's not tied to
// this node's liveness and gets relayed to every reachable unit (see the
// Gossip State section above). Plain reads/writes go through the world Proxy,
// same as self. Equivalent C API: fleece_state_manager_set_shared/get_named.
fleece_embedded_execute(embedded,
    "if (!('T1' in world)) {\n"
    "  world.T1 = { lat: 42.1, lon: -71.05, type: 'debris' };\n"
    "}");
```

Publishing a target this way is safe (any node can add or read entries), but **claiming** one needs care: two units could both read "unclaimed" in the same round before either's claim has propagated, and both would think they won. Use `worldCompareAndSet` (backed by `fleece_state_manager_set_shared_cas`) for that - it's a *local* compare-and-set (each node only sees what it currently knows, there's no central lock), so pair it with a re-check a tick or two later once gossip has had a chance to propagate:
```c
fleece_embedded_execute(embedded,
    "// Optimistically claim - fails harmlessly (returns false) if someone else already has.\n"
    "var task = world.T1;\n"
    "if (task && task.assignedTo === undefined) {\n"
    "  worldCompareAndSet('T1', task, Object.assign({}, task, { assignedTo: self.id }));\n"
    "}\n"
    "// A few ticks later, in step(): re-check the claim actually stuck (see\n"
    "// fleece_state_manager_set_shared_cas doc comment for why this recheck matters).\n"
    "if (world.T1 && world.T1.assignedTo === self.id) {\n"
    "  /* proceed with the task */\n"
    "}");
```

### Full Example: Search + Task Allocation + Delivery
`examples/example2_search_and_deliver.c` puts the pieces above together into a complete, runnable multi-unit scenario:
- **Search**: each unit independently "discovers" a fixed set of hardcoded targets after a short delay and publishes any it doesn't yet see in `world`, via the `if (!(id in world))` pattern above - duplicate discoveries by multiple units harmlessly settle to one record.
- **Task allocation**: a CBAA-flavored auction (Consensus-Based Auction Algorithm - the single-task-per-agent sibling of CBBA) built entirely in the JS layer: each unit scores itself against every open target, bids on the best one via `worldCompareAndSet`, and re-checks its claim every tick - if outbid (a higher-scoring bid displaced it), it releases the claim and re-enters the bidding pool. Each unit pursues **at most one task at a time**.
- **Delivery**: once a claim has held for a settle window (long enough for gossip to propagate and any competing bid to surface), the unit marks the task delivered, again via `worldCompareAndSet`.
- **Real transport**: unlike `example1.c`'s single-process simulated loopback, this example wires `FleeceComms`'s send/poll callbacks to a real UDP multicast socket (see the FleeceComms section above) - run it as two or more separate OS processes to see the whole thing converge over an actual network stack, not just in-process. All of the socket code lives in the example; the library itself never touches a socket.
- **`examples/run_swarm.sh [num_agents] [duration_seconds]`**: a launcher that spawns that many real agent processes for you (default 3) and prefixes/interleaves their output as `[agent N] ...`, so a single command demonstrates the whole swarm instead of needing several manually-opened terminals.

### Embodied Example: Real 2D Position, Speed, and Arrival
`examples/example3_embodied_swarm.c` is the same search-and-deliver scenario, but with genuine physical embodiment instead of an abstract instant claim:
- **`platform` in real use**: this is the first example to actually register `platform` functions - `platform.getPosition()` and `platform.moveToward(x, y)`, each bound to the process's own small physics struct (position + max speed) via `fleece_platform_register`'s `user_data`. The script calls `moveToward` every tick while pursuing a claimed target; the native side advances position by at most `max_speed` units and reports `{x, y, arrived}` back.
- **Live position, not a static spawn point**: the script mirrors the returned position onto `self.x`/`self.y` each tick, so it's visible to peers via the existing gossip mechanism, and bid scores are computed from *current*, moving position - a target that looked out of reach can become the best option (or vice versa) as agents actually move.
- **Delivery requires physical arrival**: a claim only converts to `delivered` once `moveToward` reports `arrived: true` *and* the claim has held for a short settle window - not just a fixed timer, like `example2_search_and_deliver.c` used.
- **A contest margin, added after watching it thrash**: with scores now changing every tick as agents converge on the same target, contesting on *any* marginally-better score caused two closing agents to flip the claim back and forth dozens of times before one arrived. `CONTEST_MARGIN` requires a bid to beat the current holder by a meaningful amount before it's worth contesting - dropped one test run from 40 claim events to 5, with faster, cleaner convergence.
- **A live ASCII view, per agent**: each agent periodically renders its own local view of the swarm from what it currently knows (`self`, `swarm`, `world`) - `@` for itself, `+` for a live peer, `x`/`o`/`#` for an unclaimed/claimed/delivered target. Because it's a genuinely distributed system, this is deliberately each agent's own honest belief, not a global truth. It renders cleanly when watching one agent directly; through `run_swarm3.sh`'s combined multi-process output, a multi-line grid can get interleaved with other agents' single-line status lines (an inherent limit of interleaving real concurrent process output, not something the launcher tries to solve) - the single-line `claimed`/`ARRIVED`/`DELIVERED` events stay reliable either way.

### GOAP Planning (Goal-Oriented Action Planning)
`src/planner/fleece_planner.c` implements a lightweight, engine-agnostic GOAP planner: action preconditions, effects, goal expressions, and costs are stored as opaque **JS function sources** and evaluated by host callbacks. In fleece the host is QuickJS, wired up in `src/embedded/fleece_goap_js.c`:

- **Script JS is used only to author the functions** (pre/eff/exec/goal-expr/cost). Planning, goal selection, and execution orchestration all stay in C - there is no `goap.*` global in script.
- **`bb` object**: authored functions receive `{ self, world, platform, swarm }` mirroring the script globals; `self`/`world` are detached copies the function may mutate. **`eff` is a planner heuristic only** - the A* search uses it to simulate the future; the executor never applies it. **`exec` is the action's real body** - the executor calls `exec(bb, tick)` every tick; it does the actual work (drive hardware, move, consume...), mutating its `bb` copy, and returns `true` when finished. An action with no `exec` has no runtime body: it completes at once and changes nothing (see the contract in `fleece_planner.h`).
- **Behavior-loop "brain"**: `fleece_goap_brain_tick()` (or `fleece_runtime_set_goap()`, which drives it once per main-loop tick) runs the full loop - snapshot the live state, pick the highest-utility unsatisfied goal, plan, execute the plan's first action's `exec()` body each tick (committing its work to the state manager as it happens), then **re-plan** when the action reports done.
- **Plan blob**: `fleece_goap_serialize()` compiles the whole table set (actions/goals/utilities/missions) into a compact CBOR blob for offline authoring or radio distribution (`fleece_goap_deserialize()` to load).
- **Source size cap**: each authored JS source is capped at `FLEECE_GOAP_SOURCE_MAX` (2048) bytes. Exceeding it used to silently truncate and then fail to compile with a confusing `SyntaxError: Unexpected token` deep in the action body - `copy_str` in `src/planner/fleece_planner.c` now warns explicitly on truncation so the real cause is obvious.

Example scenario in `examples/example4_goap.c`: a forager robot whose goals (`forage` food, `recharge` battery) have utility curves so selection shifts with state - watch the `[brain]` lines print the plan->execute->replan cycle, and the shared `world.foodTotal` counter grow as gossip carries each cycle's harvest.

### UAV Swarm Example: GPS, Decentralized Claiming, Mesh Gossip, and a Dashboard
`examples/example5_uav_swarm.c` runs three UAVs as **threads inside one process** - each with its own QuickJS VM, GOAP brain, and state manager - coordinating purely over a simulated packet radio:

- **Platform = flight controller + sensor only.** The only verbs are `uav_waypoint(x,y)`, `uav_arrived()` (returns `{arrived}`), `uav_scan()` (raw sensor - returns `{detect, targets:[{id,x,y}]}` with no filtering or claims), `uav_pickup(targetId)` (by-id actuator), and `uav_deliver`. Nothing about coordination lives here.
- **All coordination lives in the agent brains (JS).** Target claiming is a *decentralized protocol over the gossiped world*, not a platform feature: each brain reads `world['claim_'+targetId]`, skips targets claimed by a still-live peer (`swarm[owner]` exists), and atomically takes a claim with `worldCompareAndSet('claim_'+id, owner, self.id)` - `owner` is `undefined` for an unclaimed target (claim-if-absent) or a dead peer's id (takeover after it stops being heard from). Release is a plain `delete world['claim_'+id]`, only when we still own it.
- **Continuous GPS, no named locations.** `bb.self.x/.y/.home` are real coordinates and the GOAP functions use `Math.hypot` for ranges - the planner's A* simulates geometry, and `env_tick` mirrors a `distHome` field each tick so goals like `charge` (reachable when `distHome < 30`) are plannable against a moving unit.
- **GOAP with the framework doing the heavy lifting.** Goals `forage`/`recharge` with utility curves, actions `scan`/`hunt`/`return`/`charge` as authored JS; the brain replans whenever an action reports done - so a battery running low mid-mission *actually* reshapes the plan (a unit heading home to deliver can keep flying, or reroute to charge first).
- **Mesh gossip, in-process.** The two comms callbacks are wired to a per-peer mailbox: a send enqueues the gossip frame into every other UAV's queue, and each UAV's own poll callback drains its queue on its own thread and feeds `fleece_state_manager_import`. One broadcast frame per UAV per tick; drop-on-full mimics a lossy radio.
- **Per-unit bandwidth telemetry.** The mesh callbacks count bytes/frames per unit; `env_tick` computes a wall-clock RX/TX rate per unit, surfaced in `/state` and plotted as a sparkline per UAV card in the dashboard.
- **A tiny threaded HTTP server + web dashboard** (`examples/http_server.c/.h` + `examples/webui/`), entirely outside the library: a live map of the swarm, per-UAV goal/action/progress/battery cards with RX/TX bandwidth sparklines, and pause/step/reset/speed controls posting to `/cmd`. `worldDelivered` (in the world header) ticks up with every successful drop-off.
- **A hard-won lesson about JS results.** `platform.*` returns are JSON objects - `uav_arrived()` returns `{arrived: false}`, an object that is *truthy* in JS. The first execs wrote `if (!platform.uav_arrived()) return false;`, which never fired (an object is always truthy), so every action "completed" in a single tick and no UAV ever physically arrived. The correct form is `if (!platform.uav_arrived().arrived) return false;` - one of those silent-nothing bugs worth remembering.

### GOAP Plan Editor

`tools/goap_editor/` is a self-contained, browser-based studio for authoring GOAP plans. Open `index.html` (no build step, no server) and you get:

- Visual editors for **actions** (id/name/dest/dur/cost/exec/pre/eff), **goals** (expr/priority/curve), **utilities** (dimension + editable curve with a live SVG preview), and **missions** (goal lists + notes), all in one dark "swarm" themed UI.
- **Live JS validation** - every source is checked the way the runtime compiles it (`(<source>)` wrapped, like `compile_goap_fn` in the bridge), with per-field status dots and an overall "all JS valid" badge.
- **Byte-exact export** - the plan serializes to the exact same `['F','P',2] + CBOR` blob `fleece_goap_serialize()` produces (verified byte-for-byte against the C library). Export as a downloadable `.bin`, as hex/base64, or as an embeddable C array:
  ```c
  static const unsigned char goap_plan[] = { 0x46, 0x50, 0x02, ... };
  fleece_goap_deserialize(goap, goap_plan, sizeof(goap_plan));
  ```
- **Import** existing blobs (hex/base64/file) or JSON to keep editing them, auto-saves to `localStorage`, and ships pre-loaded with the `example4_goap` forager scenario as a starting point.

Run the serializer self-test (compares the JS output byte-for-byte against the C blob when one is provided):

```sh
node tools/goap_editor/test.js          # format sanity round-trips
node tools/goap_editor/test.js path/to/plan.blob   # byte-identity vs the C serializer
```

### Mesh Communication
```c
// Send update to all connected nodes
fleece_comms_send(comms, "all_nodes", state_data, state_size);

// Receive incoming messages from radio
fleece_comms_receive(comms, destination, &data, &size);
```

## Performance Considerations

### Memory Usage
- **128-field limit** in the LWW store
- **Static allocation** where possible
- **Memory pools** for frequent allocations
- **Efficient serialization** to minimize packet size

### Timing
- **100ms sleep** between coordination cycles (configurable)
- **Monotonic timestamps** for ordering
- **Non-blocking operations** for real-time responsiveness
- **Graceful degradation** under timing constraints

### Network Reliability
- **Packet loss tolerance** through gossip protocols
- **Redundant state synchronization**
- **Fast conflict resolution**
- **Adaptive timeout handling**

## License
MIT License - See LICENSE file for details

## Future Work

### Planned Enhancements
1. **A built-in Reticulum/radio transport** for comms - `example2_search_and_deliver.c` shows a real transport (UDP multicast) is fully possible via the existing send/poll callbacks, but fleece itself still ships only the simulated default backend
2. **A `reset()` trigger** - the lifecycle function is callable but nothing in the runtime calls it automatically yet
3. **Real hardware platform bindings** (e.g. a MAVLink or ROS2 integration) - `example3_embodied_swarm.c` shows the registry in real use (a simple 2D movement binding), but nothing resembling actual flight-controller or robot hardware is wired up yet
4. **Hardware-specific optimizations** (ESP32, STM32, etc.) and validation on an actual microcontroller target (currently desktop POSIX only)
5. **More example applications** (leader election, foraging at scale, etc.)
6. **Performance profiling** and optimization
7. **Documentation updates** and tutorials

### Design Considerations
- **Backward compatibility** in the API
- **Incremental development** approach
- **Modular architecture** for easy extension
- **Test-driven development** for reliability
