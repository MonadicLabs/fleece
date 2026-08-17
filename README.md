# Fleece - Lightweight Swarm Coordination Runtime

fleece is a **lightweight, decentralized swarm coordination runtime** designed for **microcontrollers**.

## Key Features

### 🔄 Virtual Stigmergy
- **Decentralized, eventually consistent key-value store**
- **LWW (Last-Write-Wins) gossip protocol** for conflict resolution
- **Field-level timestamps** and **node ID tie-breaking**
- Optimized for **low RAM usage** in microcontroller environments

### 💻 Embedded JavaScript
- **QuickJS integration** for high-level scripting
- Resource-aware execution for constrained hardware
- C-to-JS bindings for native system access
- Sandboxed execution for security

### 📦 Gossip State
- **Compact binary framing** for bandwidth-limited links (not CBOR - a small length-prefixed record format, see `src/state/fleece_state_manager.c`)
- Binary format optimized for microcontroller constraints
- Efficient field-level versioning and conflict resolution

## System Architecture

### Time-Step Loop
The core execution model follows a synchronized **4-phase coordination cycle**:

1. **Phase 1: Input (Sensors/Radio)**
   - Process sensor data and incoming radio packets
   - Update local state from external inputs
   - Handle wireless communication events

2. **Phase 2: Gossip (State Synchronization)**
   - Exchange state with peer nodes via mesh network
   - Resolve conflicts using LWW semantics
   - Maintain eventual consistency across the swarm

3. **Phase 3: Script Execution (QuickJS VM)**
   - Run user-defined swarm logic scripts
   - Access shared state through the LWW store
   - Coordinate behavior based on global state

4. **Phase 4: Output (Actuators/Mesh Broadcast)**
   - Control actuators and send mesh broadcasts
   - Disseminate updates to peer nodes
   - Implement swarm coordination behaviors

## Technical Decisions

### Consistency
- **Eventually consistent** via monotonic timestamps
- **Deterministic node-ID tie-breaking** for conflict resolution
- **Conflict-free Replicated Data Types (CRDT)** principles
- No central authority or master nodes

### Serialization
- **Compact binary gossip frame** for all state (not CBOR - see `fleece_state_manager_export`/`_import`)
- **Minimal overhead** for bandwidth-constrained links
- **Field-level versioning** via per-field timestamp + owner node id
- Full-state export per gossip round (no delta encoding yet)

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
fleece_runtime_start(runtime); // Main 4-phase loop
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

#### FleeceCore
```c
FleeceCore* core = fleece_core_create();
fleece_core_initialize(core);
fleece_core_run(core); // Execute coordination loop
fleece_core_destroy(core);
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
fleece_embedded_execute(embedded, "console.log('Hello from fleece!');");
fleece_embedded_destroy(embedded);
```

## Directory Structure

```
fleece/
├── include/
│   ├── runtime/           # Runtime interface headers
│   ├── state/             # State manager interface
│   ├── comms/            # Comms interface
│   ├── embedded/         # Embedded JS interface
│   └── core/             # Core system interface
├── src/
│   ├── runtime/          # Runtime implementation
│   ├── state/            # State manager implementation
│   ├── comms/           # Comms implementation
│   ├── embedded/        # Embedded JS implementation
│   └── core/            # Core implementation
├── examples/             # Example applications
│   └── example1.c       # Basic swarm coordination example
├── tests/                # Unit tests
│   └── test_state_manager.c # State manager LWW tests
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
mkdir build && cd build
cmake ..
make -j4
```

### Running Tests
```bash
./build/fleece_tests
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
// Execute JavaScript that processes swarm state
fleece_embedded_execute(embedded, 
    "if (shared_state.temperature > threshold) { act.cool(); }");
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
4. **Hardware-specific optimizations** (ESP32, STM32, etc.)
5. **More example applications** (leader election, foraging, etc.)
6. **Performance profiling** and optimization
7. **Unit test coverage** improvements
8. **Documentation updates** and tutorials

### Design Considerations
- **Backward compatibility** in the API
- **Incremental development** approach
- **Modular architecture** for easy extension
- **Test-driven development** for reliability
