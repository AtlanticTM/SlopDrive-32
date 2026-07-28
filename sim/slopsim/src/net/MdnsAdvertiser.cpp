// MdnsAdvertiser — Windows native-responder implementation (see
// MdnsAdvertiser.h for the discovery contract).

#include "net/MdnsAdvertiser.h"

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windns.h>

#include <string>

namespace slopsim {

namespace {

// The IPv4 this host would route LAN traffic from (the address SRV/A should
// carry). gethostname/getaddrinfo enumeration is a trap here: it happily
// returns APIPA 169.254.x.x from dead adapters first (field-hit on this very
// machine). The UDP-connect trick asks the ROUTING TABLE instead: connect()
// on a datagram socket sends nothing but binds the source address the kernel
// would actually use to reach the internet — that's the LAN NIC.
// Winsock is already up (ix::initNetSystem in main).
bool firstLanIp4(IP4_ADDRESS& out) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &dst.sin_addr);
    bool found = false;
    if (connect(s, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) == 0) {
        sockaddr_in local{};
        int len = sizeof(local);
        if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
            const uint32_t be = local.sin_addr.S_un.S_addr;
            const bool loopback = (be & 0xFF) == 127;
            const bool apipa = (be & 0xFFFF) == 0xFEA9;  // 169.254/16 (LE bytes)
            if (!loopback && !apipa) {
                out = be;  // IP4_ADDRESS is network byte order
                found = true;
            }
        }
    }
    closesocket(s);
    return found;
}

void CALLBACK onRegistered(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance) {
    auto* log = static_cast<SessionLog*>(context);
    if (log) {
        if (status == ERROR_SUCCESS) {
            log->logf('I', "mdns: registered slopsim._slopsync._tcp.local");
        } else {
            log->logf('W', "mdns: registration failed (status %lu)", (unsigned long)status);
        }
    }
    if (instance) DnsServiceFreeInstance(instance);  // callback owns this copy
}

}  // namespace

bool MdnsAdvertiser::begin(uint16_t wsPort, SessionLog* log) {
    _log = log;

    IP4_ADDRESS ip4 = 0;
    if (!firstLanIp4(ip4)) {
        if (log) log->logf('W', "mdns: no LAN IPv4 found — not advertising");
        return false;
    }

    wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD len = MAX_COMPUTERNAME_LENGTH + 1;
    GetComputerNameW(computer, &len);
    std::wstring hostName = std::wstring(computer) + L".local";

    // TXT records mirror the firmware's (WifiLink.cpp): proto + fw.
    PCWSTR keys[] = {L"proto", L"fw"};
    PCWSTR values[] = {L"slopsync.v1", L"slopsim-0.2.0"};

    PDNS_SERVICE_INSTANCE inst = DnsServiceConstructInstance(
        L"slopsim._slopsync._tcp.local", hostName.c_str(), &ip4, nullptr, wsPort,
        /*priority*/ 0, /*weight*/ 0, /*props*/ 2, keys, values);
    if (!inst) {
        if (log) log->logf('W', "mdns: DnsServiceConstructInstance failed");
        return false;
    }
    _instance = inst;

    DNS_SERVICE_REGISTER_REQUEST req{};
    req.Version = DNS_QUERY_REQUEST_VERSION1;
    req.InterfaceIndex = 0;  // all interfaces
    req.pServiceInstance = inst;
    req.pRegisterCompletionCallback = &onRegistered;
    req.pQueryContext = log;
    req.unicastEnabled = FALSE;

    const DWORD rc = DnsServiceRegister(&req, nullptr);
    if (rc != DNS_REQUEST_PENDING) {
        if (log) log->logf('W', "mdns: DnsServiceRegister failed (%lu)", (unsigned long)rc);
        DnsServiceFreeInstance(inst);
        _instance = nullptr;
        return false;
    }
    _registered = true;
    if (log) {
        log->logf('I', "mdns: advertising slopsim._slopsync._tcp :%u (host %ls, ip %u.%u.%u.%u)",
                  unsigned(wsPort), hostName.c_str(), unsigned(ip4 & 0xFF),
                  unsigned((ip4 >> 8) & 0xFF), unsigned((ip4 >> 16) & 0xFF),
                  unsigned((ip4 >> 24) & 0xFF));
    }
    return true;
}

void MdnsAdvertiser::stop() {
    if (!_registered || !_instance) return;
    DNS_SERVICE_REGISTER_REQUEST req{};
    req.Version = DNS_QUERY_REQUEST_VERSION1;
    req.InterfaceIndex = 0;
    req.pServiceInstance = static_cast<PDNS_SERVICE_INSTANCE>(_instance);
    req.pRegisterCompletionCallback = &onRegistered;
    req.pQueryContext = nullptr;  // no log on shutdown path
    DnsServiceDeRegister(&req, nullptr);
    DnsServiceFreeInstance(static_cast<PDNS_SERVICE_INSTANCE>(_instance));
    _instance = nullptr;
    _registered = false;
}

}  // namespace slopsim

#else  // !_WIN32 — stub until an avahi backend exists

namespace slopsim {
bool MdnsAdvertiser::begin(uint16_t, SessionLog* log) {
    if (log) log->logf('W', "mdns: not implemented on this platform yet");
    return false;
}
void MdnsAdvertiser::stop() {}
}  // namespace slopsim

#endif
