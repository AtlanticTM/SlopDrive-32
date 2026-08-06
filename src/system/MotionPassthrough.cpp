// MotionPassthrough -- GPIO-matrix pin-to-pin route for the RP2350's pulses.
// Constraints:
// - PURE MATRIX, no CPU: the RP2350's STEP/DIR enter on the S3BTN header
//   (GPIO1/GPIO2) and leave on the existing drive pins (AIM_PIN_STEP/DIR)
//   via SIG_IN_FUNC208/209, the S3's dedicated pass-through signals
//   (soc/gpio_sig_map.h; the sd-dxy TRM check, resolved 2026-08-06). No PCB
//   trace is cut, which is the whole point.
// - The input path is APB-synchronized: at most 12.5 ns of sampling jitter,
//   far under any step-edge timing the drive cares about (sd-dxy notes).
// - The route DIES with an S3 reset until re-enabled: the pulse path depends
//   on the S3 being up. Accepted constraint (sd-dxy); the RP2350's schedule
//   simply stops landing on the drive until re-enable.
// - Enabling STEALS the drive pins from whatever driver held them. Never
//   enable while FAS is the live backend and commanding motion.

#include "MotionPassthrough.h"

#include <Arduino.h>

#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"

#include "config_api.h"
#include "sloplog/sloplog.h"

namespace {
constexpr gpio_num_t kInStep  = GPIO_NUM_1;   // <- RP2350 GP6 (S3BTN BTN_CLICK)
constexpr gpio_num_t kInDir   = GPIO_NUM_2;   // <- RP2350 GP7 (S3BTN BTN_BACK)
constexpr gpio_num_t kOutStep = gpio_num_t(AIM_PIN_STEP);
constexpr gpio_num_t kOutDir  = gpio_num_t(AIM_PIN_DIR);

void route(gpio_num_t in, gpio_num_t out, uint32_t sig) {
    gpio_set_direction(in, GPIO_MODE_INPUT);
    esp_rom_gpio_connect_in_signal(uint32_t(in), sig, false);
    gpio_set_direction(out, GPIO_MODE_OUTPUT);
    esp_rom_gpio_connect_out_signal(uint32_t(out), sig, false, false);
}
}  // namespace

void motionPassthroughEnable() {
    route(kInStep, kOutStep, SIG_IN_FUNC208_IDX);
    route(kInDir, kOutDir, SIG_IN_FUNC209_IDX);
    SLOGI("motion", "RP2350 pass-through ON: GPIO%d->%d (STEP, sig208), GPIO%d->%d (DIR, sig209)",
          int(kInStep), int(kOutStep), int(kInDir), int(kOutDir));
}

void motionPassthroughDisable() {
    // Detach by routing the constant-low signal to the outputs, then hand the
    // pins back as plain idle GPIO.
    esp_rom_gpio_connect_out_signal(uint32_t(kOutStep), SIG_GPIO_OUT_IDX, false, false);
    esp_rom_gpio_connect_out_signal(uint32_t(kOutDir), SIG_GPIO_OUT_IDX, false, false);
    gpio_set_direction(kOutStep, GPIO_MODE_OUTPUT);
    gpio_set_direction(kOutDir, GPIO_MODE_OUTPUT);
    gpio_set_level(kOutStep, 0);
    gpio_set_level(kOutDir, 0);
    SLOGI("motion", "RP2350 pass-through OFF: drive pins returned to plain GPIO");
}
