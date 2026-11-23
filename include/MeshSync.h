// include/MeshSync.h
#pragma once
#include "IMeshSignal.h"
#include "MeshPacket.h"
#include <stdint.h>

#define SYNC_INTERVAL_MS  2000   // request sync every 2 seconds
#define SYNC_MAX_RTT_US   10000  // discard measurements above 10ms RTT

class MeshSync {
  NodeID       _self;
  IMeshSignal *_signal = nullptr;
  NodeID       _master = 0;
  int64_t      _offset = 0;  // µs correction applied to local clock
  bool         _synced = false;

  void _sendSyncReq();
  void _handleReq(const MeshPacket &pkt, const uint8_t *body);
  void _handleResp(const MeshPacket &pkt, const uint8_t *body);

public:
  void init(NodeID self, IMeshSignal *signal);
  void loop();

  void receive(const MeshPacket &pkt, const uint8_t *body);
  void setMaster(NodeID master) { _master = master; }

  // microseconds since boot, offset-corrected to master clock
  int64_t syncedTime() const;
  bool    synced()     const { return _synced; }
};

extern MeshSync Sync;
