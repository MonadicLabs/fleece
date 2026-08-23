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

/* Upper bound for one received packet. Generous versus the MTUs these radios
 * actually run, and the host's receive callback is what truly bounds the copy
 * anyway -- it is handed sizeof(buf) and returns what it wrote.
 */
constexpr size_t kRadioBufferBytes = 1024;

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
	char buf[256]; // was 192; the periodic status line grew four resource_* fields
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
/* Aspect index into the per-peer link slots below: "fleece" gossip and
 * "control" are different destinations (same identity, different aspect
 * string), so a peer needing a Resource/Link fallback on both needs two
 * independent links, not one.
 */
size_t aspect_index(const char *aspect)
{
	return std::strcmp(aspect, "control") == 0 ? 1 : 0;
}

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
		teardown_links(oldest);
		identities_[oldest] = identity;
		last_seen_tick_[oldest] = now_tick;
	}
	size_t count() const { return count_; }
	const RNS::Identity &identity(size_t i) const { return identities_[i]; }

	/* Oversized-payload fallback (see sendToAllPeers()): a cached Link per
	 * (peer, aspect), established on first need and reused for later
	 * sends rather than re-handshaking every time. Returns nullptr if no
	 * link has been established yet for this slot -- distinct from "the
	 * Link object is default/NONE-constructed", which is NOT a safe test
	 * here: Link's constructor always allocates a real backing object
	 * even for a {Type::NONE} destination (confirmed against Link.cpp),
	 * so operator bool() on a freshly-defaulted array slot would read
	 * true. has_link_ is the actual source of truth.
	 */
	RNS::Link *link_for(size_t peer_index, size_t aspect_idx)
	{
		return has_link_[peer_index][aspect_idx] ? &links_[peer_index][aspect_idx] : nullptr;
	}
	void set_link(size_t peer_index, size_t aspect_idx, const RNS::Link &link)
	{
		links_[peer_index][aspect_idx] = link;
		has_link_[peer_index][aspect_idx] = true;
	}
	/* Matches a closed/torn-down Link back to the (peer, aspect) slot that
	 * cached it, by hash -- onLinkClosed() only gets the Link itself, not
	 * which peer it belonged to.
	 */
	void note_link_closed(const RNS::Bytes &link_hash)
	{
		for (size_t i = 0; i < count_; i++) {
			for (size_t a = 0; a < 2; a++) {
				if (has_link_[i][a] && links_[i][a].hash().compare(link_hash) == 0) {
					has_link_[i][a] = false;
					return;
				}
			}
		}
	}

private:
	void teardown_links(size_t slot)
	{
		for (size_t a = 0; a < 2; a++) {
			if (!has_link_[slot][a]) {
				continue;
			}
			RNS::Type::Link::status st = links_[slot][a].status();
			if (st == RNS::Type::Link::HANDSHAKE || st == RNS::Type::Link::ACTIVE) {
				// Resolves any in-flight Resource for the evicted peer as a
				// real FAILED/CORRUPT via its own concluded callback
				// (counted) rather than silently orphaning it.
				links_[slot][a].teardown();
			}
			has_link_[slot][a] = false;
		}
	}

	RNS::Identity identities_[kMaxPeers] = {RNS::Type::NONE, RNS::Type::NONE, RNS::Type::NONE,
						 RNS::Type::NONE, RNS::Type::NONE, RNS::Type::NONE,
						 RNS::Type::NONE, RNS::Type::NONE};
	uint32_t last_seen_tick_[kMaxPeers] = {0};
	size_t count_ = 0;
	RNS::Link links_[kMaxPeers][2];
	bool has_link_[kMaxPeers][2] = {};
};

/* A Reticulum interface backed by the host's two packet callbacks. This is
 * the whole reason microReticulum stays invisible to integrators: they
 * implement "send a packet" and "poll for a packet" over their own link, and
 * this adapts that to whatever the mesh stack expects. Swapping the mesh
 * implementation would rewrite this class and nothing on the host side.
 */
class HostRadioInterface : public RNS::InterfaceImpl {
public:
	HostRadioInterface() : RNS::InterfaceImpl("FleeceRadio") {}

	void configure(uint32_t mtu, uint32_t bitrate)
	{
		_IN = true;
		_OUT = true;
		if (mtu > 0) {
			_HW_MTU = static_cast<std::uint16_t>(mtu);
		}
		if (bitrate > 0) {
			_bitrate = bitrate;
		}
	}

	bool start() override
	{
		_online = true;
		return true;
	}

	void stop() override { _online = false; }

	void loop() override
	{
		if (!_online || g_config.radio_receive == nullptr) {
			return;
		}
		/* Drain every packet currently available, not just one. A single
		 * fleece tick can enqueue more than one outgoing frame (the self
		 * stream and the shared/world stream are separate sends in the same
		 * runtime iteration), so a receiver taking one packet per tick falls
		 * permanently behind its own sender -- a backlog that only grows and
		 * eventually overruns the host's buffering. Observed for real before
		 * it was understood.
		 */
		uint8_t buf[kRadioBufferBytes];
		uint32_t len;
		while ((len = g_config.radio_receive(buf, sizeof(buf), g_config.user_data)) > 0) {
			handle_incoming(RNS::Bytes(buf, len));
		}
	}

private:
	bool send_outgoing(const RNS::Bytes &data) override
	{
		bool ok = true;
		if (_online && g_config.radio_send != nullptr) {
			ok = g_config.radio_send(data.data(), static_cast<uint32_t>(data.size()),
						  g_config.user_data);
		}
		InterfaceImpl::handle_outgoing(data);
		return ok;
	}
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

/* Oversized-payload fallback counters -- deliberately separate from
 * g_send_ok/g_send_fail above, which mean "this synchronous Packet attempt
 * resolved this tick". A Resource transfer is multi-tick and async, so it
 * needs its own vocabulary: started (kicked off this tick), link_pending
 * (still handshaking, this tick's payload was dropped -- see
 * sendViaResource()'s own comment), complete/failed (a concluded callback
 * fired, on either the send or receive side).
 */
uint32_t g_resource_started = 0;
uint32_t g_resource_link_pending = 0;
uint32_t g_resource_complete = 0;
uint32_t g_resource_failed = 0;

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

/* Largest payload sendToAllPeers() will put directly into one RNS::Packet.
 * Above this, a single Packet::send() would just fail against the real
 * radio's own hard per-transmission cap (swarmpu's kMeshMtuBytes,
 * configured here as g_config.radio_mtu -- see reticulum_bridge.cpp) with
 * no retry and no queueing: confirmed live, fleece's own periodic status
 * line showed sendfail climbing from ~15% to ~64% of sendcalls over a few
 * thousand ticks under sustained gossip once a delta payload grew past
 * that wall. sendToAllPeers() falls back to a Resource/Link transfer (see
 * sendViaResource()) for anything over this ceiling instead of failing
 * outright.
 *
 * Mirrors RNS::Type::Destination::ENCRYPTED_MDU's own derivation (Type.h)
 * exactly, but evaluated against our REAL configured MTU rather than
 * Reticulum's own hardcoded Type::Reticulum::MTU (500) -- larger than our
 * real 460-byte wire cap, so unsafe to reuse as-is; every other term here
 * is a named RNS::Type:: constant, not a second magic number.
 */
size_t single_packet_payload_ceiling()
{
	const size_t mtu = g_config.radio_mtu > 0 ? g_config.radio_mtu
						   : static_cast<size_t>(RNS::Type::Reticulum::MTU);
	const size_t header = RNS::Type::Reticulum::HEADER_MAXSIZE + RNS::Type::Reticulum::IFAC_MIN_SIZE;
	const size_t crypto = RNS::Type::Identity::FERNET_OVERHEAD + RNS::Type::Identity::KEYSIZE / 16;
	if (mtu <= header + crypto) {
		return 0;
	}
	const size_t mdu = mtu - header;
	return ((mdu - crypto) / RNS::Type::Identity::AES128_BLOCKSIZE) * RNS::Type::Identity::AES128_BLOCKSIZE - 1;
}

/* Fires when a Link this node opened (send-side) OR accepted (receive-side,
 * see fleece_reticulum_start()'s set_link_established_callback) closes, for
 * any reason -- timeout, teardown, the peer going away. Only send-side
 * links are cached in PeerTable (receive-side ones belong to whichever peer
 * opened them, symmetric but not something this node needs to track for
 * its own future sends), so this just clears the cache entry if there is
 * one; a no-op for a receive-side link.
 */
void onLinkClosed(RNS::Link &link)
{
	g_peers.note_link_closed(link.hash());
}

/* Send-side: a cached Link finished establishing. Nothing to do here --
 * sendViaResource() re-checks status() live on its next call rather than
 * trusting a cached "ready" flag, since a Link can also go stale/close
 * between establishment and the next gossip tick. Kept as a real callback
 * (not nullptr) purely for the log breadcrumb.
 */
void onOutgoingLinkEstablished(RNS::Link & /*link*/)
{
	log_line("fleece/reticulum: resource link established\n");
}

/* Shared by both directions on the "fleece" aspect: fires when a Resource
 * this node SENT concludes (r.initiator() == true -- just count it, the
 * data already left), and when a Resource this node RECEIVED concludes
 * (r.initiator() == false -- deliver it, same as onPacketReceived() does
 * for the Packet path, so gossip merges identically regardless of which
 * transport a given frame arrived over). Fires for FAILED/CORRUPT/REJECTED
 * too, not just COMPLETE -- must branch on status(), "the callback fired"
 * alone does not mean success.
 */
void onFleeceResourceConcluded(const RNS::Resource &r)
{
	if (r.status() != RNS::Type::Resource::COMPLETE) {
		g_resource_failed++;
		logf_line("fleece/reticulum: resource FAILED status=%d initiator=%d\n",
			  static_cast<int>(r.status()), static_cast<int>(r.initiator()));
		return;
	}
	g_resource_complete++;
	if (r.initiator()) {
		return; // we sent it -- nothing more to do
	}
	if (g_state_manager != nullptr) {
		g_packets_received++;
		if (fleece_state_manager_import(g_state_manager, r.data().data(),
						 static_cast<uint32_t>(r.data().size())) != 0) {
			g_import_failures++;
		}
	}
}

/* Mirrors onFleeceResourceConcluded() for the "control" aspect: delivers to
 * the host's control callback instead of the state manager, same as
 * onControlPacketReceived() does for the Packet path.
 */
void onControlResourceConcluded(const RNS::Resource &r)
{
	if (r.status() != RNS::Type::Resource::COMPLETE) {
		g_resource_failed++;
		logf_line("fleece/reticulum: control resource FAILED status=%d initiator=%d\n",
			  static_cast<int>(r.status()), static_cast<int>(r.initiator()));
		return;
	}
	g_resource_complete++;
	if (r.initiator()) {
		return;
	}
	if (g_control_callback != nullptr) {
		g_control_callback(r.data().data(), static_cast<uint32_t>(r.data().size()),
				    g_control_callback_user_data);
	}
}

/* Receive-side: a peer opened a Link to this node's "fleece"/"control"
 * destination (only ever done to send an oversized payload as a Resource --
 * everything that fits stays on the Packet path). ACCEPT_ALL: any peer that
 * can reach this destination is already a trusted mesh member by the same
 * standard Packet-based gossip already trusts, so there is no separate
 * admission decision to make here.
 */
void onFleeceIncomingLinkEstablished(RNS::Link &link)
{
	link.set_resource_strategy(RNS::Type::Link::ACCEPT_ALL);
	link.set_resource_concluded_callback(&onFleeceResourceConcluded);
	link.set_link_closed_callback(&onLinkClosed);
}

void onControlIncomingLinkEstablished(RNS::Link &link)
{
	link.set_resource_strategy(RNS::Type::Link::ACCEPT_ALL);
	link.set_resource_concluded_callback(&onControlResourceConcluded);
	link.set_link_closed_callback(&onLinkClosed);
}

/* Oversized-payload fallback for one peer: establishes (or reuses) a Link
 * to it and sends `payload` as a Resource instead of a Packet. Link
 * establishment is asynchronous, so a link that isn't ACTIVE yet just
 * drops this tick's payload rather than queueing it -- the tick loop
 * (fleece_runtime.c) already advances its gossip watermark unconditionally
 * after every export regardless of send success, so a dropped oversized
 * delta is simply superseded by the next tick's fresher export. Queueing
 * would need a new statically-sized per-peer buffer of unclear worst-case
 * size, against this module's existing bounded-allocation discipline.
 */
void sendViaResource(size_t peer_index, size_t aspect_idx, const RNS::Identity &id, const char *aspect,
		      const RNS::Bytes &payload, RNS::Resource::Callbacks::concluded concluded_cb)
{
	RNS::Link *link = g_peers.link_for(peer_index, aspect_idx);
	if (link != nullptr) {
		RNS::Type::Link::status st = link->status();
		if (st == RNS::Type::Link::CLOSED || st == RNS::Type::Link::STALE) {
			link = nullptr; // dead -- fall through to re-establish
		}
	}
	if (link == nullptr) {
		RNS::Destination peer_destination(id, RNS::Type::Destination::OUT,
						   RNS::Type::Destination::SINGLE, app_name(), aspect);
		RNS::Link new_link(peer_destination, &onOutgoingLinkEstablished, &onLinkClosed);
		g_peers.set_link(peer_index, aspect_idx, new_link);
		g_resource_link_pending++;
		return;
	}
	if (link->status() == RNS::Type::Link::PENDING || link->status() == RNS::Type::Link::HANDSHAKE) {
		g_resource_link_pending++;
		return;
	}
	RNS::Resource resource =
		RNS::Resource(payload, *link).auto_compress(false).set_concluded_callback(concluded_cb).start();
	(void)resource; // kept alive via Link::register_outgoing_resource(), not this local
	g_resource_started++;
}

/* Sends to every known peer's own destination individually. "Broadcast" is
 * this node's responsibility, not Reticulum's, because SINGLE destinations are
 * per-identity rather than a shared name. `aspect` picks which of a peer's two
 * destinations to address -- same identity, different aspect, so a different
 * destination hash either way.
 *
 * Packet stays the fast path for anything that fits in one -- steady-state
 * delta gossip is supposed to be small by design, and paying a Link's
 * handshake/keepalive overhead on every peer every tick for the common case
 * would be a real regression. Only a payload over single_packet_payload_ceiling()
 * falls back to sendViaResource().
 */
void sendToAllPeers(const char *aspect, const RNS::Bytes &payload)
{
	const size_t aspect_idx = aspect_index(aspect);
	const bool is_control = (aspect_idx == 1);
	/* Memory backstop (single-variant heap sizing): Reticulum's
	 * fixed TLSF pool is finite and shared (FLEECE_RNS_HEAP_POOL_BUFFER_SIZE).
	 * When it runs low, RNS::Packet / RNS::Destination construction throws
	 * std::bad_alloc (seen live as a full 32-peer mesh wedge) -- and a dropped
	 * packet must never halt the fan-out to the rest. Rather than catch-and-retry
	 * (which still pays the allocation attempt), shed BEFORE allocating: if the
	 * pool's contiguous free space falls under a red line, stop trying to send
	 * this tick. That is the graceful-degrade the node is expected to do -- a
	 * memory-poor node goes quiet like a lost one, and peer-liveness already
	 * swallows that shape. Logging the shed explicitly is what lets an operator
	 * (or a simulation) tell "memory-pressure shedding" from a real radio loss.
	 *
	 * The check is per sendToAllPeers() call, not per peer: under pressure the
	 * whole fan-out is skipped together rather than first-N-succeed-N+1-throw,
	 * which is both cheaper and fairer -- and it means no partially-sent tick
	 * with a packet only half the mesh received.
	 *
	 * Thresholds are deliberate: WARN at 50% still-allocating (observability),
	 * DROP only below 10% free (the allocator still has 10% of the pool to make
	 * the Destination/Packet objects it already committed to). Tuning these
	 * (or the pool itself) is per-target; see the bailout comment on
	 * FLEECE_RNS_HEAP_POOL_BUFFER_SIZE.
	 */
	const size_t pool_free = RNS::Utilities::Memory::heap_pool_free();
	const size_t pool_tot  = RNS::Utilities::Memory::heap_pool_size();
	if (pool_tot > 0 && pool_free * 2 < pool_tot) {
		if (pool_free * 10 < pool_tot) {
			/* Under 10% of the pool is free: drop this fan-out entirely rather
			 * than let the next RNS::Packet throw bad_alloc. Log once per tick
			 * avalanche, not per peer, so the drop is visible but not a flood. */
			if (aspect_idx == 0) {
				g_send_fail++;  /* account the dropped send like a failed one */
				logf_line("fleece/reticulum: DEPRIORITIZING gossip send (pool %.1f%% free) -- shedding this tick\n",
					  (double)pool_free / (double)pool_tot * 100.0);
			}
			return;
		}
		if (aspect_idx == 0) {
			logf_line("fleece/reticulum: gossip pool pressure %u%% free\n",
				  (unsigned)(pool_free * 100 / pool_tot));
		}
	}

	g_send_attempts++;
	RNS::Resource::Callbacks::concluded concluded_cb =
		is_control ? &onControlResourceConcluded : &onFleeceResourceConcluded;
	const bool oversized = payload.size() > single_packet_payload_ceiling();

	for (size_t i = 0; i < g_peers.count(); i++) {
		if (oversized) {
			sendViaResource(i, aspect_idx, g_peers.identity(i), aspect, payload, concluded_cb);
			continue;
		}
		try {
			RNS::Destination peer_destination(g_peers.identity(i), RNS::Type::Destination::OUT,
						   RNS::Type::Destination::SINGLE, app_name(), aspect);
			RNS::Packet packet(peer_destination, payload);
			packet.send();
			g_send_ok++;
		} catch (const std::exception &e) {
			// One bad peer must not stop the fan-out to the rest.
			g_send_fail++;
			logf_line("fleece/reticulum: send to peer %zu (%s) threw: %s\n", i, aspect, e.what());
		} catch (...) {
			g_send_fail++;
			logf_line("fleece/reticulum: send to peer %zu (%s) threw non-std exception\n", i, aspect);
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
	if (!g_configured || g_config.radio_send == nullptr || g_config.radio_receive == nullptr) {
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

		static HostRadioInterface radio;
		radio.configure(g_config.radio_mtu, g_config.radio_bitrate);
		static RNS::Interface interface(&radio);
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
		// Oversized-payload fallback (see sendToAllPeers()): accepts a Link a
		// peer opens to send a Resource-based transfer instead of a Packet.
		destination.set_link_established_callback(&onFleeceIncomingLinkEstablished);
		g_destination = &destination;

		// A second, independent destination for host-directed traffic. Same
		// identity and interface; a different aspect is what makes Reticulum
		// route and deliver it as a separate stream, with nothing on this path
		// ever reaching the state manager.
		static RNS::Destination control_destination(identity, RNS::Type::Destination::IN,
							     RNS::Type::Destination::SINGLE, app_name(),
							     "control");
		control_destination.set_packet_callback(&onControlPacketReceived);
		control_destination.set_link_established_callback(&onControlIncomingLinkEstablished);
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
				  "sendok=%u sendfail=%u resource_started=%u resource_pending=%u "
				  "resource_complete=%u resource_failed=%u pool_free=%u pool_tot=%u\n",
				  g_poll_tick_count, static_cast<unsigned>(g_peers.count()),
				  g_packets_received, g_import_failures, g_send_attempts, g_send_ok,
				  g_send_fail, g_resource_started, g_resource_link_pending,
				  g_resource_complete, g_resource_failed,
				  static_cast<unsigned>(RNS::Utilities::Memory::heap_pool_free()),
				  static_cast<unsigned>(RNS::Utilities::Memory::heap_pool_size()));
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

extern "C" size_t fleece_reticulum_single_packet_payload_ceiling(void)
{
	if (!g_configured) {
		return 0;
	}
	return single_packet_payload_ceiling();
}
