# neomesh — Getting Started

## Installation

Add to your `platformio.ini`:

```ini
lib_deps =
    /path/to/neomesh
    bblanchon/ArduinoJson @ ^7.0.0
```

Or reference via git:

```ini
lib_deps =
    https://github.com/neoworks/neomesh.git
```

---

## Minimal node

Every device needs `Mesh.begin()` in `setup()` and `Mesh.loop()` in `loop()`. That's it for a pure mesh node:

```cpp
#include <Mesh.h>

void setup() {
  Serial.begin(115200);

  Mesh.subscribe("sensors/#", [](const char *topic, const uint8_t *payload, uint16_t len) {
    Serial.printf("%s: %.*s\n", topic, len, payload);
  });

  Mesh.begin();
}

void loop() {
  Mesh.loop();

  static uint32_t last = 0;
  if (millis() - last > 5000) {
    Mesh.publish("sensors/temp", "22.4");
    last = millis();
  }
}
```

---

## Gateway candidate node

Any node with WiFi credentials participates in the election. The best-connected one becomes gateway automatically:

```cpp
#include <Mesh.h>

void setup() {
  Serial.begin(115200);

  // credential = candidate. whoever has best WiFi becomes gateway.
  Mesh.setWifi("MySSID", "MyPassword");

  // optional: know when leadership changes
  Mesh.onElection([](NodeID winner) {
    if (Mesh.isGateway())
      Serial.println("I am the gateway");
    else
      Serial.printf("Gateway is %016llX\n", winner);
  });

  Mesh.begin();
}

void loop() { Mesh.loop(); }
```

You can flash the same binary on all 50 devices. Those near the router will compete for gateway; whichever has the best RSSI + lowest packet loss wins. If it fails, the next-best candidate takes over within 15 seconds.

---

## Publishing and subscribing

### Broadcast

```cpp
// string shorthand
Mesh.publish("lamps/color", "FF8800");

// raw bytes
uint8_t data[4] = {255, 128, 0, 255};
Mesh.publish("lamps/color", data, 4);
```

### Unicast

```cpp
// send to a specific node (NodeID is the MAC as uint64_t)
NodeID target = 0xA4CF1234ABCD0000ULL;
Mesh.publish(target, "lamps/brightness", "200");
```

### Subscribe (MQTT-style wildcards)

```cpp
// exact match
Mesh.subscribe("lamps/color", cb);

// + = one level
Mesh.subscribe("lamps/+/mode", cb);  // matches "lamps/strip1/mode"

// # = everything below
Mesh.subscribe("lamps/#", cb);       // matches all lamp topics
```

`subscribe()` returns a handle for later unsubscribing:

```cpp
int h = Mesh.subscribe("lamps/#", cb);
// ...
Mesh.unsubscribe(h);
```

---

## Time sync

After a sync exchange completes (usually within 2 seconds of boot), all nodes share a common clock:

```cpp
if (Mesh.timeSynced()) {
  int64_t t = Mesh.syncedTime();  // microseconds, corrected to gateway clock
  Serial.printf("synced time: %lld µs\n", t);
}
```

Accuracy is <1ms for nodes directly adjacent to the gateway. For nodes multiple hops away, add ~0.5ms per hop.

---

## Large payloads / streaming

Large payloads (>221 bytes) are automatically fragmented into ESP-NOW chunks. You don't need to do anything special — just `publish()` with a large payload and the library handles it.

For streaming (continuous data), use unicast publish in a tight loop:

```cpp
// in loop():
if (hasAudioFrame) {
  Mesh.publish(targetNode, "stream/audio", frame, frameSize);
}
```

Frames are routed via the fewest hops. If both nodes are on the same WiFi, the transfer goes via direct TCP without fragmentation overhead.

**Note:** Large broadcast is not supported — use unicast for streams. The reason is that broadcasting large payloads to every peer simultaneously would saturate the radio.

---

## Discovering the gateway from an external app

The elected gateway registers itself via mDNS as `neomesh._neoworks._tcp`. From a Go app, Python script, phone app, etc.:

```
mdns query: _neoworks._tcp
→ neomesh.local : 7778   (MESH_BRIDGE_PORT = MESH_PORT + 1)
→ TXT: node_id=A4CF1234ABCD0000
```

Once you have the IP and port, you can connect via `TcpBridge` or implement your own `IBridge`. The wire format is just `MeshPacket` (29-byte packed header) followed by `payloadLen` bytes of body (`topic` + `payload`).

---

## Worked example: WS2812B LED control via Go CLI

`examples/led_node/main.cpp` drives a WS2812B ARGB strip on GPIO2 via FastLED, subscribed to topic `led/color` (payload: 4 bytes — `r, g, b, brightness`).

1. Edit `MESH_WIFI_SSID` / `MESH_WIFI_PASS` in the example, flash it to **two** boards (`task flash-both PORT_A=... PORT_B=...`). Both are gateway candidates; either may be elected. Only the board with an LED strip wired to GPIO2 will visibly react, but both subscribe.
2. Note the NodeID each board prints on boot (`[APP] node id: ...`).
3. Build the CLI:
   ```sh
   cd tools/meshctl
   go build -o meshctl .
   ```
4. Find the gateway:
   ```sh
   ./meshctl discover
   # gateway: 192.168.1.42:7778
   # node_id: A4CF1234ABCD0000
   ```
5. Set the LED node's color (use the NodeID from step 2 — this works regardless of which board won the election):
   ```sh
   ./meshctl set --node <led-node-id> --color ff8800 --brightness 180
   ./meshctl set --node broadcast --color 000000 --brightness 0   # all off
   ```

`meshctl set` discovers the gateway via mDNS automatically (or pass `--addr host:port` to skip discovery), opens a TCP connection to `MESH_BRIDGE_PORT`, and sends one `MESH_TYPE_DATA` packet. The gateway's `Router.publish()` re-addresses it (`src` = gateway's own NodeID) and routes it to the target — even if the target is the gateway itself.

---

## Reading the mesh topology (gateway only)

```cpp
if (Mesh.isGateway()) {
  const TopoNode *nodes = Topology.nodes();
  int count = Topology.nodeCount();
  for (int i = 0; i < 50 && count > 0; i++) {
    if (!nodes[i].active) continue;
    Serial.printf("Node %016llX — %d neighbors\n",
                  nodes[i].id, nodes[i].neighborCount);
    count--;
  }
}
```

Non-gateway nodes only have their own row populated.

---

## NodeID

NodeID is a `uint64_t` derived from the device's WiFi MAC:

```cpp
NodeID self = Mesh.nodeId();
// self == mac[0] << 40 | mac[1] << 32 | ... | mac[5]

// recover MAC:
uint8_t mac[6];
nodeIdToMac(self, mac);
```

You can read a device's NodeID from the serial monitor on first boot and use it for unicast targeting.
