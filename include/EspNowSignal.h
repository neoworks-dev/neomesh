// include/EspNowSignal.h
#pragma once
#include "IMeshSignal.h"
#include "MeshPacket.h"
#include <esp_now.h>
#include <functional>
#include <stdint.h>

#define ESPNOW_MAX_PEERS    20
#define ESPNOW_BEACON_MS    3000
#define ESPNOW_PEER_TIMEOUT 15000
#define ESPNOW_RX_QUEUE     8     // deferred-receive ring depth (power of 2)

// Right after boot, beacon quickly so peers and the election converge in a
// couple of seconds instead of a couple of beacon intervals; then settle to
// the steady interval to keep the air quiet.
#define ESPNOW_FAST_BEACON_MS     500
#define ESPNOW_FAST_BEACON_WINDOW 12000

// channel scan: used by nodes not connected to WiFi when they have no active
// peers (e.g. the gateway moved to the AP's channel and is no longer reachable
// on our current channel). A node beacons on entering each channel and any peer
// there replies immediately (see _handleBeacon), so the dwell only needs to
// cover a beacon round-trip — not a whole beacon period.
#define ESPNOW_SCAN_DWELL_MS 600
#define ESPNOW_SCAN_MIN_CH  1
#define ESPNOW_SCAN_MAX_CH  13

struct EspNowPeer {
  NodeID   id;
  uint8_t  mac[6];
  uint8_t  sendCount;
  uint8_t  failCount;
  uint32_t lastSeen;
  bool     active;
};

// A frame copied out of the ESP-NOW recv callback so it can be parsed and
// forwarded from loop() instead of the WiFi-task callback context.
struct EspNowRxFrame {
  uint8_t  data[250];
  uint16_t len;
  uint8_t  srcMac[6];
};

using EspNowPeerCb = std::function<void(NodeID, const BeaconBody &)>;

class EspNowSignal : public IMeshSignal {
  NodeID     _self;
  uint8_t    _selfMac[6];
  uint8_t    _channel = 1;
  EspNowPeer _peers[ESPNOW_MAX_PEERS];
  bool       _started = false;

  bool       _scanning   = false;
  uint32_t   _lastScanHop = 0;

  uint32_t   _startMs      = 0; // boot time, for the fast-beacon window
  uint32_t   _lastBeaconMs = 0; // last beacon TX, drives the beacon cadence
  bool       _replyPending = false; // a newly-heard neighbour to beacon back at

  // single-producer (recv callback) / single-consumer (loop) ring — safe
  // without locks on the single-core ESP32-C3
  EspNowRxFrame    _rxQueue[ESPNOW_RX_QUEUE];
  volatile uint8_t _rxHead = 0;
  volatile uint8_t _rxTail = 0;

  BeaconBody _ownBeacon{};
  uint8_t    _lastSentMac[6]{};
  EspNowPeerCb _peerDiscoverCb;

  void _sendBeacon();
  void _handleBeacon(NodeID src, const uint8_t *srcMac, const BeaconBody &b);
  void _addEspNowPeer(const uint8_t *mac);
  void _handleRecv(const uint8_t *srcMac, const uint8_t *data, int len);
  bool _enqueueRx(const uint8_t *srcMac, const uint8_t *data, int len);
  void _drainRx();
  void _handleSent(const uint8_t *mac, esp_now_send_status_t status);
  void _evictStalePeers();
  void _updateScan();

  EspNowPeer *_findPeer(NodeID id);
  EspNowPeer *_findPeerByMac(const uint8_t *mac);
  EspNowPeer *_freePeer();

  static void _recvCb(const uint8_t *mac_addr, const uint8_t *data, int len);
  static void _sentCb(const uint8_t *mac, esp_now_send_status_t status);

public:
  void init(NodeID self) override;
  void loop() override;
  void sendTo(NodeID dst, const MeshPacket &pkt, const uint8_t *body) override;
  void broadcast(const MeshPacket &pkt, const uint8_t *body) override;
  bool hasPeer(NodeID dst) override;

  void setChannel(uint8_t ch);
  void setBeaconFlags(uint8_t flags)    { _ownBeacon.flags = flags; }
  void setBeaconRssi(int8_t rssi)       { _ownBeacon.rssiToRouter = rssi; }
  void setBeaconWifiIp(uint32_t ip)     { _ownBeacon.wifiIp = ip; }

  float   peerLossRate(NodeID id);
  int     getPeers(NodeID *out, int maxCount);

  void onPeerDiscover(EspNowPeerCb cb) { _peerDiscoverCb = cb; }
};

extern EspNowSignal EspNow;
