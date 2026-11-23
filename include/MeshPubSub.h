#pragma once
#include "MeshPacket.h"
#include <functional>
#include <stdint.h>

#define PUBSUB_MAX_SUBS 16

using MeshCallback = std::function<void(const char *topic,
                                        const uint8_t *payload, uint16_t len)>;

struct Subscription {
  char topic[MESH_MAX_TOPIC];
  MeshCallback cb;
  bool active = false;
};

class MeshPubSub {
  Subscription _subs[PUBSUB_MAX_SUBS];

  static bool _match(const char *pattern, const char *topic);

public:
  // returns subscription handle (-1 on failure)
  int subscribe(const char *topic, MeshCallback cb);
  void unsubscribe(int handle);

  // called by router when a packet is delivered locally
  void dispatch(const char *topic, const uint8_t *payload, uint16_t len);
};
