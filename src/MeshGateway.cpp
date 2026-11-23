// src/MeshGateway.cpp
#include "MeshGateway.h"
#include "MeshLog.h"
#include "MeshRouter.h"
#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>

MeshGateway Gateway;

#define GW_CONNECT_TIMEOUT_MS 12000 // declare an attempt failed after this
#define GW_RETRY_MS           4000  // gap between connect attempts

// ─── config ──────────────────────────────────────────────────────────────────

void MeshGateway::setWifi(const char *ssid, const char *pass) {
  _ssid = ssid;
  _pass = pass;
}

// ─── init ────────────────────────────────────────────────────────────────────

void MeshGateway::init() {
  // nothing to do here — activation happens via election
}

// ─── activate / deactivate ───────────────────────────────────────────────────

void MeshGateway::activate() {
  _wantGateway = true;
  if (!_ssid) {
    MESH_LOGF("[GW] no WiFi credentials — cannot activate\n");
    return;
  }
  // The actual connect + service start happen in loop() so we never block the
  // main loop (which would stall the ESP-NOW RX drain and drop mesh packets).
}

void MeshGateway::deactivate() {
  _wantGateway = false;
  if (!_active) return;
  _active = false;
  Router.setGateway(false);
  MDNS.end();
  MESH_LOGF("[GW] deactivated\n");
}

// ─── loop ────────────────────────────────────────────────────────────────────

void MeshGateway::loop() {
  _maintainWifi();

  // finish activation once WiFi is up
  if (_wantGateway && !_active && WiFi.status() == WL_CONNECTED)
    _finishActivate();

  // gateway lost its uplink — drop services but keep _wantGateway so we
  // resume automatically once WiFi reconnects
  if (_active && WiFi.status() != WL_CONNECTED) {
    MESH_LOGF("[GW] WiFi lost — services down until reconnect\n");
    _active = false;
    Router.setGateway(false);
    MDNS.end();
  }

  if (_active && _bridge) _bridge->loop();
}

// ─── non-blocking WiFi ───────────────────────────────────────────────────────

// Any node with credentials keeps a WiFi connection so it has an RSSI score and
// sits on the AP's channel. This runs the connect as a state machine — it must
// never busy-wait, or loop() stalls and the RX queue overflows.
void MeshGateway::_maintainWifi() {
  if (!_ssid) return;

  if (WiFi.status() == WL_CONNECTED) {
    _rssi = (int8_t)WiFi.RSSI();
    if (_connecting) {
      _connecting = false;
      MESH_LOGF("[GW] WiFi connected: %s\n",
                WiFi.localIP().toString().c_str());
    }
    return;
  }

  if (_connecting) {
    if (millis() - _connectStart > GW_CONNECT_TIMEOUT_MS) {
      _connecting = false;  // timed out; retry after the backoff below
      MESH_DBGF("[GW] WiFi connect attempt timed out\n");
    }
    return;
  }

  if (millis() - _lastAttempt > GW_RETRY_MS)
    _beginConnect();
}

void MeshGateway::_beginConnect() {
  MESH_LOGF("[GW] connecting to \"%s\" ...\n", _ssid);
  WiFi.begin(_ssid, _pass);
  _connecting   = true;
  _connectStart = millis();
  _lastAttempt  = millis();
}

void MeshGateway::_finishActivate() {
  if (_bridge) {
    _bridge->onReceive([](const MeshPacket &pkt, const uint8_t *body) {
      char topic[MESH_MAX_TOPIC];
      meshTopicStr(body, pkt.topicLen, topic, sizeof(topic));
      const uint8_t *payload = meshPayload(body, pkt.topicLen);
      Router.publish(pkt.dst, topic, payload, meshPayloadLen(pkt));
    });
    Router.onUpstream([this](const MeshPacket &pkt, const uint8_t *body) {
      _onMeshPacket(pkt, body);
    });
    _bridge->begin();
  }

  Router.setGateway(true);
  _registerMdns();
  _active = true;
  _rssi   = (int8_t)WiFi.RSSI();

  MESH_LOGF("[GW] =========================================\n");
  MESH_LOGF("[GW] GATEWAY ACTIVE\n");
  MESH_LOGF("[GW] IP:   %s\n", WiFi.localIP().toString().c_str());
  MESH_LOGF("[GW] WiFi: %s  rssi=%d dBm\n", _ssid, _rssi);
  MESH_LOGF("[GW] =========================================\n");
}

// ─── mDNS ────────────────────────────────────────────────────────────────────

void MeshGateway::_registerMdns() {
  if (!MDNS.begin("neomesh")) {
    MESH_LOGF("[GW] mDNS start failed\n");
    return;
  }
  MDNS.addService("_neoworks", "_tcp", MESH_BRIDGE_PORT);

  char nodeIdStr[17];
  snprintf(nodeIdStr, sizeof(nodeIdStr), "%016llX", Router.selfId());
  MDNS.addServiceTxt("_neoworks", "_tcp", "node_id", String(nodeIdStr));

  MESH_LOGF("[GW] mDNS: neomesh._neoworks._tcp registered\n");
}

// ─── bridge forward ──────────────────────────────────────────────────────────

void MeshGateway::_onMeshPacket(const MeshPacket &pkt, const uint8_t *body) {
  if (!_bridge || !_bridge->connected()) return;
  _bridge->send(pkt, body);
}
