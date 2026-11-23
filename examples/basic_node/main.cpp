#include <Arduino.h>
#include <Mesh.h>

void setup() {
  Serial.begin(115200);
  // ESP32-C3 native USB-CDC: wait briefly for the host monitor to attach so the
  // first boot logs aren't dropped (otherwise the board must be reset once the
  // monitor is open). Bounded so a board with no monitor still runs.
  for (uint32_t start = millis(); !Serial && millis() - start < 4000;)
    delay(10);
  delay(150);

  // optional — makes this node a gateway candidate
  // whichever candidate has best RSSI+reliability becomes the active gateway
  Mesh.setWifi("MyNetwork", "MyPassword");

  // optional — notified when elected gateway changes
  Mesh.onElection([](NodeID winner) {
    Mesh.log("[APP] gateway elected: %016llX (me=%s)\n",
             winner, Mesh.isGateway() ? "yes" : "no");
  });

  Mesh.subscribe("sensors/#",
                 [](const char *topic, const uint8_t *payload, uint16_t len) {
                   Mesh.log("[APP] %s → %.*s  t=%lld µs\n",
                            topic, len, payload, Mesh.syncedTime());
                 });

  Mesh.begin();
}

void loop() {
  Mesh.loop();

  static uint32_t last = 0;
  if (millis() - last > 5000) {
    const char *msg = "22.4";
    Mesh.publish("sensors/temp", (const uint8_t *)msg, strlen(msg));
    last = millis();
  }
}
