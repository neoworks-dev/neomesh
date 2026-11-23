# neomesh — API Reference

## `MeshClass` (via global `Mesh`)

### Setup (call before `begin()`)

```cpp
void setWifi(const char *ssid, const char *pass)
```
Stores WiFi credentials and marks this node as a gateway candidate. The node will participate in gateway election. Whichever candidate has the best combined RSSI + reliability score wins.

```cpp
void setBridge(IBridge *bridge)
```
Attaches an external application bridge. Only active on the elected gateway. The bridge receives packets that arrive at the gateway destined for the upstream application, and can inject packets into the mesh.

```cpp
void onElection(std::function<void(NodeID winner)> cb)
```
Callback fired whenever the elected gateway changes. `winner` is the NodeID of the new gateway. Call `Mesh.isGateway()` inside the callback to check if this device won.

---

### Core

```cpp
void begin()
```
Starts the mesh. Derives NodeID from WiFi MAC. Initializes ESP-NOW, starts election broadcasts. Must be called after `setWifi()` / `setBridge()` / `onElection()`.

```cpp
void loop()
```
Must be called on every Arduino `loop()` iteration. Processes incoming packets, drives election, sync, topology, and beacon timers.

---

### PubSub

```cpp
void publish(const char *topic, const uint8_t *payload, uint16_t len)
void publish(const char *topic, const char *msg)          // string overload
void publish(NodeID dst, const char *topic, const uint8_t *payload, uint16_t len)
```
Send a message. Broadcast (no `dst`) reaches all nodes. Unicast (`dst`) is routed via fewest hops.

Payloads larger than 221 bytes are automatically fragmented. There is no practical size limit other than RAM (MESH_MAX_PAYLOAD = 4096 bytes).

```cpp
int subscribe(const char *topic, MeshCallback cb)
```
Subscribe to a topic pattern. Returns a handle.

Callback signature: `void(const char *topic, const uint8_t *payload, uint16_t len)`

Topic wildcards:
- `+` matches exactly one level: `"lamps/+/mode"` matches `"lamps/strip1/mode"`
- `#` matches everything below (must be last): `"lamps/#"` matches `"lamps/strip1/mode/brightness"`

```cpp
void unsubscribe(int handle)
```
Remove a subscription by handle.

---

### Time sync

```cpp
int64_t syncedTime() const
```
Returns microseconds since boot, offset-corrected to the gateway's clock. Only meaningful after `timeSynced()` returns `true`.

Accuracy: <1ms for nodes directly adjacent to gateway. Add ~0.5ms per hop for multi-hop nodes.

```cpp
bool timeSynced() const
```
Returns `true` once the first PTP sync exchange has completed. Usually within 2 seconds of boot (one `SYNC_INTERVAL_MS` period).

---

### Info

```cpp
NodeID nodeId() const
```
This device's NodeID (WiFi MAC as uint64_t).

```cpp
NodeID gatewayId() const
```
NodeID of the currently elected gateway. `0` if no gateway elected yet.

```cpp
bool isGateway() const
```
`true` if this device is the elected and active gateway.

---

## Packet constants

```cpp
#define MESH_PORT        7777   // TCP port for WiFi peers and bridge
#define MESH_MAX_TTL     8      // max hops before drop
#define MESH_MAX_PAYLOAD 4096   // max body bytes per message
#define ESPNOW_MAX_BODY  221    // ESP-NOW usable body after MeshPacket header
#define BROADCAST_ID     0xFFFFFFFFFFFFFFFF
```

---

## `IBridge` interface

Implement this to connect the gateway to an external application:

```cpp
class IBridge {
public:
  virtual void begin()  = 0;                                   // called on gateway activation
  virtual void loop()   = 0;                                   // called every mesh loop
  virtual void send(const MeshPacket &pkt, const uint8_t *body) = 0;  // mesh → app
  virtual bool connected() = 0;

  // wired by Gateway: call this when the app sends a packet in
  void onReceive(BridgeReceiveCb cb);
};
```

The library provides `TcpBridge` as a concrete implementation (TCP socket to a Go/Python/etc. app).

---

## Topology (gateway only)

Access via the global `Topology`:

```cpp
#include <MeshTopology.h>

const TopoNode *nodes = Topology.nodes();
int count = Topology.nodeCount();

// TopoNode fields:
struct TopoNode {
  NodeID   id;
  NodeID   neighbors[TOPO_MAX_NEIGHBORS];  // up to 20
  uint8_t  neighborCount;
  uint32_t lastUpdate;
  bool     active;
};
```

Only the gateway has the full picture. Other nodes only have their own row populated.

---

## Election internals (advanced)

Access via the global `Election`:

```cpp
#include <MeshElection.h>

NodeID gw = Election.currentGateway();
bool  won = Election.isSelfGateway();
```

---

## Helper: NodeID ↔ MAC

```cpp
void nodeIdToMac(NodeID id, uint8_t mac[6]);
// reconstructs the 6-byte MAC from a NodeID
```

---

## `meshctl` (Go CLI)

Located at `tools/meshctl`. Talks to the gateway's `TcpBridge` (`MESH_BRIDGE_PORT` = 7778), discovered via mDNS (`_neoworks._tcp`).

```sh
cd tools/meshctl && go build -o meshctl .

meshctl discover
  → gateway: <ip>:7778
  → node_id: <gateway NodeID>

meshctl set --node <16-hex-NodeID|broadcast> --color RRGGBB [--brightness 0-255] [--addr host:port]
  → sends a MESH_TYPE_DATA packet, topic "led/color", payload = {r,g,b,brightness}
```

`--addr` skips mDNS discovery. `set` works for any subscriber, not just `examples/led_node` — the topic/payload are specific to that example, but the discovery + send logic is general-purpose for any `MESH_TYPE_DATA` publish.
