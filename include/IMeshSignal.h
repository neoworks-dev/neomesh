// IMeshSignal.h
#pragma once
#include "MeshPacket.h"
#include <stdint.h>

// callback type — signal layer calls this when bytes arrive from a neighbor.
// prevHop is the direct neighbor the frame came from (NOT pkt.src, which is the
// originator and may be several hops away); 0 if unknown.
using SignalReceiveCb = void (*)(const MeshPacket &pkt, const uint8_t *body,
                                 bool fromUpstream, NodeID prevHop);

class IMeshSignal {
public:
  virtual ~IMeshSignal() = default;

  virtual void init(NodeID self) = 0;
  virtual void loop() = 0;
  virtual void sendTo(NodeID dst, const MeshPacket &pkt, const uint8_t *body) = 0;
  virtual void broadcast(const MeshPacket &pkt, const uint8_t *body) = 0;

  // returns true if dst is a currently connected direct peer
  virtual bool hasPeer(NodeID dst) { return false; }

  void onReceive(SignalReceiveCb cb) { _receiveCb = cb; }

protected:
  SignalReceiveCb _receiveCb = nullptr;
};
