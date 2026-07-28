#include "SlopSyncUdpDiscovery.h"

#include <Arduino.h>
// lwIP's OWN socket headers, not the raw POSIX <sys/socket.h>/<netinet/in.h>
// names — mixing those with arduino-esp32's IPAddress.h/lwIP umbrella
// produced a macro clash (IPADDR_NONE/u32_t ordering) at compile time.
// lwip/sockets.h is the ESP-IDF-blessed way to get the BSD socket API here.
#include <lwip/sockets.h>

#include <algorithm>

#include "sloplog/sloplog.h"

namespace slopdrive {

bool SlopSyncUdpDiscovery::begin(const char* hubName, uint64_t hubInstanceId, uint16_t wsPort,
                                  const char* fwVersion, std::span<const std::byte> catalogEtag) {
    _hubName = (hubName != nullptr) ? hubName : "";
    _hubInstanceId = hubInstanceId;
    _wsPort = wsPort;
    _fwVersion = (fwVersion != nullptr) ? fwVersion : "";
    const size_t n = std::min(catalogEtag.size(), _catalogEtag.size());
    for (size_t i = 0; i < n; ++i) _catalogEtag[i] = catalogEtag[i];

    _sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (_sock < 0) {
        SLOGE("slopsync", "UDP discovery: socket() FAILED");
        return false;
    }

    // Non-blocking: poll() runs on the hub task's 5 ms tick and must never
    // wait on a syscall (DOCTRINE.md §2 — no blocking in real-time paths).
    const int flags = fcntl(_sock, F_GETFL, 0);
    fcntl(_sock, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(discovery::kPort);
    if (bind(_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        SLOGE("slopsync", "UDP discovery: bind(%u) FAILED", unsigned(discovery::kPort));
        close(_sock);
        _sock = -1;
        return false;
    }

    SLOGI("slopsync", "UDP discovery responder up on :%u", unsigned(discovery::kPort));
    return true;
}

void SlopSyncUdpDiscovery::poll() {
    if (_sock < 0) return;

    // Bounded per call, same discipline as this file's sibling drain*()
    // functions on the hub task (publishAnomalies, drainLogBridge): a probe
    // storm must not turn one 5 ms tick into an unbounded reply burst.
    for (int i = 0; i < 4; ++i) {
        std::array<std::byte, 32> inbuf{};
        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        const int n = recvfrom(_sock, inbuf.data(), inbuf.size(), 0, reinterpret_cast<sockaddr*>(&from),
                                &fromLen);
        if (n <= 0) break;  // EAGAIN (nothing pending) or a transient error — either way, stop this tick

        auto probe = discovery::parseProbe(std::span<const std::byte>(inbuf.data(), size_t(n)));
        if (!probe.has_value()) {
            ++_probesRejected;
            continue;  // §13.8: bad magic/length rejected SILENTLY, no reply
        }

        const uint32_t nowMs = millis();
        const uint32_t sourceIp = ntohl(from.sin_addr.s_addr);
        if (!_rateLimiter.allow(sourceIp, nowMs, discovery::kReplyRateLimitPerSourceS * 1000u)) {
            ++_probesThrottled;
            continue;
        }

        discovery::ReplyFields f;
        f.nonce = probe->nonce;
        f.hub_name = _hubName;
        f.hub_instance_id = _hubInstanceId;
        f.proto_ver = probe->proto_ver;
        f.ws_port = _wsPort;
        f.fw_version = _fwVersion;
        f.catalog_etag = _catalogEtag;
        f.flags = discovery::buildFlags(_pairingOpen, _wsAvailable);

        std::array<std::byte, discovery::kReplyBytes> outbuf{};
        const size_t outLen = discovery::buildReply(f, outbuf);
        if (outLen == 0) continue;  // cannot happen with a buffer sized to kReplyBytes; defensive only

        sendto(_sock, outbuf.data(), outLen, 0, reinterpret_cast<sockaddr*>(&from), fromLen);
        ++_repliesSent;
        SLOGD_EVERY_MS(5000, "slopsync", "UDP discovery: replied to a probe (rate-limited log)");
    }
}

}  // namespace slopdrive
