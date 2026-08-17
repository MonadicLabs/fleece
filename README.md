# Fleece - Lightweight Swarm Coordination Runtime

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

### 📦 Gossip State
- **Compact binary framing** for bandwidth-limited links (not CBOR - a small length-prefixed record format, see `src/state/fleece_state_manager.c`)
- **Delta gossip**: each tick sends only fields changed since the last send, with a periodic full-state resync so a peer can recover from a dropped packet
- **Two independent streams**: `self` (owned by each node) and shared/`world` (owned by no single node, relayed by whoever holds a copy)
- **Full replication of `world`**: every node re-broadcasts everything it currently holds, not just what it personally discovered, so a `world` entry propagates node-to-node until every reachable unit has it (multi-hop, once comms has a real transport); the periodic full resync also catches any node that missed a relay or joined late
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
- **Deterministic node-ID tie-breaking** for conflict resolution
- **Conflict-free Replicated Data Types (CRDT)** principles
- No central authority or master nodes

### Serialization
- **Compact binary gossip frame** for all state (not CBOR - see `fleece_state_manager_export`/`_import`/`_export_delta`)
- **Minimal overhead** for bandwidth-constrained links
- **Field-level versioning** via per-field timestamp + owner node id
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
```c
FleeceComms* comms = fleece_comms_create();
fleece_comms_initialize(comms);
fleece_comms_process_input(comms);
fleece_comms_process_output(comms);
```

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

## Directory Structure

```
fleece/
├── include/
│   ├── runtime/           # Runtime interface headers
│   ├── state/             # State manager interface
│   ├── comms/            # Comms interface
│   ├── embedded/         # Embedded JS interface (self/swarm/world/platform bindings)
│   ├── platform/         # Platform function registry interface
│   └── core/             # Dead/unused parallel runtime - not wired into anything
├── src/
│   ├── runtime/          # Runtime implementation
│   ├── state/            # State manager implementation
│   ├── comms/           # Comms implementation (single-process simulation, no real transport)
│   ├── embedded/        # QuickJS-ng integration and self/swarm/world/platform bindings
│   ├── platform/        # Platform function registry implementation
│   └── core/            # Dead/unused - see include/core
├── third_party/
│   └── quickjs/          # QuickJS-ng, git submodule
├── examples/             # Example applications
│   └── example1.c       # Basic swarm coordination example
├── tests/                # Unit tests (one executable per file, run via ctest)
│   ├── test_state_manager.c # Raw LWW store tests
│   ├── test_gossip.c        # Export/import/merge/delta wire format tests
│   ├── test_embedded.c      # self/swarm/world QuickJS binding tests
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
```bash
# Basic swarm coordination example
./build/example1
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
// Gossip State section above). Equivalent C API: fleece_state_manager_set_shared.
fleece_embedded_execute(embedded,
    "if (!('T1' in world)) {\n"
    "  world.T1 = { lat: 42.1, lon: -71.05, type: 'debris', assignedTo: null };\n"
    "} else if (world.T1.assignedTo === null) {\n"
    "  world.T1 = Object.assign({}, world.T1, { assignedTo: self.id });\n"  // claim it
    "}");
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
1. **Real transport for comms** (Reticulum/radio) - the comm bus is currently a single-process simulation
2. **CBOR (or similar) implementation** for state serialization - state currently uses a simpler custom binary gossip frame
3. **A `reset()` trigger** - the lifecycle function is callable but nothing in the runtime calls it automatically yet
4. **Real hardware platform bindings** (e.g. a MAVLink or ROS2 integration registering functions via `fleece_platform_register`) - the registry exists but ships with nothing registered
5. **Hardware-specific optimizations** (ESP32, STM32, etc.) and validation on an actual microcontroller target (currently desktop POSIX only)
6. **More example applications** (leader election, foraging, etc.)
7. **Performance profiling** and optimization
8. **Documentation updates** and tutorials

### Design Considerations
- **Backward compatibility** in the API
- **Incremental development** approach
- **Modular architecture** for easy extension
- **Test-driven development** for reliability
