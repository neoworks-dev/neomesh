// src/signal/WifiSignal.cpp
#include "WifiSignal.h"
#include "MeshLog.h"
#include <Arduino.h>
#include <WiFi.h>

// ─── init ────────────────────────────────────────────────────────────────────

void WifiSignal::init(NodeID self) {
  _self = self;

  if (WiFi.status() != WL_CONNECTED) {
    MESH_LOGF("[WIFI] no WiFi, deferring TCP init\n");
    return;
  }

  _server.begin();
  _started = true;
  MESH_LOGF("[WIFI] TCP listening on port %d\n", MESH_PORT);
}

// ─── loop ────────────────────────────────────────────────────────────────────

void WifiSignal::loop() {
  if (!_started) {
    if (WiFi.status() == WL_CONNECTED) {
      _server.begin();
      _started = true;
      MESH_LOGF("[WIFI] TCP init, ip=%s\n", WiFi.localIP().toString().c_str());
    }
    return;
  }

  // accept new incoming connections
  WiFiClient incoming = _server.accept();
  if (incoming) {
    WifiPeer *slot = _freePeer();
    if (slot) {
      slot->client = incoming;
      slot->id     = 0;  // learned from first packet
      slot->active = true;
    } else {
      incoming.stop();
    }
  }

  // service all connected peers
  for (int i = 0; i < WIFI_MAX_PEERS; i++) {
    if (!_peers[i].active) continue;
    if (!_peers[i].client.connected()) {
      MESH_LOGF("[WIFI] peer %016llX dropped\n", _peers[i].id);
      _peers[i].active = false;
      continue;
    }
    _handleClient(_peers[i]);
  }

  _processPending();
}

// ─── send ────────────────────────────────────────────────────────────────────

void WifiSignal::sendTo(NodeID dst, const MeshPacket &pkt, const uint8_t *body) {
  WifiPeer *peer = _findPeer(dst);
  if (!peer || !peer->client.connected()) return;
  _write(peer->client, pkt, body);
}

void WifiSignal::broadcast(const MeshPacket &pkt, const uint8_t *body) {
  for (int i = 0; i < WIFI_MAX_PEERS; i++)
    if (_peers[i].active && _peers[i].client.connected())
      _write(_peers[i].client, pkt, body);
}

bool WifiSignal::hasPeer(NodeID dst) {
  WifiPeer *p = _findPeer(dst);
  return p && p->client.connected();
}

// ─── peer connection ─────────────────────────────────────────────────────────

// called from EspNow's recv callback (peer-discover path) — must not block,
// so just record the request and let loop() dial it
void WifiSignal::connectToPeer(NodeID id, uint32_t ipRaw) {
  if (_findPeer(id)) return;  // already connected

  WifiPendingConnect *p = _findPending(id);
  if (!p) p = _freePending();
  if (!p) return;  // pending table full, drop — next beacon retries

  p->id      = id;
  p->ip      = ipRaw;
  p->pending = true;
}

void WifiSignal::_processPending() {
  uint32_t now = millis();
  for (int i = 0; i < WIFI_MAX_PEERS; i++) {
    WifiPendingConnect &p = _pending[i];
    if (!p.pending) continue;

    if (_findPeer(p.id)) {
      p.pending = false;  // connected some other way meanwhile
      continue;
    }
    if (now - p.lastAttempt < WIFI_CONNECT_RETRY_MS) continue;

    p.lastAttempt = now;
    _attemptConnect(p.id, p.ip);
  }
}

void WifiSignal::_attemptConnect(NodeID id, uint32_t ipRaw) {
  if (!_started) return;

  WifiPeer *slot = _freePeer();
  if (!slot) {
    MESH_LOGF("[WIFI] peer table full\n");
    return;
  }

  IPAddress ip(ipRaw);
  if (!slot->client.connect(ip, MESH_PORT, WIFI_CONNECT_TIMEOUT_MS)) {
    MESH_LOGF("[WIFI] could not connect to %s\n", ip.toString().c_str());
    return;  // _pending entry stays set, retried after WIFI_CONNECT_RETRY_MS
  }

  slot->id     = id;
  slot->active = true;
  MESH_LOGF("[WIFI] connected to peer %016llX @ %s\n", id,
            ip.toString().c_str());
}

// ─── receive ─────────────────────────────────────────────────────────────────

void WifiSignal::_handleClient(WifiPeer &peer) {
  WiFiClient &c = peer.client;
  if (c.available() < (int)sizeof(MeshPacket)) return;

  MeshPacket hdr;
  c.readBytes((uint8_t *)&hdr, sizeof(hdr));

  if (peer.id == 0) peer.id = hdr.src;

  if (hdr.version != MESH_VERSION ||
      hdr.payloadLen > MESH_MAX_PAYLOAD + MESH_MAX_TOPIC) {
    MESH_LOGF("[WIFI] bad frame, closing\n");
    c.stop();
    peer.active = false;
    return;
  }

  if (c.available() < hdr.payloadLen) return;

  uint8_t *body = (uint8_t *)malloc(hdr.payloadLen);
  if (!body) return;
  c.readBytes(body, hdr.payloadLen);

  if (_receiveCb) _receiveCb(hdr, body, false, peer.id);
  free(body);
}

// ─── write ───────────────────────────────────────────────────────────────────

void WifiSignal::_write(WiFiClient &c, const MeshPacket &hdr,
                        const uint8_t *body) {
  c.write((uint8_t *)&hdr, sizeof(hdr));
  c.write(body, hdr.payloadLen);
}

// ─── helpers ─────────────────────────────────────────────────────────────────

WifiPeer *WifiSignal::_findPeer(NodeID id) {
  for (int i = 0; i < WIFI_MAX_PEERS; i++)
    if (_peers[i].active && _peers[i].id == id) return &_peers[i];
  return nullptr;
}

WifiPeer *WifiSignal::_freePeer() {
  for (int i = 0; i < WIFI_MAX_PEERS; i++)
    if (!_peers[i].active) return &_peers[i];
  return nullptr;
}

WifiPendingConnect *WifiSignal::_findPending(NodeID id) {
  for (int i = 0; i < WIFI_MAX_PEERS; i++)
    if (_pending[i].pending && _pending[i].id == id) return &_pending[i];
  return nullptr;
}

WifiPendingConnect *WifiSignal::_freePending() {
  for (int i = 0; i < WIFI_MAX_PEERS; i++)
    if (!_pending[i].pending) return &_pending[i];
  return nullptr;
}
