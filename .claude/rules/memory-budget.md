---
paths:
  - "src/**"
  - "include/**"
  - "platformio.ini"
---

# Memory discipline (pointers; DOCTRINE §9/§11 and TRAPS T1/T2/T21/T27-T30 own the story)

## Pools, never per-event malloc

- Steady-state comms allocate nothing: PacingRing is a fixed
  std::array<PacingEntry, 64> (include/comms/SlopSyncHubService.h:91-143);
  UART RX ring is FrameBuffer[64] per slot sized for burst tolerance, cost
  landing in PSRAM (include/comms/SlopSyncUartTransport.h:143-165);
  cross-core rings in SystemState are fixed-capacity SPSC with a
  never-blocks-never-allocates producer contract.
- Big scratch lives in members, not stack locals: TX scratch as members
  because ~1 KB of stack deep inside hub.update() is not free (T1 class,
  SlopSyncUartTransport.h:202-206); ledger/preset blobs as members of the
  PSRAM-resident service (SlopSyncHubService.h:526-542).

## PSRAM vs internal RAM

- SlopSyncHubService is placement-new'd into PSRAM from main.cpp. Never move
  it back to BSS (DOCTRINE §9, TRAPS T2: a ~100 KB static once left ~13 KB
  free heap and killed the network stack). Measured: service 240,976 B in
  PSRAM; internal-RAM cost ~101 KB (docs/c5-comms-offload.md §4.1).
- CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP redirects WiFi/lwIP bulk to PSRAM;
  exonerated as a corruption cause by A/B but load-bearing for headroom
  (int_free 41,967 vs 34,039). CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
  is the guard against internal-only allocations aborting; that abort was
  the literal 2.1.99 core dump (platformio.ini:354-389).
- PSRAM is unreachable while flash cache is disabled (OTA/flash writes):
  nothing ISR-context may touch the PSRAM-resident service
  (SlopSyncUartTransport.h:47-52).

## Budgeting rules proven by blood

- Budget any diagnostic's RAM against heap_min under load, never idle free:
  a 9 KB .bss diagnostic table flipped an A/B arm from 4-reboots-in-6 to
  0-in-7 and separately pushed int_min_free to 88 bytes (T28 item 4, T30
  item 2). A detector reports, never adjudicates: a detector's own abort()
  once bricked the OTA recovery path (T30 item 5).
- Oversized allocations cost their size plus the hole they leave: shrinking
  a 16 KB stack to 8 KB returned 11,196 B (T21). But never trim a stack
  whose comment records a canary blowout (T1).
- Heap-corruption hunt state: THE QUEUE item 0, parked by operator ruling at
  fw 2.3.20. All first-party suspects eliminated; signature survives. Read
  the LEDGER before touching teardown paths.

## "32D conformance floor": does not exist

"32D" in this workspace means the ESP32-WROOM-32D, a PARKED motion-MCU port
idea (no PSRAM; would force a hub-placement rework). The protocol's real
floor is min_transport_payload = 242 B (SPEC §13.1). Never cite a "32D RAM
budget"; there is none.
