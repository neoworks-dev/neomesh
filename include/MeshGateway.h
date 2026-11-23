// include/MeshGateway.h
#pragma once
#include "IBridge.h"
#include "MeshPacket.h"
#include <stdint.h>

class MeshGateway {
  const char *_ssid        = nullptr;
  const char *_pass        = nullptr;
  bool        _active      = false; // gateway services (bridge/mDNS) running
  bool        _wantGateway = false; // elected — activate once WiFi is up
  int8_t      _rssi        = 0;
  IBridge    *_bridge      = nullptr;

  // non-blocking WiFi connect state — never busy-wait in loop(), or the RX
  // queue drain stalls and queued mesh packets (election/data) get dropped
  bool        _connecting   = false;
  uint32_t    _connectStart = 0;
  uint32_t    _lastAttempt  = 0;

  void _onMeshPacket(const MeshPacket &pkt, const uint8_t *body);
  void _maintainWifi();
  void _beginConnect();
  void _finishActivate();
  void _registerMdns();

public:
  // set WiFi credentials — any node with credentials is a gateway candidate
  void setWifi(const char *ssid, const char *pass);

  // optionally attach a bridge to forward packets to an external app
  void setBridge(IBridge *bridge) { _bridge = bridge; }

  // called by election when this node wins / loses gateway role
  void activate();
  void deactivate();

  void init();
  void loop();

  bool   active() const { return _active; }
  int8_t rssi()   const { return _rssi; }
};

extern MeshGateway Gateway;
