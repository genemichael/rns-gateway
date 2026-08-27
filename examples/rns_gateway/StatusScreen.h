/*
 * StatusScreen — the gateway's OLED UI (128x64 SSD1306, when fitted).
 *
 * Three pages, cycled with the PRG button; RNode-inspired layout with the
 * variant name where RNode puts unsigned.io:
 *
 *   1  STATUS  — network identity, ip:port + TCP state, clients, peers,
 *                radio params, tunnel TX/RX
 *   2  PEERS   — bound gateways: name, R/E capability, last-heard age
 *   3  TUNNEL  — direct/fallback, throttle drops, queues, heap
 *
 * Plus a SETUP page that takes over while no channel PSK is configured, so
 * a fresh device literally displays its own first-boot instructions.
 *
 * Sleep: blanks after DISPLAY_TIMEOUT_MS (60 s); any PRG press wakes it.
 * A press while awake advances the page. No BT indicator: this firmware
 * has no Bluetooth, and a dead icon would be a lie — the corner shows
 * battery instead.
 *
 * Deliberately NOT MeshCore's message/announce screens: the gateway cannot
 * read tunnel content (opaque encrypted RNS) and does not participate in
 * chat. Peers and tunnel internals are the honest equivalents.
 *
 * Service from the RNS task loop only — every data source it touches
 * (MeshCoreInterface, TcpInterface, WiFi, GatewayConfig) is owned there.
 * The mesh-task values it shows (battery mV, prefs) are read-mostly.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RNS_GATEWAY_STATUS_SCREEN_H
#define RNS_GATEWAY_STATUS_SCREEN_H

#include <WiFi.h>
#include <helpers/ui/DisplayDriver.h>
#include <helpers/ui/MomentaryButton.h>
#include <helpers/CommonCLI.h>

#include "GatewayConfig.h"
#include "MeshCoreInterface.h"
#include "TcpInterface.h"

#ifndef RNS_GW_VARIANT_NAME
  #define RNS_GW_VARIANT_NAME "GATEWAY"
#endif
#define DISPLAY_TIMEOUT_MS   60000
#define DISPLAY_REFRESH_MS   1000

class StatusScreen {
public:
    // disp/btn are the variant-provided globals (display, user_btn); the
    // variant constructs the button for the PRG pin (PIN_USER_BTN=0).
    StatusScreen(DisplayDriver& disp, MomentaryButton& btn, GatewayConfig& cfg)
        : _disp(disp), _cfg(cfg), _btn(btn) {}

    // Call once from setup, after display.begin() succeeded.
    void begin(MeshCoreInterface* mc, TcpInterface* tcp,
               mesh::MainBoard* board, NodePrefs* prefs) {
        _mc = mc; _tcp = tcp; _board = board; _prefs = prefs;
        _alive = true;
        _wake_at = millis();
    }

    bool alive() const { return _alive; }

    void loop() {
        if (!_alive) return;
        uint32_t now = millis();

        int ev = _btn.check();
        if (ev == BUTTON_EVENT_CLICK) {
            if (_disp.isOn()) {
                _page = (_page + 1) % PAGE_COUNT;
            } else {
                _disp.turnOn();      // wake shows the page you left
            }
            _wake_at = now;
            _dirty = true;
        }

        if (_disp.isOn() && (now - _wake_at) > DISPLAY_TIMEOUT_MS) {
            _disp.turnOff();
            return;
        }
        if (!_disp.isOn()) return;

        if (_dirty || (now - _last_draw) >= DISPLAY_REFRESH_MS) {
            _last_draw = now;
            _dirty = false;
            draw(now);
        }
    }

private:
    static const int PAGE_COUNT = 3;

    void draw(uint32_t now) {
        _disp.startFrame();
        if (_cfg.chan_psk[0] == 0) { drawSetup(); }
        else if (_page == 0)       { drawStatus(); }
        else if (_page == 1)       { drawPeers(now); }
        else                       { drawTunnel(); }
        _disp.endFrame();
    }

    // Header: variant name left, battery right, rule underneath.
    void drawHeader(const char* title) {
        _disp.setTextSize(1);
        _disp.setColor(UIColor::title_txt);
        _disp.setCursor(0, 0);
        _disp.print(title);

        char right[12];
        uint16_t mv = _board ? _board->getBattMilliVolts() : 0;
        if (mv > 500) {
            int pct = (int)(((int32_t)mv - 3300) * 100 / (4200 - 3300));
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            snprintf(right, sizeof(right), "%d%%", pct);
        } else {
            snprintf(right, sizeof(right), "USB");
        }
        _disp.drawTextRightAlign(128, 0, right);
        _disp.fillRect(0, 10, 128, 1);
    }

    void drawSetup() {
        drawHeader("SETUP");
        _disp.setCursor(0, 16); _disp.print("Join WiFi:");
        _disp.setCursor(0, 26); _disp.print(_cfg.ap_ssid);
        _disp.setCursor(0, 40); _disp.print("http://192.168.4.1/");
        _disp.setCursor(0, 54); _disp.print("Set channel PSK");
    }

    void drawStatus() {
        drawHeader(RNS_GW_VARIANT_NAME);
        char line[26];

        bool sta = (WiFi.status() == WL_CONNECTED);
        if (sta) {
            snprintf(line, sizeof(line), "\x7F %s", WiFi.SSID().c_str());
        } else if (_cfg.ap_enabled) {
            snprintf(line, sizeof(line), "AP %s", _cfg.ap_ssid);
        } else {
            snprintf(line, sizeof(line), "\x7F (joining...)");
        }
        _disp.setCursor(0, 14); _disp.print(line);

        IPAddress ip = sta ? WiFi.localIP() : WiFi.softAPIP();
        snprintf(line, sizeof(line), "%s:%u", ip.toString().c_str(),
                 (unsigned)_cfg.tcp_port);
        _disp.setCursor(0, 24); _disp.print(line);
        bool up = _tcp && _tcp->isStarted();
        if (up) _disp.fillRect(122, 24, 5, 5);
        else    _disp.drawRect(122, 24, 5, 5);

        snprintf(line, sizeof(line), "CLI %d/%d  PEERS %u",
                 _tcp ? _tcp->clientCount() : 0, TCP_IF_MAX_CLIENTS,
                 _mc ? (unsigned)_mc->peer_count() : 0);
        _disp.setCursor(0, 34); _disp.print(line);

        snprintf(line, sizeof(line), "%.3f %.1fk SF%d",
                 _prefs ? _prefs->freq : 0.0f,
                 _prefs ? _prefs->bw : 0.0f,
                 _prefs ? (int)_prefs->sf : 0);
        _disp.setCursor(0, 44); _disp.print(line);

        snprintf(line, sizeof(line), "TX%u RX%u DF%u",
                 _mc ? (unsigned)_mc->rns_tx_packets() : 0,
                 _mc ? (unsigned)_mc->rns_rx_packets() : 0,
                 _mc ? (unsigned)_mc->direct_fallbacks() : 0);
        _disp.setCursor(0, 54); _disp.print(line);
    }

    void drawPeers(uint32_t now) {
        drawHeader("PEERS");
        if (_mc == NULL || _mc->peer_count() == 0) {
            _disp.setCursor(0, 26); _disp.print("(none bound yet)");
            return;
        }
        int y = 14;
        for (size_t i = 0; i < 5; i++) {
            char name[14]; bool rt; uint32_t seen;
            if (!_mc->peer_info(i, name, sizeof(name), rt, seen)) break;
            uint32_t age_s = (now - seen) / 1000;
            char line[26];
            if (age_s < 90)         snprintf(line, sizeof(line), "%c %-12s %lus",  rt ? 'R' : 'E', name, (unsigned long)age_s);
            else if (age_s < 5400)  snprintf(line, sizeof(line), "%c %-12s %lum",  rt ? 'R' : 'E', name, (unsigned long)(age_s / 60));
            else                    snprintf(line, sizeof(line), "%c %-12s %luh",  rt ? 'R' : 'E', name, (unsigned long)(age_s / 3600));
            _disp.setCursor(0, y); _disp.print(line);
            y += 10;
        }
    }

    void drawTunnel() {
        drawHeader("TUNNEL");
        char line[26];
        snprintf(line, sizeof(line), "DIRECT %u  FALLB %u",
                 _mc ? (unsigned)_mc->direct_tx() : 0,
                 _mc ? (unsigned)_mc->direct_fallbacks() : 0);
        _disp.setCursor(0, 14); _disp.print(line);
        snprintf(line, sizeof(line), "ANNDROP %u  OUTQ %u",
                 _mc ? (unsigned)_mc->announce_suppressed() : 0,
                 _mc ? (unsigned)_mc->outq_depth() : 0);
        _disp.setCursor(0, 24); _disp.print(line);
        snprintf(line, sizeof(line), "ROUTES %u  BIND %u/%u",
                 _mc ? (unsigned)_mc->route_count() : 0,
                 _mc ? (unsigned)_mc->bind_tx() : 0,
                 _mc ? (unsigned)_mc->bind_rx() : 0);
        _disp.setCursor(0, 34); _disp.print(line);
        snprintf(line, sizeof(line), "TCPRX %u TCPTX %u",
                 _tcp ? (unsigned)_tcp->rx_frames() : 0,
                 _tcp ? (unsigned)_tcp->tx_frames() : 0);
        _disp.setCursor(0, 44); _disp.print(line);
        snprintf(line, sizeof(line), "HEAP %uk", (unsigned)(ESP.getFreeHeap() / 1024));
        _disp.setCursor(0, 54); _disp.print(line);
    }

    DisplayDriver&     _disp;
    GatewayConfig&     _cfg;
    MomentaryButton&   _btn;
    MeshCoreInterface* _mc = NULL;
    TcpInterface*      _tcp = NULL;
    mesh::MainBoard*   _board = NULL;
    NodePrefs*         _prefs = NULL;
    bool     _alive = false;
    bool     _dirty = true;
    int      _page = 0;
    uint32_t _wake_at = 0;
    uint32_t _last_draw = 0;
};

#endif // RNS_GATEWAY_STATUS_SCREEN_H
