// SlopDrive-32 — T-Dongle C5 Transmitter Node
// Relays T-Code from the USB CDC serial link to the Waveshare C5 over
// ESP-NOW, with an on-device status display and RGB status LED.
// Constraints:
// - WiFi/ESP-NOW starts OFF at boot; only turns on when the host opens the
//   COM port (DTR asserted). Shuts down after 5 minutes with no open port.
// - Packet loss is tracked at the APPLICATION layer: broadcast ESP-NOW never
//   returns a MAC-layer ACK, so loss comes from a 1-byte seq# on every
//   outgoing packet plus a batched ACK bitmask echoed back by the Waveshare.
// - Display renders into a full 80x160 RAM framebuffer, flushed to the ST7735
//   in one DMA burst per frame — a partial/mid-scan write tears the screen.
// - Single-core target: state shared between callbacks and loop() is
//   volatile, no mutex.
// - Display layout (portrait 80x160, all 160px used):
//     Y 0-15:    Header — [WIFI] left, RDY/TX right
//     Y 16:      Divider
//     Y 17-136:  Content — left 0-37px position bar, X38 divider,
//                right 39-79px RATE/Hz, POS/mm, PKT/loss%
//     Y 137:     Divider
//     Y 138-159: Footer — "serial: CONN" / "RDY" / "WiFi:OFF"
// - Hz color coding: <50 red, <100 amber, <200 cyan, >=200 green.
// - APA102 LED (CI=4, DI=5): white = WiFi off/idle, cyan = actively
//   relaying, blue = ESP-NOW ready but no serial activity, red = error.
// - Hardware (LilyGO T-Dongle C5): ST7735 80x160 SPI display (CS=10, DC=3,
//   RST=1, MOSI=2, SCLK=6, BL=0), boot button GPIO28 (rotates display 180),
//   APA102 RGB LED (CI=4, DI=5).

#include <Arduino.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include "st7735.h"
#include "soc/usb_serial_jtag_struct.h"
#include "esp_system.h"
#include "esp_sleep.h"

// ---- Power-on self-reset ----------------------------------------------------
// Kills the DTR-triggered reboot on first enumeration. Windows asserts DTR
// when it opens the port, which resets the C5 hardware, blanks the display,
// and gives MFP a semaphore timeout. Self-resetting on power-on forces the
// DTR-triggered reset to happen during the first enumeration cycle, before
// MFP ever opens the port.
__attribute__((constructor)) static void disableUsbReset() {
    USB_SERIAL_JTAG.chip_rst.usb_uart_chip_rst_dis = 1;
    if (esp_reset_reason() == ESP_RST_POWERON) {
        esp_restart();
    }
}

#if __has_include(<secrets.h>)
  #include <secrets.h>
#else
  #include <secrets.example.h>
#endif

// ---- Pin definitions --------------------------------------------------------
static constexpr int8_t  PIN_TFT_CS   = 10;
static constexpr int8_t  PIN_TFT_DC   = 3;
static constexpr int8_t  PIN_TFT_RST  = 1;
static constexpr int8_t  PIN_TFT_SCLK = 6;
static constexpr int8_t  PIN_TFT_MOSI = 2;
static constexpr uint8_t PIN_TFT_BL   = 0;   // active LOW — 0 = backlight ON
static constexpr uint8_t PIN_BUTTON   = 28;  // boot button, active LOW
static constexpr uint8_t PIN_LED_CI   = 4;   // APA102 clock
static constexpr uint8_t PIN_LED_DI   = 5;   // APA102 data

// ---- Display geometry — portrait 80x160, all 160px used ---------------------
// HDR=16, DIV=1, CONTENT=120, DIV=1, FTR=22 -> total=160.
static constexpr int16_t DISP_W      = 80;
static constexpr int16_t DISP_H      = 160;

// Header zone: Y 0–15 (16px)
static constexpr int16_t HDR_H       = 16;
static constexpr int16_t HDR_DIV_Y   = 16;   // 1px divider line

// Content zone: Y 17–136 (120px)
static constexpr int16_t CONTENT_Y   = 17;
static constexpr int16_t CONTENT_H   = 120;

// Bar: left 37px of content zone
static constexpr int16_t BAR_X       = 0;
static constexpr int16_t BAR_W       = 37;
static constexpr int16_t BAR_Y       = CONTENT_Y;
static constexpr int16_t BAR_H       = CONTENT_H;

// Vertical divider between bar and stats: X=38 (1px)
static constexpr int16_t DIV_X       = 38;

// Stats: right side X 39–79 (41px wide)
static constexpr int16_t STATS_X     = 39;
static constexpr int16_t STATS_W     = DISP_W - STATS_X;  // 41px

// Footer divider + zone: Y 137–159 (23px)
static constexpr int16_t FTR_DIV_Y   = 137;
static constexpr int16_t FTR_Y       = 138;
static constexpr int16_t FTR_H       = DISP_H - FTR_Y;    // 22px

// ---- Color palette ----------------------------------------------------------
static constexpr uint16_t COL_BG          = 0x0001;  // #0a0a1a deep navy
static constexpr uint16_t COL_HDR_BG      = 0x0882;  // #111128 header bg
static constexpr uint16_t COL_DIVIDER     = 0x280D;  // #2d1b69 deep purple
static constexpr uint16_t COL_BAR_EMPTY   = 0x0F03;  // #1e1e2e very dark
static constexpr uint16_t COL_BAR_FILL    = 0x8B7B;  // #8b5cf6 electric purple
static constexpr uint16_t COL_TEXT        = 0xF3EF;  // #f1f5f9 near-white
static constexpr uint16_t COL_LABEL       = 0x94B3;  // #94a3b8 slate
static constexpr uint16_t COL_GREEN       = 0x2589;  // #22c55e bright green (Hz≥200)
static constexpr uint16_t COL_CYAN        = 0x07FF;  // #00ffff cyan (Hz 100-200)
static constexpr uint16_t COL_AMBER       = 0xFC60;  // #f59e0b amber (Hz 50-100)
static constexpr uint16_t COL_RED         = 0xF248;  // #ef4444 hot red (Hz<50 / loss)
static constexpr uint16_t COL_PURPLE      = 0x8B7B;  // #8b5cf6 electric purple
static constexpr uint16_t COL_TEAL        = 0x0676;  // #06b6d4 teal (pos value)
static constexpr uint16_t COL_WHITE       = 0xFFFF;

// ---- Minimal 5x7 bitmap font — column-major, LSB = top row ------------------
static const uint8_t FONT5X7[][5] PROGMEM = {
    {0x00,0x00,0x00,0x00,0x00}, // 0x20 space
    {0x00,0x00,0x5F,0x00,0x00}, // 0x21 !
    {0x00,0x07,0x00,0x07,0x00}, // 0x22 "
    {0x14,0x7F,0x14,0x7F,0x14}, // 0x23 #
    {0x24,0x2A,0x7F,0x2A,0x12}, // 0x24 $
    {0x23,0x13,0x08,0x64,0x62}, // 0x25 %
    {0x36,0x49,0x55,0x22,0x50}, // 0x26 &
    {0x00,0x05,0x03,0x00,0x00}, // 0x27 '
    {0x00,0x1C,0x22,0x41,0x00}, // 0x28 (
    {0x00,0x41,0x22,0x1C,0x00}, // 0x29 )
    {0x14,0x08,0x3E,0x08,0x14}, // 0x2A *
    {0x08,0x08,0x3E,0x08,0x08}, // 0x2B +
    {0x00,0x50,0x30,0x00,0x00}, // 0x2C ,
    {0x08,0x08,0x08,0x08,0x08}, // 0x2D -
    {0x00,0x60,0x60,0x00,0x00}, // 0x2E .
    {0x20,0x10,0x08,0x04,0x02}, // 0x2F /
    {0x3E,0x51,0x49,0x45,0x3E}, // 0x30 0
    {0x00,0x42,0x7F,0x40,0x00}, // 0x31 1
    {0x42,0x61,0x51,0x49,0x46}, // 0x32 2
    {0x21,0x41,0x45,0x4B,0x31}, // 0x33 3
    {0x18,0x14,0x12,0x7F,0x10}, // 0x34 4
    {0x27,0x45,0x45,0x45,0x39}, // 0x35 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 0x36 6
    {0x01,0x71,0x09,0x05,0x03}, // 0x37 7
    {0x36,0x49,0x49,0x49,0x36}, // 0x38 8
    {0x06,0x49,0x49,0x29,0x1E}, // 0x39 9
    {0x00,0x36,0x36,0x00,0x00}, // 0x3A :
    {0x00,0x56,0x36,0x00,0x00}, // 0x3B ;
    {0x08,0x14,0x22,0x41,0x00}, // 0x3C <
    {0x14,0x14,0x14,0x14,0x14}, // 0x3D =
    {0x00,0x41,0x22,0x14,0x08}, // 0x3E >
    {0x02,0x01,0x51,0x09,0x06}, // 0x3F ?
    {0x32,0x49,0x79,0x41,0x3E}, // 0x40 @
    {0x7E,0x11,0x11,0x11,0x7E}, // 0x41 A
    {0x7F,0x49,0x49,0x49,0x36}, // 0x42 B
    {0x3E,0x41,0x41,0x41,0x22}, // 0x43 C
    {0x7F,0x41,0x41,0x22,0x1C}, // 0x44 D
    {0x7F,0x49,0x49,0x49,0x41}, // 0x45 E
    {0x7F,0x09,0x09,0x09,0x01}, // 0x46 F
    {0x3E,0x41,0x49,0x49,0x7A}, // 0x47 G
    {0x7F,0x08,0x08,0x08,0x7F}, // 0x48 H
    {0x00,0x41,0x7F,0x41,0x00}, // 0x49 I
    {0x20,0x40,0x41,0x3F,0x01}, // 0x4A J
    {0x7F,0x08,0x14,0x22,0x41}, // 0x4B K
    {0x7F,0x40,0x40,0x40,0x40}, // 0x4C L
    {0x7F,0x02,0x0C,0x02,0x7F}, // 0x4D M
    {0x7F,0x04,0x08,0x10,0x7F}, // 0x4E N
    {0x3E,0x41,0x41,0x41,0x3E}, // 0x4F O
    {0x7F,0x09,0x09,0x09,0x06}, // 0x50 P
    {0x3E,0x41,0x51,0x21,0x5E}, // 0x51 Q
    {0x7F,0x09,0x19,0x29,0x46}, // 0x52 R
    {0x46,0x49,0x49,0x49,0x31}, // 0x53 S
    {0x01,0x01,0x7F,0x01,0x01}, // 0x54 T
    {0x3F,0x40,0x40,0x40,0x3F}, // 0x55 U
    {0x1F,0x20,0x40,0x20,0x1F}, // 0x56 V
    {0x3F,0x40,0x38,0x40,0x3F}, // 0x57 W
    {0x63,0x14,0x08,0x14,0x63}, // 0x58 X
    {0x07,0x08,0x70,0x08,0x07}, // 0x59 Y
    {0x61,0x51,0x49,0x45,0x43}, // 0x5A Z
    {0x00,0x7F,0x41,0x41,0x00}, // 0x5B [
    {0x02,0x04,0x08,0x10,0x20}, // 0x5C backslash
    {0x00,0x41,0x41,0x7F,0x00}, // 0x5D ]
    {0x04,0x02,0x01,0x02,0x04}, // 0x5E ^
    {0x40,0x40,0x40,0x40,0x40}, // 0x5F _
    {0x00,0x01,0x02,0x04,0x00}, // 0x60 `
    {0x20,0x54,0x54,0x54,0x78}, // 0x61 a
    {0x7F,0x48,0x44,0x44,0x38}, // 0x62 b
    {0x38,0x44,0x44,0x44,0x20}, // 0x63 c
    {0x38,0x44,0x44,0x48,0x7F}, // 0x64 d
    {0x38,0x54,0x54,0x54,0x18}, // 0x65 e
    {0x08,0x7E,0x09,0x01,0x02}, // 0x66 f
    {0x0C,0x52,0x52,0x52,0x3E}, // 0x67 g
    {0x7F,0x08,0x04,0x04,0x78}, // 0x68 h
    {0x00,0x44,0x7D,0x40,0x00}, // 0x69 i
    {0x20,0x40,0x44,0x3D,0x00}, // 0x6A j
    {0x7F,0x10,0x28,0x44,0x00}, // 0x6B k
    {0x00,0x41,0x7F,0x40,0x00}, // 0x6C l
    {0x7C,0x04,0x18,0x04,0x78}, // 0x6D m
    {0x7C,0x08,0x04,0x04,0x78}, // 0x6E n
    {0x38,0x44,0x44,0x44,0x38}, // 0x6F o
    {0x7C,0x14,0x14,0x14,0x08}, // 0x70 p
    {0x08,0x14,0x14,0x18,0x7C}, // 0x71 q
    {0x7C,0x08,0x04,0x04,0x08}, // 0x72 r
    {0x48,0x54,0x54,0x54,0x20}, // 0x73 s
    {0x04,0x3F,0x44,0x40,0x20}, // 0x74 t
    {0x3C,0x40,0x40,0x40,0x7C}, // 0x75 u
    {0x1C,0x20,0x40,0x20,0x1C}, // 0x76 v
    {0x3C,0x40,0x30,0x40,0x3C}, // 0x77 w
    {0x44,0x28,0x10,0x28,0x44}, // 0x78 x
    {0x0C,0x50,0x50,0x50,0x3C}, // 0x79 y
    {0x44,0x64,0x54,0x4C,0x44}, // 0x7A z
    {0x00,0x08,0x36,0x41,0x00}, // 0x7B {
    {0x00,0x00,0x7F,0x00,0x00}, // 0x7C |
    {0x00,0x41,0x36,0x08,0x00}, // 0x7D }
    {0x10,0x08,0x08,0x10,0x08}, // 0x7E ~
};

// ---- Full-screen framebuffer — 80x160x2 = 25,600 bytes ----------------------
// Every frame is rendered entirely in RAM, then pushed to the display in ONE
// DMA burst via setAddrWindow(0,0,79,159) + writePixels(). The ST7735 scans
// continuously at ~60Hz; a mid-write scan tears the screen. One atomic push
// is the only way to avoid tearing/flicker/partial-frame garbage.
// 25.6KB is well within the C5's 320KB RAM (~17% used).
static constexpr int16_t FB_W = DISP_W;   // 80
static constexpr int16_t FB_H = DISP_H;   // 160
static uint16_t s_fb[FB_W * FB_H];        // 25,600 bytes

// Write a pixel into the framebuffer — big-endian 565 for transferBytes().
static inline void fbPixel(int16_t x, int16_t y, uint16_t color) {
    if ((uint16_t)x >= (uint16_t)FB_W || (uint16_t)y >= (uint16_t)FB_H) return;
    s_fb[y * FB_W + x] = (color >> 8) | (color << 8);
}

// Fill a rectangle in the framebuffer.
static void fbFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (w <= 0 || h <= 0) return;
    uint16_t sw = (color >> 8) | (color << 8);
    int16_t x1 = x < 0 ? 0 : x;
    int16_t y1 = y < 0 ? 0 : y;
    int16_t x2 = (x + w) > FB_W ? FB_W : (x + w);
    int16_t y2 = (y + h) > FB_H ? FB_H : (y + h);
    for (int16_t row = y1; row < y2; row++) {
        uint16_t* p = &s_fb[row * FB_W + x1];
        for (int16_t col = x1; col < x2; col++) *p++ = sw;
    }
}

// Draw a horizontal line in the framebuffer.
static void fbHLine(int16_t x, int16_t y, int16_t w, uint16_t color) {
    fbFillRect(x, y, w, 1, color);
}

// Draw a vertical line in the framebuffer.
static void fbVLine(int16_t x, int16_t y, int16_t h, uint16_t color) {
    fbFillRect(x, y, 1, h, color);
}

// Draw a 5x7 glyph into the framebuffer at (sx, sy).
static void fbDrawChar(int16_t sx, int16_t sy, char c, uint16_t fg, uint16_t bg, uint8_t sc) {
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t* glyph = FONT5X7[c - 0x20];
    for (int col = 0; col < 5; col++) {
        uint8_t col_data = pgm_read_byte(&glyph[col]);
        for (int row = 0; row < 7; row++) {
            uint16_t px = (col_data & (1 << row)) ? fg : bg;
            if (sc == 1) {
                fbPixel(sx + col, sy + row, px);
            } else {
                fbFillRect(sx + col * sc, sy + row * sc, sc, sc, px);
            }
        }
    }
    // Gap column to the right of the glyph
    fbFillRect(sx + 5 * sc, sy, sc, 7 * sc, bg);
}

// Draw a string into the framebuffer. Returns x after last char.
static int16_t fbPrint(int16_t sx, int16_t sy, const char* str,
                        uint16_t fg, uint16_t bg, uint8_t sc) {
    while (*str) {
        fbDrawChar(sx, sy, *str++, fg, bg, sc);
        sx += 6 * sc;
    }
    return sx;
}

// Flush the entire framebuffer to the display in one DMA burst.
// One setAddrWindow + one writePixels = one SPI transaction for the whole
// screen — the display controller gets the full frame atomically.
static Adafruit_ST7735* s_tft_ptr = nullptr;

static void fbFlush() {
    if (!s_tft_ptr) return;
    s_tft_ptr->startWrite();
    s_tft_ptr->setAddrWindow(0, 0, FB_W - 1, FB_H - 1);
    s_tft_ptr->writePixels(s_fb, FB_W * FB_H);
    s_tft_ptr->endWrite();
}

// ---- APA102 LED driver — bitbang, single LED --------------------------------
// APA102 frame: 4 bytes 0x00, then [0xFF, B, G, R], then 4 bytes 0xFF.
static void ledWrite(uint8_t r, uint8_t g, uint8_t b) {
    // Start frame — 32 clocks with data LOW
    for (int i = 0; i < 32; i++) {
        digitalWrite(PIN_LED_CI, LOW);
        digitalWrite(PIN_LED_DI, LOW);
        digitalWrite(PIN_LED_CI, HIGH);
    }
    // LED frame: [0xFF brightness][blue][green][red] — APA102 is BGR.
    uint32_t frame = 0xFF000000UL | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
    for (int i = 31; i >= 0; i--) {
        digitalWrite(PIN_LED_CI, LOW);
        digitalWrite(PIN_LED_DI, (frame >> i) & 1 ? HIGH : LOW);
        digitalWrite(PIN_LED_CI, HIGH);
    }
    // End frame — 32 clocks with data HIGH
    for (int i = 0; i < 32; i++) {
        digitalWrite(PIN_LED_CI, LOW);
        digitalWrite(PIN_LED_DI, HIGH);
        digitalWrite(PIN_LED_CI, HIGH);
    }
}

enum class LedState : uint8_t {
    WAITING,   // dim white — WiFi off or no serial data yet
    WORKING,   // cyan     — actively relaying T-Code
    IDLE,      // blue     — ESP-NOW ready, no recent serial
    ERROR,     // red      — ESP-NOW init failed
};

static LedState s_led_state = LedState::WAITING;
static LedState s_led_last  = LedState::ERROR;  // force first write

static void applyLed(LedState state) {
    if (state == s_led_last) return;
    s_led_last = state;
    switch (state) {
        case LedState::WAITING: ledWrite(15, 15, 15);   break;  // dim white
        case LedState::WORKING: ledWrite(0,  80, 80);   break;  // cyan
        case LedState::IDLE:    ledWrite(0,  0,  50);   break;  // blue
        case LedState::ERROR:   ledWrite(80, 0,  0);    break;  // red
    }
}

// ---- Application-layer packet loss tracking ---------------------------------
// Broadcast ESP-NOW has NO MAC-layer ACK — the send callback ALWAYS returns
// ESP_NOW_SEND_FAIL for broadcast because there's no 802.11 ACK frame for
// multicast/broadcast addresses. This is documented ESP-NOW behavior, not a
// bug, so loss is tracked at the application layer instead:
//   1. Prepend a 1-byte sequence number to every outgoing packet.
//      Packet format: [seq_byte][T-Code string...]
//   2. The Waveshare receiver strips the seq byte, relays the T-Code, then
//      sends back a batched broadcast ACK every 10ms:
//      {0xAC, base_seq, mask_b0, mask_b1, mask_b2, mask_b3}
//   3. Track which seq#s came back in a 256-bit sliding window (32 bytes).
//
// At 333Hz the seq# (uint8_t, wraps at 256) completes a full cycle every
// 0.77 seconds. Resetting the tracking window every 1 second would let
// seq#s from the previous wrap cycle collide with the current one and
// double-count, so loss uses monotonic counters instead of a periodic reset:
//   - s_seq_sent_total: monotonic count of packets sent
//   - s_seq_acked_total: monotonic count of ACKs received
//   - Every second: loss = 1 - (acked_delta / sent_delta)
// The bit window still deduplicates ACKs (the Waveshare can send the same
// seq# twice) but is never reset on the 1-second boundary — it rolls with
// the seq# naturally.
static constexpr uint8_t  ACK_MAGIC    = 0xAC;  // ACK packet first byte

// Dedup window: bit N set = seq# N was already counted as acked this cycle.
// Cleared when seq# wraps (every 256 packets = ~0.77s at 333Hz).
static uint8_t  s_ack_dedup[32] = {};  // 256 bits

// Monotonic counters — never reset, delta computed each second.
static uint32_t s_seq_sent_total  = 0;
static uint32_t s_seq_acked_total = 0;
static uint8_t  s_seq_tx          = 0;   // next sequence number to send
static float    g_loss_pct        = 0.0f;

// Mark a seq# as acknowledged — dedup prevents double-counting.
// Returns true if this is a new ACK (not a duplicate).
static inline bool ackMark(uint8_t seq) {
    uint8_t byte_idx = seq >> 3;
    uint8_t bit_mask = 1u << (seq & 7);
    if (s_ack_dedup[byte_idx] & bit_mask) return false;  // duplicate
    s_ack_dedup[byte_idx] |= bit_mask;
    return true;
}

// Compute loss % from monotonic counters. Called once per second.
// Uses delta since last call — immune to seq# wrap-around.
static void ackComputeLoss() {
    static uint32_t s_last_sent  = 0;
    static uint32_t s_last_acked = 0;

    uint32_t sent_delta  = s_seq_sent_total  - s_last_sent;
    uint32_t acked_delta = s_seq_acked_total - s_last_acked;
    s_last_sent  = s_seq_sent_total;
    s_last_acked = s_seq_acked_total;

    if (sent_delta > 0) {
        if (acked_delta > sent_delta) acked_delta = sent_delta;
        g_loss_pct = (1.0f - (float)acked_delta / (float)sent_delta) * 100.0f;
    } else {
        g_loss_pct = 0.0f;
    }

    // Clear dedup window every second — seq# has likely wrapped by now.
    memset(s_ack_dedup, 0, sizeof(s_ack_dedup));
}

// ---- Shared volatile state — written by serial/ESP-NOW callbacks, read by loop() -
// Single-core C5: no mutex needed, volatile prevents register caching.
static volatile float    g_position      = 0.0f;
static volatile uint32_t g_pkt_count     = 0;
static float             g_hz            = 0.0f;
static volatile bool     g_flipped       = false;
static bool              g_last_flipped  = false;
static volatile bool     g_serial_active = false;

// ---- WiFi power management --------------------------------------------------
// WiFi starts OFF at boot, turns on when the host opens the COM port (DTR
// asserted), shuts down after 5 minutes of no open port.
static constexpr uint32_t IDLE_SHUTDOWN_MS = 300000;  // 5 minutes
static bool     s_wifi_on         = false;   // WiFi + ESP-NOW currently active
static bool     s_wifi_starting   = false;   // init task in flight (prevents double-start)
static uint32_t s_idle_start_ms   = 0;       // when Serial last dropped (DTR de-asserted)

// ---- TFT instance -----------------------------------------------------------
static Adafruit_ST7735 tft(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST,
                            PIN_TFT_SCLK, PIN_TFT_MOSI);

// ---- Minimal inline T-Code parser -------------------------------------------
static void parseTCode(const char* str, float* out_pos, bool* out_stop) {
    *out_pos  = -1.0f;
    *out_stop = false;
    const char* p = str;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;
        if ((p[0]=='D'||p[0]=='d') && (p[1]=='S'||p[1]=='s') &&
            (p[2]=='T'||p[2]=='t') && (p[3]=='O'||p[3]=='o') &&
            (p[4]=='P'||p[4]=='p')) {
            *out_stop = true; p += 5; continue;
        }
        if ((p[0]=='L'||p[0]=='l') && p[1]=='0') {
            p += 2;
            const char* mag_start = p;
            while (*p >= '0' && *p <= '9') p++;
            int mag_digits = (int)(p - mag_start);
            if (mag_digits > 0) {
                uint32_t mag = 0;
                for (int i = 0; i < mag_digits; i++) mag = mag*10 + (mag_start[i]-'0');
                uint32_t div = 1;
                for (int i = 0; i < mag_digits; i++) div *= 10;
                *out_pos = (float)mag / (float)div;
                if (*out_pos > 1.0f) *out_pos = 1.0f;
            }
            while (*p && *p!=' ' && *p!='\t' && *p!='\r' && *p!='\n') p++;
            continue;
        }
        while (*p && *p!=' ' && *p!='\t' && *p!='\r' && *p!='\n') p++;
    }
}

// ---- ESP-NOW receive callback — handles batched ACK packets -----------------
// Batched ACK format (6 bytes): [0xAC][base_seq][mask_b0][mask_b1][mask_b2][mask_b3]
// base_seq = seq# of bit 0; bits 0-31 = seq base..base+31. At 333Hz the
// Waveshare sends ~100 batched ACKs/sec instead of 333 individual ones, each
// batch covering up to 32 seq#s.
static void onEspNowRecv(const esp_now_recv_info_t* info,
                         const uint8_t* data, int len) {
    (void)info;
    // Batched broadcast ACK: exactly 6 bytes, first byte = 0xAC.
    if (len == 6 && data[0] == ACK_MAGIC) {
        uint8_t  base = data[1];
        uint32_t mask = (uint32_t)data[2]
                      | ((uint32_t)data[3] << 8)
                      | ((uint32_t)data[4] << 16)
                      | ((uint32_t)data[5] << 24);
        // Mark each set bit as acknowledged — dedup prevents double-counting.
        for (int i = 0; i < 32; i++) {
            if (mask & (1u << i)) {
                if (ackMark((uint8_t)(base + i))) {
                    __atomic_fetch_add((uint32_t*)&s_seq_acked_total, 1u, __ATOMIC_RELAXED);
                }
            }
        }
        return;
    }
    // Anything else — ignore (this node is the TX side).
}

static void initDisplay(bool flipped);
static bool s_espnow_ready = false;

// ---- Forward declarations for WiFi power management -------------------------
static void startWiFi();
static void stopWiFi();

// Bundle state — declared here so stopWiFi() can clear the accumulator.
static constexpr uint8_t  BUNDLE_MAX_CMDS   = 4;
static constexpr uint8_t  BUNDLE_CMD_MAXLEN = 60;   // max bytes per T-Code cmd
static constexpr uint32_t BUNDLE_INTERVAL_MS = 10;  // send bundle every 10ms = 100Hz

struct BundleCmd {
    char    data[BUNDLE_CMD_MAXLEN];
    uint8_t len;
    uint8_t rel_ms;  // ms since bundle window opened
};

static BundleCmd  s_bundle[BUNDLE_MAX_CMDS];
static uint8_t    s_bundle_count    = 0;
static uint32_t   s_bundle_start_ms = 0;  // when the current window opened

// ---- WiFi + ESP-NOW init task -----------------------------------------------
// Runs as a background task so USB CDC stays alive during init — without
// this, Windows fires a "semaphore timeout" when WiFi.mode() blocks. Called
// by startWiFi() only when the host opens the COM port.
static void espNowInitTask(void* arg) {
    vTaskDelay(pdMS_TO_TICKS(50));

    tft.begin();
    s_tft_ptr = &tft;
    initDisplay(false);
    digitalWrite(PIN_TFT_BL, LOW);  // backlight ON

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_err_t band_err = esp_wifi_set_band(WIFI_BAND_5G);
    if (band_err != ESP_OK)
        Serial.printf("[espnow] WARN: esp_wifi_set_band(5G): %s\n", esp_err_to_name(band_err));
    esp_err_t ch_err = esp_wifi_set_channel(SECRET_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (ch_err != ESP_OK)
        Serial.printf("[espnow] ERROR: set_channel(%d): %s\n", SECRET_ESPNOW_CHANNEL, esp_err_to_name(ch_err));
    else
        Serial.printf("[espnow] Channel %d locked. yippie! :3\n", SECRET_ESPNOW_CHANNEL);
    esp_err_t now_err = esp_now_init();
    if (now_err != ESP_OK) {
        Serial.printf("[espnow] FATAL: esp_now_init(): %s\n", esp_err_to_name(now_err));
        s_wifi_starting = false;
        s_led_state = LedState::ERROR;
        vTaskDelete(NULL);
        return;
    }
    esp_now_register_recv_cb(onEspNowRecv);
    // No send callback registered — broadcast always returns FAIL, which is
    // useless noise. App-layer ACK via onEspNowRecv() is the real signal.
    s_espnow_ready = true;
    s_wifi_on      = true;
    s_wifi_starting = false;
    s_led_state = LedState::IDLE;
    Serial.println("[espnow] Ready. yippie! :3");
    vTaskDelete(NULL);
}

// ---- startWiFi — fires up the WiFi radio and ESP-NOW when the host plugs in -
// Guarded against double-start (s_wifi_on / s_wifi_starting flags).
static void startWiFi() {
    if (s_wifi_on || s_wifi_starting) return;
    s_wifi_starting = true;
    Serial.println("[power] Host connected — waking WiFi. yippie! :3");
    xTaskCreate(espNowInitTask, "espnow_init", 4096, NULL, 1, NULL);
}

// ---- stopWiFi — shuts down ESP-NOW and WiFi radio after the idle timeout ----
// Clears the bundle accumulator so no stale packets try to send post-deinit.
static void stopWiFi() {
    if (!s_wifi_on) return;

    // Clear the bundle accumulator — about to kill ESP-NOW, no stale sends.
    s_bundle_count = 0;
    s_bundle_start_ms = 0;

    if (s_espnow_ready) {
        esp_now_deinit();
        s_espnow_ready = false;
    }

    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);

    s_wifi_on = false;
    s_led_state = LedState::WAITING;

    // Clear serial-active too — no point showing stale data.
    g_serial_active = false;

    // Reset the ACK tracking counters for a clean slate on next connect.
    s_seq_sent_total  = 0;
    s_seq_acked_total = 0;
    s_seq_tx          = 0;
    g_loss_pct        = 0.0f;
    memset(s_ack_dedup, 0, sizeof(s_ack_dedup));

    Serial.println("[power] Idle timeout — WiFi off. Going soft... :3");
}

// ---- Display rendering — all into the framebuffer, flushed once per frame ---

// Hz color: red < 50, amber 50-100, cyan 100-200, green >= 200.
static uint16_t hzColor(float hz) {
    if (hz < 50.0f)  return COL_RED;
    if (hz < 100.0f) return COL_AMBER;
    if (hz < 200.0f) return COL_CYAN;
    return COL_GREEN;
}

// Render the entire frame into the framebuffer.
// Called every loop() iteration — cheap because it's all RAM writes.
// See the file header for the layout diagram.
//
// The bar tracks a cached fill height so only the changed rows are redrawn
// in the framebuffer; the whole framebuffer still flushes every frame
// regardless, but the RAM writes are cheap — the DMA push is the bottleneck.
static int16_t s_last_fill_h = -1;

static void renderFrame(float hz, float position, float loss_pct,
                         bool wifi_up, bool serial_active) {
    // ---- Background: only clear zones that change ---------------------------
    // On first call (s_last_fill_h == -1) everything is cleared.
    bool full_clear = (s_last_fill_h < 0);

    if (full_clear) {
        // Clear entire framebuffer to background.
        fbFillRect(0, 0, FB_W, FB_H, COL_BG);
        // Header background
        fbFillRect(0, 0, FB_W, HDR_H, COL_HDR_BG);
        // Bar empty fill
        fbFillRect(BAR_X, BAR_Y, BAR_W, BAR_H, COL_BAR_EMPTY);
        // Divider lines
        fbHLine(0, HDR_DIV_Y, FB_W, COL_DIVIDER);
        fbHLine(0, FTR_DIV_Y, FB_W, COL_DIVIDER);
        fbVLine(DIV_X, CONTENT_Y, CONTENT_H, COL_DIVIDER);
    }

    // ---- Header -------------------------------------------------------------
    // Always redraw header — it's only 16px and changes rarely.
    fbFillRect(0, 0, FB_W, HDR_H, COL_HDR_BG);

    // [WIFI] badge left — cyan if up, slate if not
    fbPrint(3, 5, wifi_up ? "WIFI" : "----",
            wifi_up ? COL_CYAN : COL_LABEL, COL_HDR_BG, 1);

    // Status right — "TX" purple when active, "RDY" text when idle
    if (serial_active) {
        fbPrint(FB_W - 15, 5, "TX", COL_PURPLE, COL_HDR_BG, 1);
    } else {
        fbPrint(FB_W - 21, 5, "RDY", COL_TEXT, COL_HDR_BG, 1);
    }

    // ---- Position bar (left column) -----------------------------------------
    // Map 0.0-1.0 to 0-BAR_H pixels, fill from bottom.
    int16_t fill_h = (int16_t)(position * BAR_H);
    if (fill_h < 0)     fill_h = 0;
    if (fill_h > BAR_H) fill_h = BAR_H;

    if (fill_h != s_last_fill_h || full_clear) {
        // Clear the whole bar zone and redraw — fast in RAM.
        fbFillRect(BAR_X, BAR_Y, BAR_W, BAR_H, COL_BAR_EMPTY);
        if (fill_h > 0) {
            int16_t bar_top = BAR_Y + (BAR_H - fill_h);
            fbFillRect(BAR_X, bar_top, BAR_W, fill_h, COL_BAR_FILL);
        }
        s_last_fill_h = fill_h;
    }

    // ---- Stats panel (right column) -----------------------------------------
    // Clear stats zone — 41x120px.
    fbFillRect(STATS_X, CONTENT_Y, STATS_W, CONTENT_H, COL_BG);

    // RATE section — label + Hz value (scale 2 = 10×14px per char)
    // Label at Y+2, value at Y+12 (label 7px + 3px gap = 10px)
    fbPrint(STATS_X + 2, CONTENT_Y + 2,  "RATE", COL_LABEL, COL_BG, 1);
    {
        char hz_buf[8];
        if (hz < 1000.0f) snprintf(hz_buf, sizeof(hz_buf), "%3d", (int)hz);
        else              snprintf(hz_buf, sizeof(hz_buf), "999+");
        fbPrint(STATS_X + 2, CONTENT_Y + 12, hz_buf, hzColor(hz), COL_BG, 2);
    }

    // POS section — label at Y+42, value at Y+52
    fbPrint(STATS_X + 2, CONTENT_Y + 42, "POS",  COL_LABEL, COL_BG, 1);
    {
        char pos_buf[8];
        // 260mm travel — display as integer mm.
        snprintf(pos_buf, sizeof(pos_buf), "%3d", (int)(position * 260.0f));
        fbPrint(STATS_X + 2, CONTENT_Y + 52, pos_buf, COL_TEAL, COL_BG, 2);
    }

    // PKT section — label at Y+82, value at Y+92
    // Loss color: green=0%, amber<5%, red>=5%.
    fbPrint(STATS_X + 2, CONTENT_Y + 82, "PKT",  COL_LABEL, COL_BG, 1);
    {
        uint16_t loss_col;
        if (loss_pct < 0.5f)      loss_col = COL_GREEN;
        else if (loss_pct < 5.0f) loss_col = COL_AMBER;
        else                       loss_col = COL_RED;

        char loss_buf[8];
        if (loss_pct < 10.0f)
            snprintf(loss_buf, sizeof(loss_buf), "%.1f%%", loss_pct);
        else
            snprintf(loss_buf, sizeof(loss_buf), "%2d%%", (int)loss_pct);
        fbPrint(STATS_X + 2, CONTENT_Y + 92, loss_buf, loss_col, COL_BG, 1);
    }

    // ---- Dividers (redraw over bar/stats in case they got clobbered) --------
    fbHLine(0, HDR_DIV_Y, FB_W, COL_DIVIDER);
    fbHLine(0, FTR_DIV_Y, FB_W, COL_DIVIDER);
    fbVLine(DIV_X, CONTENT_Y, CONTENT_H, COL_DIVIDER);

    // ---- Footer -------------------------------------------------------------
    fbFillRect(0, FTR_Y, FB_W, FTR_H, COL_BG);
    {
        const char* label     = "serial:";
        const char* state_str;
        uint16_t    state_col;
        if (serial_active) {
            state_str = "CONN";
            state_col = COL_GREEN;
        } else if (wifi_up) {
            state_str = "RDY";
            state_col = COL_LABEL;
        } else {
            state_str = "WiFi:OFF";
            state_col = COL_AMBER;
        }
        // "serial: CONN" = 12 chars x 6px = 72px, centered at x=4.
        int16_t fy = FTR_Y + (FTR_H - 7) / 2;  // vertically center the 7px text
        fbPrint(4,  fy, label,     COL_LABEL, COL_BG, 1);
        fbPrint(46, fy, state_str, state_col, COL_BG, 1);
    }
}

// ---- Full display init — called at boot and on rotation change --------------
static void initDisplay(bool flipped) {
    tft.setRotation(flipped ? 0 : 2);

    // Clear framebuffer and force full redraw on next renderFrame().
    memset(s_fb, 0, sizeof(s_fb));
    s_last_fill_h = -1;

    // Push the blank frame immediately so the display isn't showing garbage.
    fbFlush();
}

// ---- Button debounce --------------------------------------------------------
static uint32_t s_btn_last_ms    = 0;
static bool     s_btn_last_state = HIGH;
static constexpr uint32_t BTN_DEBOUNCE_MS = 50;

// ---- setup() ----------------------------------------------------------------
void setup() {
    // Bump RX buffer — MFP blasts at 100Hz; the default 256-byte buffer
    // fills in ~22ms.
    Serial.setRxBufferSize(1024);
    Serial.begin(460800);

    // Disable RTS-triggered reset (belt-and-suspenders with the constructor).
    USB_SERIAL_JTAG.chip_rst.usb_uart_chip_rst_dis = 1;

    // GPIO setup
    pinMode(PIN_TFT_BL, OUTPUT);
    digitalWrite(PIN_TFT_BL, HIGH);  // backlight OFF during init
    pinMode(PIN_BUTTON, INPUT_PULLUP);

    // APA102 LED — start dim white (WiFi off, waiting for host to plug in).
    pinMode(PIN_LED_CI, OUTPUT);
    pinMode(PIN_LED_DI, OUTPUT);
    digitalWrite(PIN_LED_CI, LOW);
    digitalWrite(PIN_LED_DI, LOW);
    ledWrite(15, 15, 15);

    // Init the display immediately so it's not black — WiFi starts later,
    // only when the host opens the COM port.
    tft.begin();
    s_tft_ptr = &tft;
    initDisplay(false);
    digitalWrite(PIN_TFT_BL, LOW);  // backlight ON

    // Render the "WiFi:OFF" boot screen once before loop takes over.
    renderFrame(0.0f, 0.0f, 0.0f, false, false);
    fbFlush();
}

// ---- Serial relay — reads USB CDC, parses T-Code, forwards over ESP-NOW -----
// LATENCY CRITICAL PATH: Serial.read() -> parseTCode() -> esp_now_send().
// Everything in this path must be non-blocking and O(1).
//
// Packet format: [1-byte seq#][T-Code string]
// The seq# is prepended so the Waveshare can echo it back as an ACK.

// ---- Bundle accumulator -----------------------------------------------------
// Packs 1-4 T-Code commands per ESP-NOW packet, sent at 100Hz (every 10ms).
// Each command carries a relative timestamp so the Waveshare can replay them
// with correct inter-command spacing.
//
// Bundle packet format (max 250 bytes, ESP-NOW hard limit):
//   [seq:1][N:1][rel_ms_0:1][len_0:1][cmd_0:len_0]...[rel_ms_N:1][len_N:1][cmd_N:len_N]
//
// seq    = bundle sequence number (for ACK tracking)
// N      = number of commands in this bundle (1-4)
// rel_ms = milliseconds since bundle window opened (0-255)
//          = arrival_ms - bundle_start_ms, clamped to 255
// len    = byte length of the T-Code command string (no NUL)
// cmd    = raw T-Code bytes (no newline, no NUL)
//
// At 333Hz input, a 10ms window holds ~3-4 commands. At 100Hz send rate, even
// 26% loss drops only ~26 bundles/sec, and each bundle covers 10ms of
// commands — a single missed bundle is invisible to motion because T-Code
// I-values are typically 50-200ms.
//
// Jitter: commands arrive at the Waveshare ~0-10ms before their fire time;
// rel_ms preserves inter-command spacing exactly, so even a late arrival
// still fires in the correct order.

// Flush the accumulated bundle over ESP-NOW. Called from loop() every 10ms.
// Packs all queued commands into one packet and sends it as broadcast.
static void flushBundle() {
    if (!s_espnow_ready || s_bundle_count == 0) {
        s_bundle_count    = 0;
        s_bundle_start_ms = millis();
        return;
    }

    static const uint8_t BROADCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    static bool peer_added = false;
    if (!peer_added) {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, BROADCAST, 6);
        peer.channel = SECRET_ESPNOW_CHANNEL;
        peer.encrypt = false;
        esp_now_add_peer(&peer);
        peer_added = true;
    }

    // Pack: [seq][N][rel_ms_0][len_0][cmd_0...]...[rel_ms_N-1][len_N-1][cmd_N-1...]
    // Max size: 2 + 4*(1+1+60) = 2 + 248 = 250 bytes — exactly at ESP-NOW limit.
    uint8_t pkt[250];
    uint8_t seq = s_seq_tx++;
    pkt[0] = seq;
    pkt[1] = s_bundle_count;
    uint8_t pos = 2;
    for (uint8_t i = 0; i < s_bundle_count; i++) {
        pkt[pos++] = s_bundle[i].rel_ms;
        pkt[pos++] = s_bundle[i].len;
        memcpy(pkt + pos, s_bundle[i].data, s_bundle[i].len);
        pos += s_bundle[i].len;
    }

    esp_now_send(BROADCAST, pkt, pos);
    __atomic_fetch_add((uint32_t*)&s_seq_sent_total, 1u, __ATOMIC_RELAXED);

    s_bundle_count    = 0;
    s_bundle_start_ms = millis();
}

// Push a T-Code command into the bundle accumulator. Called from
// flushSerialBuf(). Does NOT send immediately — waits for the 10ms window.
static void pushToBundle(const char* cmd, uint8_t len) {
    if (len == 0 || len > BUNDLE_CMD_MAXLEN) return;

    // If accumulator is full, flush immediately to make room. Shouldn't
    // happen at 333Hz (only ~3-4 cmds per 10ms window) but handles burst
    // cases gracefully — better to send early than drop.
    if (s_bundle_count >= BUNDLE_MAX_CMDS) {
        flushBundle();
    }

    uint32_t now_ms = millis();
    // Open a new window on first command.
    if (s_bundle_count == 0) {
        s_bundle_start_ms = now_ms;
    }

    uint8_t rel = (uint8_t)((now_ms - s_bundle_start_ms) & 0xFF);
    BundleCmd& slot = s_bundle[s_bundle_count++];
    slot.len    = len;
    slot.rel_ms = rel;
    memcpy(slot.data, cmd, len);
}

static char     s_serial_buf[BUNDLE_CMD_MAXLEN + 2];
static uint8_t  s_serial_len = 0;
static uint32_t s_last_serial_ms = 0;

static void flushSerialBuf() {
    if (s_serial_len == 0) return;
    s_serial_buf[s_serial_len] = '\0';

    // Push to bundle accumulator — actual send happens in loop() every 10ms.
    pushToBundle(s_serial_buf, s_serial_len);

    // Update shared state for display
    float pos = -1.0f; bool stop = false;
    parseTCode(s_serial_buf, &pos, &stop);
    if (stop)             g_position = 0.0f;
    else if (pos >= 0.0f) g_position = pos;
    __atomic_fetch_add((uint32_t*)&g_pkt_count, 1u, __ATOMIC_RELAXED);
    g_serial_active = true;
    s_last_serial_ms = millis();

    s_serial_len = 0;
}

static void pollSerial() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r' || c == ' ') {
            flushSerialBuf();
        } else if (s_serial_len < (uint8_t)(sizeof(s_serial_buf) - 2)) {
            s_serial_buf[s_serial_len++] = c;
        } else {
            flushSerialBuf();
            s_serial_buf[s_serial_len++] = c;
        }
    }
}

// ---- loop() — non-blocking, no delay() --------------------------------------
// Pumps serial, renders frame, flushes. Handles lazy WiFi start on COM port
// open and 5-minute idle shutdown.
void loop() {
    uint32_t now_ms = millis();

    // ---- WiFi power management: turn on when host opens COM, off after 5 min idle --
    // `(bool)Serial` returns true when DTR is asserted (host has port open).
    // The CDC driver's operator bool checks the DTR/RTS line state.
    bool host_connected = (bool)Serial;

    if (host_connected && !s_wifi_on && !s_wifi_starting) {
        startWiFi();
        s_idle_start_ms = 0;  // reset idle timer
    }

    if (!host_connected && s_wifi_on) {
        if (s_idle_start_ms == 0) {
            s_idle_start_ms = now_ms;  // start the 5-minute countdown
        } else if (now_ms - s_idle_start_ms >= IDLE_SHUTDOWN_MS) {
            stopWiFi();
            s_idle_start_ms = 0;
        }
    }

    if (host_connected && s_idle_start_ms != 0) {
        s_idle_start_ms = 0;  // host reconnected before timeout — reset timer
    }

    // ---- LATENCY CRITICAL: drain serial first -------------------------------
    pollSerial();

    // Serial-active timeout — clear after 500ms of silence.
    if (g_serial_active && (now_ms - s_last_serial_ms) > 500) {
        g_serial_active = false;
    }

    // ---- Button: rotate display 180 degrees on press ------------------------
    bool btn_state = digitalRead(PIN_BUTTON);
    if (btn_state == LOW && s_btn_last_state == HIGH &&
        (now_ms - s_btn_last_ms) > BTN_DEBOUNCE_MS) {
        g_flipped = !g_flipped;
        s_btn_last_ms = now_ms;
    }
    s_btn_last_state = btn_state;

    if ((bool)g_flipped != g_last_flipped) {
        g_last_flipped = g_flipped;
        if (s_tft_ptr) initDisplay(g_flipped);
    }

    // ---- Bundle flush — every 10ms = 100Hz send rate ------------------------
    // Packs accumulated T-Code commands into one ESP-NOW broadcast packet.
    // See the bundle accumulator comment above for the loss-tolerance math.
    static uint32_t s_bundle_last_ms = 0;
    if (now_ms - s_bundle_last_ms >= BUNDLE_INTERVAL_MS) {
        s_bundle_last_ms = now_ms;
        flushBundle();
    }

    // ---- Hz + packet loss — once per second ---------------------------------
    static uint32_t s_hz_last_ms    = 0;
    static uint32_t s_hz_last_count = 0;

    if (now_ms - s_hz_last_ms >= 1000) {
        uint32_t cur    = g_pkt_count;
        g_hz            = (float)(cur - s_hz_last_count);
        s_hz_last_count = cur;
        ackComputeLoss();  // compute loss from the ACK window, reset for next second.
        s_hz_last_ms = now_ms;
    }

    // ---- LED state machine --------------------------------------------------
    if (!s_wifi_on) {
        s_led_state = LedState::WAITING;
    } else if (!s_espnow_ready) {
        s_led_state = LedState::WAITING;  // still initializing
    } else if (g_serial_active) {
        s_led_state = LedState::WORKING;
    } else {
        s_led_state = LedState::IDLE;
    }
    applyLed(s_led_state);

    // ---- Display: render + flush at 30fps max -------------------------------
    // fbFlush() pushes 25,600 bytes over SPI at 40MHz = ~5ms per flush. At
    // 333Hz that would be 333 x 5ms = 1.6 seconds of SPI per second —
    // completely saturated, starving the serial relay path. Throttled to
    // 30fps (every 33ms) so the loop spends <15% of its time on display.
    static uint32_t s_disp_last_ms = 0;
    if (s_tft_ptr != nullptr && (now_ms - s_disp_last_ms) >= 33) {
        s_disp_last_ms = now_ms;
        // Pass s_wifi_on (not s_espnow_ready) so the footer shows "WiFi:OFF"
        // before init finishes and after idle shutdown.
        bool show_wifi_up = s_wifi_on && s_espnow_ready;
        renderFrame(g_hz, g_position, g_loss_pct, show_wifi_up, g_serial_active);
        fbFlush();
    }
    // No delay() — the C5 at 240MHz keeps up without one.
}