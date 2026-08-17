// Fleece Example 2: Search + CBAA task allocation + delivery, over real UDP
// multicast.
//
// Unlike example1.c (single process, simulated in-process peer loopback),
// this is meant to be run as TWO OR MORE SEPARATE OS PROCESSES on the same
// machine, each joining the same UDP multicast group, to demonstrate real
// multi-process gossip:
//
//   ./example2_search_and_deliver 1 &
//   ./example2_search_and_deliver 2 &
//
// All the UDP socket handling below lives ONLY in this example - fleece
// itself stays fully transport-agnostic. This is wired in via two of
// FleeceComms's existing callback hooks:
//   - the send callback (fleece_comms_set_send_callback) does the real
//     sendto() when the runtime broadcasts a gossip frame
//   - the new poll callback (fleece_comms_set_poll_callback), invoked once
//     per runtime tick, drains the socket with non-blocking recvfrom() and
//     feeds each datagram straight into fleece_state_manager_import() -
//     bypassing fleece_comms_receive()/its receive callback entirely, the
//     same pattern example1.c's simulated-peer loopback already uses.
//
// Task allocation uses a CBAA-flavored protocol (Consensus-Based Auction
// Algorithm - the single-task-per-agent sibling of CBBA) implemented
// entirely in the JS layer on top of "world": targets are published as
// { status: 'unclaimed' | 'claimed' | 'delivered', assignedTo, bidScore, ... }
// records, claimed via worldCompareAndSet (optimistic compare-and-set, so a
// losing bid fails harmlessly rather than corrupting state), and re-checked
// every tick so an agent that gets outbid releases its claim and re-enters
// the bidding pool. This is a simplified, demo-scale analog of CBAA, not a
// formally verified implementation - see fleece_state_manager_set_shared_cas
// in the state manager header for the known limitation (shared-field
// timestamp ties are per-node local counters, not a synchronized clock) this
// inherits.

// _DEFAULT_SOURCE (rather than a strict _POSIX_C_SOURCE) is needed here for
// struct ip_mreq / IP_ADD_MEMBERSHIP - these are glibc BSD-socket extensions,
// not part of the POSIX base that fleece's own sources target.
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "runtime/fleece_runtime.h"
#include "comms/fleece_comms.h"
#include "state/fleece_state_manager.h"
#include "example_common.h"

#define MULTICAST_GROUP "239.1.1.1"
#define MULTICAST_PORT 5555
#define MAX_DATAGRAM 4096

typedef struct UdpTransport {
    int sock;
    struct sockaddr_in group_addr;
    FleeceStateManager* state_manager;  // for direct import() on receive
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

// fleece_comms send callback: forwards a gossip frame out over the real socket.
static void udp_on_comms_send(const char* destination, const uint8_t* data, uint32_t size, void* user_data) {
    (void)destination;
    UdpTransport* t = (UdpTransport*)user_data;
    ssize_t sent = sendto(t->sock, data, size, 0, (struct sockaddr*)&t->group_addr, sizeof(t->group_addr));
    if (sent < 0) {
        perror("sendto");
    }
}

// fleece_comms poll callback (invoked once per runtime tick): drains
// whatever's arrived on the socket, feeding each datagram directly into the
// state manager. A multicast socket receives its own sends by default
// (IP_MULTICAST_LOOP), but fleece_state_manager_import() already rejects any
// frame claiming to be from our own node id, so that's harmless - no
// self-filtering needed here.
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

int main(int argc, char** argv) {
    int agent_num = argc > 1 ? atoi(argv[1]) : 1;
    if (agent_num <= 0) agent_num = 1;
    uint64_t node_id = 0xA6E0000000000000ULL | (uint64_t)(uint32_t)agent_num;

    printf("Fleece Example 2: Search + CBAA + Delivery over UDP multicast\n");
    printf("===============================================================\n\n");
    printf("Agent #%d, node id %016llx\n", agent_num, (unsigned long long)node_id);
    printf("Multicast group %s:%d - run another instance with a different\n", MULTICAST_GROUP, MULTICAST_PORT);
    printf("agent number (e.g. `%s 2`) to see real multi-process gossip.\n\n", argv[0]);

    FleeceRuntime* runtime = fleece_runtime_create_with_node_id(node_id);
    if (!runtime) {
        fprintf(stderr, "Failed to create runtime\n");
        return 1;
    }

    FleeceComms* comms = (FleeceComms*)fleece_runtime_get_comms(runtime);
    FleeceStateManager* state_manager = (FleeceStateManager*)fleece_runtime_get_state_manager(runtime);

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

    char* script = fleece_example_load_script(argv[0], "example2_search_and_deliver.js");
    if (!script) {
        fprintf(stderr, "Failed to locate example2_search_and_deliver.js (expected alongside examples/, near the executable)\n");
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
