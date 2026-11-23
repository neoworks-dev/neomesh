// include/MeshLog.h
#pragma once
#include "MeshConfig.h"
#include "MeshPacket.h"
#include <stdarg.h>
#include <stdint.h>

#define MESH_LOG_MAX_MSG 200

// Local serial logging. Lines are printed to the device's own USB serial only.
// (Earlier revisions also broadcast each line to ESP-NOW neighbors so it would
// show up on whichever node's monitor was open — removed now that the test
// harness connects to every board's serial port directly.)
class MeshLog {
  NodeID _self = 0;

public:
  void init(NodeID self);

  // formats and prints the line with a [sec.mmm] timestamp to Serial
  void logf(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
  void vlogf(const char *fmt, va_list args);
};

extern MeshLog Log;

// always-on, timestamped logging
#define MESH_LOGF(...) Log.logf(__VA_ARGS__)

// verbose diagnostics — compiled out unless MESH_DEBUG=1
#if MESH_DEBUG
#define MESH_DBGF(...) Log.logf(__VA_ARGS__)
#else
#define MESH_DBGF(...) \
  do {                 \
  } while (0)
#endif
