// src/MeshTopology.cpp
#include "MeshTopology.h"
#include <Arduino.h>
#include <string.h>

MeshTopology Topology;

void MeshTopology::init(NodeID self, IMeshSignal *signal) {
  _self   = self;
  _signal = signal;
  memset(_nodes, 0, sizeof(_nodes));
}

// ─── loop ────────────────────────────────────────────────────────────────────

void MeshTopology::loop() {
  static uint32_t lastUpdate = 0;
  if (millis() - lastUpdate > TOPO_UPDATE_MS) {
    _broadcastSelf();
    lastUpdate = millis();
  }
  _expireNodes();
}

// ─── receive ─────────────────────────────────────────────────────────────────

void MeshTopology::receive(const MeshPacket &pkt, const uint8_t *body) {
  if (pkt.payloadLen < 1) return;

  // only gateway builds full topology; others skip foreign updates
  if (!_isGateway && pkt.src != _self) return;

  uint8_t count = body[0];
  if (count > TOPO_MAX_NEIGHBORS) count = TOPO_MAX_NEIGHBORS;

  if ((int)pkt.payloadLen < 1 + (int)(count * sizeof(NodeID))) return;

  NodeID neighbors[TOPO_MAX_NEIGHBORS];
  memcpy(neighbors, body + 1, count * sizeof(NodeID));
  _updateNode(pkt.src, neighbors, count);
}

// ─── update self ─────────────────────────────────────────────────────────────

void MeshTopology::updateSelf(const NodeID *neighbors, uint8_t count) {
  _updateNode(_self, neighbors, count);
}

// ─── broadcast ───────────────────────────────────────────────────────────────

void MeshTopology::_broadcastSelf() {
  TopoNode *self = _findNode(_self);
  if (!self || !self->active) return;

  uint8_t count = self->neighborCount;
  // body: [count(1)] + [NodeID × count]
  uint16_t bodyLen = 1 + (uint16_t)(count * sizeof(NodeID));
  uint8_t  body[1 + TOPO_MAX_NEIGHBORS * sizeof(NodeID)];
  body[0] = count;
  memcpy(body + 1, self->neighbors, count * sizeof(NodeID));

  MeshPacket pkt{};
  pkt.version    = MESH_VERSION;
  pkt.type       = MESH_TYPE_ROUTE_UPDATE;
  pkt.msgId      = esp_random();
  pkt.src        = _self;
  pkt.dst        = BROADCAST_ID;
  pkt.ttl        = MESH_MAX_TTL;
  pkt.payloadLen = bodyLen;
  pkt.flags      = MESH_FLAG_BROADCAST;

  _signal->broadcast(pkt, body);
}

// ─── node management ─────────────────────────────────────────────────────────

void MeshTopology::_updateNode(NodeID id, const NodeID *neighbors, uint8_t count) {
  TopoNode *node = _findNode(id);
  if (!node) {
    node = _freeSlot();
    if (!node) return;
    node->id     = id;
    node->active = true;
  }
  node->neighborCount = count > TOPO_MAX_NEIGHBORS ? TOPO_MAX_NEIGHBORS : count;
  memcpy(node->neighbors, neighbors, node->neighborCount * sizeof(NodeID));
  node->lastUpdate = millis();
}

TopoNode *MeshTopology::_findNode(NodeID id) {
  for (int i = 0; i < TOPO_MAX_NODES; i++)
    if (_nodes[i].active && _nodes[i].id == id)
      return &_nodes[i];
  return nullptr;
}

TopoNode *MeshTopology::_freeSlot() {
  for (int i = 0; i < TOPO_MAX_NODES; i++)
    if (!_nodes[i].active) return &_nodes[i];
  return nullptr;
}

void MeshTopology::_expireNodes() {
  uint32_t now = millis();
  for (int i = 0; i < TOPO_MAX_NODES; i++) {
    if (_nodes[i].active &&
        now - _nodes[i].lastUpdate > TOPO_NODE_TIMEOUT_MS)
      _nodes[i].active = false;
  }
}

int MeshTopology::nodeCount() const {
  int n = 0;
  for (int i = 0; i < TOPO_MAX_NODES; i++)
    if (_nodes[i].active) n++;
  return n;
}
