# neomesh — Design Decisions

This document captures the reasoning behind architectural choices, so future contributors understand why things are the way they are and can make informed trade-offs when changing them.

---

## Why ESP-NOW instead of BLE?

The project originally used BLE (NimBLE). It was migrated to ESP-NOW because:

- BLE GATT write/notify throughput is too low for streaming
- ESP-NOW operates at the WiFi layer and shares the radio cleanly with WiFi TCP
- ESP-NOW has ~250B frame limit but latency is ~1–3ms, suitable for control and sync
- No pairing or connection state to manage — just send to a MAC address

BLE was considered again briefly for discovery but rejected: ESP-NOW broadcast already handles discovery, and adding BLE would require the BLE stack to run concurrently with WiFi (possible but adds RAM pressure and complexity).

---

## Why dual transport (ESP-NOW + WiFi TCP)?

### ESP-NOW alone

- Can't send more than 221 bytes of body per frame
- Would require fragmentation for *everything* > 221 bytes
- Fragmentation + reassembly adds latency and reassembly RAM

### WiFi TCP alone

- Requires all devices to be associated to the same AP
- Not all devices will be gateway-capable or near enough to the router
- Overhead (TCP handshake, ACKs) is excessive for small control packets

### The split

Router decides per packet:
- Small/control → ESP-NOW (always works, minimal overhead)
- Large + both ends on WiFi → TCP (no fragmentation, streaming-friendly)
- Large + no mutual WiFi → fragment into ESP-NOW (works everywhere)

This lets 50 lamps communicate at all times while keeping large streams efficient where possible.

---

## Why dynamic gateway election instead of a fixed gateway?

Static gateway = single point of failure. In a room with 50 lamps, the "gateway lamp" might be:

- Behind furniture (worse RSSI)
- Turned off for a night
- The only one plugged in during setup

Election means the library user doesn't need to designate a specific device. They call `setWifi()` on any devices they want to be candidates. The library figures out the rest.

---

## Why RSSI × 0.6 + (1 - loss) × 0.4 for election score?

RSSI alone is misleading: a lamp at -60dBm but with 30% packet loss (interference) is a worse gateway than one at -70dBm with 0% loss.

The weighting (60/40) prioritizes signal strength slightly over reliability, because:
- RSSI is a stronger predictor of throughput than intermittent loss
- Loss rate is noisier (small sample, bursty interference)

These weights are not scientifically derived. They're a reasonable starting point and can be adjusted via `ELECTION_BROADCAST_MS`, `ELECTION_TIMEOUT_MS`, and the score formula in `MeshElection::_calcScore()`.

---

## Why is `BEACON_PORT` gone and the old `MESH_BEACON_PORT` removed?

In the original design, peer discovery used a UDP broadcast on `MESH_BEACON_PORT` (7778). This required all devices to be on the same WiFi subnet — useless for mesh-only nodes that aren't on WiFi.

ESP-NOW broadcast has no such constraint. Any device within radio range receives it, regardless of WiFi connectivity. The UDP beacon was removed entirely.

---

## Why does the router decide transport, not the signal layer?

Two options were considered:

**A. Transparent composite signal** — one object that picks ESP-NOW or WiFi internally, router doesn't know.

**B. Router decides** — router has both signals as members, applies policy.

Option B was chosen because:
- Transport choice depends on payload size (only the router knows the assembled body size)
- Transport choice depends on whether the next hop has a WiFi peer (route table knowledge)
- The policy will likely change (e.g. quality-based selection) — centralizing it in the router makes future changes easier
- Option A would need to inspect the packet anyway, duplicating logic

---

## Why is topology stored only fully at the gateway?

Full topology on every node would require:
- Flooding all route updates to all nodes (already done)
- Every node building and maintaining an adjacency table (50 nodes × ~170 bytes = 8.5KB)

8.5KB is acceptable for 50 nodes on ESP32 (400KB RAM). But non-gateway nodes don't *need* the full picture: their routing decisions are made hop-by-hop using the route table, not global path planning.

Gateway needs the full picture to:
- Answer "how do I route from the LAN into the mesh" optimally
- Provide network-wide diagnostics
- Plan multi-hop streams

This is the same split used in link-state routing (OSPF, IS-IS): each router has a full map, but edge devices only need their next hop.

---

## Why PTP-lite instead of NTP for time sync?

NTP over WiFi:
- Requires internet access (or a local NTP server) — not guaranteed
- Polling rate is limited to ~1 per minute by most servers
- Doesn't account for variable network delay well at mesh-hop granularity

PTP-lite (4-timestamp exchange):
- Runs entirely within the mesh over ESP-NOW
- Uses `esp_timer_get_time()` at 1µs resolution
- Achieves <1ms accuracy between direct peers
- No external dependencies

The master is always the elected gateway (already the most stable node by definition). If gateway changes, nodes automatically switch sync master via the election callback.

### Known limitation: multi-hop accuracy

Each hop adds ~0.5ms of clock offset error, because intermediate nodes add forwarding delay that the sync measurement doesn't account for. For a 4-hop mesh, worst-case error is ~2ms. 

True sub-1ms multi-hop sync would require:
- Hardware IEEE 1588 (not available on ESP32-C3)
- Hierarchical sync chains (each node syncs from its closest gateway-side neighbor)

Hierarchical sync is a future enhancement. For now, nodes directly adjacent to the gateway (usually most nodes in a room) get <1ms accuracy.

---

## Why are ELECTION and ROUTE_UPDATE packets flooded with dedup instead of unicast to gateway?

Two options:

**A. Broadcast with TTL + dedup** — each packet bounces through all nodes, router drops duplicates.

**B. Unicast to gateway** — each node sends directly to gateway (or routes there).

Option A was chosen because:
- Route to gateway may not be known yet at boot
- Election scores need to reach ALL nodes (not just gateway) so every node can independently elect the same leader without consensus rounds
- Topology updates similarly need to reach every node that cares

The dedup ring buffer prevents infinite flooding.

---

## Why NodeID = MAC address?

Alternatives:
- Random 64-bit ID (assigned at boot or flashed) — requires persistent storage or coordination
- Sequential IDs — requires a coordinator to assign them

MAC address is unique, stable, requires no storage, and lets the signal layer recover the peer's MAC from its NodeID (necessary for ESP-NOW unicast registration). The formula is:

```
NodeID = mac[0]<<40 | mac[1]<<32 | mac[2]<<24 | mac[3]<<16 | mac[4]<<8 | mac[5]
```

Recoverable via `nodeIdToMac()`.

---

## Why fragment reassembly only at destination?

Two options:

**A. Reassemble and re-fragment at each hop** — each intermediate node fully reassembles the large packet before forwarding.

**B. Forward fragments blindly, reassemble only at destination** — intermediate nodes treat FRAG packets as small (<221B) ESP-NOW frames.

Option B was chosen because:
- Each fragment IS a valid small ESP-NOW packet — no intermediate processing needed
- Re-assembly at each hop wastes RAM (FRAG_STREAM_BUFSIZE × FRAG_MAX_STREAMS = 16KB per hop)
- Re-assembly at each hop adds latency (must wait for all fragments before forwarding any)
- Intermediate nodes can start forwarding fragment 0 before fragment 15 arrives

The trade-off: if a fragment is lost in transit, the entire stream times out (5 seconds). Future work: per-fragment ACK + selective retransmit.

---

## mDNS service type: `_neoworks._tcp`

Options considered:
- `_http._tcp` — generic HTTP type; shows up automatically in any mDNS browser. Simple for clients that just want to do HTTP.
- `_neoworks._tcp` — custom type; only clients that explicitly query for it find it.

Custom type was chosen because:
- Avoids collision with other HTTP services on the network
- Makes intent explicit: this is a neomesh gateway, not a generic web server
- Go apps and phone apps query by type anyway (`_neoworks._tcp`)
- Can still serve HTTP on the same port; the service type doesn't restrict protocols

Instance name is `"neomesh"` → resolves as `neomesh.local` via mDNS.

---

## Why does `publish()` dispatch locally for self/broadcast destinations?

Originally, `publish()` always called `_forward()`, which only sends over the radio — a node's own subscribers never saw its own broadcasts or self-addressed unicasts (ESP-NOW broadcast/send doesn't loop back to the sender's own recv callback).

This is invisible in most cases (a node doesn't need to tell itself something it already knows). But it breaks the gateway-bridge path: an external app addressing a packet to `dst = <gateway's own NodeID>` arrives via `IBridge::onReceive` → `Router.publish(dst=self, ...)`. With the old behavior, this packet would be broadcast out over ESP-NOW and never delivered to the gateway's own subscribers.

This matters concretely for the LED example (`examples/led_node`): both nodes are gateway candidates, so either could be elected. If the LED-equipped node wins the election, an external CLI command addressed to its NodeID must still reach its local `led/color` subscriber.

**Fix:** `MeshRouter::publish()` now calls `_pubsub.dispatch()` directly when `dst == _self || dst == BROADCAST_ID`, in addition to `_forward()`. `_forward()` itself early-returns for `dst == _self` (nothing useful to send over radio to yourself).

---

## Why is the mDNS-advertised port `MESH_PORT + 1`?

`MESH_PORT` (7777) is used for mesh-internal WiFi TCP between peer nodes (`WifiSignal`). `MESH_BRIDGE_PORT` (7778, `MESH_PORT + 1`) is `TcpBridge`'s listen port for **external applications**.

mDNS is for external discovery (Go apps, phone apps, CLIs) — it must advertise the bridge port, not the peer port. An earlier version of `MeshGateway::_registerMdns()` advertised `MESH_PORT`, which would have pointed external clients at the wrong socket. Fixed to advertise `MESH_BRIDGE_PORT`.

---

## Channel management

ESP32's WiFi/ESP-NOW radio operates on one 2.4GHz channel at a time. When a device connects to an AP it locks to the AP's channel. Devices not on WiFi default to channel 1.

**Problem:** If the AP is on channel 6 and non-WiFi nodes are on channel 1, they can't hear each other's ESP-NOW packets.

**Beacon-follow (insufficient alone):** The gateway broadcasts its channel in every beacon, and a node hearing it switches to match (`EspNowSignal::_handleBeacon`). This only helps a node that is *already* on the gateway's channel — a beacon transmitted on channel 6 is never received by a radio tuned to channel 1, so a stranded node can never learn the new channel this way. In practice this happens whenever the gateway connects to WiFi after election and locks to the AP's channel, leaving any out-of-WiFi-range node stuck on channel 1.

**Solution: channel scan fallback.** A node not connected to WiFi that has zero active ESP-NOW peers (`EspNowSignal::_updateScan`) starts hopping channels 1–13, dwelling `ESPNOW_BEACON_MS + 1500` ms on each (longer than one beacon period, guaranteeing it catches a beacon if the gateway is on that channel) and sending its own beacon on arrival. As soon as any peer reappears, scanning stops and the node stays put. This recovers both the initial AP-channel ≠ 1 case and later gateway channel changes (e.g. re-election, AP roams channels), at the cost of up to ~13 × dwell ≈ 1 minute worst case to re-converge. The AP should still use a fixed channel where possible to minimize churn.
