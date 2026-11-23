// include/WifiSignal.h
#pragma once
#include "IMeshSignal.h"
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <stdint.h>

#define WIFI_MAX_PEERS          8
#define WIFI_CONNECT_RETRY_MS   10000  // min gap between connect attempts to the same peer
#define WIFI_CONNECT_TIMEOUT_MS 1000   // bound on the blocking client.connect() call

struct WifiPeer {
  NodeID     id;
  WiFiClient client;
  bool       active = false;
};

struct WifiPendingConnect {
  NodeID   id;
  uint32_t ip;
  uint32_t lastAttempt;
  bool     pending = false;
};

class WifiSignal : public IMeshSignal {
  NodeID     _self;
  WiFiServer _server{MESH_PORT};
  WifiPeer   _peers[WIFI_MAX_PEERS];
  WifiPendingConnect _pending[WIFI_MAX_PEERS];
  bool       _started = false;

  void      _handleClient(WifiPeer &peer);
  void      _write(WiFiClient &client, const MeshPacket &pkt,
                   const uint8_t *body);
  void      _processPending();
  void      _attemptConnect(NodeID id, uint32_t ipRaw);
  WifiPeer *_findPeer(NodeID id);
  WifiPeer *_freePeer();
  WifiPendingConnect *_findPending(NodeID id);
  WifiPendingConnect *_freePending();

public:
  void init(NodeID self) override;
  void loop() override;
  void sendTo(NodeID dst, const MeshPacket &pkt, const uint8_t *body) override;
  void broadcast(const MeshPacket &pkt, const uint8_t *body) override;
  bool hasPeer(NodeID dst) override;

  // called when EspNow discovers a WiFi-capable peer — queues a connect
  // attempt, actually dialed from loop() so the ESP-NOW recv callback
  // never blocks on a TCP connect
  void connectToPeer(NodeID id, uint32_t ipRaw);
};
