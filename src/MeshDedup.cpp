#include "MeshDedup.h"

bool MeshDedup::seen(uint32_t msgId) {
  // scan existing entries
  uint8_t count = _full ? DEDUP_RING_SIZE : _head;
  for (uint8_t i = 0; i < count; i++) {
    if (_ring[i] == msgId)
      return true;
  }

  // not seen — store it
  _ring[_head] = msgId;
  _head = (_head + 1) & (DEDUP_RING_SIZE - 1);
  if (_head == 0)
    _full = true;

  return false;
}
