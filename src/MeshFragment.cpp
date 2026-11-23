// src/MeshFragment.cpp
#include "MeshFragment.h"
#include "MeshLog.h"
#include <Arduino.h>
#include <string.h>

MeshFragment Fragment;

void MeshFragment::init(NodeID self, IMeshSignal *signal) {
  _self   = self;
  _signal = signal;
  memset(_streams, 0, sizeof(_streams));
}

// ─── loop ────────────────────────────────────────────────────────────────────

void MeshFragment::loop() { _expireStreams(); }

// ─── send ────────────────────────────────────────────────────────────────────

void MeshFragment::send(NodeID nextHop, const MeshPacket &origPkt,
                        const uint8_t *body) {
  uint32_t streamId  = esp_random();
  uint32_t totalLen  = origPkt.payloadLen;
  uint16_t maxData   = (uint16_t)FRAG_MAX_DATA;
  uint16_t fragCount = (uint16_t)((totalLen + maxData - 1) / maxData);

  for (uint16_t i = 0; i < fragCount; i++) {
    uint32_t offset      = (uint32_t)i * maxData;
    uint16_t fragDataLen = (uint16_t)(totalLen - offset);
    if (fragDataLen > maxData) fragDataLen = maxData;

    uint8_t fragBody[ESPNOW_MAX_BODY];
    FragBody hdr;
    hdr.streamId  = streamId;
    hdr.totalLen  = totalLen;
    hdr.fragIndex = i;
    hdr.fragCount = fragCount;
    hdr.origType  = origPkt.type;
    memcpy(fragBody, &hdr, FRAG_HEADER_SIZE);
    memcpy(fragBody + FRAG_HEADER_SIZE, body + offset, fragDataLen);

    MeshPacket pkt  = origPkt;
    pkt.type        = MESH_TYPE_FRAG;
    pkt.msgId       = esp_random();
    pkt.payloadLen  = (uint16_t)(FRAG_HEADER_SIZE + fragDataLen);

    _signal->sendTo(nextHop, pkt, fragBody);
  }
}

// ─── receive ─────────────────────────────────────────────────────────────────

void MeshFragment::receive(const MeshPacket &pkt, const uint8_t *body) {
  if (pkt.payloadLen < FRAG_HEADER_SIZE) return;

  FragBody hdr;
  memcpy(&hdr, body, FRAG_HEADER_SIZE);

  if (hdr.fragCount == 0 || hdr.fragCount > FRAG_MAX_FRAGS) return;
  if (hdr.totalLen  > FRAG_STREAM_BUFSIZE)                   return;
  if (hdr.fragIndex >= hdr.fragCount)                        return;

  FragStream *s = _findStream(hdr.streamId, pkt.src);
  if (!s) {
    s = _openStream(hdr.streamId, pkt.src, pkt.dst, hdr.totalLen,
                    hdr.fragCount, hdr.origType, pkt);
    if (!s) {
      MESH_LOGF("[FRAG] no stream slot\n");
      return;
    }
  }

  if (s->received[hdr.fragIndex]) return;  // duplicate fragment

  uint32_t offset      = (uint32_t)hdr.fragIndex * (uint32_t)FRAG_MAX_DATA;
  uint16_t fragDataLen = (uint16_t)(pkt.payloadLen - FRAG_HEADER_SIZE);
  if (offset + fragDataLen > FRAG_STREAM_BUFSIZE) return;

  memcpy(s->buffer + offset, body + FRAG_HEADER_SIZE, fragDataLen);
  s->received[hdr.fragIndex] = true;
  s->recvCount++;
  s->lastActivity = millis();

  if (s->recvCount == s->fragCount) {
    // reassembly complete
    MeshPacket out    = s->origPkt;
    out.type          = s->origType;
    out.payloadLen    = (uint16_t)s->totalLen;
    if (_completeCb) _completeCb(out, s->buffer);
    s->active = false;
  }
}

// ─── stream management ───────────────────────────────────────────────────────

FragStream *MeshFragment::_findStream(uint32_t streamId, NodeID src) {
  for (int i = 0; i < FRAG_MAX_STREAMS; i++)
    if (_streams[i].active && _streams[i].streamId == streamId &&
        _streams[i].src == src)
      return &_streams[i];
  return nullptr;
}

FragStream *MeshFragment::_openStream(uint32_t streamId, NodeID src, NodeID dst,
                                       uint32_t totalLen, uint16_t fragCount,
                                       uint8_t origType, const MeshPacket &origPkt) {
  for (int i = 0; i < FRAG_MAX_STREAMS; i++) {
    if (!_streams[i].active) {
      FragStream &s   = _streams[i];
      s.streamId      = streamId;
      s.src           = src;
      s.dst           = dst;
      s.origType      = origType;
      s.totalLen      = totalLen;
      s.fragCount     = fragCount;
      s.recvCount     = 0;
      s.origPkt       = origPkt;
      s.lastActivity  = millis();
      s.active        = true;
      memset(s.received, 0, sizeof(s.received));
      return &s;
    }
  }
  return nullptr;
}

void MeshFragment::_expireStreams() {
  uint32_t now = millis();
  for (int i = 0; i < FRAG_MAX_STREAMS; i++) {
    if (_streams[i].active &&
        now - _streams[i].lastActivity > FRAG_STREAM_TIMEOUT) {
      MESH_LOGF("[FRAG] stream %08X expired (%d/%d frags)\n",
                _streams[i].streamId, _streams[i].recvCount,
                _streams[i].fragCount);
      _streams[i].active = false;
    }
  }
}
