// Main controller firmware for the ESP32-S3.
// This node orchestrates the whole multi-device network, serving the WebUI,
// processing motion commands, and keeping the C5 nodes in sync.

#include <Arduino.h>
#include "SharedProtocol.h"

void setup() {
    Serial.begin(115200);
    delay(100);  // Brief boot delay is allowed during initialization.
    Serial.println("[s3_main] SlopDrive-32 main controller booted.");
}

void loop() {
    // Main controller runtime loop: non-blocking networking and orchestration.
    // Real-time motion generation runs on Core 1; this loop stays on Core 0.
    delay(1);  // Placeholder; will be replaced with event-driven logic.
}
