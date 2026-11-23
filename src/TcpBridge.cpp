#include "TcpBridge.h"
#include "MeshLog.h"
#include "MeshPacket.h"
#include <Arduino.h>

// ─── begin ───────────────────────────────────────────────────────────────────

void TcpBridge::begin() {
  _server.begin();
  MESH_LOGF("[BRIDGE] Go app TCP server on port %d\n", MESH_PORT + 1);
}

// ─── loop ────────────────────────────────────────────────────────────────────

void TcpBridge::loop() {
  // accept new Go app connection — only one at a time
  if (!_client.connected()) {
    WiFiClient incoming = _server.accept();
    if (incoming) {
      _client = incoming;
      MESH_LOGF("[BRIDGE] Go app connected from %s\n",
                _client.remoteIP().toString().c_str());
    }
  }

  if (_client.connected())
    _handleIncoming();
}

// ─── receive from Go app ─────────────────────────────────────────────────────

void TcpBridge::_handleIncoming() {
  if (_client.available() < (int)sizeof(MeshPacket))
    return;

  MeshPacket hdr;
  _client.readBytes((uint8_t *)&hdr, sizeof(hdr));

  if (hdr.version != MESH_VERSION ||
      hdr.payloadLen > MESH_MAX_PAYLOAD + MESH_MAX_TOPIC) {
    MESH_LOGF("[BRIDGE] bad frame from Go app, dropping\n");
    _client.stop();
    return;
  }

  if (_client.available() < hdr.payloadLen)
    return;

  uint8_t *body = (uint8_t *)malloc(hdr.payloadLen);
  if (!body)
    return;

  _client.readBytes(body, hdr.payloadLen);

  if (_receiveCb)
    _receiveCb(hdr, body);

  free(body);
}

// ─── send to Go app ──────────────────────────────────────────────────────────

void TcpBridge::send(const MeshPacket &pkt, const uint8_t *body) {
  if (!_client.connected())
    return;
  _client.write((uint8_t *)&pkt, sizeof(pkt));
  _client.write(body, pkt.payloadLen);
}

bool TcpBridge::connected() { return _client.connected(); }
