// MeshPubSub.cpp
#include "MeshPubSub.h"
#include "MeshLog.h"
#include <string.h>

int MeshPubSub::subscribe(const char *topic, MeshCallback cb) {
  for (int i = 0; i < PUBSUB_MAX_SUBS; i++) {
    if (!_subs[i].active) {
      strncpy(_subs[i].topic, topic, MESH_MAX_TOPIC - 1);
      _subs[i].topic[MESH_MAX_TOPIC - 1] = '\0';
      _subs[i].cb = cb;
      _subs[i].active = true;
      return i;
    }
  }
  return -1; // no space
}

void MeshPubSub::unsubscribe(int handle) {
  if (handle >= 0 && handle < PUBSUB_MAX_SUBS)
    _subs[handle].active = false;
}

void MeshPubSub::dispatch(const char *topic, const uint8_t *payload,
                          uint16_t len) {
  for (int i = 0; i < PUBSUB_MAX_SUBS; i++) {
    if (_subs[i].active && _match(_subs[i].topic, topic)) {
      MESH_DBGF("[PUBSUB] dispatch topic=\"%s\" -> sub[%d]=\"%s\" len=%u\n",
                topic, i, _subs[i].topic, len);
      _subs[i].cb(topic, payload, len);
    }
  }
}

// ─── topic matching ──────────────────────────────────────────────────────────
//
// supports two wildcards (same as MQTT):
//   +  matches exactly one level    e.g. "mesh/+/mode" matches
//   "mesh/strip1/mode" #  matches everything below     e.g. "mesh/led/#"
//   matches "mesh/led/strip1/mode" #  is only valid at the end

bool MeshPubSub::_match(const char *pattern, const char *topic) {
  const char *p = pattern;
  const char *t = topic;

  while (*p && *t) {
    if (*p == '#')
      return true; // matches everything remaining

    if (*p == '+') {
      // skip one level in topic
      while (*t && *t != '/')
        t++;
      p++;
    } else {
      if (*p != *t)
        return false;
      p++;
      t++;
    }
  }

  // handle trailing # in pattern
  if (*p == '#')
    return true;

  return *p == '\0' && *t == '\0';
}
