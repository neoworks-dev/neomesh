
// include/IBridge.h
#pragma once
#include "MeshPacket.h"
#include <functional>
#include <stdint.h>

using BridgeReceiveCb =
    std::function<void(const MeshPacket &pkt, const uint8_t *body)>;

class IBridge {
public:
  virtual ~IBridge() = default;

  virtual void begin() = 0;
  virtual void loop() = 0;
  virtual void send(const MeshPacket &pkt, const uint8_t *body) = 0;
  virtual bool connected() = 0;

  void onReceive(BridgeReceiveCb cb) { _receiveCb = cb; }

protected:
  BridgeReceiveCb _receiveCb = nullptr;
};
