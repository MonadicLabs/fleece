/*
 * Reticulum transport for fleece -- see include/reticulum/fleece_reticulum.h
 * for the split between what lives here and what the host supplies.
 *
 * Compiled WITH exceptions, because microReticulum's headers require them.
 * Everything that can throw is caught at the C boundary and turned into a
 * bool (the init/start entry points) or swallowed (send/poll, which run every
 * tick and have nowhere useful to report into -- a dropped mesh tick must
 * never halt the vehicle carrying the node). Nothing exception-shaped escapes
 * into C callers.
 *
 * Destinations are SINGLE, not PLAIN. Confirmed against microReticulum's own
 * Transport.cpp (packet_filter(): a PLAIN or GROUP packet is dropped once
 * hops() > 1) and the Reticulum manual (only SINGLE destinations get
 * Transport's multi-hop path discovery). A SINGLE destination is tied to an
 * identity, which is why identity is this module's problem and not the
 * host's, and why "broadcast" here means fanning out to each known peer
 * individually rather than emitting one packet.
 */
#include "reticulum/fleece_reticulum.h"

#include <microStore/Adapters/NoopFileSystem.h>
#include <microStore/FileSystem.h>

#include <microReticulum.h>

/* Crypto's RNG global, for seeding before identity generation. Via
 * microReticulum's compat shim rather than <RNG.h> directly: some SoC vendor
 * headers (STM32Cube, reached transitively under Zephyr) define an object-like
 * `RNG` macro that otherwise corrupts both the declaration and every use site.
 */
#include <microReticulum/Cryptography/RNGCompat.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace {

/* Bounded rather than dynamic, matching fleece's static-allocation
 * discipline everywhere else -- a development-scale swarm, not a production
 * ceiling.
 */
constexpr size_t kMaxPeers = 8;

/* fleece ticks about every 100ms, so 50 ticks is ~5s between re-announces.
 * Re-announcing periodically rather than only at boot matters twice over: a
 * node joining later still needs to hear from this one, and Reticulum's own
 * announce/path table entries are not guaranteed to persist on every node
 * that relayed them.
 */
constexpr uint32_t kAnnounceIntervalTicks = 50;

FleeceReticulumConfig g_config = {};
bool g_configured = false;

void log_line(const char *line)
{
	if (g_config.log != nullptr) {
		g_config.log(line, g_config.user_data);
	}
}

/* Small printf-into-a-buffer helper so call sites can format without each
 * one owning a buffer. Bounded; truncation is acceptable for log text.
 */
void logf_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void logf_line(const char *fmt, ...)
{
	if (g_config.log == nullptr) {
		return;
	}
	char buf[192];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	log_line(buf);
}

const char *app_name()
{
	return (g_config.app_name != nullptr) ? g_config.app_name : "fleece";
}

/* One entry per physical node this instance has heard an announce from,
 * shared across both destinations: they ride the same identity, so whichever
 * announced first is enough to know the node exists and to reconstruct either
 * of its destinations on demand.
 *
 * The table deliberately does NOT try to answer "is this the same physical
 * node as before". It only tracks "is this identity still worth sending to",
 * and staleness answers that regardless of why an identity went quiet -- a
 * node can vanish for good without this table ever being told, and a genuinely
 * new one can start announcing at any time. Hence last-seen-tick plus
 * evict-oldest-when-full.
 */
class PeerTable {
public:
	void add(const RNS::Identity &identity, uint32_t now_tick)
	{
		const RNS::Bytes &h = identity.hash();
		for (size_t i = 0; i < count_; i++) {
			if (identities_[i].hash().compare(h) == 0) {
				last_seen_tick_[i] = now_tick; // refresh, already known
				return;
			}
		}
		if (count_ < kMaxPeers) {
			identities_[count_] = identity;
			last_seen_tick_[count_] = now_tick;
			count_++;
			return;
		}
		// Full: evict whichever entry has gone longest without a fresh
		// announce. A live peer re-announces every kAnnounceIntervalTicks
		// and so is never the oldest; the oldest is the most likely ghost.
		size_t oldest = 0;
		for (size_t i = 1; i < count_; i++) {
			if (last_seen_tick_[i] < last_seen_tick_[oldest]) {
				oldest = i;
			}
		}
		identities_[oldest] = identity;
		last_seen_tick_[oldest] = now_tick;
	}
	size_t count() const { return count_; }
	const RNS::Identity &identity(size_t i) const { return identities_[i]; }

private:
	RNS::Identity identities_[kMaxPeers] = {RNS::Type::NONE, RNS::Type::NONE, RNS::Type::NONE,
						 RNS::Type::NONE, RNS::Type::NONE, RNS::Type::NONE,
						 RNS::Type::NONE, RNS::Type::NONE};
	uint32_t last_seen_tick_[kMaxPeers] = {0};
	size_t count_ = 0;
};

RNS::Reticulum *g_reticulum = nullptr;
RNS::Destination *g_destination = nullptr;
RNS::Destination *g_control_destination = nullptr;
FleeceStateManager *g_state_manager = nullptr;
FleeceReticulumControlRecvFn g_control_callback = nullptr;
void *g_control_callback_user_data = nullptr;
PeerTable g_peers;
uint32_t g_poll_tick_count = 0;
uint32_t g_packets_received = 0;
uint32_t g_import_failures = 0;
uint32_t g_send_attempts = 0;
uint32_t g_send_ok = 0;
uint32_t g_send_fail = 0;

bool g_identity_ready = false;
uint64_t g_node_id = 0;
RNS::Identity *g_identity_ptr = nullptr;

class PeerAnnounceHandler : public RNS::AnnounceHandler {
public:
	explicit PeerAnnounceHandler(const char *aspect_filter) : RNS::AnnounceHandler(aspect_filter) {}
	void received_announce(const RNS::Bytes & /*destination_hash*/, const RNS::Identity &announced_identity,
				const RNS::Bytes & /*app_data*/) override
	{
		g_peers.add(announced_identity, g_poll_tick_count);
	}
};

void onPacketReceived(const RNS::Bytes &data, const RNS::Packet & /*packet*/)
{
	if (g_state_manager != nullptr) {
		g_packets_received++;
		if (fleece_state_manager_import(g_state_manager, data.data(),
						 static_cast<uint32_t>(data.size())) != 0) {
			g_import_failures++;
		}
	}
}

void onControlPacketReceived(const RNS::Bytes &data, const RNS::Packet & /*packet*/)
{
	if (g_control_callback != nullptr) {
		g_control_callback(data.data(), static_cast<uint32_t>(data.size()),
				    g_control_callback_user_data);
	}
}

/* Sends to every known peer's own destination individually. "Broadcast" is
 * this node's responsibility, not Reticulum's, because SINGLE destinations are
 * per-identity rather than a shared name. `aspect` picks which of a peer's two
 * destinations to address -- same identity, different aspect, so a different
 * destination hash either way.
 */
void sendToAllPeers(const char *aspect, const RNS::Bytes &payload)
{
	g_send_attempts++;
	for (size_t i = 0; i < g_peers.count(); i++) {
		try {
			RNS::Destination peer_destination(g_peers.identity(i), RNS::Type::Destination::OUT,
							   RNS::Type::Destination::SINGLE, app_name(), aspect);
			RNS::Packet packet(peer_destination, payload);
			packet.send();
			g_send_ok++;
		} catch (...) {
			// One bad peer must not stop the fan-out to the rest.
			g_send_fail++;
		}
	}
}

/* Seeds Crypto's global RNG before any key material is drawn from it.
 *
 * Load-bearing, not defensive. Crypto's RNG.begin() is only called from
 * Reticulum::start(), which happens in fleece_reticulum_start() -- but the
 * identity has to exist before that, because its hash is the node id needed to
 * create the runtime whose state manager start() consumes. So without this,
 * key generation reads an RNG that was never seeded, and every unit produces
 * the identical keypair. That was observed for real: two nodes booting the
 * same image derived the same identity, and therefore the same SINGLE
 * destination hash, so each treated the other's announce as its own and
 * neither ever saw a peer. On real hardware it would mean shipping identical
 * private keys.
 *
 * Two sources, mixed with deliberately different entropy credits: the device
 * id with credit 0 (unique per unit, but readable by anyone holding the part,
 * so it buys divergence and not secrecy), and the host's CSPRNG credited its
 * full bit count (the actual unpredictability). A missing CSPRNG is reported
 * rather than quietly tolerated, so a build without real entropy can never be
 * mistaken for one with it.
 */
void seed_rng_for_identity()
{
	RNG.begin(app_name());

	if (g_config.device_id != nullptr) {
		uint8_t id[32];
		uint32_t len = g_config.device_id(id, sizeof(id), g_config.user_data);
		if (len > 0) {
			RNG.stir(id, len, 0);
		} else {
			log_line("fleece/reticulum: WARNING no device id to stir\n");
		}
	} else {
		log_line("fleece/reticulum: WARNING no device id hook configured\n");
	}

	if (g_config.entropy != nullptr) {
		uint8_t entropy[32];
		int rc = g_config.entropy(entropy, sizeof(entropy), g_config.user_data);
		if (rc == 0) {
			RNG.stir(entropy, sizeof(entropy), sizeof(entropy) * 8);
			return;
		}
		logf_line("fleece/reticulum: WARNING entropy source failed (%d) -- identity is unique "
			  "per device id but NOT unpredictable\n",
			  rc);
		return;
	}
	log_line("fleece/reticulum: WARNING no entropy hook configured -- identity is unique per "
		 "device id but NOT unpredictable\n");
}

} // namespace

extern "C" void fleece_reticulum_configure(const FleeceReticulumConfig *config)
{
	if (config == nullptr) {
		return;
	}
	g_config = *config;
	g_configured = true;
}

extern "C" bool fleece_reticulum_identity_init(void)
{
	if (g_identity_ready) {
		return true;
	}
	if (!g_configured) {
		return false;
	}
	try {
		// Function-local static: constructed once on first call, lives for
		// the rest of the process, same idiom as the long-lived Reticulum
		// objects in start() below.
		static RNS::Identity identity(false);

		uint8_t key[FLEECE_RETICULUM_IDENTITY_KEY_SIZE];
		bool loaded = g_config.identity_load != nullptr &&
			       g_config.identity_load(key, sizeof(key), g_config.user_data);

		if (loaded) {
			identity.load_private_key(RNS::Bytes(key, sizeof(key)));
			log_line("fleece/reticulum: identity loaded from storage\n");
		} else {
			// MUST precede RNS::Identity(true) -- see seed_rng_for_identity's
			// own comment for the identical-keypair bug that ordering fixes.
			seed_rng_for_identity();
			identity = RNS::Identity(true);
			RNS::Bytes prv = identity.get_private_key();
			if (prv.data() == nullptr || prv.size() != sizeof(key)) {
				logf_line("fleece/reticulum: FAILED unexpected private key size %u\n",
					  static_cast<unsigned>(prv.size()));
				return false;
			}
			std::memcpy(key, prv.data(), sizeof(key));

			int rc = (g_config.identity_save != nullptr)
					 ? g_config.identity_save(key, sizeof(key), g_config.user_data)
					 : -1;
			if (rc != 0) {
				// Not fatal to this boot -- the identity works in memory --
				// but it will not survive a reboot, and every peer would
				// then have to rediscover this node under a new address.
				logf_line("fleece/reticulum: identity generated but NOT saved (%d) -- will "
					  "not survive reboot\n",
					  rc);
			} else {
				log_line("fleece/reticulum: identity generated and saved\n");
			}
		}

		const RNS::Bytes &ih = identity.hash();
		if (ih.data() == nullptr || ih.size() < sizeof(g_node_id)) {
			log_line("fleece/reticulum: FAILED identity hash too short\n");
			return false;
		}
		std::memcpy(&g_node_id, ih.data(), sizeof(g_node_id));

		g_identity_ptr = &identity;
		g_identity_ready = true;
		return true;
	} catch (...) {
		log_line("fleece/reticulum: FAILED identity init threw\n");
		return false;
	}
}

extern "C" uint64_t fleece_reticulum_node_id(void)
{
	return g_node_id;
}

extern "C" bool fleece_reticulum_start(FleeceStateManager *state_manager)
{
	if (!g_configured || g_config.rns_interface == nullptr) {
		return false;
	}
	g_state_manager = state_manager;

	try {
		// Reticulum::start() requires SOME filesystem regardless of the
		// RNS_USE_FS/RNS_PERSIST_* flags, which gate persistence rather than
		// whether it looks for files at all. NoopFileSystem reports "not
		// found" for everything, which is correct here: identity persistence
		// deliberately goes through the host's own storage hooks instead of
		// Reticulum's file layer.
		static microStore::FileSystem filesystem{microStore::Adapters::NoopFileSystem()};
		filesystem.init();
		RNS::Utilities::OS::register_filesystem(filesystem);

		// Every node relays for every other. True multi-hop needs the nodes
		// BETWEEN two distant peers to forward, so absent a reason to
		// differentiate by role, everyone participates. Must be set before
		// start(), which is when Transport reads it.
		RNS::Reticulum::transport_enabled(true);

		static RNS::Interface interface(
			static_cast<RNS::InterfaceImpl *>(g_config.rns_interface));
		RNS::Transport::register_interface(interface);
		interface.start();

		static RNS::Reticulum reticulum;
		reticulum.start();
		g_reticulum = &reticulum;

		// A real keypair, not RNS::Type::NONE: a SINGLE destination always
		// has an identity tied to it. Idempotent, so this is a no-op when the
		// caller already ran it to obtain the node id -- which is the normal
		// order, since the runtime must exist before there is a state manager.
		if (!fleece_reticulum_identity_init()) {
			log_line("fleece/reticulum: FAILED start, no identity\n");
			return false;
		}
		RNS::Identity &identity = *g_identity_ptr;
		{
			const RNS::Bytes &ih = identity.hash();
			const uint8_t *hb = ih.data();
			if (hb != nullptr && ih.size() >= 4) {
				logf_line("fleece/reticulum: self hash=%02x%02x%02x%02x\n", hb[0], hb[1],
					  hb[2], hb[3]);
			}
		}

		static RNS::Destination destination(identity, RNS::Type::Destination::IN,
						     RNS::Type::Destination::SINGLE, app_name(), "fleece");
		destination.set_packet_callback(&onPacketReceived);
		g_destination = &destination;

		// A second, independent destination for host-directed traffic. Same
		// identity and interface; a different aspect is what makes Reticulum
		// route and deliver it as a separate stream, with nothing on this path
		// ever reaching the state manager.
		static RNS::Destination control_destination(identity, RNS::Type::Destination::IN,
							     RNS::Type::Destination::SINGLE, app_name(),
							     "control");
		control_destination.set_packet_callback(&onControlPacketReceived);
		g_control_destination = &control_destination;

		// One shared peer table fed by announces from either destination. An
		// empty aspect filter would also match everything, but filtering per
		// exact destination name is what the mechanism is for and avoids ever
		// having to guess which aspect an announce belonged to.
		static std::string fleece_filter = std::string(app_name()) + ".fleece";
		static std::string control_filter = std::string(app_name()) + ".control";
		static PeerAnnounceHandler fleece_announce_handler(fleece_filter.c_str());
		static PeerAnnounceHandler control_announce_handler(control_filter.c_str());
		RNS::Transport::register_announce_handler(std::shared_ptr<RNS::AnnounceHandler>(
			&fleece_announce_handler, [](RNS::AnnounceHandler *) {}));
		RNS::Transport::register_announce_handler(std::shared_ptr<RNS::AnnounceHandler>(
			&control_announce_handler, [](RNS::AnnounceHandler *) {}));

		// Announce once at boot; periodic re-announces come from poll().
		destination.announce();
		control_destination.announce();

		return true;
	} catch (...) {
		log_line("fleece/reticulum: FAILED start threw\n");
		return false;
	}
}

extern "C" void fleece_reticulum_send(const char * /*destination*/, const uint8_t *data, uint32_t size,
				       void * /*user_data*/)
{
	try {
		sendToAllPeers("fleece", RNS::Bytes(data, size));
	} catch (...) {
		// A dropped send must never halt the node -- see this file's header.
	}
}

extern "C" void fleece_reticulum_poll(void * /*user_data*/)
{
	if (g_reticulum == nullptr) {
		return;
	}
	try {
		g_reticulum->loop();
		g_poll_tick_count++;
		if (g_poll_tick_count % kAnnounceIntervalTicks == 0) {
			// One line per re-announce cycle rather than per tick: a
			// breadcrumb trail of peer count over time, cheap enough to
			// leave on for real hardware.
			logf_line("fleece/reticulum: tick=%u peers=%u rx=%u rxfail=%u sendcalls=%u "
				  "sendok=%u sendfail=%u\n",
				  g_poll_tick_count, static_cast<unsigned>(g_peers.count()),
				  g_packets_received, g_import_failures, g_send_attempts, g_send_ok,
				  g_send_fail);
			if (g_destination != nullptr) {
				g_destination->announce();
			}
			if (g_control_destination != nullptr) {
				g_control_destination->announce();
			}
		}
	} catch (...) {
		// Same reasoning as send: a dropped tick must never halt the node.
	}
}

extern "C" void fleece_reticulum_control_send(const uint8_t *data, uint32_t size)
{
	try {
		sendToAllPeers("control", RNS::Bytes(data, size));
	} catch (...) {
		// As above.
	}
}

extern "C" void fleece_reticulum_control_set_receive_callback(FleeceReticulumControlRecvFn callback,
							       void *user_data)
{
	g_control_callback = callback;
	g_control_callback_user_data = user_data;
}
