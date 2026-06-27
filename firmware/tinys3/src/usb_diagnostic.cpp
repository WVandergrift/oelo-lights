// SPDX-License-Identifier: MIT
#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("TinyS3 USB diagnostic ready");
}

void loop() {
  static uint32_t lastHeartbeat = 0;
  const uint32_t now = millis();
  if (now - lastHeartbeat >= 1000) {
    lastHeartbeat = now;
    Serial.printf("heartbeat %lu ms\n", static_cast<unsigned long>(now));
  }
  delay(10);
}
