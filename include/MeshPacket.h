#pragma once
#include <stdint.h>
#include <string.h>

// ─── constants ───────────────────────────────────────────────────────────────

#define MESH_VERSION     1
#define MESH_PORT        7777            // mesh peer-to-peer TCP (WifiSignal)
#define MESH_BRIDGE_PORT (MESH_PORT + 1) // external app bridge (TcpBridge), advertised via mDNS
#define MESH_MAX_TTL     8
#define MESH_MAX_TOPIC   64
#define MESH_MAX_PAYLOAD 4096
#define ESPNOW_MAX_BODY  221   // 250 - sizeof(MeshPacket)

typedef uint64_t NodeID;
static constexpr NodeID BROADCAST_ID = 0xFFFFFFFFFFFFFFFF;

// ─── packet types ────────────────────────────────────────────────────────────

#define MESH_TYPE_DATA         0x01  // pubsub; body = topic + payload
#define MESH_TYPE_BEACON       0x02  // neighbor discovery (signal layer only)
#define MESH_TYPE_ROUTE_UPDATE 0x03  // topology broadcast
#define MESH_TYPE_SYNC_REQ     0x04  // PTP sync request
#define MESH_TYPE_SYNC_RESP    0x05  // PTP sync response
#define MESH_TYPE_ELECTION     0x06  // gateway election score
#define MESH_TYPE_FRAG         0x07  // fragmented data chunk
#define MESH_TYPE_LOG          0x08  // remote log line (signal layer only)

// ─── flags ───────────────────────────────────────────────────────────────────

#define MESH_FLAG_BROADCAST 0x01
#define MESH_FLAG_RETAIN    0x02
#define MESH_FLAG_ACK_REQ   0x04
#define MESH_FLAG_IS_ACK    0x08
#define MESH_FLAG_NO_FWD    0x10

// ─── packet header ───────────────────────────────────────────────────────────

#pragma pack(push, 1)
struct MeshPacket {
  uint8_t  version;
  uint8_t  type;        // MESH_TYPE_*
  uint16_t payloadLen;  // bytes following header
  uint32_t msgId;       // random, for dedup
  uint16_t seq;
  NodeID   src;
  NodeID   dst;
  uint8_t  ttl;
  uint8_t  flags;
  uint8_t  topicLen;    // only valid for MESH_TYPE_DATA
};
#pragma pack(pop)

// ─── control packet bodies ───────────────────────────────────────────────────

#pragma pack(push, 1)

struct BeaconBody {
  uint8_t  channel;
  int8_t   rssiToRouter; // INT8_MIN if not connected to AP
  uint8_t  flags;        // BEACON_FLAG_*
  uint32_t wifiIp;       // own IP if BEACON_FLAG_WIFI_READY, else 0
};
#define BEACON_FLAG_CANDIDATE  0x01  // has WiFi credentials
#define BEACON_FLAG_WIFI_READY 0x02  // currently connected to AP

struct ElectionBody {
  int16_t score;
  int8_t  rssiToRouter;
  uint8_t lossRate;  // 0-255 maps to 0-100%
  NodeID  voteFor;   // current gateway opinion (0 = no opinion)
};

struct SyncReqBody {
  int64_t t1; // requester send time (µs)
};

struct SyncRespBody {
  int64_t t1; // echoed from request
  int64_t t2; // master recv time (µs)
  int64_t t3; // master send time (µs)
};

struct FragBody {
  uint32_t streamId;
  uint32_t totalLen;  // total reassembled body size
  uint16_t fragIndex;
  uint16_t fragCount;
  uint8_t  origType;  // MESH_TYPE_* of original packet
  // fragment data follows immediately
};
#define FRAG_HEADER_SIZE ((int)sizeof(FragBody))
#define FRAG_MAX_DATA    (ESPNOW_MAX_BODY - FRAG_HEADER_SIZE)

#pragma pack(pop)

// ─── helpers ─────────────────────────────────────────────────────────────────

inline const char    *meshTopic(const uint8_t *body) { return (const char *)body; }
inline const uint8_t *meshPayload(const uint8_t *body, uint8_t topicLen) { return body + topicLen; }
inline uint16_t       meshPayloadLen(const MeshPacket &p) { return p.payloadLen - p.topicLen; }

// meshTopic(body) points into the raw wire buffer (topic bytes immediately
// followed by payload bytes) and is NOT null-terminated — copy it out before
// treating it as a C string (e.g. for pubsub matching).
inline void meshTopicStr(const uint8_t *body, uint8_t topicLen, char *out, size_t outSize) {
  uint8_t n = topicLen < outSize - 1 ? topicLen : (uint8_t)(outSize - 1);
  memcpy(out, body, n);
  out[n] = '\0';
}

inline void nodeIdToMac(NodeID id, uint8_t *mac) {
  for (int i = 5; i >= 0; i--) { mac[i] = id & 0xFF; id >>= 8; }
}

// inverse of nodeIdToMac — matches the NodeID derivation in MeshClass::begin()
inline NodeID macToNodeId(const uint8_t *mac) {
  NodeID id = 0;
  for (int i = 0; i < 6; i++) id = (id << 8) | mac[i];
  return id;
}
