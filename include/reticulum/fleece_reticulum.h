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
 *   - the radio itself, as a pair of send/receive callbacks over whatever
 *     link the host has,
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
#include <stddef.h>
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

/* Sends one packet on the host's radio. Returns true if it went out.
 *
 * Packets, not bytes: message boundaries are the host's problem, because they
 * are a property of its link. A packet radio has them natively; a transparent
 * serial radio needs framing the host supplies. Either way fleece hands over
 * one complete packet and gets one back, and never learns which it is.
 */
typedef bool (*FleeceRadioSendFn)(const uint8_t *data, uint32_t size, void *user_data);

/* Polls for one received packet, copying it into `out`. Returns its length, or
 * 0 if none is waiting. Must not block.
 *
 * Called repeatedly until it returns 0, so a host that has several packets
 * buffered should return them one per call rather than dropping any: a single
 * fleece tick can enqueue more than one outgoing frame, and a receiver that
 * hands back only one per tick falls permanently behind its own sender.
 */
typedef uint32_t (*FleeceRadioReceiveFn)(uint8_t *out, uint32_t max_size, void *user_data);

typedef struct FleeceReticulumConfig {
	/* The host's radio. fleece wraps these in a Reticulum interface of its
	 * own and drives it; microReticulum never appears in the host's code.
	 */
	FleeceRadioSendFn radio_send;
	FleeceRadioReceiveFn radio_receive;

	/* Largest packet the radio accepts, in bytes. Reported to Reticulum as
	 * the interface MTU, so it bounds what gets handed to radio_send.
	 */
	uint32_t radio_mtu;

	/* Nominal link rate in bits/second. Used only for Reticulum's own
	 * link-quality bookkeeping; fleece configures no hardware from it. 0
	 * leaves it unset.
	 */
	uint32_t radio_bitrate;

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

/* Fires after every inbound gossip frame is merged, reporting whether THIS
 * node's view of the shared/"world" stream diverges from the sender's
 * (fleece_state_manager_import_from's digest check). TRUE usually means "a
 * delta was dropped somewhere - run a targeted repair against this peer";
 * FALSE confirms we are current with it. sender_node_id names the frame's
 * sender (protocol v5 header) so the repair can be attributed and UNICAST to
 * that one peer instead of fanned out.
 *
 * This is how the runtime's on-demand resync learns about gaps when the mesh
 * transport is in play: inbound gossip merges inside this module (it never
 * passes through FleeceComms' receive path), so without this callback the
 * runtime would never see a behind report and dropped deltas would only heal
 * by luck.
 */
typedef void (*FleeceReticulumGapFn)(bool behind_shared, uint64_t sender_node_id, void *user_data);

void fleece_reticulum_set_gap_callback(FleeceReticulumGapFn callback, void *user_data);

/* Sends one packet ONLY to the peer whose fleece node id is given (node ids
 * are derived from identity hashes, see fleece_reticulum_node_id()). Single-
 * packet path only - handshake-sized payloads, not general bulk transfer.
 * Returns false if the peer is unknown or the send failed, leaving the caller
 * free to fall back to fan-out or retry.
 */
bool fleece_reticulum_send_to_node(uint64_t node_id, const uint8_t *data, uint32_t size);

/* Test-only accessor for the largest gossip payload sendToAllPeers() will put
 * directly into one RNS::Packet before falling back to a Resource/Link
 * transfer (see fleece_reticulum.cpp). Mirrors Reticulum's own
 * Type::Destination::ENCRYPTED_MDU derivation, evaluated against this node's
 * actually-configured radio_mtu rather than Reticulum's own hardcoded MTU
 * constant (500), which is larger than this project's real wire cap (460,
 * see swarmpu's kMeshMtuBytes) and so unsafe to reuse directly. Requires
 * fleece_reticulum_configure() to have been called first; 0 before that.
 */
size_t fleece_reticulum_single_packet_payload_ceiling(void);

#ifdef __cplusplus
}
#endif

#endif /* FLEECE_RETICULUM_H */
