# Project: fleece

fleece is a lightweight, decentralized swarm coordination runtime designed for microcontrollers. It leverages **QuickJS** for logic execution and **Reticulum** for robust, peer-to-peer, mesh-based communication.

## Core Concepts
- **Virtual Stigmergy:** Implements a decentralized, eventually consistent key-value store using a LWW (Last-Write-Wins) gossip protocol.
- **Embedded JavaScript:** Uses QuickJS to provide a high-level scripting environment on resource-constrained hardware.
- **Mesh Messaging:** Uses Reticulum to handle network discovery, encryption, and packet delivery across volatile wireless topologies.
- **CBOR State:** Shared swarm data is serialized using CBOR for maximum space efficiency over bandwidth-limited radio links.

## System Architecture
1. **BVM (Buzz Virtual Machine) Replacement:** A runtime environment built in C/C++ that hosts the QuickJS engine.
2. **State Manager:** A key-value store that tracks field-level timestamps and node IDs for conflict resolution.
3. **Comms Interface:** An abstraction layer bridging the QuickJS state to Reticulum's `Link` and `Transport` primitives.
4. **Time-Step Loop:** A synchronized or loosely-coupled execution loop:
   - Phase 1: Input (Sensors/Radio)
   - Phase 2: Gossip (State Synchronization)
   - Phase 3: Script Execution (QuickJS VM)
   - Phase 4: Output (Actuators/Mesh Broadcast)

## Key Technical Decisions
- **Consistency:** Eventually consistent via monotonic timestamps and deterministic node-ID tie-breaking (LWW).
- **Serialization:** All swarm-shared objects are serialized to CBOR to minimize packet size.
- **Constraints:** Optimized for low RAM usage by avoiding heavy CRDT metadata structures in favor of shallow LWW maps.

## Development Principles
- **Resource Awareness:** Always prioritize memory footprint over abstraction depth. Avoid heavy object allocation in the main loop.
- **Fail-Fast Networking:** Assume the network is volatile; assume nodes are dropping packets. Design logic to handle out-of-order delivery.
- **Decentralization:** No nodes are "masters." Every node is a full participant in the gossip protocol.
