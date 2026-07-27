# Applied by FetchContent PATCH_COMMAND inside the ixwebsocket source dir on
# fresh population (idempotent via the marker string). Upstream's server
# handshake never echoes Sec-WebSocket-Protocol; RFC 6455 §4.2.2 requires it
# when the client requested one, and strict clients (browsers, python
# websocket-client) hard-fail the connect without it. slopsim pins slopsync.v1.
set(F "ixwebsocket/IXWebSocketHandshake.cpp")
file(READ ${F} C)
if(NOT C MATCHES "slopsim-subprotocol-echo")
    set(ANCHOR [[ss << "Server: " << userAgent() << "\r\n";]])
    set(PATCHED [[ss << "Server: " << userAgent() << "\r\n";

        // slopsim-subprotocol-echo: RFC 6455 §4.2.2 — a server accepting a
        // connection that requested subprotocols MUST echo one, or strict
        // clients (browsers, python websocket-client) fail the handshake.
        // Upstream IXWebSocket omits this; slopsim's hub pins slopsync.v1.
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
    message(STATUS "slopsim: patched IXWebSocket server handshake (subprotocol echo)")
endif()
