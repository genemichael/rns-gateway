/*
 * ConfigPortal — captive-portal web configuration, on the ESP32 core's own
 * WebServer/DNSServer/ESPmDNS. No new lib_deps, which keeps the fork's
 * additive-only parity rule cheap.
 *
 * Modelled on RTNode's boundary config portal, with two deliberate changes:
 *
 *   - It is authenticated. RTNode's portal has no auth at all; anyone who
 *     joins the AP can rewrite the WiFi credentials and the channel PSK. HTTP
 *     basic auth against ADMIN_PASSWORD costs almost nothing and closes that.
 *   - Config persists as JSON on SPIFFS (see GatewayConfig) rather than EEPROM
 *     at fixed byte offsets, matching MeshCore's own /prefs.json convention.
 *
 * The radio parameters are NOT owned here. They live in MeshCore's NodePrefs
 * and are edited through the callbacks below, which drive the same `set radio`
 * path the serial CLI uses — one source of truth for a setting whose failure
 * mode is silent.
 *
 * Runs on its own low-priority task. A slow HTTP request must never stall the
 * Reticulum stack on core 0 or MeshCore's cooperative loop on core 1.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RNS_GATEWAY_CONFIG_PORTAL_H
#define RNS_GATEWAY_CONFIG_PORTAL_H

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "GatewayConfig.h"
#include "SerialLog.h"

#ifndef ADMIN_PASSWORD
  #define ADMIN_PASSWORD "password"
#endif
#ifndef PORTAL_USER
  #define PORTAL_USER "admin"
#endif

#define PORTAL_HTTP_PORT     80
#define PORTAL_DNS_PORT      53
#define PORTAL_TASK_STACK    8192
#define PORTAL_TASK_PRIORITY 1      // below the RNS task (2)
#define PORTAL_TASK_CORE     0

// Radio parameters belong to MeshCore. main.cpp supplies these so the portal
// never includes a MeshCore header — MeshCore.h #defines PUB_KEY_SIZE and
// friends as bare macros that collide with RNS::Type's constants.
typedef void (*RadioReadFn)(float& freq, float& bw, uint8_t& sf, uint8_t& cr, uint8_t& txp);
typedef bool (*RadioApplyFn)(float freq, float bw, uint8_t sf, uint8_t cr, uint8_t txp);

class ConfigPortal {
public:
    ConfigPortal() : _server(PORTAL_HTTP_PORT), _cfg(nullptr),
                     _read_radio(nullptr), _apply_radio(nullptr),
                     _dns_active(false), _started(false) {}

    void begin(GatewayConfig& cfg, RadioReadFn read_radio, RadioApplyFn apply_radio) {
        if (_started) return;
        _cfg = &cfg;
        _read_radio = read_radio;
        _apply_radio = apply_radio;

        _server.on("/",       HTTP_GET,  [this]{ handleRoot(); });
        _server.on("/save",   HTTP_POST, [this]{ handleSave(); });
        _server.on("/reset",  HTTP_POST, [this]{ handleReset(); });
        _server.on("/reboot", HTTP_POST, [this]{ handleReboot(); });

        // Captive-portal probes. Each OS uses its own URL and decides "this
        // network needs sign-in" from the response; redirecting them all to /
        // is what makes the config page open by itself on joining the AP.
        _server.on("/generate_204",       HTTP_GET, [this]{ redirectToRoot(); }); // Android
        _server.on("/gen_204",            HTTP_GET, [this]{ redirectToRoot(); }); // Android
        _server.on("/hotspot-detect.html",HTTP_GET, [this]{ redirectToRoot(); }); // Apple
        _server.on("/library/test/success.html", HTTP_GET, [this]{ redirectToRoot(); }); // Apple
        _server.on("/ncsi.txt",           HTTP_GET, [this]{ redirectToRoot(); }); // Windows
        _server.on("/connecttest.txt",    HTTP_GET, [this]{ redirectToRoot(); }); // Windows
        _server.on("/redirect",           HTTP_GET, [this]{ redirectToRoot(); }); // Windows
        _server.onNotFound([this]{ redirectToRoot(); });

        _server.begin();
        slog("[portal] http://%s/ (user '%s')\r\n",
             WiFi.softAPIP().toString().c_str(), PORTAL_USER);

        startDns();
        startMdns();

        _started = true;
        xTaskCreatePinnedToCore(taskTrampoline, "portal", PORTAL_TASK_STACK, this,
                                PORTAL_TASK_PRIORITY, NULL, PORTAL_TASK_CORE);
    }

    // The AP has to exist before the wildcard DNS responder is worth running.
    void startDns() {
        if (_dns_active || !_cfg || !_cfg->ap_enabled) return;
        _dns.setErrorReplyCode(DNSReplyCode::NoError);
        if (_dns.start(PORTAL_DNS_PORT, "*", WiFi.softAPIP())) {
            _dns_active = true;
            slog("[portal] captive DNS answering for * at %s\r\n",
                 WiFi.softAPIP().toString().c_str());
        } else {
            slogln("[portal] captive DNS failed to start");
        }
    }

    void startMdns() {
        if (!_cfg || !_cfg->mdns_enabled || _cfg->mdns_host[0] == '\0') return;
        if (!MDNS.begin(_cfg->mdns_host)) {
            slogln("[portal] mDNS failed to start");
            return;
        }
        MDNS.addService("http", "tcp", PORTAL_HTTP_PORT);
        if (_cfg->tcp_enabled) {
            // Advertised so a client can find the RNS interface by name rather
            // than by a DHCP address that changes on every reconnect.
            MDNS.addService("reticulum", "tcp", _cfg->tcp_port);
        }
        slog("[portal] mDNS: %s.local (reticulum tcp/%u)\r\n",
             _cfg->mdns_host, (unsigned)_cfg->tcp_port);
    }

private:
    static void taskTrampoline(void* arg) { static_cast<ConfigPortal*>(arg)->taskLoop(); }

    void taskLoop() {
        while (true) {
            if (_dns_active) _dns.processNextRequest();
            _server.handleClient();
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    // Every handler goes through this first. Returns true when the request has
    // already been answered with a 401 and the caller must stop.
    bool denied() {
        const char* pwd = (_cfg && _cfg->portal_pwd[0]) ? _cfg->portal_pwd
                                                        : ADMIN_PASSWORD;
        if (!_server.authenticate(PORTAL_USER, pwd)) {
            _server.requestAuthentication(BASIC_AUTH, "RNS Gateway",
                                          "Authentication required");
            return true;
        }
        return false;
    }

    void redirectToRoot() {
        // Absolute URL to the AP address: a relative redirect leaves some
        // captive-portal probes pointed at the domain they originally asked for.
        String url = "http://" + WiFi.softAPIP().toString() + "/";
        _server.sendHeader("Location", url, true);
        _server.send(302, "text/plain", "");
    }

    static String esc(const char* s) {
        String out;
        for (const char* p = s; p && *p; ++p) {
            switch (*p) {
                case '&':  out += "&amp;";  break;
                case '<':  out += "&lt;";   break;
                case '>':  out += "&gt;";   break;
                case '"':  out += "&quot;"; break;
                case '\'': out += "&#39;";  break;
                default:   out += *p;       break;
            }
        }
        return out;
    }

    // The address a client should actually point at. mDNS name when we have
    // one, otherwise whichever interface the browser reached us on.
    String clientTarget() const {
        if (_cfg->mdns_enabled && _cfg->mdns_host[0]) {
            return String(_cfg->mdns_host) + ".local";
        }
        if (_sta_ip().length()) return _sta_ip();
        return WiFi.softAPIP().toString();
    }

    static String _sta_ip() {
        return (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String();
    }

    void handleRoot() {
        if (denied()) return;

        float freq = 0, bw = 0; uint8_t sf = 0, cr = 0, txp = 0;
        if (_read_radio) _read_radio(freq, bw, sf, cr, txp);

        _server.setContentLength(CONTENT_LENGTH_UNKNOWN);
        _server.send(200, "text/html", "");

        _server.sendContent(F(
          "<!doctype html><html><head><meta charset='utf-8'>"
          "<meta name='viewport' content='width=device-width,initial-scale=1'>"
          "<title>RNS Gateway</title><style>"
          "body{font:15px/1.45 system-ui,sans-serif;margin:0;padding:1rem;"
          "max-width:34rem;background:#111;color:#eee}"
          "h1{font-size:1.2rem;margin:.2rem 0 1rem}"
          "h2{font-size:.95rem;margin:1.4rem 0 .4rem;color:#8bd;"
          "border-bottom:1px solid #333;padding-bottom:.2rem}"
          "label{display:block;margin:.5rem 0 .1rem;font-size:.85rem;color:#bbb}"
          "input[type=text],input[type=password],input[type=number]{width:100%;"
          "box-sizing:border-box;padding:.45rem;background:#1c1c1c;color:#eee;"
          "border:1px solid #444;border-radius:4px}"
          "input[type=checkbox]{margin-right:.4rem}"
          ".row{display:flex;gap:.5rem}.row>div{flex:1}"
          "button{margin-top:1.2rem;padding:.6rem 1rem;border:0;border-radius:4px;"
          "background:#2a7;color:#000;font-weight:600;cursor:pointer}"
          "button.warn{background:#a33;color:#fff;margin-left:.5rem}"
          "pre{background:#000;border:1px solid #333;padding:.6rem;overflow-x:auto;"
          "font-size:.8rem;border-radius:4px}"
          ".note{color:#999;font-size:.8rem;margin:.3rem 0}"
          "</style></head><body><h1>RNS Gateway</h1><form method='POST' action='/save'>"
        ));

        // ── Client connection details, first: this is the thing people need ──
        _server.sendContent(F("<h2>Connect a Reticulum client</h2>"
                              "<p class='note'>Add this to your client's config:</p><pre>"));
        _server.sendContent("[[RNS Gateway]]\n  type = TCPClientInterface\n"
                            "  enabled = yes\n  target_host = " + esc(clientTarget().c_str()) +
                            "\n  target_port = " + String(_cfg->tcp_port) + "\n");
        _server.sendContent(F("</pre>"));
        if (_sta_ip().length()) {
            _server.sendContent("<p class='note'>Station address right now: " +
                                esc(_sta_ip().c_str()) +
                                " (DHCP — prefer the mDNS name above).</p>");
        }

        // ── WiFi station ────────────────────────────────────────────────────
        _server.sendContent(F("<h2>WiFi station</h2>"));
        checkbox("sta_en", "Join an existing network", _cfg->sta_enabled);
        text("sta_ssid", "SSID", _cfg->sta_ssid, 32, false);
        text("sta_pwd", "Password", _cfg->sta_pwd, 64, true);

        // ── softAP ──────────────────────────────────────────────────────────
        _server.sendContent(F("<h2>Access point</h2>"));
        checkbox("ap_en", "Host an access point", _cfg->ap_enabled);
        text("ap_ssid", "AP SSID", _cfg->ap_ssid, 32, false);
        text("ap_pwd", "AP password (blank = open network)", _cfg->ap_pwd, 64, true);
        number("ap_chan", "AP channel (used only when no station is joined)",
               _cfg->ap_channel, 1, 13);
        _server.sendContent(F("<p class='note'>One radio: the AP follows the "
                              "station's channel once joined, dropping AP clients "
                              "when it moves.</p>"));

        // ── TCP server ──────────────────────────────────────────────────────
        _server.sendContent(F("<h2>RNS TCP server</h2>"));
        checkbox("tcp_en", "Accept Reticulum clients", _cfg->tcp_enabled);
        number("tcp_port", "Listen port", _cfg->tcp_port, 1, 65535);

        // ── mDNS ────────────────────────────────────────────────────────────
        _server.sendContent(F("<h2>mDNS</h2>"));
        checkbox("mdns_en", "Advertise over mDNS", _cfg->mdns_enabled);
        text("mdns_host", "Hostname (without .local)", _cfg->mdns_host, 30, false);

        // ── Bridge channel ──────────────────────────────────────────────────
        _server.sendContent(F("<h2>MeshCore bridge channel</h2>"));
        text("chan_name", "Channel name", _cfg->chan_name, 30, false);
        text("chan_psk", "Channel PSK (base64)", _cfg->chan_psk, 44, false);
        _server.sendContent(F("<p class='note'>Must match the other sites exactly. "
                              "Channels match by PSK hash, not index.</p>"));

        // ── Tunnel policy ───────────────────────────────────────────────────
        _server.sendContent(F("<h2>Tunnel policy</h2>"));
        number("pr_rate", "Path-request throttle, seconds per destination (0 = off)",
               _cfg->path_req_rate_s, 0, 86400);
        number("ann_rate", "Announce throttle, seconds per destination (0 = off)",
               _cfg->announce_rate_s, 0, 86400);
        text("prop_dests", "Prop destination hashes (32 hex chars, comma-separated)",
             _cfg->prop_dests, 139, false);
        _server.sendContent(F("<p class='note'>Prop destinations are only enforced "
                              "by the prop-restricted firmware variants; other "
                              "builds store but ignore them.</p>"));

        // ── Portal access ───────────────────────────────────────────────────
        _server.sendContent(F("<h2>Portal access</h2>"));
        text("portal_pwd", "New portal password (blank = keep current)", "", 32, true);
        _server.sendContent(F("<p class='note'>Login user is always 'admin'. "
                              "Change the shipped default before deployment.</p>"));

        // ── Radio ───────────────────────────────────────────────────────────
        _server.sendContent(F("<h2>LoRa radio</h2>"));
        _server.sendContent(F("<div class='row'><div>"));
        numberf("freq", "Frequency (MHz)", freq, 3);
        _server.sendContent(F("</div><div>"));
        numberf("bw", "Bandwidth (kHz)", bw, 1);
        _server.sendContent(F("</div></div><div class='row'><div>"));
        number("sf", "SF", sf, 5, 12);
        _server.sendContent(F("</div><div>"));
        number("cr", "CR", cr, 5, 8);
        _server.sendContent(F("</div><div>"));
        number("txp", "TX dBm", txp, 0, 30);
        _server.sendContent(F("</div></div>"));
        _server.sendContent(F("<p class='note'>Must match the target mesh exactly. "
                              "Wrong values fail silently — the node simply hears "
                              "nothing. Stored in MeshCore's prefs, not this file.</p>"));

        _server.sendContent(F(
          "<button type='submit'>Save &amp; reboot</button></form>"
          "<form method='POST' action='/reset' style='display:inline' "
          "onsubmit=\"return confirm('Erase saved configuration and reboot?')\">"
          "<button class='warn' type='submit'>Factory reset</button></form>"
          "</body></html>"
        ));
        _server.sendContent("");
    }

    void handleSave() {
        if (denied()) return;
        GatewayConfig& c = *_cfg;

        // Checkboxes only appear in the POST body when ticked.
        c.sta_enabled  = _server.hasArg("sta_en");
        c.ap_enabled   = _server.hasArg("ap_en");
        c.tcp_enabled  = _server.hasArg("tcp_en");
        c.mdns_enabled = _server.hasArg("mdns_en");

        copyArg("sta_ssid",  c.sta_ssid,  sizeof(c.sta_ssid));
        copyArg("sta_pwd",   c.sta_pwd,   sizeof(c.sta_pwd));
        copyArg("ap_ssid",   c.ap_ssid,   sizeof(c.ap_ssid));
        copyArg("ap_pwd",    c.ap_pwd,    sizeof(c.ap_pwd));
        copyArg("mdns_host", c.mdns_host, sizeof(c.mdns_host));
        copyArg("chan_name", c.chan_name, sizeof(c.chan_name));
        copyArg("chan_psk",  c.chan_psk,  sizeof(c.chan_psk));
        copyArg("prop_dests", c.prop_dests, sizeof(c.prop_dests));

        if (_server.hasArg("pr_rate")) {
            long v = _server.arg("pr_rate").toInt();
            if (v >= 0 && v <= 86400) c.path_req_rate_s = (uint32_t)v;
        }
        if (_server.hasArg("ann_rate")) {
            long v = _server.arg("ann_rate").toInt();
            if (v >= 0 && v <= 86400) c.announce_rate_s = (uint32_t)v;
        }
        // Blank means keep — an empty password would silently fall back to the
        // build default, which is worse than whatever the user had.
        if (_server.hasArg("portal_pwd") && _server.arg("portal_pwd").length() > 0) {
            copyArg("portal_pwd", c.portal_pwd, sizeof(c.portal_pwd));
        }

        if (_server.hasArg("ap_chan")) {
            long v = _server.arg("ap_chan").toInt();
            if (v >= 1 && v <= 13) c.ap_channel = (uint8_t)v;
        }
        if (_server.hasArg("tcp_port")) {
            long v = _server.arg("tcp_port").toInt();
            if (v >= 1 && v <= 65535) c.tcp_port = (uint16_t)v;
        }

        bool cfg_ok = c.save();

        // Radio goes to MeshCore's prefs, not our JSON.
        bool radio_ok = true;
        if (_apply_radio && _server.hasArg("freq") && _server.hasArg("bw") &&
            _server.hasArg("sf") && _server.hasArg("cr")) {
            radio_ok = _apply_radio(_server.arg("freq").toFloat(),
                                    _server.arg("bw").toFloat(),
                                    (uint8_t)_server.arg("sf").toInt(),
                                    (uint8_t)_server.arg("cr").toInt(),
                                    (uint8_t)_server.arg("txp").toInt());
        }

        String body = F("<!doctype html><html><head><meta charset='utf-8'>"
                        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                        "<title>Saved</title></head>"
                        "<body style='font:15px system-ui;background:#111;color:#eee;padding:1rem'>");
        if (cfg_ok && radio_ok) {
            body += F("<h2>Saved</h2><p>Rebooting. If you changed the AP settings "
                      "you will need to rejoin the new network.</p>");
        } else {
            body += F("<h2>Partly saved</h2><p>");
            if (!cfg_ok)   body += F("Writing the config file failed. ");
            if (!radio_ok) body += F("The radio parameters were rejected. ");
            body += F("Check the serial log.</p>");
        }
        body += F("</body></html>");
        _server.send(200, "text/html", body);

        if (cfg_ok) deferredReboot();
    }

    void handleReset() {
        if (denied()) return;
        GatewayConfig::clear();
        _server.send(200, "text/html",
                     F("<!doctype html><body style='font:15px system-ui;background:#111;"
                       "color:#eee;padding:1rem'><h2>Configuration erased</h2>"
                       "<p>Rebooting on build-flag defaults.</p></body>"));
        deferredReboot();
    }

    void handleReboot() {
        if (denied()) return;
        _server.send(200, "text/plain", "Rebooting");
        deferredReboot();
    }

    // Let the response actually reach the browser before the reset.
    void deferredReboot() {
        _server.client().flush();
        vTaskDelay(pdMS_TO_TICKS(400));
        slogln("[portal] rebooting to apply configuration");
        ESP.restart();
    }

    void copyArg(const char* name, char* dst, size_t len) {
        if (!_server.hasArg(name)) return;
        strlcpy(dst, _server.arg(name).c_str(), len);
    }

    // ── Tiny form helpers, streamed rather than concatenated ────────────────
    void checkbox(const char* name, const char* label, bool checked) {
        _server.sendContent(String(F("<label><input type='checkbox' name='")) + name +
                            "'" + (checked ? " checked" : "") + ">" + label + "</label>");
    }
    void text(const char* name, const char* label, const char* value,
              int maxlen, bool secret) {
        _server.sendContent(String(F("<label for='")) + name + "'>" + label + "</label>" +
                            "<input id='" + name + "' name='" + name + "' type='" +
                            (secret ? "password" : "text") + "' maxlength='" + maxlen +
                            "' value='" + esc(value) + "'>");
    }
    void number(const char* name, const char* label, long value, long lo, long hi) {
        _server.sendContent(String(F("<label for='")) + name + "'>" + label + "</label>" +
                            "<input id='" + name + "' name='" + name +
                            "' type='number' min='" + lo + "' max='" + hi +
                            "' value='" + value + "'>");
    }
    void numberf(const char* name, const char* label, float value, int decimals) {
        _server.sendContent(String(F("<label for='")) + name + "'>" + label + "</label>" +
                            "<input id='" + name + "' name='" + name +
                            "' type='text' inputmode='decimal' value='" +
                            String(value, decimals) + "'>");
    }

    WebServer      _server;
    DNSServer      _dns;
    GatewayConfig* _cfg;
    RadioReadFn    _read_radio;
    RadioApplyFn   _apply_radio;
    bool           _dns_active;
    bool           _started;
};

#endif // RNS_GATEWAY_CONFIG_PORTAL_H
