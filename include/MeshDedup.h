#pragma once
#include <stdint.h>

#define DEDUP_RING_SIZE 64 // must be power of 2

class MeshDedup {
  uint32_t _ring[DEDUP_RING_SIZE];
  uint8_t _head = 0;
  bool _full = false;

public:
  // returns true if we've seen this msgId before
  bool seen(uint32_t msgId);
};
