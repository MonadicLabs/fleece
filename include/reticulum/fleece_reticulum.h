/*
 * fleece's mesh transport: Reticulum.
 *
 * CLAUDE.md has described fleece as leveraging "Reticulum for robust,
 * peer-to-peer, mesh-based communication" since the beginning; until this
 * module existed that was aspirational, and every integrator had to bring
 * their own transport and wire it into FleeceComms' send/poll callbacks by
 * hand. This is that transport, in-tree.
 *
 * What lives here is everything about being a mesh peer that is NOT
 * platform-specific: node identity (generation, persistence, and the
 * derivation of fleece's own node id from it), the peer table, the two
 * Reticulum destinations, announce handling, and packet fan-out.
 *
 * What does NOT live here, and is supplied by the host through
 * FleeceReticulumConfig, is anything that needs a specific chip or OS:
 *
 *   - the radio itself (an RNS::InterfaceImpl the host constructs over its
 *     own UART/SPI/whatever),
 *   - non-volatile storage for the identity key,
 *   - an entropy source and a device id,
 *   - a log sink.
 *
 * That split is deliberate: it is what lets this file be the same code on an
 * STM32 over a LoRa radio, on a host process over UDP, and in a simulator,
 * without fleece itself ever learning what a Zephyr device or an NVS
 * partition is.
 *
 * Plain C interface so C callers (and C++ ones) can both use it; the
 * implementation is C++ because microReticulum is. Nothing exception-shaped
 * crosses this boundary -- every entry point catches and converts, matching
 * the discipline the swarmpu bridge established before this moved here.
 */
#ifndef FLEECE_RETICULUM_H
#define FLEECE_RETICULUM_H

#include <stdbool.h>
#include <stdint.h>

#include "state/fleece_state_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Size of the identity key blob the host is asked to persist: a 32-byte
 * X25519 private key followed by a 32-byte Ed25519 one, which is what
 * RNS::Identity::get_private_key() returns and load_private_key() expects.
 */
#define FLEECE_RETICULUM_IDENTITY_KEY_SIZE 64

/* Loads a previously persisted identity key. Return true if `out` was filled
 * with exactly FLEECE_RETICULUM_IDENTITY_KEY_SIZE bytes; false if nothing has
 * been stored yet (first boot), which makes fleece generate and then save one.
 */
typedef bool (*FleeceReticulumIdentityLoadFn)(uint8_t *out, uint32_t size, void *user_data);

/* Persists an identity key. Return 0 on success, negative on failure. Failure
 * is not fatal -- the node runs on the in-memory identity for this boot -- but
 * it IS reported, because the difference between "stable across reboots" and
 * "new address every power cycle" matters to every peer.
 */
typedef int (*FleeceReticulumIdentitySaveFn)(const uint8_t *key, uint32_t size, void *user_data);

/* Writes up to `size` bytes that differ between physical units -- a factory
 * serial, a MAC, a burnt-in unique id. Returns the number written, or 0 if
 * none is available.
 *
 * Explicitly NOT required to be secret or unpredictable; it is mixed into the
 * identity RNG with zero entropy credit. Its job is to guarantee that two
 * units diverge even when the entropy source below is weak or missing, which
 * is exactly the case in most simulators.
 */
typedef uint32_t (*FleeceReticulumDeviceIdFn)(uint8_t *out, uint32_t size, void *user_data);

/* Fills `out` with `size` cryptographically secure random bytes. Returns 0 on
 * success. This is the source of actual unpredictability for a generated
 * identity; if it fails, fleece logs that the resulting identity is unique but
 * predictable rather than silently pretending otherwise.
 */
typedef int (*FleeceReticulumEntropyFn)(uint8_t *out, uint32_t size, void *user_data);

/* Receives one already-formatted, newline-terminated log line. */
typedef void (*FleeceReticulumLogFn)(const char *line, void *user_data);

/* Receives one inbound control-channel packet (see the control aspect below). */
typedef void (*FleeceReticulumControlRecvFn)(const uint8_t *data, uint32_t size, void *user_data);

typedef struct FleeceReticulumConfig {
	/* An RNS::InterfaceImpl* the host has constructed over its own radio
	 * hardware, passed opaquely so this header stays C. fleece registers it
	 * with Reticulum's Transport and drives it; it must outlive the runtime.
	 */
	void *rns_interface;

	/* Reticulum app name for both destinations, e.g. "swarmpu". The two
	 * aspects underneath it are fixed: "<app_name>.fleece" carries gossip,
	 * "<app_name>.control" carries host-directed messages. NULL means
	 * "fleece".
	 */
	const char *app_name;

	FleeceReticulumIdentityLoadFn identity_load;
	FleeceReticulumIdentitySaveFn identity_save;
	FleeceReticulumDeviceIdFn device_id;
	FleeceReticulumEntropyFn entropy;
	FleeceReticulumLogFn log;

	void *user_data;
} FleeceReticulumConfig;

/* Installs the host's hooks. Call once, before anything else here. The struct
 * is copied; the pointers inside it (rns_interface, app_name, user_data) must
 * outlive the runtime.
 */
void fleece_reticulum_configure(const FleeceReticulumConfig *config);

/* Loads this node's identity from the host's storage, or generates one and
 * asks the host to persist it. Idempotent -- later calls are no-ops.
 *
 * Separate from fleece_reticulum_start() on purpose: the node id below is
 * needed to create the fleece runtime, which must exist before there is a
 * state manager to hand to start(). Callers therefore do
 * identity_init -> create runtime -> start.
 *
 * Returns true on success.
 */
bool fleece_reticulum_identity_init(void);

/* fleece's node id for this node -- the first 8 bytes of the Reticulum
 * identity hash (itself a truncated SHA-256 of the public key). Pass straight
 * to fleece_runtime_create_with_node_id().
 *
 * Deriving it from the identity rather than assigning it by hand is what makes
 * two units built from the same image distinct without any per-unit
 * configuration, and keeps a node's application-level id and its transport
 * address describing the same node. Requires fleece_reticulum_identity_init().
 */
uint64_t fleece_reticulum_node_id(void);

/* Brings up Reticulum on the configured interface and opens both destinations.
 * `state_manager` receives inbound gossip directly. Returns true on success.
 */
bool fleece_reticulum_start(FleeceStateManager *state_manager);

/* Matches FleeceCommsSendCallback -- pass to fleece_comms_set_send_callback().
 * Fans the payload out to every currently known peer's own gossip destination.
 */
void fleece_reticulum_send(const char *destination, const uint8_t *data, uint32_t size,
			    void *user_data);

/* Matches FleeceCommsPollCallback -- pass to fleece_comms_set_poll_callback().
 * Drives Reticulum's own loop, re-announces periodically, and delivers inbound
 * packets synchronously from within this call.
 */
void fleece_reticulum_poll(void *user_data);

/* Sends one packet on the control aspect -- a separate destination from
 * gossip, so host-directed traffic is routed and delivered independently and
 * never touches the state manager. Must fit the interface's MTU; chunking is
 * the caller's job.
 */
void fleece_reticulum_control_send(const uint8_t *data, uint32_t size);

/* Registers the sink for inbound control packets. One at a time; a second call
 * replaces the first.
 */
void fleece_reticulum_control_set_receive_callback(FleeceReticulumControlRecvFn callback,
						    void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* FLEECE_RETICULUM_H */
