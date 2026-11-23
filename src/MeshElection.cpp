// src/MeshElection.cpp
#include "MeshElection.h"
#include "MeshLog.h"
#include <Arduino.h>
#include <math.h>
#include <string.h>

MeshElection Election;

void MeshElection::init(NodeID self, IMeshSignal *signal) {
  _self   = self;
  _signal = signal;
  memset(_candidates, 0, sizeof(_candidates));

  // broadcast our first score immediately instead of waiting for the first
  // ELECTION_BROADCAST_MS tick — otherwise both sides sit silent for up to
  // 5s before the gateway discussion even starts
  _broadcast();
}

// ─── loop ────────────────────────────────────────────────────────────────────

void MeshElection::loop() {
  static uint32_t lastBcast = millis();
  if (millis() - lastBcast > ELECTION_BROADCAST_MS) {
    _broadcast();
    lastBcast = millis();
  }

  uint32_t now = millis();
  for (int i = 0; i < ELECTION_MAX_CANDIDATES; i++) {
    if (!_candidates[i].active) continue;
    if (now - _candidates[i].lastSeen > ELECTION_TIMEOUT_MS) {
      MESH_LOGF("[ELECT] dropped %016llX — silent for >%lums\n",
                _candidates[i].id, (unsigned long)ELECTION_TIMEOUT_MS);
      NodeID expired = _candidates[i].id;
      _candidates[i].active = false;
      if (expired == _currentGateway)
        _electBest();
    }
  }
}

// ─── receive ─────────────────────────────────────────────────────────────────

void MeshElection::receive(const MeshPacket &pkt, const uint8_t *body) {
  if (pkt.payloadLen < sizeof(ElectionBody)) return;

  ElectionBody eb;
  memcpy(&eb, body, sizeof(eb));

  if (eb.score < 0) return;  // non-candidate, ignore

  Candidate *c = _findCandidate(pkt.src);
  if (!c) {
    c = _freeSlot();
    if (!c) return;
    c->id     = pkt.src;
    c->active = true;
  }
  c->score        = eb.score;
  c->rssiToRouter = eb.rssiToRouter;
  c->lossRate     = eb.lossRate;
  c->voteFor      = eb.voteFor;
  c->lastSeen     = millis();

  MESH_DBGF("[ELECT] heard %016llX advertise score=%d (rssi=%d loss=%u)\n",
            pkt.src, eb.score, eb.rssiToRouter, eb.lossRate);

  _electBest();
}

// ─── broadcast ───────────────────────────────────────────────────────────────

void MeshElection::_broadcast() {
  ElectionBody eb{};
  eb.score        = _isCandidate ? _calcScore() : -1;
  eb.rssiToRouter = _rssiToRouter;
  eb.lossRate     = _lossRate;
  eb.voteFor      = _currentGateway;

  MESH_DBGF("[ELECT] advertising self score=%d (rssi=%d loss=%u), voting gateway=%016llX\n",
            eb.score, eb.rssiToRouter, eb.lossRate, _currentGateway);

  MeshPacket pkt{};
  pkt.version    = MESH_VERSION;
  pkt.type       = MESH_TYPE_ELECTION;
  pkt.msgId      = esp_random();
  pkt.src        = _self;
  pkt.dst        = BROADCAST_ID;
  pkt.ttl        = MESH_MAX_TTL;
  pkt.payloadLen = sizeof(ElectionBody);
  pkt.flags      = MESH_FLAG_BROADCAST;

  _signal->broadcast(pkt, (const uint8_t *)&eb);
}

// ─── election ────────────────────────────────────────────────────────────────

int16_t MeshElection::_calcScore() const {
  if (!_isCandidate || _rssiToRouter == INT8_MIN) return -1;

  int rssiScore = (_rssiToRouter + 100);
  rssiScore = rssiScore < 0 ? 0 : (rssiScore > 100 ? 100 : rssiScore);

  int lossScore = 100 - (_lossRate * 100 / 255);

  return (int16_t)(rssiScore * 60 / 100 + lossScore * 40 / 100);
}

// Deterministic total order over candidates: higher score wins, NodeID breaks
// ties. Every node ranks the same way, so equal scores can't make two nodes
// each pick themselves.
static bool outranks(int16_t scoreA, NodeID idA, int16_t scoreB, NodeID idB) {
  if (scoreA != scoreB) return scoreA > scoreB;
  return idA > idB;
}

void MeshElection::_electBest() {
  NodeID  best      = 0;
  int16_t bestScore = -1;

  MESH_DBGF("[ELECT] choosing gateway, comparing candidates:\n");
  for (int i = 0; i < ELECTION_MAX_CANDIDATES; i++) {
    if (!_candidates[i].active) continue;
    MESH_DBGF("[ELECT]   peer %016llX score=%d (seen %lums ago)\n",
              _candidates[i].id, _candidates[i].score,
              (unsigned long)(millis() - _candidates[i].lastSeen));
    if (outranks(_candidates[i].score, _candidates[i].id, bestScore, best)) {
      bestScore = _candidates[i].score;
      best      = _candidates[i].id;
    }
  }

  if (_isCandidate) {
    int16_t myScore = _calcScore();
    MESH_DBGF("[ELECT]   self %016llX score=%d\n", _self, myScore);
    if (outranks(myScore, _self, bestScore, best)) {
      bestScore = myScore;
      best      = _self;
    }
  }

  if (best == 0 || bestScore < 0 || best == _currentGateway) return;

  if (!_shouldHandOff(best, bestScore)) return;

  bool selfWon = (best == _self);
  MESH_LOGF("[ELECT] elected gateway %016llX score=%d%s\n", best, bestScore,
            selfWon ? " (this node)" : "");
  _currentGateway = best;
  if (_onGatewayCb) _onGatewayCb(best);

  // tell neighbours our new opinion immediately instead of waiting for the
  // next periodic broadcast — speeds convergence after a change
  _broadcast();
}

// ─── helpers ─────────────────────────────────────────────────────────────────

Candidate *MeshElection::_findCandidate(NodeID id) {
  for (int i = 0; i < ELECTION_MAX_CANDIDATES; i++)
    if (_candidates[i].active && _candidates[i].id == id)
      return &_candidates[i];
  return nullptr;
}

Candidate *MeshElection::_freeSlot() {
  for (int i = 0; i < ELECTION_MAX_CANDIDATES; i++)
    if (!_candidates[i].active) return &_candidates[i];
  return nullptr;
}

// ─── hand-off policy ─────────────────────────────────────────────────────────

// Decide whether to replace the current gateway with the freshly computed
// winner. Hysteresis damps flapping when two nodes sit within a few score points
// of each other, but it must never strand the mesh with zero or two gateways —
// the voteFor opinions broadcast by each node let us tell those cases apart.
bool MeshElection::_shouldHandOff(NodeID winner, int16_t winnerScore) {
  if (_currentGateway == 0) return true; // no gateway yet — take the role

  // Incumbent has stepped down (it now votes for someone else, or vanished).
  // Fill the vacuum immediately so the mesh is never left gateway-less.
  if (!_incumbentClaimsRole()) return true;

  // Bootstrap race: we hold the role but a higher-ranked node also claims it.
  // Collapse to the deterministic winner at once rather than run two gateways.
  if (_currentGateway == _self && _candidateClaimsRole(winner)) return true;

  // Steady state: keep the incumbent unless the winner clears the margin. This
  // protects a self-gateway too, so RSSI noise can't make us abdicate to a peer
  // that is barely ahead and isn't even claiming the role.
  int16_t incumbentScore = _incumbentScore();
  if (incumbentScore >= 0 && winnerScore <= incumbentScore + ELECTION_HYSTERESIS)
    return false;
  return true;
}

bool MeshElection::_incumbentClaimsRole() {
  if (_currentGateway == _self) return true;
  Candidate *cur = _findCandidate(_currentGateway);
  if (!cur) return false;             // incumbent aged out entirely — vacuum
  if (cur->voteFor == 0) return true; // opinion unknown — assume it still holds
  return cur->voteFor == _currentGateway;
}

bool MeshElection::_candidateClaimsRole(NodeID id) {
  Candidate *c = _findCandidate(id);
  return c && c->voteFor == id;
}

int16_t MeshElection::_incumbentScore() {
  if (_currentGateway == _self) return _calcScore();
  Candidate *cur = _findCandidate(_currentGateway);
  if (!cur) return -1;
  return cur->score;
}

// ─── signal smoothing ────────────────────────────────────────────────────────

static float emaStep(float prev, float sample) {
  return ELECTION_EMA_ALPHA * sample + (1.0f - ELECTION_EMA_ALPHA) * prev;
}

void MeshElection::updateRssi(int8_t rssi) {
  // Seed on the first sample so _rssiToRouter leaves its INT8_MIN "no signal"
  // sentinel immediately instead of ramping up from it.
  if (!_haveRssi) {
    _rssiEma  = rssi;
    _haveRssi = true;
  } else {
    _rssiEma = emaStep(_rssiEma, rssi);
  }
  _rssiToRouter = (int8_t)lroundf(_rssiEma);
}

void MeshElection::updateLossRate(uint8_t rate) {
  _lossEma  = emaStep(_lossEma, rate);
  _lossRate = (uint8_t)lroundf(_lossEma);
}
