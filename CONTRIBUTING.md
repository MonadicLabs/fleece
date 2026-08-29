# Contributing to fleece

fleece is a decentralized swarm coordination runtime, sized for a
microcontroller rather than a companion computer, built and battle-tested as
the coordination layer inside [swarmpu](https://monadiclabs.github.io/products/spu)
(our own hardware product) — but designed from the start to be a standalone
runtime, reusable by anyone building a fleet of constrained, intermittently-
connected devices, not just us. If you're working on multi-robot
coordination, gossip-based state sync, or embedded planning and any of that
sounds like your problem, this is meant to be genuinely useful to you, not
just a showcase for our own hardware.

## License, in plain terms

fleece is [AGPLv3](LICENSE). You can use it, read it, modify it, and ship
products built on it, for free, as long as your own code stays open under
the same terms — including if you're running a modified fleece as a network
service, not just distributing a binary (that's the "A" in AGPL).

If that doesn't work for you — you want to embed fleece in a closed-source
product — get in touch about a commercial license instead of assuming AGPL
is a wall. We'd rather have that conversation than have you route around
fleece entirely.

Note: this repository and its license cover fleece only. swarmpu, the
hardware product that embeds fleece, is a separate, closed-source codebase.

## Where to actually help

The README's own "Future Work" section is the honest, current list, not
aspirational filler — pick anything off it. A few worth calling out
specifically:

- **ROS2 platform bindings.** `examples/example3_embodied_swarm.c` shows
  the platform registry in real use for a toy 2D movement binding. A real
  MAVLink binding exists downstream (swarmpu's own firmware), but there's
  no ROS2 equivalent in this repo yet — if you work with ROS2 robots, this
  is the most directly useful thing you could add for yourself and for
  everyone downstream of this repo.
- **Hardware targets beyond STM32H7.** The one real embedded deployment
  target that exists today lives downstream in swarmpu; this repo itself
  still only builds and tests on desktop POSIX. Getting fleece running
  (and its test suite passing) on another real microcontroller target —
  ESP32, a different STM32 family, anything resource-constrained — is
  real, valuable, verifiable work.
- **More example applications.** Leader election, foraging at scale,
  anything that exercises the gossip/CRDT layer or the GOAP planner in a
  new way. Examples are also documentation — a good one teaches the next
  person faster than a paragraph of prose does.
- **Performance and footprint work.** fleece's whole design premise is
  "sized for a microcontroller" — profiling, trimming, and reducing memory
  pressure is never off-topic here.

If none of that fits what you're actually trying to build, open an issue
and describe it anyway. A real, honest example of what you need is more
useful to us than a Future Work list we wrote for ourselves.

## Before you send a PR

- Run the existing test suite (`README.md`'s own "Running Tests" section
  has the exact commands) and make sure it's still green.
- If you're touching the state manager, gossip, or CRDT merge logic
  specifically, add a test that would have caught your change breaking
  something — that code's correctness is the entire point of this project.
- Keep the "resource awareness" and "fail-fast networking" principles in
  `CLAUDE.md` in mind: this runs on hardware that doesn't have RAM or
  CPU cycles to spare, and the network it talks over is assumed lossy and
  partition-prone, not an afterthought to handle later.

## Questions before you commit to writing code

Open an issue first if you're not sure a change fits the project's
direction — a five-minute conversation before you write 500 lines beats
finding out after.
