/*
 * UdpLog — push every slog() line to a host as a UDP datagram.
 *
 * Bring-up builds only. It exists because the two ways of reading a log
 * from this board both fail in BLE mode: opening USB serial resets it, and
 * with the BLE controller running the ESP32 forces WiFi modem sleep, under
 * which the station answers multicast but drops most inbound unicast — so
 * the portal's /log cannot be fetched. OUTBOUND traffic still works (mDNS
 * replies proved it), so the board pushes instead of being polled. Pyxis
 * uses the same trick for the same reason.
 *
 *   -D RNS_GW_UDP_LOG_HOST='"192.168.0.10"'   (required to enable)
 *   -D RNS_GW_UDP_LOG_PORT=5140               (default)
 *
 * Listen with:  nc -ul 5140
 *
 * One datagram per line, no buffering, no retries. Lines logged before the
 * station has an address are only in the ring and on serial. Never enable
 * in a release env: it is plaintext, and it keeps WiFi up in BLE mode.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RNS_GATEWAY_UDP_LOG_H
#define RNS_GATEWAY_UDP_LOG_H

#include <WiFi.h>
#include <WiFiUdp.h>
#include "SerialLog.h"

#ifndef RNS_GW_UDP_LOG_PORT
  #define RNS_GW_UDP_LOG_PORT 5140
#endif

#ifdef RNS_GW_UDP_LOG_HOST

inline WiFiUDP& _udplog_socket() {
    static WiFiUDP u;
    return u;
}

// Runs under the serial lock from whichever task logged. WiFiUDP is not
// thread-safe on its own; the lock is what serialises it.
inline void _udplog_sink(const char* line, size_t len) {
    if (WiFi.status() != WL_CONNECTED) return;
    WiFiUDP& u = _udplog_socket();
    if (!u.beginPacket(RNS_GW_UDP_LOG_HOST, RNS_GW_UDP_LOG_PORT)) return;
    u.write((const uint8_t*)line, len);
    if (len == 0 || line[len - 1] != '\n') u.write((const uint8_t*)"\r\n", 2);
    u.endPacket();
}

inline void udplog_begin() {
    slog_set_sink(_udplog_sink);
    slog("[udplog] pushing log lines to %s:%d\r\n", RNS_GW_UDP_LOG_HOST, RNS_GW_UDP_LOG_PORT);
}

#else
inline void udplog_begin() {}
#endif

#endif // RNS_GATEWAY_UDP_LOG_H
