/*
 * TcpInterface — an RNS InterfaceImpl that carries Reticulum over TCP/WiFi.
 *
 * Ported from ~/RTNode-HeltecV4/TcpInterface.h with the BOUNDARY_MODE/HAS_RNS
 * guards dropped and send_outgoing adapted to microReticulum cd0338e7, where it
 * returns bool rather than void. The wire framing, the echo-prevention, the
 * SO_LINGER teardown and both MICRORETICULUM_BUGS.md fixes are unchanged:
 *
 *   §8b  _FIXED_MTU = true, so Transport treats HW_MTU as authoritative and
 *        clamps link MTU when forwarding LINKREQUEST. Without it, clamping is
 *        skipped for this interface entirely.
 *   §8c  Frames larger than TCP_IF_HW_MTU are dropped with a diagnostic rather
 *        than silently truncated. A truncated frame delivered to Transport as
 *        if complete corrupts resource segments and stalls the transfer.
 *
 * This is the client-access side of the gateway: local Reticulum clients on
 * WiFi connect *in* to this server. Client mode is retained from the original
 * because it costs one branch, but note that dialling out to a shared rnsd is
 * exactly what invalidates the milestone-5 end-to-end test — if both sites
 * peer with the same instance, Reticulum routes around the mesh and the test
 * proves nothing. Server mode is the default for that reason.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Based on microReticulum_Firmware by Mark Qvist.
 */
#ifndef RNS_GATEWAY_TCP_INTERFACE_H
#define RNS_GATEWAY_TCP_INTERFACE_H

// microReticulum before any MeshCore header — MeshCore.h #defines PUB_KEY_SIZE
// and friends as bare macros that collide with RNS::Type's constants.
#include <microReticulum/Interface.h>
#include <microReticulum/Transport.h>
#include <microReticulum/Bytes.h>
#include <microReticulum/Log.h>

#include <WiFi.h>
#include <lwip/sockets.h>   // SO_LINGER — force RST to free lwIP PCBs immediately

#include "MeshCoreTunnelCodec.h"   // ptype_name/dtype_name for frame logging

// ─── TCP Interface Configuration ─────────────────────────────────────────────
#ifndef TCP_IF_DEFAULT_PORT
  #define TCP_IF_DEFAULT_PORT    4242
#endif
#ifndef TCP_IF_MAX_CLIENTS
  // Sized for a small site: a household's worth of clients. Each slot costs a
  // TCP_IF_HW_MTU rx buffer plus lwIP's own per-PCB send/receive buffers, and
  // at ~300 bps of mesh backhaul they all contend for the same tiny pipe.
  #define TCP_IF_MAX_CLIENTS     4
#endif
#define TCP_IF_HW_MTU            1064
#define TCP_IF_CONNECT_TIMEOUT   6000    // ms
#define TCP_IF_WRITE_TIMEOUT     2000    // ms — short to avoid WDT
// Idle reaper for clients that vanished without a FIN (WiFi drop, sleep) —
// without it, ghost sockets exhaust the client slots. But it must outlast a
// HEALTHY client's longest silence: Python RNS clients keepalive constantly,
// while some implementations (Columba on iOS, observed 2026-08-23) send
// nothing when idle and were being reaped every 2 minutes. 10 min reclaims
// ghosts fast enough for 4 slots while leaving quiet-but-live clients alone.
#define TCP_IF_READ_TIMEOUT      600000  // ms — 10 minutes
#define TCP_IF_RECONNECT_MIN     10000   // ms — initial reconnect interval
#define TCP_IF_RECONNECT_MAX     120000  // ms — max backoff (2 minutes)
#define TCP_IF_KEEPALIVE_INTERVAL 30000  // ms — empty HDLC frames keep the link alive

// HDLC-like framing for TCP (matches Python RNS and Reticulum-rust tcp_interface)
#ifndef HDLC_FLAG
  #define HDLC_FLAG  0x7E
#endif
#ifndef HDLC_ESC
  #define HDLC_ESC   0x7D
#endif
#ifndef HDLC_ESC_MASK
  #define HDLC_ESC_MASK 0x20
#endif

enum TcpIfMode {
    TCP_IF_MODE_SERVER = 0,  // Listen for incoming client connections
    TCP_IF_MODE_CLIENT = 1,  // Connect out to an rnsd TCP server
};

struct TcpClient {
    WiFiClient client;
    uint32_t   last_activity;
    bool       active;
    // HDLC deframe state
    bool       in_frame;
    bool       escape;
    bool       truncated;
    uint8_t    rxbuf[TCP_IF_HW_MTU];
    uint16_t   rxlen;
};

class TcpInterface : public RNS::InterfaceImpl {
public:
    TcpInterface(TcpIfMode mode, uint16_t port = TCP_IF_DEFAULT_PORT,
                 const char* target_host = nullptr, uint16_t target_port = 0,
                 const char* name = "TCPInterface")
        : RNS::InterfaceImpl(name),
          _mode(mode),
          _port(port),
          _target_port(target_port),
          _server(nullptr),
          _num_clients(0),
          _last_reconnect(0),
          _last_keepalive(0),
          _reconnect_interval(TCP_IF_RECONNECT_MIN),
          _read_timeout(TCP_IF_READ_TIMEOUT),
          _resolved_ip((uint32_t)0),
          _consecutive_failures(0),
          _started(false)
    {
        _IN = true;
        _OUT = true;
        _HW_MTU = TCP_IF_HW_MTU;
        // MICRORETICULUM_BUGS.md §8b: tell Transport this interface has a known
        // fixed MTU, which is what enables link MTU clamping on forwarded
        // LINKREQUEST packets. Left false, clamping is skipped for us entirely.
        _FIXED_MTU = true;
        // TCP links are effectively 10 Mbps+. A realistic bitrate lets Transport
        // prefer the TCP path over LoRa when both exist for a destination —
        // which is exactly what we want for client-to-client traffic at a site.
        // announce_cap = 2% keeps announce flooding in check.
        _bitrate = 10000000;
        _announce_cap = 2.0;
        if (target_host != nullptr) {
            strncpy(_target_host, target_host, sizeof(_target_host) - 1);
            _target_host[sizeof(_target_host) - 1] = '\0';
        } else {
            _target_host[0] = '\0';
        }
        for (int i = 0; i < TCP_IF_MAX_CLIENTS; i++) {
            _clients[i].active = false;
            _clients[i].in_frame = false;
            _clients[i].escape = false;
            _clients[i].truncated = false;
            _clients[i].rxlen = 0;
            _clients[i].last_activity = 0;
        }
    }

    virtual ~TcpInterface() {
        stop();
    }

    // ─── Lifecycle ───────────────────────────────────────────────────────────
    virtual bool start() override {
        if (_started) return true;

        if (_mode == TCP_IF_MODE_SERVER) {
            _server = new WiFiServer(_port, TCP_IF_MAX_CLIENTS);
            _server->begin();
            _server->setNoDelay(true);
            // MESH_DEBUG_PRINTLN compiles to nothing without -D MESH_DEBUG=1;
            // anything that must always be visible uses Serial directly.
            slog("[TcpIF] Server listening on port %d (max %d clients)\r\n",
                          _port, TCP_IF_MAX_CLIENTS);
            _started = true;
        } else {
            _started = true;
            _connect_client();
        }
        // The two-board bridge's client interface sets this in start(); status
        // reporting (and anything upstream later gates on online()) needs it.
        _online = _started;
        return _started;
    }

    virtual void stop() override {
        for (int i = 0; i < TCP_IF_MAX_CLIENTS; i++) {
            if (_clients[i].active) {
                // Force RST to free lwIP PCBs immediately (no TIME_WAIT)
                int fd = _clients[i].client.fd();
                if (fd >= 0) {
                    struct linger lin;
                    lin.l_onoff = 1;
                    lin.l_linger = 0;
                    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lin, sizeof(lin));
                }
                _clients[i].client.stop();
                _clients[i].client = WiFiClient();
                _clients[i].active = false;
            }
        }
        if (_server) {
            _server->end();
            delete _server;
            _server = nullptr;
        }
        _started = false;
        _online = false;
        _num_clients = 0;
    }

    // ─── Serviced from the RNS task, alongside Reticulum::loop() ─────────────
    virtual void loop() override {
        if (!_started) return;

        // Accept new connections in server mode
        if (_mode == TCP_IF_MODE_SERVER && _server) {
            WiFiClient newClient = _server->available();
            if (newClient) {
                _accept_client(newClient);
            }
        }

        // Client mode reconnection (with WiFi check + exponential backoff)
        if (_mode == TCP_IF_MODE_CLIENT && _num_clients == 0) {
            uint32_t now = millis();
            if (now - _last_reconnect >= _reconnect_interval) {
                if (WiFi.status() == WL_CONNECTED) {
                    _connect_client();
                } else {
                    // WiFi not connected — skip the TCP attempt, just re-arm
                    _last_reconnect = now;
                }
            }
        }

        // Keepalive (empty HDLC frames) prevents the read timeout firing on
        // either side of an idle link.
        if (_num_clients > 0) {
            uint32_t now = millis();
            if (now - _last_keepalive >= TCP_IF_KEEPALIVE_INTERVAL) {
                _last_keepalive = now;
                uint8_t ka[] = { HDLC_FLAG, HDLC_FLAG };
                for (int i = 0; i < TCP_IF_MAX_CLIENTS; i++) {
                    if (_clients[i].active && _clients[i].client.connected()) {
                        _clients[i].client.write(ka, 2);
                    }
                }
            }
        }

        // Process incoming data from all active clients
        for (int i = 0; i < TCP_IF_MAX_CLIENTS; i++) {
            if (!_clients[i].active) continue;

            if (!_clients[i].client.connected()) {
                _cleanup_client(i, "disconnected");
                continue;
            }

            // Check read timeout (0 = disabled)
            if (_read_timeout > 0 &&
                _clients[i].last_activity > 0 &&
                (millis() - _clients[i].last_activity) > _read_timeout) {
                _cleanup_client(i, "read timeout");
                continue;
            }

            while (_clients[i].client.available()) {
                uint8_t byte = _clients[i].client.read();
                _clients[i].last_activity = millis();
                _hdlc_deframe(i, byte);
            }
        }
    }

    virtual inline std::string toString() const override {
        return "TcpInterface[" + _name + "/" +
               (_mode == TCP_IF_MODE_SERVER ? ":" + std::to_string(_port)
                                            : std::string(_target_host) + ":" +
                                              std::to_string(_target_port)) + "]";
    }

    // ─── Diagnostics ─────────────────────────────────────────────────────────
    int  clientCount() const { return _num_clients; }
    bool isStarted()   const { return _started; }
    bool isConnected() const { return _num_clients > 0; }
    void setReadTimeout(uint32_t timeout_ms) { _read_timeout = timeout_ms; }
    // Cumulative frame counts across the TCP boundary. These exist because a
    // client's packet can arrive here and then die silently inside Transport —
    // without a counter at this boundary, "the phone never sent it" and "the
    // board swallowed it" produce identical logs.
    uint32_t rx_frames() const { return _rx_frames; }
    uint32_t tx_frames() const { return _tx_frames; }

protected:
    // ─── RNS InterfaceImpl: outgoing packet from RNS Transport ───────────────
    // Returns bool on microReticulum cd0338e7 (it was void in older trees).
    virtual bool send_outgoing(const RNS::Bytes& data) override {
        if (!_started || _num_clients == 0) return false;

        // HDLC frame the data
        uint8_t frame_buf[TCP_IF_HW_MTU * 2 + 4]; // worst case: every byte escaped + 2 flags
        uint16_t flen = 0;

        frame_buf[flen++] = HDLC_FLAG;
        for (size_t i = 0; i < data.size(); i++) {
            uint8_t b = data.data()[i];
            if (b == HDLC_FLAG || b == HDLC_ESC) {
                frame_buf[flen++] = HDLC_ESC;
                frame_buf[flen++] = b ^ HDLC_ESC_MASK;
            } else {
                frame_buf[flen++] = b;
            }
            if (flen >= sizeof(frame_buf) - 4) break; // safety
        }
        frame_buf[flen++] = HDLC_FLAG;

        // Send to all connected clients EXCEPT the one that sent this packet.
        // Echo prevention: if this send_outgoing was triggered by Transport
        // forwarding a packet received from client N, skipping client N stops
        // an echo-back that floods TCP buffers and stalls resource transfers.
        bool sent = false;
        for (int i = 0; i < TCP_IF_MAX_CLIENTS; i++) {
            if (i == _last_rx_client_idx) {
                continue;  // Don't echo back to sender
            }
            if (_clients[i].active && _clients[i].client.connected()) {
                size_t written = _clients[i].client.write(frame_buf, flen);
                if (written == 0) {
                    _cleanup_client(i, "write failed");
                } else {
                    sent = true;
                    if (written < flen) {
                        slog("[TcpIF] PARTIAL write to client %d: %u/%u bytes\r\n",
                                      i, (unsigned)written, (unsigned)flen);
                    }
                }
            }
        }
        yield(); // feed the WDT between TCP writes and RNS processing

        if (sent) _tx_frames++;

        // Post-send housekeeping
        InterfaceImpl::handle_outgoing(data);
        return sent;
    }

    // ─── RNS InterfaceImpl: incoming packet to RNS Transport ─────────────────
    virtual void handle_incoming(const RNS::Bytes& data) override {
        TRACEF("TcpInterface.handle_incoming: (%u bytes)", data.size());
        InterfaceImpl::handle_incoming(data);
    }

private:
    // ─── Cleanup a client slot, freeing all lwIP resources ───────────────────
    void _cleanup_client(int idx, const char* reason) {
        TcpClient& c = _clients[idx];
        if (!c.active) return;

        uint32_t heap_before = ESP.getFreeHeap();

        // SO_LINGER with timeout 0 forces RST instead of FIN, which skips
        // TIME_WAIT and immediately frees the lwIP PCB and its send/receive
        // buffers (~2-4 KB each). On a 2 MB-PSRAM node with WiFi and the radio
        // sharing the internal heap, that matters.
        int fd = c.client.fd();
        if (fd >= 0) {
            struct linger lin;
            lin.l_onoff = 1;
            lin.l_linger = 0;
            setsockopt(fd, SOL_SOCKET, SO_LINGER, &lin, sizeof(lin));
        }

        c.client.stop();
        c.client = WiFiClient();  // Release any residual shared_ptr state
        c.active = false;
        c.in_frame = false;
        c.escape = false;
        c.truncated = false;
        c.rxlen = 0;
        _num_clients--;

        uint32_t heap_after = ESP.getFreeHeap();
        slog("[TcpIF] Client %d %s (heap: %u -> %u, delta: %+d)\r\n",
                      idx, reason, heap_before, heap_after,
                      (int)(heap_after - heap_before));
    }

    // ─── HDLC byte-level deframing ──────────────────────────────────────────
    void _hdlc_deframe(int idx, uint8_t byte) {
        TcpClient& c = _clients[idx];

        if (byte == HDLC_FLAG) {
            if (c.in_frame && c.rxlen > 0) {
                // MICRORETICULUM_BUGS.md §8c: a frame that overran the buffer is
                // dropped entirely. Delivering the truncated head to Transport as
                // though it were whole corrupts resource segments and hashmap
                // updates, and the transfer stalls rather than failing loudly.
                if (c.truncated) {
                    slog("[TcpIF] DROPPED oversized frame from client %d (>%d bytes, buffered %u)\r\n",
                                  idx, TCP_IF_HW_MTU, c.rxlen);
                    c.truncated = false;
                    c.rxlen = 0;
                } else {
                    // End of frame — deliver to RNS. _last_rx_client_idx lets
                    // send_outgoing() skip echoing this packet back to its
                    // sender; the whole chain (handle_incoming → Transport::
                    // inbound → transmit → send_outgoing) is synchronous, so
                    // the scoped set/clear below is safe.
                    RNS::Bytes data(c.rxbuf, c.rxlen);
                    _rx_frames++;
                    // One line per inbound frame, BEFORE Transport sees it.
                    // Transport drops packets for unknown destinations with no
                    // logging at all; this line is the only record that the
                    // frame existed. byte0 = RNS flags, then hops, then the
                    // 16-byte destination hash.
                    if (c.rxlen >= 6) {
                        INFOF("TcpIF: rx#%u %s/%s %u bytes dst=%02x%02x%02x%02x",
                              (unsigned)_rx_frames,
                              MeshCoreTunnel::ptype_name(c.rxbuf[0]),
                              MeshCoreTunnel::dtype_name(c.rxbuf[0] >> 2),
                              (unsigned)c.rxlen,
                              c.rxbuf[2], c.rxbuf[3], c.rxbuf[4], c.rxbuf[5]);
                    }
                    _last_rx_client_idx = idx;
                    handle_incoming(data);
                    _last_rx_client_idx = -1;
                    c.rxlen = 0;
                }
            }
            c.in_frame = true;
            c.escape = false;
            c.truncated = false;
            c.rxlen = 0;
        } else if (c.in_frame) {
            if (c.escape) {
                byte ^= HDLC_ESC_MASK;
                c.escape = false;
                if (c.rxlen < TCP_IF_HW_MTU) {
                    c.rxbuf[c.rxlen++] = byte;
                } else {
                    c.truncated = true;
                }
            } else if (byte == HDLC_ESC) {
                c.escape = true;
            } else {
                if (c.rxlen < TCP_IF_HW_MTU) {
                    c.rxbuf[c.rxlen++] = byte;
                } else {
                    c.truncated = true;
                }
            }
        }
    }

    // ─── Accept a new server-mode client ─────────────────────────────────────
    void _accept_client(WiFiClient& newClient) {
        for (int i = 0; i < TCP_IF_MAX_CLIENTS; i++) {
            if (!_clients[i].active) {
                // Defensive: force-release any residual lwIP resources in this
                // slot before assigning the new client (prevents PCB leaks).
                int fd = _clients[i].client.fd();
                if (fd >= 0) {
                    struct linger lin;
                    lin.l_onoff = 1;
                    lin.l_linger = 0;
                    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lin, sizeof(lin));
                    _clients[i].client.stop();
                }
                _clients[i].client = WiFiClient();  // Reset to clean state

                _clients[i].client = newClient;
                _clients[i].client.setNoDelay(true);
                _clients[i].client.setTimeout(TCP_IF_WRITE_TIMEOUT / 1000);
                _clients[i].active = true;
                _clients[i].in_frame = false;
                _clients[i].escape = false;
                _clients[i].truncated = false;
                _clients[i].rxlen = 0;
                _clients[i].last_activity = millis();
                _num_clients++;
                slog("[TcpIF] Client %d connected from %s\r\n",
                              i, _clients[i].client.remoteIP().toString().c_str());
                return;
            }
        }
        slogln("[TcpIF] Max clients reached, rejecting connection");
        newClient.stop();
    }

    // ─── Client-mode outbound connection ─────────────────────────────────────
    void _connect_client() {
        if (_target_host[0] == '\0') {
            slogln("[TcpIF] No target host configured for client mode");
            return;
        }

        WiFiClient client;
        client.setTimeout(TCP_IF_CONNECT_TIMEOUT / 1000);

        bool connected = false;

        // Try the cached IP first — avoids a DNS lookup on every reconnect.
        if (_resolved_ip != (uint32_t)0) {
            slog("[TcpIF] Connecting to %s:%d (cached IP)...\r\n", _target_host, _target_port);
            connected = client.connect(_resolved_ip, _target_port);
            if (!connected) {
                _resolved_ip = (uint32_t)0;
                slogln("[TcpIF] Cached IP failed, retrying with DNS");
            }
        }

        if (!connected) {
            slog("[TcpIF] Connecting to %s:%d (DNS)...\r\n", _target_host, _target_port);
            IPAddress resolved;
            if (WiFi.hostByName(_target_host, resolved)) {
                _resolved_ip = resolved;
                slog("[TcpIF] Resolved %s -> %s\r\n", _target_host, resolved.toString().c_str());
                connected = client.connect(resolved, _target_port);
            } else {
                slog("[TcpIF] DNS failed for %s\r\n", _target_host);
            }
        }

        if (connected) {
            client.setNoDelay(true);
            client.setTimeout(TCP_IF_WRITE_TIMEOUT / 1000);
            _clients[0].client = client;
            _clients[0].active = true;
            _clients[0].in_frame = false;
            _clients[0].escape = false;
            _clients[0].truncated = false;
            _clients[0].rxlen = 0;
            _clients[0].last_activity = millis();
            _num_clients = 1;
            _consecutive_failures = 0;
            _reconnect_interval = TCP_IF_RECONNECT_MIN;
            slog("[TcpIF] Connected to %s:%d\r\n", _target_host, _target_port);
        } else {
            _consecutive_failures++;
            // Exponential backoff: 10s -> 20s -> 40s -> 80s -> 120s (max)
            _reconnect_interval = _reconnect_interval * 2;
            if (_reconnect_interval > TCP_IF_RECONNECT_MAX) {
                _reconnect_interval = TCP_IF_RECONNECT_MAX;
            }
            slog("[TcpIF] Failed to connect to %s:%d (attempt %d, next retry in %ds)\r\n",
                          _target_host, _target_port, _consecutive_failures,
                          _reconnect_interval / 1000);
        }
        _last_reconnect = millis();
    }

    // ─── Member variables ────────────────────────────────────────────────────
    TcpIfMode   _mode;
    uint16_t    _port;
    char        _target_host[64];
    uint16_t    _target_port;
    WiFiServer* _server;
    TcpClient   _clients[TCP_IF_MAX_CLIENTS];
    int         _num_clients;
    uint32_t    _last_reconnect;
    uint32_t    _last_keepalive;
    uint32_t    _reconnect_interval;
    uint32_t    _read_timeout;
    IPAddress   _resolved_ip;
    uint16_t    _consecutive_failures;
    bool        _started;
    int         _last_rx_client_idx = -1;  // echo prevention: which client is delivering an inbound frame
    uint32_t    _rx_frames = 0;   // frames delivered from clients into Transport
    uint32_t    _tx_frames = 0;   // frames written out to at least one client
};

#endif // RNS_GATEWAY_TCP_INTERFACE_H
