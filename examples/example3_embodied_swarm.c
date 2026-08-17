// Fleece Example 3: Embodied swarm - search + CBAA task allocation + delivery
// with real 2D physical movement (position, speed, arrival), over real UDP
// multicast.
//
// Builds directly on example2_search_and_deliver.c (same UDP multicast
// transport, same CBAA-flavored claim protocol on "world") and adds genuine
// embodiment: each agent has a real position and a max speed, exposed to
// script as a "platform" binding - fleece's pure name->function hardware
// registry (see include/platform/fleece_platform.h) - rather than the
// abstract, instantly-updated position example2 used. A claimed target is
// only delivered once the agent has PHYSICALLY arrived there, not just after
// holding the claim for a fixed number of ticks.
//
// Run as two or more separate processes, same as example2:
//   ./example3_embodied_swarm 1 &
//   ./example3_embodied_swarm 2 &
// or via examples/run_swarm3.sh for a single-command multi-agent launch.
//
// Two platform functions are registered, each bound to this process's own
// AgentPhysics struct via fleece_platform_register()'s user_data:
//   - platform.getPosition()       -> {x, y}                  (snapshot read)
//   - platform.moveToward(x, y)    -> {x, y, arrived}          (advance <= max_speed
//                                                                units toward (x,y) this tick)
// The script mirrors the returned x/y onto self.x/self.y each time, so a
// moving agent's live position is visible to peers via the existing self/swarm
// gossip - no new plumbing needed for that part.

// _DEFAULT_SOURCE (rather than a strict _POSIX_C_SOURCE) is needed here for
// struct ip_mreq / IP_ADD_MEMBERSHIP - these are glibc BSD-socket extensions,
// not part of the POSIX base that fleece's own sources target.
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "runtime/fleece_runtime.h"
#include "comms/fleece_comms.h"
#include "state/fleece_state_manager.h"
#include "platform/fleece_platform.h"

#define MULTICAST_GROUP "239.1.2.3"  // distinct from example2's group, so both can run at once without interfering
#define MULTICAST_PORT 5556
#define MAX_DATAGRAM 4096
#define WORLD_SIZE 100.0  // both axes range over [0, WORLD_SIZE)

// --- UDP multicast transport (identical technique to example2_search_and_deliver.c) ---

typedef struct UdpTransport {
    int sock;
    struct sockaddr_in group_addr;
    FleeceStateManager* state_manager;
} UdpTransport;

static int udp_transport_init(UdpTransport* t, FleeceStateManager* state_manager) {
    t->state_manager = state_manager;
    t->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (t->sock < 0) {
        perror("socket");
        return -1;
    }

    int reuse = 1;
    setsockopt(t->sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(t->sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(MULTICAST_PORT);
    if (bind(t->sock, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("bind");
        close(t->sock);
        return -1;
    }

    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = inet_addr(MULTICAST_GROUP);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(t->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        perror("IP_ADD_MEMBERSHIP (is a multicast-capable interface available?)");
        close(t->sock);
        return -1;
    }

    memset(&t->group_addr, 0, sizeof(t->group_addr));
    t->group_addr.sin_family = AF_INET;
    t->group_addr.sin_addr.s_addr = inet_addr(MULTICAST_GROUP);
    t->group_addr.sin_port = htons(MULTICAST_PORT);

    int flags = fcntl(t->sock, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(t->sock, F_SETFL, flags | O_NONBLOCK);
    }

    return 0;
}

static void udp_transport_close(UdpTransport* t) {
    if (t->sock >= 0) {
        close(t->sock);
        t->sock = -1;
    }
}

static void udp_on_comms_send(const char* destination, const uint8_t* data, uint32_t size, void* user_data) {
    (void)destination;
    UdpTransport* t = (UdpTransport*)user_data;
    ssize_t sent = sendto(t->sock, data, size, 0, (struct sockaddr*)&t->group_addr, sizeof(t->group_addr));
    if (sent < 0) {
        perror("sendto");
    }
}

static void udp_on_comms_poll(void* user_data) {
    UdpTransport* t = (UdpTransport*)user_data;
    uint8_t buf[MAX_DATAGRAM];
    for (;;) {
        ssize_t n = recvfrom(t->sock, buf, sizeof(buf), 0, NULL, NULL);
        if (n < 0) {
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                perror("recvfrom");
            }
            break;
        }
        if (n > 0) {
            fleece_state_manager_import(t->state_manager, buf, (uint32_t)n);
        }
    }
}

// --- Physics: this process's "hardware" - a simple point-mass with a max speed ---

typedef struct AgentPhysics {
    double x, y;
    double max_speed;  // world units per tick
} AgentPhysics;

// splitmix64-style finalizer - just needs to spread small sequential agent
// numbers (1, 2, 3, ...) across the coordinate space; not a hash needing any
// cryptographic property. Deterministic (unlike seeding from wall-clock
// randomness): two processes started in the same instant still land on
// distinct positions, since it's derived from each process's own node id.
static uint32_t hash_u64(uint64_t v) {
    v ^= v >> 33;
    v *= 0xff51afd7ed558ccdULL;
    v ^= v >> 33;
    v *= 0xc4ceb9fe1a85ec53ULL;
    v ^= v >> 33;
    return (uint32_t)v;
}

static void physics_init(AgentPhysics* phys, uint64_t node_id) {
    uint32_t h = hash_u64(node_id);
    phys->x = (double)(h % 10007) / 10007.0 * WORLD_SIZE;
    phys->y = (double)((h >> 12) % 10007) / 10007.0 * WORLD_SIZE;
    phys->max_speed = 4.0;
}

// Writes {"x":..,"y":..} or {"x":..,"y":..,"arrived":true|false} as the
// platform function's JSON result.
static int write_position_result(uint8_t** result_json, uint32_t* result_size, double x, double y, const char* arrived_literal) {
    char buf[160];
    int n = arrived_literal
        ? snprintf(buf, sizeof(buf), "{\"x\":%.4f,\"y\":%.4f,\"arrived\":%s}", x, y, arrived_literal)
        : snprintf(buf, sizeof(buf), "{\"x\":%.4f,\"y\":%.4f}", x, y);
    if (n < 0 || (size_t)n >= sizeof(buf)) return -1;

    uint8_t* copy = (uint8_t*)malloc((size_t)n);
    if (!copy) return -1;
    memcpy(copy, buf, (size_t)n);
    *result_json = copy;
    *result_size = (uint32_t)n;
    return 0;
}

static int platform_get_position(const uint8_t* args_json, uint32_t args_size, uint8_t** result_json, uint32_t* result_size, void* user_data) {
    (void)args_json; (void)args_size;
    AgentPhysics* phys = (AgentPhysics*)user_data;
    return write_position_result(result_json, result_size, phys->x, phys->y, NULL);
}

// platform.moveToward(x, y) -> {x, y, arrived}. Advances at most max_speed
// units toward (x, y) this tick and reports the resulting position.
static int platform_move_toward(const uint8_t* args_json, uint32_t args_size, uint8_t** result_json, uint32_t* result_size, void* user_data) {
    AgentPhysics* phys = (AgentPhysics*)user_data;

    // fleece_platform_call always marshals JS call arguments as a JSON array
    // (see js_platform_call in fleece_embedded.c) - for platform.moveToward(x, y)
    // that's always exactly "[<number>,<number>]" with no whitespace (QuickJS's
    // JSON.stringify without an indent argument never inserts any). This
    // sscanf is a safe, sufficient parse for that fixed shape - fleece_platform
    // itself has no JSON parser (see fleece_platform.h); it's not meant to.
    if (!args_json || args_size == 0 || args_size >= 128) return -1;
    char buf[128];
    memcpy(buf, args_json, args_size);
    buf[args_size] = '\0';

    double tx, ty;
    if (sscanf(buf, "[%lf,%lf]", &tx, &ty) != 2) {
        return -1;
    }

    double dx = tx - phys->x;
    double dy = ty - phys->y;
    double dist = sqrt(dx * dx + dy * dy);

    bool arrived;
    if (dist <= phys->max_speed || dist < 1e-9) {
        phys->x = tx;
        phys->y = ty;
        arrived = true;
    } else {
        phys->x += dx / dist * phys->max_speed;
        phys->y += dy / dist * phys->max_speed;
        arrived = false;
    }

    return write_position_result(result_json, result_size, phys->x, phys->y, arrived ? "true" : "false");
}

// --- Script ---

static const char* SCRIPT =
    "var TARGETS = {\n"
    "  T1: { x: 10, y: 80, type: 'debris' },\n"
    "  T2: { x: 70, y: 20, type: 'survivor' },\n"
    "  T3: { x: 40, y: 50, type: 'debris' }\n"
    "};\n"
    "var SETTLE_TICKS = 2;    // consecutive ticks (after physical arrival) a claim must hold before delivering\n"
    "var DISCOVERY_TICK = 2;  // tick at which each agent publishes any target it doesn't yet see\n"
    "var CONTEST_MARGIN = 5;  // must beat the current holder's score by more than this to contest\n"
    "                         // an already-claimed target - unlike example2, scores here are LIVE\n"
    "                         // (based on current position, which changes every tick while moving),\n"
    "                         // so with no margin two agents converging on the same target endlessly\n"
    "                         // leapfrog each other's claim as their live distances see-saw past one\n"
    "                         // another - observed in practice before this margin was added.\n"
    "var RENDER_EVERY = 50;   // how often (in ticks) to print this agent's local view of the swarm -\n"
    "                         // kept infrequent because the grid is several lines: when run via\n"
    "                         // run_swarm3.sh, multiple real processes' output is interleaved\n"
    "                         // line-by-line, so a multi-line grid from one agent can get shredded\n"
    "                         // by another agent's single-line status output between its rows. For\n"
    "                         // a clean, unbroken grid, run a single agent directly instead (e.g.\n"
    "                         // `./example3_embodied_swarm 1`) - the single-line claimed/ARRIVED/\n"
    "                         // DELIVERED events remain the reliable signal either way.\n"
    "var GRID_W = 40, GRID_H = 18;\n"
    "\n"
    "function dist(a, b) {\n"
    "  var dx = a.x - b.x, dy = a.y - b.y;\n"
    "  return Math.sqrt(dx * dx + dy * dy);\n"
    "}\n"
    "\n"
    "function scoreFor(targetId) {\n"
    "  return -dist(self, TARGETS[targetId]);  // higher (closer to 0) = better; based on CURRENT (live) position\n"
    "}\n"
    "\n"
    "function init() {\n"
    "  var pos = platform.getPosition();\n"
    "  self.x = pos.x; self.y = pos.y;\n"
    "  self.myTask = null;\n"
    "  self.holdTicks = 0;\n"
    "  self.delivered = 0;\n"
    "  console.log('agent', self.id, 'ready at (' + self.x.toFixed(1) + ',' + self.y.toFixed(1) + ')');\n"
    "}\n"
    "\n"
    "// Renders this agent's OWN local view of the swarm (self + swarm + world) as\n"
    "// ASCII - no agent has a global view; this is honestly what it currently\n"
    "// believes, same as everything else in fleece. Legend: @ = me, + = a live\n"
    "// peer, x = unclaimed target, o = claimed target, # = delivered target.\n"
    "function renderGrid() {\n"
    "  var grid = [];\n"
    "  for (var r = 0; r < GRID_H; r++) { var row = []; for (var c = 0; c < GRID_W; c++) row.push('.'); grid.push(row); }\n"
    "  function plot(x, y, ch) {\n"
    "    var c = Math.min(GRID_W - 1, Math.max(0, Math.round(x / 100 * (GRID_W - 1))));\n"
    "    var r = Math.min(GRID_H - 1, Math.max(0, Math.round((100 - y) / 100 * (GRID_H - 1))));\n"
    "    grid[r][c] = ch;\n"
    "  }\n"
    "  for (var tid in TARGETS) {\n"
    "    var t = world[tid];\n"
    "    if (t) plot(t.x, t.y, t.status === 'delivered' ? '#' : (t.status === 'claimed' ? 'o' : 'x'));\n"
    "  }\n"
    "  var peers = Object.keys(swarm);\n"
    "  for (var i = 0; i < peers.length; i++) {\n"
    "    var p = swarm[peers[i]];\n"
    "    if (p && typeof p.x === 'number' && typeof p.y === 'number') plot(p.x, p.y, '+');\n"
    "  }\n"
    "  plot(self.x, self.y, '@');\n"
    "  console.log('--- agent', self.id, 'view @ tick', self.uptime, '---');\n"
    "  for (var row2 = 0; row2 < GRID_H; row2++) console.log(grid[row2].join(''));\n"
    "}\n"
    "\n"
    "function step() {\n"
    "  self.uptime = (self.uptime || 0) + 1;\n"
    "\n"
    "  if (self.uptime === DISCOVERY_TICK) {\n"
    "    for (var id in TARGETS) {\n"
    "      if (!(id in world)) {\n"
    "        var t = TARGETS[id];\n"
    "        world[id] = { x: t.x, y: t.y, type: t.type, status: 'unclaimed', assignedTo: null, bidScore: null };\n"
    "        console.log('agent', self.id, 'published target', id);\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "\n"
    "  if (self.myTask !== null) {\n"
    "    var mine = world[self.myTask];\n"
    "    if (!mine || mine.assignedTo !== self.id) {\n"
    "      console.log('agent', self.id, 'lost claim on', self.myTask, '- re-entering bid pool');\n"
    "      self.myTask = null;\n"
    "      self.holdTicks = 0;\n"
    "    } else if (mine.status === 'claimed') {\n"
    "      var t = TARGETS[self.myTask];\n"
    "      var pos = platform.moveToward(t.x, t.y);\n"
    "      self.x = pos.x; self.y = pos.y;\n"
    "      if (pos.arrived) {\n"
    "        if (self.holdTicks === 0) console.log('agent', self.id, 'ARRIVED at', self.myTask);\n"
    "        self.holdTicks++;\n"
    "        if (self.holdTicks >= SETTLE_TICKS) {\n"
    "          var delivered = Object.assign({}, mine, { status: 'delivered' });\n"
    "          if (worldCompareAndSet(self.myTask, mine, delivered)) {\n"
    "            console.log('agent', self.id, 'DELIVERED', self.myTask);\n"
    "            self.delivered++;\n"
    "            self.myTask = null;\n"
    "            self.holdTicks = 0;\n"
    "          }\n"
    "        }\n"
    "      } else {\n"
    "        self.holdTicks = 0;  // still traveling - reset arrival-stability counter\n"
    "      }\n"
    "    }\n"
    "  } else {\n"
    "    var bestId = null, bestScore = -Infinity, bestCurrent = null;\n"
    "    for (var tid in TARGETS) {\n"
    "      var current = world[tid];\n"
    "      if (!current || current.status === 'delivered') continue;\n"
    "      var myScore = scoreFor(tid);\n"
    "      var eligible = current.status === 'unclaimed' || myScore > current.bidScore + CONTEST_MARGIN;\n"
    "      if (eligible && myScore > bestScore) {\n"
    "        bestScore = myScore;\n"
    "        bestId = tid;\n"
    "        bestCurrent = current;\n"
    "      }\n"
    "    }\n"
    "    if (bestId !== null) {\n"
    "      var claimed = Object.assign({}, bestCurrent, { status: 'claimed', assignedTo: self.id, bidScore: bestScore });\n"
    "      if (worldCompareAndSet(bestId, bestCurrent, claimed)) {\n"
    "        self.myTask = bestId;\n"
    "        self.holdTicks = 0;\n"
    "        console.log('agent', self.id, 'claimed', bestId, 'score', bestScore.toFixed(2), '- moving...');\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "\n"
    "  if (self.uptime % RENDER_EVERY === 0) renderGrid();\n"
    "\n"
    "  var allDelivered = true;\n"
    "  for (var k in TARGETS) {\n"
    "    var c = world[k];\n"
    "    if (!c || c.status !== 'delivered') { allDelivered = false; break; }\n"
    "  }\n"
    "  if (allDelivered && !self.announcedDone) {\n"
    "    self.announcedDone = true;\n"
    "    console.log('agent', self.id, ': all targets delivered');\n"
    "  }\n"
    "}\n"
    "\n"
    "function destroy() {\n"
    "  console.log('agent', self.id, 'shutting down, delivered', self.delivered, 'target(s)');\n"
    "}\n";

int main(int argc, char** argv) {
    int agent_num = argc > 1 ? atoi(argv[1]) : 1;
    if (agent_num <= 0) agent_num = 1;
    uint64_t node_id = 0xE0B0000000000000ULL | (uint64_t)(uint32_t)agent_num;

    printf("Fleece Example 3: Embodied Swarm (search + CBAA + real movement)\n");
    printf("===================================================================\n\n");
    printf("Agent #%d, node id %016llx\n", agent_num, (unsigned long long)node_id);
    printf("Multicast group %s:%d - run another instance with a different\n", MULTICAST_GROUP, MULTICAST_PORT);
    printf("agent number (e.g. `%s 2`), or use examples/run_swarm3.sh.\n\n", argv[0]);

    FleeceRuntime* runtime = fleece_runtime_create_with_node_id(node_id);
    if (!runtime) {
        fprintf(stderr, "Failed to create runtime\n");
        return 1;
    }

    FleeceComms* comms = (FleeceComms*)fleece_runtime_get_comms(runtime);
    FleeceStateManager* state_manager = (FleeceStateManager*)fleece_runtime_get_state_manager(runtime);
    FleecePlatform* platform = (FleecePlatform*)fleece_runtime_get_platform(runtime);

    AgentPhysics physics;
    physics_init(&physics, node_id);
    fleece_platform_register(platform, "getPosition", platform_get_position, &physics);
    fleece_platform_register(platform, "moveToward", platform_move_toward, &physics);

    UdpTransport transport;
    if (udp_transport_init(&transport, state_manager) != 0) {
        fprintf(stderr, "Failed to initialize UDP multicast transport\n");
        fleece_runtime_destroy(runtime);
        return 1;
    }

    if (fleece_comms_initialize(comms) != 0) {
        fprintf(stderr, "Failed to initialize comms\n");
        udp_transport_close(&transport);
        fleece_runtime_destroy(runtime);
        return 1;
    }
    fleece_comms_set_send_callback(comms, udp_on_comms_send, &transport);
    fleece_comms_set_poll_callback(comms, udp_on_comms_poll, &transport);

    if (fleece_runtime_load_script(runtime, SCRIPT) != 0) {
        fprintf(stderr, "Failed to load script\n");
    }

    printf("Starting runtime...\n");
    printf("Press Ctrl+C to stop\n\n");

    int result = fleece_runtime_start(runtime);

    fleece_comms_close(comms);
    udp_transport_close(&transport);
    fleece_runtime_destroy(runtime);

    return result;
}
