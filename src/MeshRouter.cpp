// src/MeshRouter.cpp
#include "MeshRouter.h"
#include "MeshElection.h"
#include "MeshFragment.h"
#include "MeshLog.h"
#include "MeshSync.h"
#include "MeshTopology.h"
#include <Arduino.h>
#include <string.h>

MeshRouter Router;

void MeshRouter::init(NodeID self, IMeshSignal *espNow, IMeshSignal *wifi) {
  _self   = self;
  _espNow = espNow;
  _wifi   = wifi;
  memset(_routes, 0, sizeof(_routes));
}

// ─── receive ─────────────────────────────────────────────────────────────────

void MeshRouter::receive(const MeshPacket &pkt, const uint8_t *body,
                         bool fromUpstream, NodeID prevHop) {
  if (pkt.version != MESH_VERSION) return;
  if (pkt.ttl == 0) return;

  switch (pkt.type) {

    case MESH_TYPE_DATA:
      if (_dedup.seen(pkt.msgId)) return;
      _updateRoute(pkt.src, prevHop, 1);
      _handleData(pkt, body, fromUpstream);
      break;

    case MESH_TYPE_FRAG:
      _updateRoute(pkt.src, prevHop, 1);
      if (pkt.dst == _self) {
        Fragment.receive(pkt, body);
      } else {
        _forward(pkt, body);
      }
      break;

    case MESH_TYPE_ELECTION:
      if (_dedup.seen(pkt.msgId)) return;
      Election.receive(pkt, body);
      _forward(pkt, body);
      break;

    case MESH_TYPE_SYNC_REQ:
    case MESH_TYPE_SYNC_RESP:
      Sync.receive(pkt, body);
      // sync packets are unicast; forward if not for us
      if (pkt.dst != _self) _forward(pkt, body);
      break;

    case MESH_TYPE_ROUTE_UPDATE:
      if (_dedup.seen(pkt.msgId)) return;
      Topology.receive(pkt, body);
      _forward(pkt, body);
      break;

    default:
      break;
  }
}

// ─── publish ─────────────────────────────────────────────────────────────────

void MeshRouter::publish(NodeID dst, const char *topic,
                         const uint8_t *payload, uint16_t payloadLen) {
  MESH_DBGF("[PUBSUB] publish topic=\"%s\" dst=%016llX len=%u\n", topic, dst,
            payloadLen);

  uint8_t topicLen = (uint8_t)strnlen(topic, MESH_MAX_TOPIC);

  MeshPacket pkt{};
  pkt.version    = MESH_VERSION;
  pkt.type       = MESH_TYPE_DATA;
  pkt.topicLen   = topicLen;
  pkt.payloadLen = topicLen + payloadLen;
  pkt.msgId      = esp_random();
  pkt.seq        = 0;
  pkt.src        = _self;
  pkt.dst        = dst;
  pkt.ttl        = MESH_MAX_TTL;
  pkt.flags      = (dst == BROADCAST_ID) ? MESH_FLAG_BROADCAST : 0;

  uint8_t body[MESH_MAX_TOPIC + MESH_MAX_PAYLOAD];
  memcpy(body, topic, topicLen);
  memcpy(body + topicLen, payload, payloadLen);

  // record our own msgId so a copy looping back through a neighbor is dropped
  // by dedup instead of being re-processed
  _dedup.seen(pkt.msgId);

  // local subscribers don't get a copy via the radio (broadcast/unicast-to-self
  // never loops back), so dispatch directly here too
  if (dst == _self || dst == BROADCAST_ID)
    _pubsub.dispatch(topic, payload, payloadLen);

  _forward(pkt, body);
}

// ─── data delivery ───────────────────────────────────────────────────────────

void MeshRouter::_handleData(const MeshPacket &pkt, const uint8_t *body,
                              bool fromUpstream) {
  bool forUs = (pkt.dst == _self || pkt.dst == BROADCAST_ID);

  if (forUs) {
    if (_isGateway && _upstreamCb) {
      _upstreamCb(pkt, body);
    } else {
      char topic[MESH_MAX_TOPIC];
      meshTopicStr(body, pkt.topicLen, topic, sizeof(topic));
      _pubsub.dispatch(topic, meshPayload(body, pkt.topicLen),
                       meshPayloadLen(pkt));
    }
  }

  bool shouldFwd = (pkt.dst == BROADCAST_ID || !forUs);
  if (shouldFwd && !(pkt.flags & MESH_FLAG_NO_FWD))
    _forward(pkt, body);
}

// ─── forward with transport selection ────────────────────────────────────────

void MeshRouter::_forward(const MeshPacket &pkt, const uint8_t *body) {
  if (pkt.dst == _self) return;  // local delivery already handled by caller

  MeshPacket fwd = pkt;
  fwd.ttl--;
  if (fwd.ttl == 0) return;

  // resolve next hop
  NodeID nextHop;
  if (fwd.dst == BROADCAST_ID) {
    nextHop = BROADCAST_ID;
  } else {
    RouteEntry *route = _findRoute(fwd.dst);
    nextHop = route ? route->nextHop : BROADCAST_ID;
  }

  uint16_t frameBytes = sizeof(MeshPacket) + fwd.payloadLen;

  if (frameBytes <= 250) {
    // fits in ESP-NOW directly
    if (nextHop == BROADCAST_ID)
      _espNow->broadcast(fwd, body);
    else
      _espNow->sendTo(nextHop, fwd, body);
    return;
  }

  // oversized — try WiFi first if nextHop is a direct WiFi peer
  if (_wifi && nextHop != BROADCAST_ID && _wifi->hasPeer(nextHop)) {
    _wifi->sendTo(nextHop, fwd, body);
    return;
  }

  // fragment into ESP-NOW chunks
  NodeID fragNextHop = (nextHop == BROADCAST_ID) ? BROADCAST_ID : nextHop;
  // for broadcast fragmentation send to each peer individually via loop above;
  // for unicast, Fragment.send handles chunking to fragNextHop
  if (fragNextHop == BROADCAST_ID) {
    // large broadcasts are not supported (would saturate all peers)
    MESH_LOGF("[ROUTER] large broadcast dropped — use unicast for streams\n");
    return;
  }
  Fragment.send(fragNextHop, fwd, body);
}

// ─── routing table ───────────────────────────────────────────────────────────

RouteEntry *MeshRouter::_findRoute(NodeID dst) {
  for (int i = 0; i < ROUTER_MAX_ROUTES; i++)
    if (_routes[i].active && _routes[i].dst == dst)
      return &_routes[i];
  return nullptr;
}

void MeshRouter::_updateRoute(NodeID dst, NodeID nextHop, uint8_t hopCount) {
  // nextHop 0 means the delivering neighbor is unknown (e.g. a reassembled
  // fragment) — don't install a route we can't trust to point the right way
  if (nextHop == 0) return;

  for (int i = 0; i < ROUTER_MAX_ROUTES; i++) {
    if (_routes[i].active && _routes[i].dst == dst) {
      if (hopCount <= _routes[i].hopCount) {
        _routes[i].nextHop   = nextHop;
        _routes[i].hopCount  = hopCount;
      }
      _routes[i].lastSeen = millis();
      return;
    }
  }

  for (int i = 0; i < ROUTER_MAX_ROUTES; i++) {
    if (!_routes[i].active) {
      _routes[i] = {dst, nextHop, hopCount, millis(), true};
      return;
    }
  }

  // evict oldest
  uint32_t oldest = millis();
  int evict = 0;
  for (int i = 0; i < ROUTER_MAX_ROUTES; i++) {
    if (_routes[i].lastSeen < oldest) {
      oldest = _routes[i].lastSeen;
      evict  = i;
    }
  }
  _routes[evict] = {dst, nextHop, hopCount, millis(), true};
}

void MeshRouter::_expireRoutes() {
  uint32_t now = millis();
  for (int i = 0; i < ROUTER_MAX_ROUTES; i++)
    if (_routes[i].active && (now - _routes[i].lastSeen) > ROUTER_ROUTE_TTL)
      _routes[i].active = false;
}

void MeshRouter::loop() { _expireRoutes(); }
