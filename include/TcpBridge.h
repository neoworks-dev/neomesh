#pragma once
#include "IBridge.h"
#include <WiFiClient.h>
#include <WiFiServer.h>

class TcpBridge : public IBridge {
  WiFiServer _server{MESH_PORT + 1}; // separate port from mesh peer traffic
  WiFiClient _client;

  void _handleIncoming();

public:
  void begin() override;
  void loop() override;
  void send(const MeshPacket &pkt, const uint8_t *body) override;
  bool connected() override;
};
