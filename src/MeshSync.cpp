// src/MeshSync.cpp
#include "MeshSync.h"
#include <Arduino.h>
#include <esp_timer.h>
#include <string.h>

MeshSync Sync;

void MeshSync::init(NodeID self, IMeshSignal *signal) {
  _self   = self;
  _signal = signal;
}

// ─── loop ────────────────────────────────────────────────────────────────────

void MeshSync::loop() {
  // master doesn't sync to itself; unset master = nothing to do
  if (_master == 0 || _master == _self) return;

  static uint32_t lastSync = 0;
  if (millis() - lastSync > SYNC_INTERVAL_MS) {
    _sendSyncReq();
    lastSync = millis();
  }
}

// ─── receive ─────────────────────────────────────────────────────────────────

void MeshSync::receive(const MeshPacket &pkt, const uint8_t *body) {
  if (pkt.type == MESH_TYPE_SYNC_REQ)
    _handleReq(pkt, body);
  else if (pkt.type == MESH_TYPE_SYNC_RESP)
    _handleResp(pkt, body);
}

// ─── request ─────────────────────────────────────────────────────────────────

void MeshSync::_sendSyncReq() {
  SyncReqBody req;
  req.t1 = esp_timer_get_time();

  MeshPacket pkt{};
  pkt.version    = MESH_VERSION;
  pkt.type       = MESH_TYPE_SYNC_REQ;
  pkt.msgId      = esp_random();
  pkt.src        = _self;
  pkt.dst        = _master;
  pkt.ttl        = MESH_MAX_TTL;
  pkt.payloadLen = sizeof(SyncReqBody);
  pkt.flags      = 0;

  _signal->sendTo(_master, pkt, (const uint8_t *)&req);
}

void MeshSync::_handleReq(const MeshPacket &pkt, const uint8_t *body) {
  if (_master != _self) return;  // only master responds
  if (pkt.payloadLen < sizeof(SyncReqBody)) return;

  SyncReqBody req;
  memcpy(&req, body, sizeof(req));

  SyncRespBody resp;
  resp.t1 = req.t1;
  resp.t2 = esp_timer_get_time();
  resp.t3 = esp_timer_get_time();  // very close to t2

  MeshPacket rpkt{};
  rpkt.version    = MESH_VERSION;
  rpkt.type       = MESH_TYPE_SYNC_RESP;
  rpkt.msgId      = esp_random();
  rpkt.src        = _self;
  rpkt.dst        = pkt.src;
  rpkt.ttl        = MESH_MAX_TTL;
  rpkt.payloadLen = sizeof(SyncRespBody);
  rpkt.flags      = 0;

  _signal->sendTo(pkt.src, rpkt, (const uint8_t *)&resp);
}

void MeshSync::_handleResp(const MeshPacket &pkt, const uint8_t *body) {
  if (pkt.dst != _self) return;
  if (pkt.payloadLen < sizeof(SyncRespBody)) return;

  SyncRespBody resp;
  memcpy(&resp, body, sizeof(resp));
  int64_t t4 = esp_timer_get_time();

  int64_t rtt = (t4 - resp.t1) - (resp.t3 - resp.t2);
  if (rtt < 0 || rtt > SYNC_MAX_RTT_US) return;

  _offset = ((resp.t2 - resp.t1) + (resp.t3 - t4)) / 2;
  _synced = true;
}

// ─── query ───────────────────────────────────────────────────────────────────

int64_t MeshSync::syncedTime() const {
  return esp_timer_get_time() + _offset;
}
