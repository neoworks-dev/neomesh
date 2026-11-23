// include/MeshTopology.h
#pragma once
#include "IMeshSignal.h"
#include "MeshPacket.h"
#include <stdint.h>

#define TOPO_MAX_NODES       50
#define TOPO_MAX_NEIGHBORS   20
#define TOPO_UPDATE_MS       10000  // broadcast own neighbors every 10s
#define TOPO_NODE_TIMEOUT_MS 30000

struct TopoNode {
  NodeID   id;
  NodeID   neighbors[TOPO_MAX_NEIGHBORS];
  uint8_t  neighborCount;
  uint32_t lastUpdate;
  bool     active;
};

class MeshTopology {
  NodeID       _self;
  IMeshSignal *_signal = nullptr;
  bool         _isGateway = false;
  TopoNode     _nodes[TOPO_MAX_NODES];

  void     _updateNode(NodeID id, const NodeID *neighbors, uint8_t count);
  TopoNode *_findNode(NodeID id);
  TopoNode *_freeSlot();
  void     _expireNodes();
  void     _broadcastSelf();

public:
  void init(NodeID self, IMeshSignal *signal);
  void loop();

  void receive(const MeshPacket &pkt, const uint8_t *body);
  void setGateway(bool gw) { _isGateway = gw; }

  // update this node's own neighbor list (call when ESP-NOW peer list changes)
  void updateSelf(const NodeID *neighbors, uint8_t count);

  // read back topology (gateway only — other nodes have partial view)
  const TopoNode *nodes()    const { return _nodes; }
  int             nodeCount() const;
};

extern MeshTopology Topology;
