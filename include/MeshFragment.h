// include/MeshFragment.h
#pragma once
#include "IMeshSignal.h"
#include "MeshPacket.h"
#include <functional>
#include <stdint.h>

#define FRAG_MAX_STREAMS      4
#define FRAG_STREAM_BUFSIZE   4096
#define FRAG_MAX_FRAGS        64   // max fragments per stream (ceil(4096/208) = 20, headroom added)
#define FRAG_STREAM_TIMEOUT   5000 // ms

struct FragStream {
  uint32_t   streamId;
  NodeID     src;
  NodeID     dst;
  uint8_t    origType;
  uint32_t   totalLen;
  uint16_t   fragCount;
  uint16_t   recvCount;
  MeshPacket origPkt;
  uint8_t    buffer[FRAG_STREAM_BUFSIZE];
  bool       received[FRAG_MAX_FRAGS];
  uint32_t   lastActivity;
  bool       active;
};

class MeshFragment {
  NodeID       _self;
  IMeshSignal *_signal = nullptr;
  FragStream   _streams[FRAG_MAX_STREAMS];

  std::function<void(const MeshPacket &, const uint8_t *)> _completeCb;

  FragStream *_findStream(uint32_t streamId, NodeID src);
  FragStream *_openStream(uint32_t streamId, NodeID src, NodeID dst,
                          uint32_t totalLen, uint16_t fragCount,
                          uint8_t origType, const MeshPacket &origPkt);
  void _expireStreams();

public:
  void init(NodeID self, IMeshSignal *signal);
  void loop();

  // fragment pkt+body and send each chunk to nextHop via ESP-NOW
  void send(NodeID nextHop, const MeshPacket &pkt, const uint8_t *body);

  // called by Router when a MESH_TYPE_FRAG packet arrives for this node
  void receive(const MeshPacket &pkt, const uint8_t *body);

  // fires with reassembled DATA packet when all fragments arrive
  void onComplete(std::function<void(const MeshPacket &, const uint8_t *)> cb) {
    _completeCb = cb;
  }
};

extern MeshFragment Fragment;
