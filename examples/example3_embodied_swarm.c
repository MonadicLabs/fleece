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
#include "example_common.h"

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

    char* script = fleece_example_load_script(argv[0], "example3_embodied_swarm.js");
    if (!script) {
        fprintf(stderr, "Failed to locate example3_embodied_swarm.js (expected alongside examples/, near the executable)\n");
    } else {
        if (fleece_runtime_load_script(runtime, script) != 0) {
            fprintf(stderr, "Failed to load script\n");
        }
        free(script);
    }

    printf("Starting runtime...\n");
    printf("Press Ctrl+C to stop\n\n");

    int result = fleece_runtime_start(runtime);

    fleece_comms_close(comms);
    udp_transport_close(&transport);
    fleece_runtime_destroy(runtime);

    return result;
}
