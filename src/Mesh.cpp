// src/Mesh.cpp
#include "Mesh.h"
#include <Arduino.h>
#include <WiFi.h>

MeshClass Mesh;
static WifiSignal WifiSig;

// ─── begin ───────────────────────────────────────────────────────────────────

void MeshClass::begin() {
  if (_started) return;

  // derive NodeID from WiFi MAC — stable, unique, no config needed
  uint8_t mac[6];
  WiFi.macAddress(mac);
  NodeID self = 0;
  for (int i = 0; i < 6; i++) self = (self << 8) | mac[i];

  Log.init(self);
  MESH_LOGF("[MESH] node id: %016llX\n", self);

  // ── signal init ─────────────────────────────────────────────────────────

  // ESP-NOW: always active, primary transport + peer discovery
  EspNow.init(self);
  EspNow.onReceive([](const MeshPacket &pkt, const uint8_t *body, bool up,
                      NodeID prevHop) {
    Router.receive(pkt, body, up, prevHop);
  });

  // WiFi TCP: active only when WiFi is connected, used for large packets
  WifiSig.init(self);
  WifiSig.onReceive([](const MeshPacket &pkt, const uint8_t *body, bool up,
                       NodeID prevHop) {
    Router.receive(pkt, body, up, prevHop);
  });

  // when ESP-NOW discovers a WiFi-capable peer, connect TCP to it
  EspNow.onPeerDiscover([](NodeID id, const BeaconBody &b) {
    if ((b.flags & BEACON_FLAG_WIFI_READY) && b.wifiIp != 0)
      WifiSig.connectToPeer(id, b.wifiIp);
  });

  // ── router ──────────────────────────────────────────────────────────────

  Router.init(self, &EspNow, &WifiSig);

  // reassembled fragments re-enter the router as DATA packets
  Fragment.init(self, &EspNow);
  Fragment.onComplete([](const MeshPacket &pkt, const uint8_t *body) {
    // reassembled from fragments — the original previous hop is not tracked
    Router.receive(pkt, body, false, 0);
  });

  // ── election ────────────────────────────────────────────────────────────

  Election.init(self, &EspNow);
  Election.onGatewayChange([this](NodeID winner) {
    if (winner == Router.selfId()) {
      Gateway.activate();
      Topology.setGateway(true);
      Sync.setMaster(winner);  // self is master — no sync needed, offset stays 0
    } else {
      Gateway.deactivate();
      Topology.setGateway(false);
      Sync.setMaster(winner);
    }
    // update beacon: candidates always advertise that fact; WiFi-ready nodes add IP
    if (_isCandidate) {
      uint8_t beaconFlags = BEACON_FLAG_CANDIDATE;
      if (WiFi.status() == WL_CONNECTED) {
        beaconFlags |= BEACON_FLAG_WIFI_READY;
        EspNow.setBeaconRssi((int8_t)WiFi.RSSI());
        EspNow.setBeaconWifiIp((uint32_t)WiFi.localIP());
      }
      EspNow.setBeaconFlags(beaconFlags);
    }
    if (_electionCb) _electionCb(winner);
  });

  // ── sync ────────────────────────────────────────────────────────────────

  Sync.init(self, &EspNow);

  // ── topology ────────────────────────────────────────────────────────────

  Topology.init(self, &EspNow);

  // ── gateway ─────────────────────────────────────────────────────────────

  Gateway.init();

  // if we have WiFi credentials, advertise candidacy in beacons
  if (_isCandidate)
    EspNow.setBeaconFlags(BEACON_FLAG_CANDIDATE);

  _started = true;
  MESH_LOGF("[MESH] started\n");
}

// ─── loop ────────────────────────────────────────────────────────────────────

void MeshClass::loop() {
  if (!_started) return;

  EspNow.loop();
  WifiSig.loop();
  Router.loop();
  Election.loop();
  Sync.loop();
  Fragment.loop();
  Gateway.loop();

  // update topology with current ESP-NOW peer list every loop tick
  // (cheap since it just checks a local array)
  static uint32_t lastTopoUpdate = 0;
  if (millis() - lastTopoUpdate > 5000) {
    NodeID peers[ESPNOW_MAX_PEERS];
    int count = EspNow.getPeers(peers, ESPNOW_MAX_PEERS);
    Topology.updateSelf(peers, (uint8_t)count);
    lastTopoUpdate = millis();

    // keep election loss rate in sync with actual ESP-NOW peer quality
    float totalLoss = 0;
    for (int i = 0; i < count; i++)
      totalLoss += EspNow.peerLossRate(peers[i]);
    if (count > 0) {
      uint8_t avgLoss = (uint8_t)((totalLoss / count) * 255.0f);
      Election.updateLossRate(avgLoss);
    }

    // keep beacon RSSI/channel/flags fresh — non-WiFi nodes follow the channel
    // to find the gateway's AP channel, and peers use WIFI_READY to know who
    // can bridge. (Previously the WIFI_READY flag was only refreshed on an
    // election change, so a connected node could advertise a stale "no WiFi"
    // beacon for a long time.)
    if (WiFi.status() == WL_CONNECTED) {
      EspNow.setBeaconRssi((int8_t)WiFi.RSSI());
      EspNow.setChannel((uint8_t)WiFi.channel());
      Election.updateRssi((int8_t)WiFi.RSSI());
      if (_isCandidate) {
        EspNow.setBeaconFlags(BEACON_FLAG_CANDIDATE | BEACON_FLAG_WIFI_READY);
        EspNow.setBeaconWifiIp((uint32_t)WiFi.localIP());
      }
    } else if (_isCandidate) {
      EspNow.setBeaconFlags(BEACON_FLAG_CANDIDATE);
      EspNow.setBeaconWifiIp(0);
    }
  }

  Topology.loop();
}
