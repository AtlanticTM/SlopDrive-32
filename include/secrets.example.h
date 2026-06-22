#pragma once
//
// SECRETS TEMPLATE  ->  copy this file to "secrets.h" and fill in your values.
//
//   include/secrets.example.h   (committed to git - safe, no real secrets)
//   include/secrets.h           (git-IGNORED - your real WiFi/network values)
//
// secrets.h is listed in .gitignore so your password and local network
// addresses are never pushed to GitHub. config.h includes secrets.h if it
// exists; if it doesn't, config.h falls back to harmless placeholder defaults
// so the project still compiles for anyone who clones it.
//
// After cloning:  copy secrets.example.h -> secrets.h, edit, build.

// ---- WiFi ----
#define SECRET_WIFI_SSID       "YourWiFiName"
#define SECRET_WIFI_PASSWORD   "YourWiFiPassword"

// ---- Intiface (only used in WiFi/WSDM mode; ignored in serial-control mode) ----
// IP of the PC running Intiface, and the WSDM device-server port it prints.
#define SECRET_INTIFACE_HOST   "192.168.1.100"
#define SECRET_INTIFACE_PORT   54817

// ---- ESP-NOW 5GHz Channel Configuration ----
//
// The T-Dongle C5 and Waveshare C5 communicate via ESP-NOW on a 5GHz channel.
// The ESP32-C5 supports 5GHz Wi-Fi (802.11ax), which includes DFS channels
// (Dynamic Frequency Selection) — channels that require radar detection in
// some regulatory domains but are perfectly legal for short-range unlicensed
// use in many regions. Check your local regulations. :3
//
// Channel mapping (5GHz, 20MHz bandwidth):
//   Channel 36  = 5180 MHz  (UNII-1, universally safe, no DFS)
//   Channel 40  = 5200 MHz  (UNII-1, universally safe, no DFS)
//   Channel 44  = 5220 MHz  (UNII-1, universally safe, no DFS)
//   Channel 48  = 5240 MHz  (UNII-1, universally safe, no DFS)
//   Channel 52  = 5260 MHz  (UNII-2A, DFS required in most regions)
//   Channel 100 = 5500 MHz  (UNII-2C, DFS required in most regions)
//   Channel 149 = 5745 MHz  (UNII-3, legal without DFS in US/CA/AU)
//   Channel 153 = 5765 MHz  (UNII-3, legal without DFS in US/CA/AU)
//
// Set this to any valid 5GHz channel. Both nodes MUST use the same channel.
// The default (36) is the safest choice — UNII-1, no DFS, works everywhere.
#define SECRET_ESPNOW_CHANNEL  36

// MAC address of the peer node to unicast ESP-NOW packets to.
// Set this to the MAC address of the receiving ESP32-C5 (Waveshare node).
// Format: { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }
// Run `pio device monitor` on the receiver and read its MAC from boot log.
#define SECRET_ESPNOW_PEER_MAC { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }
