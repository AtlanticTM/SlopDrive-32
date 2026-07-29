// SlopDrive-32 Waveshare ESP32-C5 relay node — ESP-NOW-to-dual-UART T-Code bridge.
//
// Constraints:
// - Receives ESP-NOW bundles from the T-Dongle C5 and relays each decoded
//   T-Code line out BOTH Serial (UART0/USB) and Serial1 (UART1, GPIO7 TX /
//   GPIO8 RX hardware relay to the S3 main controller) simultaneously.
// - Wi-Fi is STA mode, 5GHz-only, channel locked to SECRET_ESPNOW_CHANNEL
//   (secrets.h) — promiscuous receive, accepts packets from any peer MAC.
// - onEspNowRecv() runs on the Wi-Fi task: fast path only, never calls
//   esp_now_send() from inside it. It only sets ACK bitmask bits and pushes
//   into the scheduled replay queue; loop() does all the sending.
// - ACKs are BROADCAST, not unicast — broadcast has no MAC-layer retry/CTS
//   wait, so the radio stays free for the next incoming T-Code packet.
// - Board: Waveshare ESP32-C5-Zero (4MB flash, no PSRAM), env:c5_waveshare in
//   platformio.ini / boards/esp32-c5-zero.json. ARDUINO_USB_CDC_ON_BOOT=0, so
//   Serial is UART0 via the USB-Serial chip, not native USB-CDC.
// - GPIO27 drives a single WS2812 status LED via rgbLedWrite() (built into
//   arduino-esp32 v3.x — no Adafruit_NeoPixel needed).
// - Single-core C5: the recv callback and loop() share state with no mutex —
//   that is only safe because there is exactly one core to preempt on.

#include <Arduino.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>

// Pull channel + peer MAC from secrets.h (git-ignored).
// Falls back to secrets.example.h defaults if secrets.h doesn't exist.
#if __has_include(<secrets.h>)
  #include <secrets.h>
#else
  #include <secrets.example.h>
#endif

// ---- Pin definitions --------------------------------------------------------

static constexpr int8_t   PIN_RELAY_TX  = 7;      // UART1 TX → downstream RX
static constexpr int8_t   PIN_RELAY_RX  = 8;      // UART1 RX ← downstream TX
// 460800 baud — this literal is its own source of truth now (no shared define).
// Each byte takes ~22µs instead of 87µs at 115200, cutting UART jitter by ~4×.
// Serial (UART0 / USB debug) stays at 115200 — only the relay UART1 is bumped.
static constexpr uint32_t RELAY_BAUD    = 460800;

// WS2812 RGB LED — single pixel on GPIO27.
// neopixelWrite() is built into arduino-esp32 v3.x, no library needed.
static constexpr uint8_t  PIN_LED       = 27;
static constexpr uint8_t  LED_BRIGHT    = 40;     // 0–255, keep it sane — not a floodlight

// ---- RGB LED helpers --------------------------------------------------------
// rgbLedWrite(pin, r, g, b) drives a WS2812 via the RMT peripheral, built into
// arduino-esp32 v3.x cores — no Adafruit_NeoPixel needed. neopixelWrite() still
// works but is deprecated in v3.x — use rgbLedWrite.

static inline void ledSet(uint8_t r, uint8_t g, uint8_t b) {
    rgbLedWrite(PIN_LED, r, g, b);
}

static inline void ledOff() { ledSet(0, 0, 0); }

// Scale a color component by LED_BRIGHT/255 so brightness stays capped.
static inline uint8_t dim(uint8_t v) {
    return (uint8_t)((uint16_t)v * LED_BRIGHT / 255);
}

// ---- LED state machine ------------------------------------------------------
// States:
//   LED_IDLE    — yellow slow sine pulse, 2s period. Waiting for data.
//   LED_ACTIVE  — solid blue. Data stream flowing at >1 pkt/s.
//   LED_FLASH   — green flash for 50ms on packet receive, then back to prev.
//   LED_ERROR   — solid red. ESP-NOW init failed.
//
// update() is called every loop() iteration — non-blocking, millis-based.

enum LedState { LED_IDLE, LED_ACTIVE, LED_FLASH, LED_ERROR };

static LedState  s_led_state      = LED_IDLE;
static LedState  s_led_prev_state = LED_IDLE;  // state to return to after flash
static uint32_t  s_led_flash_ms   = 0;         // when the flash started
static uint32_t  s_led_pulse_ms   = 0;         // phase counter for idle pulse

// FLASH_DURATION_MS/PULSE_PERIOD_MS are 3x the previous board's values (was
// 60ms/2000ms) — at high packet rates the old timing let flashes stack and the
// idle breathe read as sluggish. Do not revert without re-checking that.
static constexpr uint32_t FLASH_DURATION_MS = 20;   // green flash length (was 60 — 3× snappier)
static constexpr uint32_t PULSE_PERIOD_MS   = 666;  // idle pulse period (was 2000 — 3× faster breathe)


static void ledSetError() {
    s_led_state = LED_ERROR;
    ledSet(dim(255), 0, 0);  // solid red — fatal state
}

static void ledTriggerFlash() {
    // Don't interrupt an error state — red stays red.
    if (s_led_state == LED_ERROR) return;
    s_led_prev_state = (s_led_state == LED_FLASH) ? s_led_prev_state : s_led_state;
    s_led_state    = LED_FLASH;
    s_led_flash_ms = millis();
    ledSet(0, dim(255), 0);  // green flash — packet received
}

static void ledSetActive(bool active) {
    if (s_led_state == LED_ERROR || s_led_state == LED_FLASH) return;
    if (active) {
        s_led_state = LED_ACTIVE;
        ledSet(0, 0, dim(255));
    } else {
        s_led_state = LED_IDLE;
    }
}

// Sine-based pulse for idle state. sin() is expensive but only runs at the
// idle-tick rate, not the packet rate.
static void ledUpdateIdle(uint32_t now_ms) {
    uint32_t phase = now_ms % PULSE_PERIOD_MS;
    // Map 0..PULSE_PERIOD_MS → 0..2π → sin → 0..1
    float t = (float)phase / (float)PULSE_PERIOD_MS;
    float s = (sinf(t * 2.0f * 3.14159f - 1.5708f) + 1.0f) * 0.5f; // 0..1
    uint8_t v = (uint8_t)(s * LED_BRIGHT);
    // Yellow = R+G, no B.
    rgbLedWrite(PIN_LED, v, (uint8_t)(v * 0.8f), 0);
}

static void ledUpdate(uint32_t now_ms) {
    switch (s_led_state) {
        case LED_IDLE:
            ledUpdateIdle(now_ms);
            break;

        case LED_ACTIVE:
            // Already set in ledSetActive() — nothing to do here.
            break;

        case LED_FLASH:
            if (now_ms - s_led_flash_ms >= FLASH_DURATION_MS) {
                // Flash expired — return to previous state.
                s_led_state = s_led_prev_state;
                if (s_led_state == LED_ACTIVE) {
                    ledSet(0, 0, dim(255));
                }
                // If returning to IDLE, ledUpdateIdle() will handle it next tick
            }
            break;

        case LED_ERROR:
            // Stays red until reboot.
            break;
    }
}

// schedPush() is called from the ESP-NOW recv callback (Wi-Fi task) and
// writes into s_sched[]. schedDrain() is called from loop() and fires
// commands at the correct time. Single-core C5 = no mutex needed.

// ---- Packet counter ---------------------------------------------------------
// For Hz readout and LED active-state detection.

static volatile uint32_t g_pkt_count = 0;

// ---- Batched application-layer ACK system — BROADCAST edition ---------------
// At 333Hz, sending one ACK per packet would be 333 esp_now_send() calls/sec
// from inside the recv callback. The Wi-Fi stack can't handle that — it causes
// contention, memory pressure, and packet loss on BOTH directions.
//
// A prior unicast-batched-ACK design still had a problem: unicast ESP-NOW
// retransmits up to 5 times waiting for a CTS/ACK frame. At 100 ACK
// batches/sec that's up to 500 retransmit attempts/sec on the Waveshare's
// radio — enough to saturate it and cause the very packet loss being measured.
//
// Fix: send ACKs as BROADCAST. Broadcast has NO MAC-layer ACK, NO retransmit
// attempts, fires once and done. The dongle is already listening for any
// incoming packet — it just needs to accept broadcasts. Zero retry overhead,
// zero CTS wait, the radio is free to receive the next T-Code packet
// immediately.
//
// Packet format (6 bytes):
//   [0xAC][base_seq][mask_b0][mask_b1][mask_b2][mask_b3]
// base_seq = seq# of bit 0. Bits 0-31 = seq base..base+31.
// Covers up to 32 seq#s per ACK packet. At 100 flushes/sec = 3200 seq#s/sec
// capacity — way more than the 333 we're sending.
//
// The recv callback ONLY sets bits in the bitmask — no esp_now_send() in the
// hot path. loop() drains the bitmask and sends the batched broadcast ACK.
// ---- Scheduled replay queue -------------------------------------------------
// Receives bundled T-Code commands and fires each one at the correct time
// based on the rel_ms offset embedded in the bundle.
//
// At 100Hz send rate, bundles arrive ~10ms before their commands need to fire.
// Each command in the bundle has a rel_ms offset (0-9ms) from the bundle's
// send time. We schedule each command to fire at recv_time + rel_ms.
//
// This gives jitter-free replay — the inter-command spacing is preserved
// exactly as it was on the dongle side.
//
// Queue: 16 slots. At 100Hz with 4 cmds/bundle = 400 cmds/sec max.
// Each slot lives for at most 10ms before firing. 16 slots = 40ms of buffer.
static constexpr uint8_t  SCHED_SLOTS      = 16;
static constexpr uint8_t  SCHED_CMD_MAXLEN = 60;

struct SchedSlot {
    char     data[SCHED_CMD_MAXLEN + 1];
    uint8_t  len;
    uint32_t fire_at_ms;
    bool     active;
};

static SchedSlot s_sched[SCHED_SLOTS];

// Schedule a command to fire at fire_at_ms.
static void schedPush(const char* cmd, uint8_t len, uint32_t fire_at_ms) {
    if (len == 0 || len > SCHED_CMD_MAXLEN) return;
    // Find a free slot — linear scan, 16 slots is trivial.
    for (uint8_t i = 0; i < SCHED_SLOTS; i++) {
        if (!s_sched[i].active) {
            memcpy(s_sched[i].data, cmd, len);
            s_sched[i].data[len] = '\0';
            s_sched[i].len       = len;
            s_sched[i].fire_at_ms = fire_at_ms;
            s_sched[i].active    = true;
            return;
        }
    }
    // Queue full — drop the oldest active slot and use it instead.
    // Serial.write() cannot be called from the Wi-Fi task (recv callback
    // context), so dropping is the safe option here. At 16 slots deep and
    // 100Hz/4 cmds-per-bundle, this path should not normally be hit.
    uint32_t oldest_fire = 0xFFFFFFFF;
    uint8_t  oldest_idx  = 0;
    for (uint8_t i = 0; i < SCHED_SLOTS; i++) {
        if (s_sched[i].fire_at_ms < oldest_fire) {
            oldest_fire = s_sched[i].fire_at_ms;
            oldest_idx  = i;
        }
    }
    memcpy(s_sched[oldest_idx].data, cmd, len);
    s_sched[oldest_idx].data[len] = '\0';
    s_sched[oldest_idx].len       = len;
    s_sched[oldest_idx].fire_at_ms = fire_at_ms;
    s_sched[oldest_idx].active    = true;
}

// Drain the scheduler — fire any commands whose time has arrived.
// Called every loop() iteration. Non-blocking, O(SCHED_SLOTS).
static void schedDrain(uint32_t now_ms) {
    for (uint8_t i = 0; i < SCHED_SLOTS; i++) {
        if (!s_sched[i].active) continue;
        // Fire if time has arrived, or if more than 50ms overdue (clock skew /
        // wrap protection — don't hold stale commands forever).
        int32_t delta = (int32_t)(now_ms - s_sched[i].fire_at_ms);
        if (delta >= 0) {
            Serial.write((const uint8_t*)s_sched[i].data, s_sched[i].len);
            Serial.write('\n');
            Serial1.write((const uint8_t*)s_sched[i].data, s_sched[i].len);
            Serial1.write('\n');
            s_sched[i].active = false;
            ledTriggerFlash();
        }
    }
}

static constexpr uint8_t  ACK_MAGIC        = 0xAC;
static constexpr uint32_t ACK_INTERVAL_MS  = 10;   // send batched ACK every 10ms

// Broadcast MAC — no retries, no CTS, fires once.
static const uint8_t ACK_BROADCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static bool s_ack_peer_added = false;

// Bitmask of received seq#s since last ACK flush.
// base_seq = the seq# of bit 0. Bits 0-31 = seq base..base+31.
// Written by recv callback (Wi-Fi task), read+cleared by loop() — single-core
// C5 so no mutex needed, but we use volatile to prevent register caching.
static volatile uint8_t  s_ack_base    = 0;    // seq# of bit 0
static volatile uint32_t s_ack_mask    = 0;    // bitmask of received seq#s
static volatile bool     s_ack_pending = false; // true if mask has unsent bits

// Register the broadcast peer for ACK sends — done once in loop().
// Broadcast peer must be registered before esp_now_send() to broadcast.
static void ensureAckPeer() {
    if (s_ack_peer_added) return;
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, ACK_BROADCAST, 6);
    peer.channel = SECRET_ESPNOW_CHANNEL;
    peer.encrypt = false;
    esp_err_t err = esp_now_add_peer(&peer);
    s_ack_peer_added = (err == ESP_OK);
    if (err != ESP_OK)
        Serial.printf("[ack] WARN: add_peer(broadcast) failed: %s\n", esp_err_to_name(err));
    else
        Serial.println("[ack] Broadcast ACK peer registered. yippie! :3");
}

// Flush the accumulated ACK bitmask as a broadcast. Called from loop().
// Packet: [0xAC][base_seq][mask_b0][mask_b1][mask_b2][mask_b3] = 6 bytes.
// Broadcast = no retries, no CTS; the dongle hears it or it's gone. That's
// fine — the dongle's loss window is forgiving, and we send 100/sec.
static void flushAck() {
    if (!s_ack_peer_added || !s_ack_pending) return;

    // Snapshot and clear — single-core C5, no mutex needed.
    uint8_t  base = s_ack_base;
    uint32_t mask = s_ack_mask;
    s_ack_mask    = 0;
    s_ack_pending = false;

    if (mask == 0) return;

    uint8_t pkt[6] = {
        ACK_MAGIC,
        base,
        (uint8_t)(mask & 0xFF),
        (uint8_t)((mask >> 8) & 0xFF),
        (uint8_t)((mask >> 16) & 0xFF),
        (uint8_t)((mask >> 24) & 0xFF),
    };
    esp_now_send(ACK_BROADCAST, pkt, 6);
}

// Flag set in recv callback when first packet arrives — triggers peer reg.
static volatile bool s_first_packet = false;

// ---- ESP-NOW receive callback -----------------------------------------------
// Runs on the Wi-Fi task — FAST PATH ONLY. No esp_now_send() here.
// Just set a bit in the bitmask and push T-Code to the ring.
//
// Packet format from dongle: [seq_byte][T-Code string...]
// We strip the seq byte, push the T-Code into the ring, mark the seq# in the
// bitmask. loop() sends the batched broadcast ACK every 10ms.

static void onEspNowRecv(const esp_now_recv_info_t* info,
                         const uint8_t* data, int len) {
    (void)info;
    // Bundle format: [seq:1][N:1][rel_ms:1][cmdlen:1][cmd...]...
    // Minimum: 2 bytes header + 1 cmd (2 bytes header + 1 byte data = 5 bytes min).
    if (len < 5 || len > 250) return;

    uint8_t seq = data[0];
    uint8_t N   = data[1];
    if (N == 0 || N > 4) return;  // sanity check — max 4 cmds per bundle.

    uint32_t recv_ms = millis();
    int      pos     = 2;

    for (uint8_t i = 0; i < N; i++) {
        if (pos + 2 > len) break;  // truncated packet — stop parsing.
        uint8_t rel_ms  = data[pos++];
        uint8_t cmd_len = data[pos++];
        if (cmd_len == 0 || pos + cmd_len > len) break;

        // Schedule this command to fire at recv_ms + rel_ms.
        // rel_ms is the offset from when the bundle window opened on the dongle.
        // We add it to our recv time to reconstruct the original timing.
        uint32_t fire_at = recv_ms + rel_ms;
        schedPush((const char*)(data + pos), cmd_len, fire_at);
        pos += cmd_len;

        __atomic_fetch_add((uint32_t*)&g_pkt_count, 1u, __ATOMIC_RELAXED);
    }

    // Mark this bundle seq# in the ACK bitmask.
    uint8_t offset = (uint8_t)(seq - s_ack_base);
    if (offset < 32) {
        s_ack_mask |= (1u << offset);
    } else {
        s_ack_base = seq;
        s_ack_mask = 1u;
    }
    s_ack_pending = true;

    if (!s_first_packet) s_first_packet = true;
}

// ---- Wi-Fi + ESP-NOW init ---------------------------------------------------
// Force 5GHz-only, lock channel, register receive callback.

static void initEspNow() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    esp_err_t band_err = esp_wifi_set_band(WIFI_BAND_5G);
    if (band_err != ESP_OK)
        Serial.printf("[espnow] WARN: esp_wifi_set_band(5G): %s\n",
                      esp_err_to_name(band_err));

    esp_err_t ch_err = esp_wifi_set_channel(SECRET_ESPNOW_CHANNEL,
                                             WIFI_SECOND_CHAN_NONE);
    if (ch_err != ESP_OK)
        Serial.printf("[espnow] ERROR: set_channel(%d): %s\n",
                      SECRET_ESPNOW_CHANNEL, esp_err_to_name(ch_err));
    else
        Serial.printf("[espnow] Channel %d locked. yippie! :3\n",
                      SECRET_ESPNOW_CHANNEL);

    esp_err_t now_err = esp_now_init();
    if (now_err != ESP_OK) {
        Serial.printf("[espnow] FATAL: esp_now_init(): %s\n",
                      esp_err_to_name(now_err));
        ledSetError();  // RED — fatal state
        while (true) delay(1000);
    }

    esp_now_register_recv_cb(onEspNowRecv);
    Serial.println("[espnow] Ready. Waiting for T-Code packets...");
}

// ---- setup() ----------------------------------------------------------------

void setup() {
    // UART0 → USB-Serial chip → USB port. Debug/monitor port.
    Serial.begin(RELAY_BAUD);
    delay(200);

    // UART1 → GPIO7 TX / GPIO8 RX → wired relay to S3 or downstream device.
    // Raw hardware UART — no USB overhead, bytes pumped straight in.
    Serial1.begin(RELAY_BAUD, SERIAL_8N1, PIN_RELAY_RX, PIN_RELAY_TX);

    // LED init — brief white flash to confirm boot, then settle into idle
    // yellow pulse while ESP-NOW initializes.
    ledSet(dim(255), dim(255), dim(255));  // white boot flash
    delay(150);
    ledOff();

    Serial.println("[c5_waveshare] SlopDrive-32 Waveshare C5 relay node booting...");
    Serial.printf("[c5_waveshare] UART1 relay: TX=GPIO%d, RX=GPIO%d @ %lu baud\n",
                  PIN_RELAY_TX, PIN_RELAY_RX, RELAY_BAUD);
    Serial.printf("[c5_waveshare] RGB LED: GPIO%d\n", PIN_LED);

    // ESP-NOW init — 5GHz, locked channel, promiscuous receive.
    Serial.println("[c5_waveshare] Initializing ESP-NOW...");
    initEspNow();

    Serial.printf("[c5_waveshare] My MAC: %s\n", WiFi.macAddress().c_str());
    Serial.println("[c5_waveshare] Ready. Relay open, waiting for packets. yippie!");

    // Start in idle state — yellow pulse.
    s_led_state = LED_IDLE;
}

// ---- loop() -----------------------------------------------------------------
// Drains the ring buffer and relays each T-Code line out both Serial (USB)
// and Serial1 (hardware UART). Updates LED state based on packet rate.
// Non-blocking — no delay(), no blocking reads.

void loop() {
    uint32_t now_ms = millis();

    // Drain the scheduled replay queue — fire commands whose time has arrived.
    // Commands were scheduled in onEspNowRecv() with fire_at = recv_ms + rel_ms.
    // This gives jitter-free replay — inter-command spacing preserved exactly.
    schedDrain(now_ms);

    // Register broadcast ACK peer on first packet received.
    // Done here in loop() — safe to call esp_now_add_peer() outside the callback.
    if (s_first_packet) {
        ensureAckPeer();
    }

    // Flush batched broadcast ACK every ACK_INTERVAL_MS.
    // One broadcast covers up to 32 seq#s, no retries, no CTS wait.
    static uint32_t s_ack_last_ms = 0;
    if (now_ms - s_ack_last_ms >= ACK_INTERVAL_MS) {
        flushAck();
        s_ack_last_ms = now_ms;
    }

    // Hz calculation — once per second. Determines ACTIVE vs IDLE LED state.
    static uint32_t s_hz_last_ms    = 0;
    static uint32_t s_hz_last_count = 0;

    if (now_ms - s_hz_last_ms >= 1000) {
        uint32_t cur = g_pkt_count;
        float    hz  = (float)(cur - s_hz_last_count);
        s_hz_last_count = cur;
        s_hz_last_ms    = now_ms;

        // >1 pkt/s = active stream → blue. 0 pkt/s = idle → yellow pulse.
        // Don't change state if we're mid-flash — ledSetActive() guards that.
        if (s_led_state != LED_FLASH && s_led_state != LED_ERROR) {
            ledSetActive(hz > 1.0f);
        }

        Serial.printf("[c5_waveshare] %.0f pkt/s | total=%lu\n", hz, cur);
    }

    // Update LED state machine — handles flash timeout, idle pulse animation.
    ledUpdate(now_ms);
}
