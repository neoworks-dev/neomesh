// include/Mesh.h
#pragma once
#include "EspNowSignal.h"
#include "IBridge.h"
#include "MeshElection.h"
#include "MeshFragment.h"
#include "MeshGateway.h"
#include "MeshLog.h"
#include "MeshPacket.h"
#include "MeshRouter.h"
#include "MeshSync.h"
#include "MeshTopology.h"
#include "WifiSignal.h"
#include <functional>
#include <stdarg.h>
#include <stdint.h>

class MeshClass {
  bool _started      = false;
  bool _isCandidate  = false;
  std::function<void(NodeID)> _electionCb;

public:
  // ── setup ────────────────────────────────────────────────────────────────

  // set WiFi credentials — makes this node a gateway candidate
  // call before begin(); any node with credentials participates in election
  void setWifi(const char *ssid, const char *pass) {
    Gateway.setWifi(ssid, pass);
    Election.setCandidate(true);
    _isCandidate = true;
  }

  // attach an external app bridge (optional, gateway only)
  void setBridge(IBridge *bridge) { Gateway.setBridge(bridge); }

  // starts the mesh — derives NodeID from MAC
  void begin();

  // ── loop ─────────────────────────────────────────────────────────────────

  void loop();

  // ── pub / sub ────────────────────────────────────────────────────────────

  void publish(const char *topic, const uint8_t *payload, uint16_t len) {
    Router.publish(BROADCAST_ID, topic, payload, len);
  }
  void publish(NodeID dst, const char *topic, const uint8_t *payload,
               uint16_t len) {
    Router.publish(dst, topic, payload, len);
  }
  void publish(const char *topic, const char *msg) {
    Router.publish(BROADCAST_ID, topic, (const uint8_t *)msg,
                   (uint16_t)strlen(msg));
  }

  int  subscribe(const char *topic, MeshCallback cb) {
    return Router.subscribe(topic, cb);
  }
  void unsubscribe(int handle) { Router.unsubscribe(handle); }

  // ── election ─────────────────────────────────────────────────────────────

  // fired whenever the elected gateway changes
  void onElection(std::function<void(NodeID winner)> cb) { _electionCb = cb; }
  NodeID gatewayId() const { return Election.currentGateway(); }
  bool   isGateway() const { return Election.isSelfGateway(); }

  // ── time sync ────────────────────────────────────────────────────────────

  // microseconds since boot, offset-corrected to gateway clock (<1ms for direct peers)
  int64_t syncedTime() const { return Sync.syncedTime(); }
  bool    timeSynced() const { return Sync.synced(); }

  // ── info ─────────────────────────────────────────────────────────────────

  NodeID nodeId() const { return Router.selfId(); }

  // ── logging ──────────────────────────────────────────────────────────────

  // timestamped, also forwarded to direct neighbors so it shows up on
  // whichever node's serial monitor you have open
  void log(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
    va_list args;
    va_start(args, fmt);
    Log.vlogf(fmt, args);
    va_end(args);
  }
};

extern MeshClass Mesh;
