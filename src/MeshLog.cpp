// src/MeshLog.cpp
#include "MeshLog.h"
#include <Arduino.h>

MeshLog Log;

void MeshLog::init(NodeID self) { _self = self; }

void MeshLog::logf(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vlogf(fmt, args);
  va_end(args);
}

void MeshLog::vlogf(const char *fmt, va_list args) {
  char msg[MESH_LOG_MAX_MSG];
  int len = vsnprintf(msg, sizeof(msg), fmt, args);
  if (len < 0) return;
  if (len >= (int)sizeof(msg)) len = sizeof(msg) - 1;

  uint32_t ms = millis();
  Serial.printf("[%5lu.%03lu] %.*s", ms / 1000, ms % 1000, len, msg);
}
