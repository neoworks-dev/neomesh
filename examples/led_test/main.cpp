// examples/led_test/main.cpp
//
// Bare FastLED test, no mesh. Lights the first 5 LEDs white on boot.
// Use this to verify wiring/power before debugging the mesh firmware.

#include <Arduino.h>
#include <FastLED.h>

#define LED_PIN 4
#define NUM_LEDS 5

CRGB leds[NUM_LEDS];

void setup() {
  Serial.begin(115200);

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(64);
  FastLED.clear();
  fill_solid(leds, NUM_LEDS, CRGB::White);
  FastLED.show();

  Serial.println("[LED_TEST] first 5 LEDs set to white");
}

void loop() {}
