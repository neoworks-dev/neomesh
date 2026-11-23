// include/MeshRouter.h
#pragma once
#include "IMeshSignal.h"
#include "MeshDedup.h"
#include "MeshPacket.h"
#include "MeshPubSub.h"
#include <functional>
#include <stdint.h>

#define ROUTER_MAX_ROUTES 32
#define ROUTER_ROUTE_TTL  30000  // ms before a route expires

struct RouteEntry {
  NodeID   dst;
  NodeID   nextHop;
  uint8_t  hopCount;
  uint32_t lastSeen;
  bool     active;
};

class MeshRouter {
  NodeID       _self;
  IMeshSignal *_espNow = nullptr;  // always present — small + fragmented packets
  IMeshSignal *_wifi   = nullptr;  // optional — large direct-peer packets

  MeshDedup  _dedup;
  MeshPubSub _pubsub;

  RouteEntry _routes[ROUTER_MAX_ROUTES];
  bool       _isGateway = false;

  std::function<void(const MeshPacket &, const uint8_t *)> _upstreamCb;

  void        _handleData(const MeshPacket &pkt, const uint8_t *body,
                          bool fromUpstream);
  void        _forward(const MeshPacket &pkt, const uint8_t *body);
  RouteEntry *_findRoute(NodeID dst);
  void        _updateRoute(NodeID dst, NodeID nextHop, uint8_t hopCount);
  void        _expireRoutes();

public:
  void init(NodeID self, IMeshSignal *espNow, IMeshSignal *wifi = nullptr);

  // called by signal layer when bytes arrive from a neighbor.
  // prevHop is the direct neighbor that delivered the frame (0 if unknown).
  void receive(const MeshPacket &pkt, const uint8_t *body, bool fromUpstream,
               NodeID prevHop);

  // send a pubsub message
  void publish(NodeID dst, const char *topic, const uint8_t *payload,
               uint16_t len);

  int  subscribe(const char *topic, MeshCallback cb) { return _pubsub.subscribe(topic, cb); }
  void unsubscribe(int handle)                        { _pubsub.unsubscribe(handle); }

  void onUpstream(std::function<void(const MeshPacket &, const uint8_t *)> cb) {
    _upstreamCb = cb;
  }

  void setGateway(bool gw) { _isGateway = gw; }
  bool isGateway()   const { return _isGateway; }
  NodeID selfId()    const { return _self; }

  void loop();
};

extern MeshRouter Router;
