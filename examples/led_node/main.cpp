// examples/led_node/main.cpp
//
// Flash this same firmware to two boards. Both join the mesh and become
// gateway candidates; whichever has the better WiFi connection is elected
// gateway and registers "neomesh._neoworks._tcp" via mDNS.
//
// Only the board with WS2812B LEDs wired to GPIO2 will visibly react, but
// both subscribe to "led/color" — control either by NodeID (printed on
// boot) or broadcast to all.
//
// Wire format for "led/color" payload (4 bytes): r, g, b, brightness

#include <Arduino.h>
#include <FastLED.h>
#include <Mesh.h>
#include <TcpBridge.h>

#define LED_PIN                                                                \
  4 // NOTE: GPIO2 is a strapping pin on ESP32-C3 — leave
    // floating/HIGH at boot or LEDs may glitch on reset
#define NUM_LEDS 30

// Onboard status LED — lit on whichever board is the elected gateway, so it's
// obvious which one meshctl/meshtest talks to. ESP32-C3 SuperMini: GPIO8,
// active LOW. (Adjust for your board if the LED is elsewhere or active HIGH.)
#define STATUS_LED_PIN 8
#define STATUS_LED_ON  LOW
#define STATUS_LED_OFF HIGH

// edit before flashing
#define MESH_WIFI_SSID "Moritz"
#define MESH_WIFI_PASS "asdfgh01"

CRGB leds[NUM_LEDS];
TcpBridge bridge;

// The elected gateway blinks its onboard LED; every other node holds it off.
// A blink (vs a steady power LED) makes the gateway unmistakable regardless of
// the board's LED polarity.
bool gatewayActive = false;

struct ColorCmd {
  uint8_t r, g, b, brightness;
};

void setup() {
  Serial.begin(115200);
  // ESP32-C3 native USB-CDC: wait briefly for the host monitor to attach so the
  // first boot logs aren't dropped (otherwise the board must be reset once the
  // monitor is open). Bounded so a board with no monitor still runs.
  for (uint32_t start = millis(); !Serial && millis() - start < 4000;)
    delay(10);
  delay(150);

  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, STATUS_LED_OFF);

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(64);
  FastLED.clear();
  FastLED.show();

  // any node with credentials is a gateway candidate — best-connected wins
  Mesh.setWifi(MESH_WIFI_SSID, MESH_WIFI_PASS);

  // whichever node wins election exposes this bridge on MESH_BRIDGE_PORT,
  // advertised via mDNS — this is what meshctl talks to
  Mesh.setBridge(&bridge);

  Mesh.onElection([](NodeID winner) {
    gatewayActive = Mesh.isGateway();
    if (!gatewayActive)
      digitalWrite(STATUS_LED_PIN, STATUS_LED_OFF);
    Mesh.log("[APP] gateway elected: %016llX (me=%s)\n", winner,
             gatewayActive ? "yes" : "no");
  });

  Mesh.subscribe(
      "led/color", [](const char *topic, const uint8_t *payload, uint16_t len) {
        Mesh.log("[MSG] received topic=%s payload=%d bytes\n", topic, len);
        if (len < sizeof(ColorCmd))
          return;

        ColorCmd cmd;
        memcpy(&cmd, payload, sizeof(cmd));

        fill_solid(leds, NUM_LEDS, CRGB(cmd.r, cmd.g, cmd.b));
        FastLED.setBrightness(cmd.brightness);
        FastLED.show();

        Mesh.log("[LED] r=%u g=%u b=%u brightness=%u\n", cmd.r, cmd.g, cmd.b,
                 cmd.brightness);
      });

  Mesh.begin();

  Mesh.log("[APP] node id: %016llX\n", Mesh.nodeId());
}

void loop() {
  Mesh.loop();

  // blink the onboard LED while this node is the gateway
  static uint32_t lastBlink = 0;
  static bool ledOn = false;
  if (gatewayActive && millis() - lastBlink > 250) {
    ledOn = !ledOn;
    digitalWrite(STATUS_LED_PIN, ledOn ? STATUS_LED_ON : STATUS_LED_OFF);
    lastBlink = millis();
  }
}
