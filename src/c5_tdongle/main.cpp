// =============================================================================
// SlopDrive-32 — T-Dongle C5 Transmitter Node
// src/c5_tdongle/main.cpp
//
// This little guy sits between MFP and the machine, swallowing every T-Code
// byte that comes down the USB pipe and squirting it out over ESP-NOW to the
// Waveshare C5 relay. The display shows you exactly how hard it's working —
// a vertical bar that fills up as the position climbs, a live Hz readout that
// changes color depending on how fast the data is pumping, and a status line
// at the bottom so you know if the serial link is alive. yippie! :3
//
// Display layout (portrait 80×160) — full 160px used, no dead zones:
//   Y 0–15:    Header — [WIFI] left, RDY/TX right (16px)
//   Y 16:      Divider line (1px)
//   Y 17–136:  Content zone (120px):
//              Left  0–37px:  vertical position bar (fills bottom-to-top)
//              X 38:          1px vertical divider
//              Right 39–79px: RATE label + Hz value (color-coded)
//                             POS label + position in mm
//                             PKT label + loss % (app-layer ACK)
//   Y 137:     Divider line (1px)
//   Y 138–159: Footer — "serial: CONN" / "RDY" / "IDLE" (22px)
//
// Hz color coding:
//   < 50 Hz  → RED    (barely dribbling, something is wrong)
//   < 100 Hz → ORANGE (sluggish, check your connection)
//   < 200 Hz → CYAN   (good, steady stream)
//   ≥ 200 Hz → GREEN  (absolutely railing it, full send)
//
// Packet loss tracking — APPLICATION LAYER, not hardware ACK.
// Broadcast ESP-NOW has NO MAC-layer ACK — the send callback ALWAYS returns
// FAIL for broadcast. Instead: we embed a 1-byte seq# at the front of every
// packet. The Waveshare echoes back a 2-byte ACK {0xAC, seq}. We track which
// seq#s came back in a 256-bit sliding window. Real RF loss, real numbers. :3
//
// Rendering — FULL FRAMEBUFFER, ONE DMA PUSH PER FRAME.
// 80×160×2 = 25,600 bytes rendered in RAM, then blasted to the display in a
// single setAddrWindow + writePixels call. The ST7735 scans continuously; if
// we're mid-write when it scans, we get tearing. One atomic push = no tearing,
// no flicker, no partial-frame garbage. Like inflating the whole cavity at once
// instead of pumping one cc at a time — the stomach bulges all at once. hehee :3
//
// APA102 LED status (CI=4, DI=5):
//   WHITE  — waiting for serial data / idle
//   PURPLE — actively receiving and forwarding T-Code
//   BLUE   — ESP-NOW ready but no serial activity
//   RED    — error / ESP-NOW not ready
//
// Hardware (LilyGO T-Dongle C5):
//   Display:  ST7735 80×160, SPI — CS=10, DC=3, RST=1, MOSI=2, SCLK=6, BL=0
//   Button:   GPIO 28 (boot button — rotates display 180°)
//   LED:      APA102 RGB — CI=4, DI=5
// =============================================================================

#include <Arduino.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include "st7735.h"
#include "soc/usb_serial_jtag_struct.h"
#include "esp_system.h"
#include "esp_sleep.h"

// =============================================================================
// POWER-ON SELF-RESET — kills the DTR-triggered reboot on first enumeration.
// Windows asserts DTR when it opens the port, the C5 hardware resets, display
// goes black, MFP gets a semaphore timeout. We self-reset on power-on so the
// DTR storm hits during the first enumeration cycle before MFP opens the port.
// The hole takes the hit before the fist arrives. Smart. :3
// =============================================================================
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

// =============================================================================
// Pin definitions
// =============================================================================
static constexpr int8_t  PIN_TFT_CS   = 10;
static constexpr int8_t  PIN_TFT_DC   = 3;
static constexpr int8_t  PIN_TFT_RST  = 1;
static constexpr int8_t  PIN_TFT_SCLK = 6;
static constexpr int8_t  PIN_TFT_MOSI = 2;
static constexpr uint8_t PIN_TFT_BL   = 0;   // active LOW — 0 = backlight ON
static constexpr uint8_t PIN_BUTTON   = 28;  // boot button, active LOW
static constexpr uint8_t PIN_LED_CI   = 4;   // APA102 clock
static constexpr uint8_t PIN_LED_DI   = 5;   // APA102 data

// =============================================================================
// Display geometry — portrait 80×160, ALL 160px used. No dead zones. :3
//
// Old layout wasted 32px at the bottom (only used 128 of 160px).
// New layout: HDR=16, DIV=1, CONTENT=120, DIV=1, FTR=22 → total=160. yippie!
// =============================================================================
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

// =============================================================================
// Colour palette — reworked for readability and contrast. :3
//
// Background: deep navy #0a0a1a (darker than before, more contrast)
// Accent:     electric purple #8b5cf6
// Good:       bright green #22c55e (replaces cyan for Hz≥200 — pops more)
// Mid:        amber #f59e0b (replaces yellow — warmer, less harsh)
// Bad:        hot red #ef4444
// Text:       near-white #f1f5f9
// Label:      slate #94a3b8
// Bar fill:   electric purple #8b5cf6
// Bar empty:  very dark #1e1e2e
// Header bg:  slightly lighter than bg #111128
// Divider:    deep purple #2d1b69
// =============================================================================
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

// =============================================================================
// Minimal 5×7 bitmap font — column-major, LSB = top row. :3
// =============================================================================
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

// =============================================================================
// FULL-SCREEN FRAMEBUFFER — 80×160×2 = 25,600 bytes.
//
// Every frame is rendered entirely in RAM, then pushed to the display in ONE
// DMA burst via setAddrWindow(0,0,79,159) + writePixels(). The ST7735 scans
// continuously at ~60Hz; if we're mid-write when it scans, we get tearing.
// One atomic push = zero tearing, zero flicker, zero partial-frame garbage.
//
// Like fisting the whole thing open in one push — the stomach bulges all at
// once, nothing leaks out the sides, the whole cavity fills simultaneously.
// That's the energy we're going for. yippie! :3
//
// 25.6KB is fine — C5 has 320KB RAM, we're at ~17% used. owo
// =============================================================================
static constexpr int16_t FB_W = DISP_W;   // 80
static constexpr int16_t FB_H = DISP_H;   // 160
static uint16_t s_fb[FB_W * FB_H];        // 25,600 bytes

// Write a pixel into the framebuffer — big-endian 565 for transferBytes(). :3
static inline void fbPixel(int16_t x, int16_t y, uint16_t color) {
    if ((uint16_t)x >= (uint16_t)FB_W || (uint16_t)y >= (uint16_t)FB_H) return;
    s_fb[y * FB_W + x] = (color >> 8) | (color << 8);
}

// Fill a rectangle in the framebuffer. :3
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

// Draw a horizontal line in the framebuffer. :3
static void fbHLine(int16_t x, int16_t y, int16_t w, uint16_t color) {
    fbFillRect(x, y, w, 1, color);
}

// Draw a vertical line in the framebuffer. :3
static void fbVLine(int16_t x, int16_t y, int16_t h, uint16_t color) {
    fbFillRect(x, y, 1, h, color);
}

// Draw a 5×7 glyph into the framebuffer at (sx, sy). :3
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

// Draw a string into the framebuffer. Returns x after last char. :3
static int16_t fbPrint(int16_t sx, int16_t sy, const char* str,
                        uint16_t fg, uint16_t bg, uint8_t sc) {
    while (*str) {
        fbDrawChar(sx, sy, *str++, fg, bg, sc);
        sx += 6 * sc;
    }
    return sx;
}

// Flush the entire framebuffer to the display in one DMA burst.
// One setAddrWindow + one writePixels = one SPI transaction for the whole screen.
// The display controller gets the full frame atomically — no tearing possible. :3
static Adafruit_ST7735* s_tft_ptr = nullptr;

static void fbFlush() {
    if (!s_tft_ptr) return;
    s_tft_ptr->startWrite();
    s_tft_ptr->setAddrWindow(0, 0, FB_W - 1, FB_H - 1);
    s_tft_ptr->writePixels(s_fb, FB_W * FB_H);
    s_tft_ptr->endWrite();
}

// =============================================================================
// APA102 LED driver — bitbang, single LED. No library needed. :3
// APA102 frame: 4 bytes 0x00, then [0xFF, B, G, R], then 4 bytes 0xFF.
// =============================================================================
static void ledWrite(uint8_t r, uint8_t g, uint8_t b) {
    // Start frame — 32 clocks with data LOW
    for (int i = 0; i < 32; i++) {
        digitalWrite(PIN_LED_CI, LOW);
        digitalWrite(PIN_LED_DI, LOW);
        digitalWrite(PIN_LED_CI, HIGH);
    }
    // LED frame: [0xFF brightness][blue][green][red] — APA102 is BGR. owo
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
    WAITING,   // dim white — no serial data yet
    WORKING,   // purple   — actively relaying T-Code
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
        case LedState::WORKING: ledWrite(80, 0,  80);   break;  // purple
        case LedState::IDLE:    ledWrite(0,  0,  50);   break;  // blue
        case LedState::ERROR:   ledWrite(80, 0,  0);    break;  // red
    }
}

// =============================================================================
// Application-layer packet loss tracking — fixed window math.
//
// Broadcast ESP-NOW has NO MAC-layer ACK — the send callback ALWAYS returns
// ESP_NOW_SEND_FAIL for broadcast because there's no 802.11 ACK frame for
// multicast/broadcast addresses. This is documented behavior, not a bug.
//
// Instead we do it properly:
//   1. Prepend a 1-byte sequence number to every outgoing packet.
//      Packet format: [seq_byte][T-Code string...]
//   2. The Waveshare receiver strips the seq byte, relays the T-Code,
//      then sends back a batched broadcast ACK every 10ms:
//      {0xAC, base_seq, mask_b0, mask_b1, mask_b2, mask_b3}
//   3. We track which seq#s came back in a 256-bit sliding window (32 bytes).
//
// WINDOW MATH FIX: At 333Hz the seq# (uint8_t, wraps at 256) completes a full
// cycle every 0.77 seconds. If we reset the window every 1 second, seq#s from
// the previous cycle overwrite bits from the current cycle — double-counting.
//
// Fix: track loss using a rolling counter instead of a bit window.
//   - s_seq_sent_total: monotonic count of packets sent
//   - s_seq_acked_total: monotonic count of ACKs received
//   - Every second: loss = 1 - (acked_delta / sent_delta)
//
// The bit window is still used to deduplicate ACKs (prevent double-counting
// when the Waveshare sends the same seq# twice). But we don't reset it on
// the 1-second boundary — we let it roll with the seq# naturally. :3
//
// This gives us REAL RF packet loss numbers. The hole knows when it's been
// filled and when the fist missed. :3
// =============================================================================
static constexpr uint8_t  ACK_MAGIC    = 0xAC;  // ACK packet first byte

// Dedup window: bit N set = seq# N was already counted as acked this cycle. :3
// Cleared when seq# wraps (every 256 packets = ~0.77s at 333Hz). owo
static uint8_t  s_ack_dedup[32] = {};  // 256 bits

// Monotonic counters — never reset, delta computed each second. :3
static uint32_t s_seq_sent_total  = 0;
static uint32_t s_seq_acked_total = 0;
static uint8_t  s_seq_tx          = 0;   // next sequence number to send
static float    g_loss_pct        = 0.0f;

// Mark a seq# as acknowledged — dedup prevents double-counting. :3
// Returns true if this is a new ACK (not a duplicate). owo
static inline bool ackMark(uint8_t seq) {
    uint8_t byte_idx = seq >> 3;
    uint8_t bit_mask = 1u << (seq & 7);
    if (s_ack_dedup[byte_idx] & bit_mask) return false;  // duplicate
    s_ack_dedup[byte_idx] |= bit_mask;
    return true;
}

// Compute loss % from monotonic counters. Called once per second. :3
// Uses delta since last call — immune to seq# wrap-around. yippie! :3
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

    // Clear dedup window every second — seq# has likely wrapped by now. :3
    memset(s_ack_dedup, 0, sizeof(s_ack_dedup));
}

// =============================================================================
// Shared volatile state — written by serial/ESP-NOW callbacks, read by loop()
// Single-core C5 so no mutex needed, volatile prevents register caching. :3
// =============================================================================
static volatile float    g_position      = 0.0f;
static volatile uint32_t g_pkt_count     = 0;
static float             g_hz            = 0.0f;
static volatile bool     g_flipped       = false;
static bool              g_last_flipped  = false;
static volatile bool     g_serial_active = false;

// =============================================================================
// TFT instance
// =============================================================================
static Adafruit_ST7735 tft(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST,
                            PIN_TFT_SCLK, PIN_TFT_MOSI);

// =============================================================================
// Minimal inline T-Code parser — same as before, it's correct. :3
// =============================================================================
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

// =============================================================================
// ESP-NOW receive callback — handles BATCHED ACK packets from the Waveshare.
//
// Batched ACK format (6 bytes):
//   [0xAC][base_seq][mask_b0][mask_b1][mask_b2][mask_b3]
//
// base_seq = seq# of bit 0. Bits 0-31 = seq base..base+31.
// We unpack the bitmask and mark each set bit as acknowledged. :3
//
// At 333Hz the Waveshare sends ~100 batched ACKs/sec instead of 333 individual
// ones. Each batch covers up to 32 seq#s. The Wi-Fi stack breathes again. owo
// =============================================================================
static void onEspNowRecv(const esp_now_recv_info_t* info,
                         const uint8_t* data, int len) {
    (void)info;
    // Batched broadcast ACK: exactly 6 bytes, first byte = 0xAC. :3
    if (len == 6 && data[0] == ACK_MAGIC) {
        uint8_t  base = data[1];
        uint32_t mask = (uint32_t)data[2]
                      | ((uint32_t)data[3] << 8)
                      | ((uint32_t)data[4] << 16)
                      | ((uint32_t)data[5] << 24);
        // Mark each set bit as acknowledged — dedup prevents double-counting. :3
        for (int i = 0; i < 32; i++) {
            if (mask & (1u << i)) {
                if (ackMark((uint8_t)(base + i))) {
                    __atomic_fetch_add((uint32_t*)&s_seq_acked_total, 1u, __ATOMIC_RELAXED);
                }
            }
        }
        return;
    }
    // Anything else — ignore (we're the TX side). :3
}

static void initDisplay(bool flipped);
static bool s_espnow_ready = false;

// =============================================================================
// Wi-Fi + ESP-NOW init task — background so USB CDC stays alive during init.
// Without this, Windows fires "semaphore timeout" when WiFi.mode() blocks. :3
// =============================================================================
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
        s_led_state = LedState::ERROR;
        vTaskDelete(NULL);
        return;
    }
    esp_now_register_recv_cb(onEspNowRecv);
    // NOTE: We do NOT register a send callback — broadcast always returns FAIL
    // and it's useless noise. App-layer ACK via onEspNowRecv() is the real deal. :3
    s_espnow_ready = true;
    s_led_state = LedState::IDLE;
    Serial.println("[espnow] Ready. yippie! :3");
    vTaskDelete(NULL);
}

// =============================================================================
// Display rendering — all into the framebuffer, flushed once per frame. :3
// =============================================================================

// Hz color: red < 50, amber 50-100, cyan 100-200, green >= 200. :3
static uint16_t hzColor(float hz) {
    if (hz < 50.0f)  return COL_RED;
    if (hz < 100.0f) return COL_AMBER;
    if (hz < 200.0f) return COL_CYAN;
    return COL_GREEN;
}

// Render the entire frame into the framebuffer.
// Called every loop() iteration — cheap because it's all RAM writes. :3
//
// Layout:
//   Header (Y 0–15):   [WIFI/----] left | [TX/RDY] right
//   Divider (Y 16):    1px deep purple line
//   Bar (Y 17–136):    X 0–36: vertical fill bar (bottom-to-top, purple)
//   VDiv (X 38):       1px deep purple vertical line
//   Stats (Y 17–136):  X 39–79: RATE/Hz, POS/mm, PKT/loss%
//   Divider (Y 137):   1px deep purple line
//   Footer (Y 138–159): centered "serial: CONN/RDY/IDLE"
//
// The bar uses a cached fill height to avoid clearing the whole bar every frame.
// Only the delta pixels get redrawn in the framebuffer — then the whole thing
// flushes anyway, but the RAM writes are fast. The DMA push is the bottleneck. :3
static int16_t s_last_fill_h = -1;

static void renderFrame(float hz, float position, float loss_pct,
                         bool wifi_up, bool serial_active) {
    // ---- Background: only clear zones that change ----
    // On first call (s_last_fill_h == -1) we clear everything. :3
    bool full_clear = (s_last_fill_h < 0);

    if (full_clear) {
        // Clear entire framebuffer to background. :3
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

    // ---- Header ----
    // Always redraw header — it's only 16px and changes rarely. :3
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

    // ---- Position bar (left column) ----
    // Map 0.0–1.0 to 0–BAR_H pixels, fill from bottom. :3
    int16_t fill_h = (int16_t)(position * BAR_H);
    if (fill_h < 0)     fill_h = 0;
    if (fill_h > BAR_H) fill_h = BAR_H;

    if (fill_h != s_last_fill_h || full_clear) {
        // Clear the whole bar zone and redraw — fast in RAM. :3
        fbFillRect(BAR_X, BAR_Y, BAR_W, BAR_H, COL_BAR_EMPTY);
        if (fill_h > 0) {
            int16_t bar_top = BAR_Y + (BAR_H - fill_h);
            fbFillRect(BAR_X, bar_top, BAR_W, fill_h, COL_BAR_FILL);
        }
        s_last_fill_h = fill_h;
    }

    // ---- Stats panel (right column) ----
    // Clear stats zone — 41×120px. :3
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
        // 260mm travel — display as integer mm. :3
        snprintf(pos_buf, sizeof(pos_buf), "%3d", (int)(position * 260.0f));
        fbPrint(STATS_X + 2, CONTENT_Y + 52, pos_buf, COL_TEAL, COL_BG, 2);
    }

    // PKT section — label at Y+82, value at Y+92
    // Loss color: green=0%, amber<5%, red>=5%. :3
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

    // ---- Dividers (redraw over bar/stats in case they got clobbered) ----
    fbHLine(0, HDR_DIV_Y, FB_W, COL_DIVIDER);
    fbHLine(0, FTR_DIV_Y, FB_W, COL_DIVIDER);
    fbVLine(DIV_X, CONTENT_Y, CONTENT_H, COL_DIVIDER);

    // ---- Footer ----
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
            state_str = "INIT";
            state_col = COL_AMBER;
        }
        // "serial: CONN" = 12 chars × 6px = 72px → center at x=4. :3
        int16_t fy = FTR_Y + (FTR_H - 7) / 2;  // vertically center the 7px text
        fbPrint(4,  fy, label,     COL_LABEL, COL_BG, 1);
        fbPrint(46, fy, state_str, state_col, COL_BG, 1);
    }
}

// =============================================================================
// Full display init — called at boot and on rotation change. :3
// =============================================================================
static void initDisplay(bool flipped) {
    tft.setRotation(flipped ? 0 : 2);

    // Clear framebuffer and force full redraw on next renderFrame(). :3
    memset(s_fb, 0, sizeof(s_fb));
    s_last_fill_h = -1;

    // Push the blank frame immediately so the display isn't showing garbage. :3
    fbFlush();
}

// =============================================================================
// Button debounce
// =============================================================================
static uint32_t s_btn_last_ms    = 0;
static bool     s_btn_last_state = HIGH;
static constexpr uint32_t BTN_DEBOUNCE_MS = 50;

// =============================================================================
// setup()
// =============================================================================
void setup() {
    // Bump RX buffer — MFP blasts at 100Hz, default 256-byte buffer fills in ~22ms. :3
    Serial.setRxBufferSize(1024);
    Serial.begin(460800);

    // Disable RTS-triggered reset (belt-and-suspenders with the constructor). :3
    USB_SERIAL_JTAG.chip_rst.usb_uart_chip_rst_dis = 1;

    // GPIO setup
    pinMode(PIN_TFT_BL, OUTPUT);
    digitalWrite(PIN_TFT_BL, HIGH);  // backlight OFF during init
    pinMode(PIN_BUTTON, INPUT_PULLUP);

    // APA102 LED — start dim white (waiting)
    pinMode(PIN_LED_CI, OUTPUT);
    pinMode(PIN_LED_DI, OUTPUT);
    digitalWrite(PIN_LED_CI, LOW);
    digitalWrite(PIN_LED_DI, LOW);
    ledWrite(15, 15, 15);

    // Kick off heavy init in background — keeps USB CDC alive. :3
    xTaskCreate(espNowInitTask, "espnow_init", 4096, NULL, 1, NULL);
}

// =============================================================================
// Serial relay — reads USB CDC, parses T-Code, forwards over ESP-NOW.
//
// LATENCY CRITICAL PATH: Serial.read() → parseTCode() → esp_now_send().
// Everything in this path must be non-blocking and O(1). :3
//
// Packet format: [1-byte seq#][T-Code string]
// The seq# is prepended so the Waveshare can echo it back as an ACK. :3
// =============================================================================
// =============================================================================
// Bundle accumulator — pack 1-4 T-Code commands per ESP-NOW packet, send at
// 100Hz (every 10ms). Each command carries a relative timestamp so the
// Waveshare can replay them with correct inter-command spacing. No jitter. :3
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
// At 333Hz input, 10ms window = ~3-4 commands/bundle.
// At 100Hz send rate, even 26% loss = only ~26 bundles/sec lost.
// Each bundle covers 10ms of commands — a single missed bundle is invisible
// to motion because T-Code I-values are typically 50-200ms. yippie! :3
//
// Jitter analysis:
//   Commands arrive at the Waveshare ~0-10ms before their fire time.
//   rel_ms preserves inter-command spacing exactly.
//   Even a 2ms late arrival still fires in the correct order. :3
// =============================================================================

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

// Flush the accumulated bundle over ESP-NOW. Called from loop() every 10ms. :3
// Packs all queued commands into one packet and blasts it out as broadcast.
// Like stuffing everything in at once — the hole takes the whole fist, not
// one finger at a time. The stomach bulges with the full payload. hehee :3
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
    // Max size: 2 + 4*(1+1+60) = 2 + 248 = 250 bytes — exactly at ESP-NOW limit. :3
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

// Push a T-Code command into the bundle accumulator. Called from flushSerialBuf(). :3
// Does NOT send immediately — waits for the 10ms window to fill up. owo
static void pushToBundle(const char* cmd, uint8_t len) {
    if (len == 0 || len > BUNDLE_CMD_MAXLEN) return;

    // If accumulator is full, flush immediately to make room. :3
    // This shouldn't happen at 333Hz (only ~3-4 cmds per 10ms window) but
    // handles burst cases gracefully — better to send early than drop. uhoh :C
    if (s_bundle_count >= BUNDLE_MAX_CMDS) {
        flushBundle();
    }

    uint32_t now_ms = millis();
    // Open a new window on first command. :3
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

    // Push to bundle accumulator — actual send happens in loop() every 10ms. :3
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

// =============================================================================
// loop() — non-blocking, no delay(). Pumps serial, renders frame, flushes. :3
// =============================================================================
void loop() {
    uint32_t now_ms = millis();

    // ---- LATENCY CRITICAL: drain serial first ----
    pollSerial();

    // Serial-active timeout — clear after 500ms of silence. :3
    if (g_serial_active && (now_ms - s_last_serial_ms) > 500) {
        g_serial_active = false;
    }

    // ---- Button: rotate display 180° on press ----
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

    // ---- Bundle flush — every 10ms = 100Hz send rate ----
    // Packs accumulated T-Code commands into one ESP-NOW broadcast packet.
    // At 333Hz input this bundles ~3-4 commands per packet. At 100Hz send rate
    // even 26% loss only drops ~26 bundles/sec — each covering 10ms of motion.
    // T-Code I-values are 50-200ms so a single missed bundle is invisible. :3
    static uint32_t s_bundle_last_ms = 0;
    if (now_ms - s_bundle_last_ms >= BUNDLE_INTERVAL_MS) {
        s_bundle_last_ms = now_ms;
        flushBundle();
    }

    // ---- Hz + packet loss — once per second ----
    static uint32_t s_hz_last_ms    = 0;
    static uint32_t s_hz_last_count = 0;

    if (now_ms - s_hz_last_ms >= 1000) {
        uint32_t cur    = g_pkt_count;
        g_hz            = (float)(cur - s_hz_last_count);
        s_hz_last_count = cur;
        ackComputeLoss();  // compute loss from the ACK window, reset for next second. :3
        s_hz_last_ms = now_ms;
    }

    // ---- LED state machine ----
    if (!s_espnow_ready) {
        s_led_state = LedState::WAITING;
    } else if (g_serial_active) {
        s_led_state = LedState::WORKING;
    } else {
        s_led_state = LedState::IDLE;
    }
    applyLed(s_led_state);

    // ---- Display: render + flush at 30fps max ----
    // fbFlush() pushes 25,600 bytes over SPI at 40MHz = ~5ms per flush.
    // At 333Hz that would be 333 × 5ms = 1.6 seconds of SPI per second —
    // completely saturated, starving the serial relay path. Throttle to 30fps
    // (every 33ms) so the loop spends <15% of its time on display. :3
    static uint32_t s_disp_last_ms = 0;
    if (s_tft_ptr != nullptr && (now_ms - s_disp_last_ms) >= 33) {
        s_disp_last_ms = now_ms;
        renderFrame(g_hz, g_position, g_loss_pct, s_espnow_ready, g_serial_active);
        fbFlush();
    }
    // No delay() — let it pump freely. The C5 at 240MHz can handle it. :3
}
