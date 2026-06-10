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
