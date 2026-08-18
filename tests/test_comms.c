// Fleece comms telemetry tests.
#include <stdio.h>
#include <string.h>

#include "fleece_comms.h"

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s\n", msg); \
        g_failures++; \
    } \
} while (0)

static unsigned g_sent_packets = 0;
static unsigned g_sent_bytes = 0;
static void count_send(const char* destination, const uint8_t* data, uint32_t size, void* user_data) {
    (void)destination; (void)user_data;
    g_sent_packets++;
    g_sent_bytes += size;
}

int main(void) {
    printf("=== Fleece Comms Telemetry Tests ===\n");

    FleeceComms* comms = fleece_comms_create();
    CHECK(comms != NULL, "comms create");

    FleeceCommsStats st = {0};
    fleece_comms_get_stats(comms, &st);
    CHECK(st.packets_sent == 0 && st.bytes_sent == 0, "fresh comms has zero counters");

    // Send before init: rejected and counted as a send failure.
    const uint8_t frame[] = { 1, 2, 3 };
    CHECK(fleece_comms_send(comms, "peer", frame, sizeof(frame)) != 0, "send before init fails");
    fleece_comms_get_stats(comms, &st);
    CHECK(st.send_failures == 1, "rejected send counted as send failure");

    fleece_comms_set_send_callback(comms, count_send, NULL);
    CHECK(fleece_comms_initialize(comms) == 0, "comms init");

    // Two sends -> counted, and the transport callback still fires.
    CHECK(fleece_comms_send(comms, "peer", frame, sizeof(frame)) == 0, "send 1");
    CHECK(fleece_comms_send(comms, "peer", frame, sizeof(frame)) == 0, "send 2");
    fleece_comms_get_stats(comms, &st);
    CHECK(st.packets_sent == 2, "two packets counted");
    CHECK(st.bytes_sent == 6, "six bytes counted");
    CHECK(g_sent_packets == 2 && g_sent_bytes == 6, "transport callback still receives every send");

    // Received side via the poll-callback import pattern.
    CHECK(fleece_comms_notify_received(comms, 5) == 0, "notify_received ok");
    CHECK(fleece_comms_notify_received(comms, 7) == 0, "notify_received ok");
    fleece_comms_get_stats(comms, &st);
    CHECK(st.packets_received == 2, "two frames received counted");
    CHECK(st.bytes_received == 12, "twelve received bytes counted");

    // Notify while closed: failure counter.
    fleece_comms_close(comms);
    CHECK(fleece_comms_notify_received(comms, 1) != 0, "notify after close fails");
    fleece_comms_get_stats(comms, &st);
    CHECK(st.recv_failures == 1, "failed notify counted as recv failure");
    CHECK(st.bytes_received == 12, "failed notify did not credit bytes");

    fleece_comms_destroy(comms);

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}