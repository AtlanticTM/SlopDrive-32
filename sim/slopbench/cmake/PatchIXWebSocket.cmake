# Applied by FetchContent PATCH_COMMAND inside the ixwebsocket source dir on
# fresh population (idempotent via the marker string). Upstream's server
# handshake never echoes Sec-WebSocket-Protocol; RFC 6455 §4.2.2 requires it
# when the client requested one, and strict clients (browsers) hard-fail the
# connect without it. SlopBench pins slopsync.v1, same as sim/slopsim.
#
# Vendored rather than pointed at sim/slopsim/cmake/PatchIXWebSocket.cmake:
# SlopBench's own FetchContent population is a separate source tree (each
# sim has its own build/ directory), so sharing the file would only add a
# cross-directory dependency for no benefit.
set(F "ixwebsocket/IXWebSocketHandshake.cpp")
file(READ ${F} C)
if(NOT C MATCHES "slopbench-subprotocol-echo")
    # The bare "Server: ..." line appears TWICE in this file: once in the real
    # accept-handshake path (where `headers` is in scope) and once in
    # sendErrorResponse (where it is NOT — CMake's string(REPLACE) rewrites
    # every occurrence, not just the first, so anchoring on the bare line
    # miscompiles the error path). This three-line anchor is unique to the
    # accept path.
    set(ANCHOR [[        ss << "Upgrade: websocket\r\n";
        ss << "Connection: Upgrade\r\n";
        ss << "Server: " << userAgent() << "\r\n";]])
    set(PATCHED [[        ss << "Upgrade: websocket\r\n";
        ss << "Connection: Upgrade\r\n";
        ss << "Server: " << userAgent() << "\r\n";

        // slopbench-subprotocol-echo: RFC 6455 §4.2.2 — a server accepting a
        // connection that requested subprotocols MUST echo one, or strict
        // clients (browsers) fail the handshake. Upstream IXWebSocket omits
        // this; SlopBench's hub pins slopsync.v1.
        {
            std::string protocol = headers["sec-websocket-protocol"];
            auto comma = protocol.find(',');
            if (comma != std::string::npos) protocol = protocol.substr(0, comma);
            while (!protocol.empty() && protocol.front() == ' ') protocol.erase(0, 1);
            while (!protocol.empty() && protocol.back() == ' ') protocol.pop_back();
            if (!protocol.empty())
            {
                ss << "Sec-WebSocket-Protocol: " << protocol << "\r\n";
            }
        }]])
    string(REPLACE "${ANCHOR}" "${PATCHED}" C "${C}")
    file(WRITE ${F} "${C}")
    message(STATUS "slopbench: patched IXWebSocket server handshake (subprotocol echo)")
endif()
