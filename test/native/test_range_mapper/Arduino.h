#pragma once

// Host shim for the native suite: config_api.h includes <Arduino.h> purely for
// `constrain` and the fixed-width integer typedefs.
// Constraints:
// - Test scaffolding only. Never add a symbol here to make production code
//   compile on the host; move the code off Arduino instead.

#include <stdint.h>
#include <stddef.h>

#ifndef constrain
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))
#endif
