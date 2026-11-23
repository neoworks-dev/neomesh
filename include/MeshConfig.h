// include/MeshConfig.h
#pragma once

// Verbose [DBG] logging across mesh modules — election candidate tables,
// RSSI/loss samples, gateway connect attempts, etc.
//
// Enable via platformio.ini:
//   build_flags =
//       -D MESH_DEBUG=1
#ifndef MESH_DEBUG
#define MESH_DEBUG 0
#endif
