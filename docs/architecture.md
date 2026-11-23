# neomesh — Architecture

## What this is

A mesh networking library for ESP32-C3 microcontrollers. It lets you flash a single binary onto any number of ESP32-C3 devices and have them self-organize into a message-passing mesh with:

- Automatic peer discovery (no manual IP config)
- Dynamic gateway election (whoever has best WiFi becomes the bridge to the LAN)
- Sub-millisecond time synchronization across the mesh
- PubSub messaging (MQTT-style topics with wildcards)
- Transparent large-payload streaming via automatic fragmentation

The primary target is ~50 devices (e.g. smart lamps) in a room. Not all devices need WiFi; they talk to each other via ESP-NOW and only the elected gateway connects to the local router.

---

## Module overview

```
Mesh.h / Mesh.cpp          ← public API surface; wires all modules together
│
├── EspNowSignal            ← peer discovery + primary transport (ESP-NOW)
├── WifiSignal              ← secondary transport (WiFi TCP, large payloads only)
│
├── MeshRouter              ← routing table, packet dispatch, transport selection
│   ├── MeshPubSub          ← topic subscribe/publish with wildcard matching
│   ├── MeshDedup           ← ring buffer dedup by msgId
│   ├── MeshElection        ← gateway election (RSSI + loss metric)
│   ├── MeshSync            ← PTP-lite time synchronization
│   ├── MeshFragment        ← fragmentation / reassembly
│   └── MeshTopology        ← mesh topology tracking
│
└── MeshGateway             ← WiFi connect, mDNS advertisement, bridge to external app
```

---

## Packet types

Every packet on the wire is prefixed with `MeshPacket` (29 bytes, packed):

```
version | type | payloadLen | msgId | seq | src | dst | ttl | flags | topicLen
```

`type` determines how the router dispatches it:

| Type             | Value | Forwarded? | Dedup? | Description |
|------------------|-------|-----------|--------|-------------|
| `DATA`           | 0x01  | yes       | yes    | PubSub message |
| `BEACON`         | 0x02  | no        | no     | Neighbor discovery (signal layer only) |
| `ROUTE_UPDATE`   | 0x03  | yes       | yes    | Topology advertisement |
| `SYNC_REQ`       | 0x04  | if not us | no     | PTP sync request |
| `SYNC_RESP`      | 0x05  | if not us | no     | PTP sync response |
| `ELECTION`       | 0x06  | yes       | yes    | Gateway election score |
| `FRAG`           | 0x07  | if not us | no     | Fragment of a large packet |

BEACON packets are consumed in the signal layer and never reach the router.

---

## Transport layer

### Why two transports?

ESP-NOW has a hard 250-byte frame limit but requires no WiFi association — it works between any two devices that know each other's MAC address. WiFi TCP has no size limit but requires both devices to be connected to the same AP.

The design uses both:

- **ESP-NOW**: discovery, control, small data, and fragmented streams
- **WiFi TCP**: large direct-peer transfers where both ends have WiFi

The router decides which to use per packet in `MeshRouter::_forward()`:

```
frame ≤ 250 bytes          → ESP-NOW (always available)
frame > 250 bytes
  + dst is direct WiFi peer → WiFi TCP
  + no WiFi peer for dst    → Fragment into ESP-NOW chunks
large broadcast             → dropped (use unicast for streams)
```

### Why not WiFi TCP for everything?

Not all devices will be connected to WiFi. Only gateway candidates connect to the AP; pure mesh nodes are WiFi-off and reachable only via ESP-NOW.

### Channel locking

ESP-NOW and WiFi share the same radio on ESP32-C3. When a device connects to an AP it locks to the AP's channel. Devices not on WiFi must use the same channel or ESP-NOW packets won't be received.

**How it's handled:** The elected gateway's channel is broadcast in every BEACON. Devices not connected to WiFi switch to that channel automatically when they hear the first beacon. No manual channel config needed.

---

## Peer discovery

ESP-NOW requires knowing a peer's MAC address before sending unicast. Discovery is bootstrapped via broadcast beacons:

1. On boot: `EspNowSignal` broadcasts a BEACON (TTL=1) to `FF:FF:FF:FF:FF:FF`.
2. Any device that hears it learns the sender's MAC from the ESP-NOW recv info.
3. The sender's NodeID is in `pkt.src` (NodeID = MAC cast to uint64_t, so MAC is recoverable).
4. The receiver adds the sender as an ESP-NOW peer and replies on its next beacon interval.

Beacons also carry:
- `channel`: receiver switches to this if not connected to WiFi
- `rssiToRouter`: used in election score
- `flags`: CANDIDATE (has WiFi credentials), WIFI_READY (currently connected)
- `wifiIp`: IPv4 of sender; if WIFI_READY and non-zero, receiver opens a WiFi TCP connection

---

## Gateway election

### Why dynamic election?

Static gateway config breaks when that device goes down. With 50 devices, it's reasonable for multiple to have WiFi credentials. The best-connected one should be the gateway.

### Algorithm

Every candidate (device with `setWifi()` called) broadcasts a score every 5 seconds:

```
score = RSSI_score * 0.6 + (1 - loss_rate) * 0.4
RSSI_score  = clamp(rssiToRouter + 100, 0, 100)   // -100dBm→0, 0dBm→100
loss_rate   = failedSends / totalSends             // per ESP-NOW peer, averaged
```

Non-candidates broadcast score = -1 and are ignored.

Each node maintains a table of recently seen candidate scores. Whenever the table changes (new entry, expiry), it runs `_electBest()` — finds highest score, fires `onGatewayChange()` callback if winner changed.

The winning node calls `Gateway.activate()` → connects to WiFi → registers mDNS.

If a gateway goes silent for 15 seconds, its score entry expires and the next-best candidate takes over.

### What "gateway" means

- Only the gateway connects to the LAN router
- Only the gateway registers `neomesh._neoworks._tcp` via mDNS (discoverable by phone apps, Go servers, etc.)
- Only the gateway builds the full mesh topology tree
- All other nodes communicate via the gateway to reach the LAN

---

## Time synchronization

### Why <1ms?

The primary use case requires synchronized playback across all devices (e.g. all lamps switching at the same instant, or audio streams that must be in phase). Drift above ~1ms becomes perceptible.

### PTP-lite

Based on IEEE 1588 Precision Time Protocol but without hardware timestamping:

```
Node                          Master (gateway)
  |--[T1: SYNC_REQ]---------->|
  |                           | records T2 (recv)
  |<--[T1,T2,T3: SYNC_RESP]--|  records T3 (send, ≈T2)
  | records T4 (recv)         |

offset = ((T2-T1) + (T3-T4)) / 2
```

`esp_timer_get_time()` provides 1µs resolution. Typical ESP-NOW round-trip is 1–3ms, giving offset accuracy ≈ 0.5–1.5ms for **direct peers**.

### Multi-hop accuracy

Each hop adds ~0.5ms error. For a mesh where most nodes are within 2 hops of the gateway, worst-case error is ~1–2ms. This is acceptable for most use cases; it can be improved later by implementing hierarchical sync (each node syncs to the closest-to-gateway direct peer rather than the gateway itself).

### Usage

```cpp
int64_t t = Mesh.syncedTime();  // microseconds, master-corrected
bool ok   = Mesh.timeSynced();  // false until first exchange completes
```

---

## Fragmentation

Large payloads (> 221 bytes body) are broken into ESP-NOW-sized chunks:

```
FRAG_MAX_DATA = ESPNOW_MAX_BODY - sizeof(FragBody) = 221 - 13 = 208 bytes/chunk
```

`Fragment.send(nextHop, pkt, body)` sends all chunks to the next hop. Intermediate nodes forward FRAG packets blindly toward dst. Only the destination reassembles.

Fragment header per chunk:
```
streamId | totalLen | fragIndex | fragCount | origType
```

Up to 4 concurrent reassembly streams (4 × 4096B = 16KB RAM). Streams time out after 5 seconds if not completed.

---

## Topology

Every node broadcasts a ROUTE_UPDATE packet every 10 seconds containing its current ESP-NOW peer list.

**Gateway** receives these from all nodes (they're flooded TTL-limited through the mesh) and builds a full adjacency list (`TopoNode[50]`).

**Non-gateway nodes** only store their own row (direct neighbors). They don't have the global picture.

The gateway can use its full topology to compute optimal routes; currently the route table uses reactive learning (updates when packets arrive). Proactive path planning from topology is a future enhancement.

---

## Route table

Each node maintains `RouteEntry[32]`:
```
dst | nextHop | hopCount | lastSeen | active
```

Routes are learned reactively: when a packet from `src` arrives, we record `src → src` with hop count 1 (or the actual hop count if the signal layer provides it). Routes expire after 30 seconds.

On `_forward()`, the router looks up `dst` and sends to `nextHop`. If no route found, it floods via broadcast (and learns from responses).

---

## Sequence: node startup

```
boot
  ↓
EspNowSignal.init()       ← ESP-NOW initialized, broadcast beacon
  ↓
WifiSignal.init()         ← deferred until WiFi connects
  ↓
Election.init()           ← if setWifi() was called: election starts broadcasting score
  ↓
Gateway.init()            ← passive; waits for election callback
  ↓
[beacons fly between nodes]
  ↓
Election score gathered   ← each candidate hears others' scores
  ↓
Election.onGatewayChange()← winner determined
  ↓
[winner] Gateway.activate()  ← connects WiFi, registers mDNS
[others] Sync.setMaster()    ← start syncing to gateway
```

---

## External app integration

The gateway can bridge packets to an external application via `IBridge`:

```cpp
Mesh.setBridge(&myTcpBridge);  // before begin()
```

The library provides `TcpBridge` as a default implementation. The external app connects via TCP to the gateway's IP on `MESH_PORT` (7777), discoverable via mDNS at `neomesh._neoworks._tcp`.

Packets from the external app are injected as `publish()` calls. Packets destined for the gateway (`dst == self`) are forwarded out the bridge.
