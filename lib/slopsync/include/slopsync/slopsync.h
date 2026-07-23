// slopsync — SlopSync protocol reference implementation (slopsync/1).
// Umbrella header. Spec: docs/slopsync/SPEC.md ; registry: registry.yaml.
// Zero dependencies, hardware-free, no heap in steady state, no threads —
// callers drive update(). See lib/slopsync/library.json.
#pragma once

#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/core/result.hpp"
#include "slopsync/core/clock.hpp"
#include "slopsync/core/rng.hpp"
#include "slopsync/util/byte_io.hpp"
#include "slopsync/util/serial_arithmetic.hpp"
#include "slopsync/wire/frame_header.hpp"
#include "slopsync/wire/crc32.hpp"
#include "slopsync/wire/estop_frame.hpp"
#include "slopsync/wire/serial_cobs.hpp"
#include "slopsync/wire/cbor/cbor_writer.hpp"
#include "slopsync/wire/cbor/cbor_reader.hpp"
#include "slopsync/wire/messages/hello.hpp"
#include "slopsync/wire/messages/welcome.hpp"
// M2+: catalog, stream bundles, transport, channels, session, hub, client.
