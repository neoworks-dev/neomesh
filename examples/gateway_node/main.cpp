#include "WiFi.h"
#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Mesh.h>
#include <TcpBridge.h>

#include "secrets.h"

TcpBridge bridge;
AsyncWebServer server(80);

void setup() {
  Serial.begin(115200);
  delay(5000);

  // connect WiFi first, before Mesh.begin()
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED)
    delay(100);
  Serial.println("[APP] WiFi connected");

  Mesh.configureGateway(WIFI_SSID, WIFI_PASS, &bridge);
  Mesh.begin();

  // subscribe so we can see responses from other nodes
  Mesh.subscribe("response/#",
                 [](const char *topic, const uint8_t *payload, uint16_t len) {
                   Serial.printf("[MESH] %s → %.*s\n", topic, len, payload);
                 });

  // POST /pub?topic=lights/all&msg=on
  server.on("/pub", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!req->hasParam("topic") || !req->hasParam("msg")) {
      req->send(400, "text/plain", "missing topic or msg");
      return;
    }
    String topic = req->getParam("topic")->value();
    String msg = req->getParam("msg")->value();

    Mesh.publish(topic.c_str(), msg.c_str());

    Serial.printf("[WEB] published topic=%s msg=%s\n", topic.c_str(),
                  msg.c_str());
    req->send(200, "text/plain", "ok");
  });

  // GET /status
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    String body = "{";
    body += "\"nodeId\":\"" + String((uint32_t)Mesh.nodeId(), HEX) + "\",";
    body += "\"gateway\":" + String(Mesh.isGateway() ? "true" : "false");
    body += "}";
    req->send(200, "application/json", body);
  });

  server.begin();
  Serial.printf("[WEB] server up at http://%s\n",
                WiFi.localIP().toString().c_str());
}

void loop() { Mesh.loop(); }
