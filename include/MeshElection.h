// include/MeshElection.h
#pragma once
#include "IMeshSignal.h"
#include "MeshPacket.h"
#include <functional>
#include <stdint.h>

#define ELECTION_MAX_CANDIDATES 8
#define ELECTION_BROADCAST_MS   5000
#define ELECTION_TIMEOUT_MS     25000  // ~5x broadcast interval — slack against missed broadcasts
#define ELECTION_HYSTERESIS     5      // min score margin to hand off between two live candidates
#define ELECTION_EMA_ALPHA      0.3f   // weight of each new RSSI/loss sample; lower = smoother

struct Candidate {
  NodeID   id;
  int16_t  score;
  int8_t   rssiToRouter;
  uint8_t  lossRate;
  NodeID   voteFor;   // who this node last said the gateway is (0 = no opinion)
  uint32_t lastSeen;
  bool     active;
};

class MeshElection {
  NodeID       _self;
  IMeshSignal *_signal = nullptr;
  bool         _isCandidate = false;
  int8_t       _rssiToRouter = INT8_MIN; // EMA-smoothed RSSI used by _calcScore
  uint8_t      _lossRate = 0;            // EMA-smoothed loss used by _calcScore
  NodeID       _currentGateway = 0;

  // EMA accumulators behind the smoothed values above
  float        _rssiEma  = 0;
  float        _lossEma  = 0;
  bool         _haveRssi = false;

  Candidate _candidates[ELECTION_MAX_CANDIDATES];
  std::function<void(NodeID)> _onGatewayCb;

  void       _broadcast();
  int16_t    _calcScore() const;
  void       _electBest();
  bool       _shouldHandOff(NodeID winner, int16_t winnerScore);
  bool       _incumbentClaimsRole();
  bool       _candidateClaimsRole(NodeID id);
  int16_t    _incumbentScore();
  Candidate *_findCandidate(NodeID id);
  Candidate *_freeSlot();

public:
  void init(NodeID self, IMeshSignal *signal);
  void loop();

  void receive(const MeshPacket &pkt, const uint8_t *body);

  void setCandidate(bool v) { _isCandidate = v; }
  void updateRssi(int8_t rssi);      // EMA-smoothed to damp transient spikes
  void updateLossRate(uint8_t rate); // EMA-smoothed to damp transient spikes

  void onGatewayChange(std::function<void(NodeID)> cb) { _onGatewayCb = cb; }

  NodeID currentGateway() const  { return _currentGateway; }
  bool   isSelfGateway() const   { return _isCandidate && _currentGateway == _self; }
};

extern MeshElection Election;
