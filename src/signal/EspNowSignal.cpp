// src/signal/EspNowSignal.cpp
#include "EspNowSignal.h"
#include "MeshLog.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <string.h>

EspNowSignal EspNow;
static EspNowSignal *_inst = nullptr;

// ─── init ────────────────────────────────────────────────────────────────────

void EspNowSignal::init(NodeID self) {
  _self = self;
  _inst = this;
  _startMs = millis();
  nodeIdToMac(self, _selfMac);

  _ownBeacon.channel       = _channel;
  _ownBeacon.rssiToRouter  = INT8_MIN;
  _ownBeacon.flags         = 0;
  _ownBeacon.wifiIp        = 0;

  if (WiFi.getMode() == WIFI_OFF)
    WiFi.mode(WIFI_STA);

  if (WiFi.status() != WL_CONNECTED)
    esp_wifi_set_channel(_channel, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    MESH_LOGF("[ESPNOW] init failed\n");
    return;
  }

  esp_now_register_recv_cb(_recvCb);
  esp_now_register_send_cb(_sentCb);

  // register broadcast peer
  // channel=0 means "whatever channel the radio is currently on" — required
  // so sends keep working after WiFi.begin() moves the radio to the AP's
  // channel (a peer registered with a stale fixed channel causes
  // "Peer channel is not equal to the home channel, send fail!")
  esp_now_peer_info_t bcast{};
  memset(bcast.peer_addr, 0xFF, 6);
  bcast.channel = 0;
  bcast.encrypt = false;
  esp_now_add_peer(&bcast);

  _started = true;
  MESH_LOGF("[ESPNOW] started on channel %d\n", _channel);
  _sendBeacon();
}

// ─── loop ────────────────────────────────────────────────────────────────────

void EspNowSignal::loop() {
  if (!_started) return;

  // process frames captured by the recv callback — forwarding happens here,
  // in loop() context, never inside the WiFi-task callback
  _drainRx();

  // beacon fast right after boot so peers and the election converge quickly,
  // then settle to the steady interval
  uint32_t interval = (millis() - _startMs < ESPNOW_FAST_BEACON_WINDOW)
                          ? ESPNOW_FAST_BEACON_MS
                          : ESPNOW_BEACON_MS;
  if (millis() - _lastBeaconMs > interval)
    _sendBeacon();

  // a neighbour we just heard for the first time may not know us yet — beacon
  // back so it learns us immediately instead of waiting a whole interval.
  // Deferred out of the recv callback (esp_now_send must not run there).
  if (_replyPending) {
    _replyPending = false;
    if (millis() - _lastBeaconMs > 150)
      _sendBeacon();
  }

  _evictStalePeers();
  _updateScan();
}

// ─── send ────────────────────────────────────────────────────────────────────

void EspNowSignal::sendTo(NodeID dst, const MeshPacket &pkt, const uint8_t *body) {
  EspNowPeer *peer = _findPeer(dst);
  if (!peer) {
    broadcast(pkt, body);
    return;
  }

  uint16_t total = sizeof(MeshPacket) + pkt.payloadLen;
  if (total > 250) {
    MESH_LOGF("[ESPNOW] frame too large, dropping\n");
    return;
  }

  uint8_t frame[250];
  memcpy(frame, &pkt, sizeof(MeshPacket));
  if (pkt.payloadLen > 0)
    memcpy(frame + sizeof(MeshPacket), body, pkt.payloadLen);

  peer->sendCount++;
  memcpy(_lastSentMac, peer->mac, 6);
  esp_now_send(peer->mac, frame, total);
}

void EspNowSignal::broadcast(const MeshPacket &pkt, const uint8_t *body) {
  uint16_t total = sizeof(MeshPacket) + pkt.payloadLen;
  if (total > 250) return;

  uint8_t frame[250];
  memcpy(frame, &pkt, sizeof(MeshPacket));
  if (pkt.payloadLen > 0)
    memcpy(frame + sizeof(MeshPacket), body, pkt.payloadLen);

  uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_send(bcast, frame, total);
}

bool EspNowSignal::hasPeer(NodeID dst) {
  return _findPeer(dst) != nullptr;
}

// ─── channel ─────────────────────────────────────────────────────────────────

void EspNowSignal::setChannel(uint8_t ch) {
  if (_channel == ch) return;
  _channel = ch;
  _ownBeacon.channel = ch;
  if (_started && WiFi.status() != WL_CONNECTED)
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  MESH_LOGF("[ESPNOW] channel → %d\n", ch);
}

// ─── beacon ──────────────────────────────────────────────────────────────────

void EspNowSignal::_sendBeacon() {
  MeshPacket pkt{};
  pkt.version    = MESH_VERSION;
  pkt.type       = MESH_TYPE_BEACON;
  pkt.msgId      = esp_random();
  pkt.src        = _self;
  pkt.dst        = BROADCAST_ID;
  pkt.ttl        = 1;  // beacons never get forwarded
  pkt.payloadLen = sizeof(BeaconBody);
  pkt.flags      = MESH_FLAG_BROADCAST | MESH_FLAG_NO_FWD;

  broadcast(pkt, (const uint8_t *)&_ownBeacon);
  _lastBeaconMs = millis();
}

// ─── callbacks ───────────────────────────────────────────────────────────────

void EspNowSignal::_recvCb(const uint8_t *mac_addr,
                            const uint8_t *data, int len) {
  if (_inst)
    _inst->_handleRecv(mac_addr, data, len);
}

void EspNowSignal::_sentCb(const uint8_t *mac, esp_now_send_status_t status) {
  if (_inst)
    _inst->_handleSent(mac, status);
}

// ─── recv handler ────────────────────────────────────────────────────────────

void EspNowSignal::_handleRecv(const uint8_t *srcMac,
                                const uint8_t *data, int len) {
  if (len < (int)sizeof(MeshPacket)) return;

  MeshPacket pkt;
  memcpy(&pkt, data, sizeof(MeshPacket));

  if (pkt.version != MESH_VERSION) return;
  if (pkt.src == _self) return;

  int bodyLen = len - (int)sizeof(MeshPacket);
  if (bodyLen < (int)pkt.payloadLen) return;
  const uint8_t *body = data + sizeof(MeshPacket);

  if (pkt.type == MESH_TYPE_BEACON) {
    if (bodyLen >= (int)sizeof(BeaconBody)) {
      BeaconBody b;
      memcpy(&b, body, sizeof(BeaconBody));
      _handleBeacon(pkt.src, srcMac, b);
    }
    return;  // beacons consumed here, never reach router
  }

  // defer router packets to loop(): calling esp_now_send() while forwarding
  // from inside this callback (WiFi task) overflows the small ESP-NOW TX queue
  // and silently drops forwarded frames — the cause of multi-hop delivery
  // failing while direct delivery to the gateway worked.
  if (!_enqueueRx(srcMac, data, len))
    MESH_LOGF("[ESPNOW] rx queue full, frame dropped\n");
}

// ─── deferred receive ────────────────────────────────────────────────────────

bool EspNowSignal::_enqueueRx(const uint8_t *srcMac, const uint8_t *data,
                              int len) {
  uint8_t next = (uint8_t)((_rxHead + 1) % ESPNOW_RX_QUEUE);
  if (next == _rxTail)
    return false;  // queue full

  EspNowRxFrame &frame = _rxQueue[_rxHead];
  memcpy(frame.data, data, len);
  frame.len = (uint16_t)len;
  memcpy(frame.srcMac, srcMac, 6);

  _rxHead = next;
  return true;
}

void EspNowSignal::_drainRx() {
  while (_rxTail != _rxHead) {
    EspNowRxFrame &frame = _rxQueue[_rxTail];

    MeshPacket pkt;
    memcpy(&pkt, frame.data, sizeof(MeshPacket));
    const uint8_t *body = frame.data + sizeof(MeshPacket);

    EspNowPeer *peer = _findPeer(pkt.src);
    if (peer)
      peer->lastSeen = millis();

    if (_receiveCb)
      _receiveCb(pkt, body, false, macToNodeId(frame.srcMac));

    _rxTail = (uint8_t)((_rxTail + 1) % ESPNOW_RX_QUEUE);
  }
}

void EspNowSignal::_handleBeacon(NodeID src, const uint8_t *srcMac,
                                  const BeaconBody &b) {
  EspNowPeer *peer = _findPeer(src);
  if (!peer) {
    peer = _freePeer();
    if (!peer) {
      MESH_LOGF("[ESPNOW] peer table full\n");
      return;
    }
    peer->id        = src;
    peer->active    = true;
    peer->sendCount = 0;
    peer->failCount = 0;
    memcpy(peer->mac, srcMac, 6);
    _addEspNowPeer(srcMac);
    _replyPending = true;  // beacon back so the newcomer learns us right away
    MESH_LOGF("[ESPNOW] peer discovered %016llX\n", src);
  }
  peer->lastSeen = millis();

  MESH_DBGF("[ESPNOW] beacon %016llX ch=%u rssi=%d flags=0x%02X ip=%u.%u.%u.%u\n",
            src, b.channel, b.rssiToRouter, b.flags, (b.wifiIp >> 0) & 0xFF,
            (b.wifiIp >> 8) & 0xFF, (b.wifiIp >> 16) & 0xFF,
            (b.wifiIp >> 24) & 0xFF);

  // follow gateway's channel if we aren't connected to WiFi
  if (b.channel != _channel && WiFi.status() != WL_CONNECTED)
    setChannel(b.channel);

  if (_peerDiscoverCb)
    _peerDiscoverCb(src, b);
}

void EspNowSignal::_handleSent(const uint8_t *mac, esp_now_send_status_t status) {
  EspNowPeer *peer = _findPeerByMac(mac);
  if (!peer) return;
  if (status != ESP_NOW_SEND_SUCCESS)
    peer->failCount++;
}

// ─── peer management ─────────────────────────────────────────────────────────

void EspNowSignal::_addEspNowPeer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) return;
  esp_now_peer_info_t info{};
  memcpy(info.peer_addr, mac, 6);
  info.channel = 0;  // follow current radio channel, see init()
  info.encrypt = false;
  esp_now_add_peer(&info);
}

void EspNowSignal::_evictStalePeers() {
  uint32_t now = millis();
  for (int i = 0; i < ESPNOW_MAX_PEERS; i++) {
    if (!_peers[i].active) continue;
    if (now - _peers[i].lastSeen > ESPNOW_PEER_TIMEOUT) {
      MESH_LOGF("[ESPNOW] peer %016llX timed out\n", _peers[i].id);
      esp_now_del_peer(_peers[i].mac);
      _peers[i].active = false;
    }
  }
}

// ─── channel scan ────────────────────────────────────────────────────────────
//
// If we're not connected to WiFi (so free to change channel) and have no
// active peers, the gateway likely moved to the AP's channel and we're
// stranded on the wrong one — beacon-based channel-follow can't help here
// since it requires already being on the sender's channel. Hop through all
// channels, dwelling long enough on each to catch a beacon if the gateway
// is there, until a peer reappears.

void EspNowSignal::_updateScan() {
  if (WiFi.status() == WL_CONNECTED) {
    _scanning = false;
    return;
  }

  bool hasPeers = false;
  for (int i = 0; i < ESPNOW_MAX_PEERS; i++) {
    if (_peers[i].active) { hasPeers = true; break; }
  }

  if (hasPeers) {
    if (_scanning)
      MESH_LOGF("[ESPNOW] peer found on channel %d, scan stopped\n", _channel);
    _scanning = false;
    return;
  }

  uint32_t now = millis();
  if (!_scanning) {
    _scanning    = true;
    _lastScanHop = now;
    MESH_LOGF("[ESPNOW] no peers — scanning channels\n");
    return;
  }

  if (now - _lastScanHop < ESPNOW_SCAN_DWELL_MS) return;

  uint8_t next = (_channel >= ESPNOW_SCAN_MAX_CH) ? ESPNOW_SCAN_MIN_CH
                                                   : (uint8_t)(_channel + 1);
  setChannel(next);
  _sendBeacon();
  _lastScanHop = now;
}

EspNowPeer *EspNowSignal::_findPeer(NodeID id) {
  for (int i = 0; i < ESPNOW_MAX_PEERS; i++)
    if (_peers[i].active && _peers[i].id == id)
      return &_peers[i];
  return nullptr;
}

EspNowPeer *EspNowSignal::_findPeerByMac(const uint8_t *mac) {
  for (int i = 0; i < ESPNOW_MAX_PEERS; i++)
    if (_peers[i].active && memcmp(_peers[i].mac, mac, 6) == 0)
      return &_peers[i];
  return nullptr;
}

EspNowPeer *EspNowSignal::_freePeer() {
  for (int i = 0; i < ESPNOW_MAX_PEERS; i++)
    if (!_peers[i].active) return &_peers[i];
  return nullptr;
}

// ─── metrics ─────────────────────────────────────────────────────────────────

float EspNowSignal::peerLossRate(NodeID id) {
  EspNowPeer *p = _findPeer(id);
  if (!p || p->sendCount == 0) return 0.0f;
  return (float)p->failCount / p->sendCount;
}

int EspNowSignal::getPeers(NodeID *out, int maxCount) {
  int n = 0;
  for (int i = 0; i < ESPNOW_MAX_PEERS && n < maxCount; i++)
    if (_peers[i].active) out[n++] = _peers[i].id;
  return n;
}
