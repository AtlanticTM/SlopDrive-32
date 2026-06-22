// Shared protocol structs and networking logic for the multi-device SlopDrive-32 network.
// This library is hardware-agnostic and is compiled into every node environment.

#ifndef SHARED_PROTOCOL_H
#define SHARED_PROTOCOL_H

#include <cstdint>
#include <cstring>

// Network roles for the three-node architecture.
enum class NodeRole : uint8_t {
    MAIN_CONTROLLER = 0,  // ESP32-S3: orchestrates the whole scene
    RECEIVER = 1,         // ESP32-C5 Waveshare: takes it deep
    TRANSMITTER = 2       // ESP32-C5 LilyGO T-Dongle: gives the commands
};

// Message types that nodes exchange over the shared transport.
enum class MessageType : uint8_t {
    HEARTBEAT = 0,
    MOTION_COMMAND = 1,
    MOTION_STATE = 2,
    CONFIG_UPDATE = 3,
    EMERGENCY_STOP = 4
};

// Compact motion command payload, designed for raw binary streaming.
// All positions are normalized [0.0f, 1.0f] and speeds are in units/sec.
struct MotionCommand {
    float targetPosition;   // Where we want the actuator to end up
    float speed;            // How fast we want it to get there
    float acceleration;     // How hard we slam it at the start
    uint32_t timestampMs;   // Sender's local millis() for latency tracking
};

// State report broadcast back to the main controller.
struct MotionState {
    float currentPosition;
    float currentSpeed;
    uint8_t isHomed;
    uint8_t faultFlags;
    uint32_t timestampMs;
};

// Generic packet header used for framing over UDP/TCP/WebSocket/Serial.
struct PacketHeader {
    uint8_t magic[2];       // {'S', 'D'} for SlopDrive
    uint8_t version;      // Protocol version
    MessageType type;
    NodeRole senderRole;
    uint16_t payloadLength;
    uint16_t checksum;      // Simple sum-of-bytes checksum
};

static constexpr uint8_t PACKET_MAGIC[2] = {'S', 'D'};
static constexpr uint8_t PROTOCOL_VERSION = 1;

// Helper to compute a simple checksum over a byte buffer.
inline uint16_t computeChecksum(const uint8_t* data, uint16_t length) {
    uint16_t sum = 0;
    for (uint16_t i = 0; i < length; ++i) {
        sum += data[i];
    }
    return sum;
}

#endif // SHARED_PROTOCOL_H
