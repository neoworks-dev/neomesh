#include <Arduino.h>
#include <Mesh.h>

#define LED_PIN 8 // built-in LED on ESP32-C3 supermini

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);

  Mesh.subscribe("lights/#", [](const char *topic, const uint8_t *payload,
                                uint16_t len) {
    String msg((const char *)payload, len);
    Serial.printf("[APP] %s → %s\n", topic, msg.c_str());

    if (msg == "on")
      digitalWrite(LED_PIN, HIGH);
    if (msg == "off")
      digitalWrite(LED_PIN, LOW);

    // send a response back
    String response = "ack from node " + String((uint32_t)Mesh.nodeId(), HEX);
    Mesh.publish("response/led", (const uint8_t *)response.c_str(),
                 response.length());
  });

  Mesh.begin();
  Serial.println("[APP] node ready");
}

void loop() { Mesh.loop(); }
