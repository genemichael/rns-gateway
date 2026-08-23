/*
 * GatewayConfig — runtime configuration, stored on SPIFFS as JSON.
 *
 * Everything here was a build flag until the first field deployment made that
 * untenable: changing WiFi should not mean a rebuild and a USB cable. The
 * pattern follows RTNode's boundary config — stored values win, build flags are
 * the fallback when nothing has been saved yet — but persists as JSON on SPIFFS
 * rather than EEPROM at fixed byte offsets, because MeshCore already mounts
 * SPIFFS and stores its own NodePrefs as /prefs.json. One storage mechanism,
 * not two.
 *
 * DELIBERATELY NOT HERE: the LoRa radio parameters. Those belong to MeshCore's
 * NodePrefs, are persisted by CommonCLI to /prefs.json, and are reachable over
 * the serial CLI. Duplicating them here would create two sources of truth for
 * a setting whose failure mode is silent (wrong params = the node simply hears
 * nothing). The portal edits them by driving the existing `set radio` CLI path.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RNS_GATEWAY_CONFIG_H
#define RNS_GATEWAY_CONFIG_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>

#include "SerialLog.h"

#define GATEWAY_CONFIG_PATH  "/rns_gateway.json"

// Build-flag fallbacks. These seed the config the first time the node boots
// with no stored file; after that the stored values win.
#ifndef WIFI_SSID
  #define WIFI_SSID  ""
#endif
#ifndef WIFI_PWD
  #define WIFI_PWD   ""
#endif
#ifndef WIFI_AP_SSID
  #define WIFI_AP_SSID  "RNSGateway"
#endif
#ifndef WIFI_AP_PWD
  #define WIFI_AP_PWD   ""
#endif
#ifndef WIFI_AP_CHANNEL
  #define WIFI_AP_CHANNEL  1
#endif
#ifndef RNS_TCP_LISTEN_PORT
  #define RNS_TCP_LISTEN_PORT  4242
#endif
#ifndef MDNS_HOSTNAME
  #define MDNS_HOSTNAME  "rnsgateway"
#endif
#ifndef BRIDGE_CHANNEL_NAME
  #define BRIDGE_CHANNEL_NAME  "RNSTesting"
#endif
#ifndef BRIDGE_CHANNEL_PSK
  #define BRIDGE_CHANNEL_PSK   ""
#endif

struct GatewayConfig {
    // ── WiFi station ────────────────────────────────────────────────────────
    bool     sta_enabled;
    char     sta_ssid[33];
    char     sta_pwd[65];

    // ── softAP (client access, and the config portal itself) ────────────────
    bool     ap_enabled;
    char     ap_ssid[33];
    char     ap_pwd[65];
    // Only used when there is no station to inherit a channel from — one radio
    // means the AP always follows the station's channel once associated.
    uint8_t  ap_channel;

    // ── The device-hosted RNS TCP server ────────────────────────────────────
    bool     tcp_enabled;
    uint16_t tcp_port;

    // ── mDNS, so clients get a stable name instead of a DHCP address ────────
    bool     mdns_enabled;
    char     mdns_host[32];

    // ── Portal admin password ───────────────────────────────────────────────
    // HTTP basic auth for the portal (user is always "admin"). Seeded from the
    // ADMIN_PASSWORD build flag; changeable in the portal so a release user
    // can rotate the shipped default without a rebuild.
    char     portal_pwd[33];

    // ── Tunnel policy ───────────────────────────────────────────────────────
    // Per-destination path-request throttle, seconds. 0 disables the throttle
    // (bring-up only — a fresh-boot client can storm hundreds of requests onto
    // the 300 bps channel). Reference default: 1800.
    uint32_t path_req_rate_s;
    // Per-destination announce rebroadcast throttle, seconds. 0 disables.
    // Reference default: 600. Lower it to speed re-convergence after reboots
    // (paths are RAM-only by design), at the cost of more airtime.
    uint32_t announce_rate_s;
    // Whitelisted prop destination hashes: comma-separated 32-hex-char tokens,
    // up to 4. Only enforced by the *_prop build variants; harmless elsewhere.
    char     prop_dests[140];

    // ── MeshCore bridge channel ─────────────────────────────────────────────
    // Index 0 is MeshCore public and takes no private PSK; a private tunnel
    // channel must be 1-7. addChannel() assigns the next free local slot, and
    // channels match by PSK hash rather than index, so the local index may
    // differ between nodes without breaking anything.
    char     chan_name[32];
    char     chan_psk[45];   // base64 of a 16- or 32-byte key

    void setDefaults() {
        sta_enabled = (sizeof(WIFI_SSID) > 1);
        strlcpy(sta_ssid, WIFI_SSID, sizeof(sta_ssid));
        strlcpy(sta_pwd,  WIFI_PWD,  sizeof(sta_pwd));

        ap_enabled = (sizeof(WIFI_AP_SSID) > 1);
        strlcpy(ap_ssid, WIFI_AP_SSID, sizeof(ap_ssid));
        strlcpy(ap_pwd,  WIFI_AP_PWD,  sizeof(ap_pwd));
        ap_channel = WIFI_AP_CHANNEL;

        tcp_enabled = true;
        tcp_port    = RNS_TCP_LISTEN_PORT;

        mdns_enabled = true;
        strlcpy(mdns_host, MDNS_HOSTNAME, sizeof(mdns_host));

        strlcpy(portal_pwd, ADMIN_PASSWORD, sizeof(portal_pwd));

        path_req_rate_s = 1800;
        announce_rate_s = 600;
        prop_dests[0]   = 0;

        strlcpy(chan_name, BRIDGE_CHANNEL_NAME, sizeof(chan_name));
        strlcpy(chan_psk,  BRIDGE_CHANNEL_PSK,  sizeof(chan_psk));
    }

    // Stored config wins over build flags; a missing or corrupt file falls back
    // to defaults rather than failing, so a bad save can never brick the node
    // into an unconfigurable state — the AP and portal always come up.
    bool load() {
        setDefaults();

        if (!SPIFFS.exists(GATEWAY_CONFIG_PATH)) {
            slog("[cfg] no %s — using build-flag defaults\r\n", GATEWAY_CONFIG_PATH);
            return false;
        }
        File f = SPIFFS.open(GATEWAY_CONFIG_PATH, FILE_READ);
        if (!f) {
            slog("[cfg] %s exists but will not open — using defaults\r\n",
                 GATEWAY_CONFIG_PATH);
            return false;
        }

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f);
        f.close();
        if (err) {
            slog("[cfg] %s is not valid JSON (%s) — using defaults\r\n",
                 GATEWAY_CONFIG_PATH, err.c_str());
            return false;
        }

        sta_enabled = doc["sta_enabled"] | sta_enabled;
        strlcpy(sta_ssid, doc["sta_ssid"] | sta_ssid, sizeof(sta_ssid));
        strlcpy(sta_pwd,  doc["sta_pwd"]  | sta_pwd,  sizeof(sta_pwd));

        ap_enabled = doc["ap_enabled"] | ap_enabled;
        strlcpy(ap_ssid, doc["ap_ssid"] | ap_ssid, sizeof(ap_ssid));
        strlcpy(ap_pwd,  doc["ap_pwd"]  | ap_pwd,  sizeof(ap_pwd));
        ap_channel = doc["ap_channel"] | ap_channel;

        tcp_enabled = doc["tcp_enabled"] | tcp_enabled;
        tcp_port    = doc["tcp_port"]    | tcp_port;

        mdns_enabled = doc["mdns_enabled"] | mdns_enabled;
        strlcpy(mdns_host, doc["mdns_host"] | mdns_host, sizeof(mdns_host));

        strlcpy(portal_pwd, doc["portal_pwd"] | portal_pwd, sizeof(portal_pwd));
        if (portal_pwd[0] == 0) strlcpy(portal_pwd, ADMIN_PASSWORD, sizeof(portal_pwd));

        path_req_rate_s = doc["path_req_rate_s"] | path_req_rate_s;
        announce_rate_s = doc["announce_rate_s"] | announce_rate_s;
        strlcpy(prop_dests, doc["prop_dests"] | prop_dests, sizeof(prop_dests));

        strlcpy(chan_name, doc["chan_name"] | chan_name, sizeof(chan_name));
        strlcpy(chan_psk,  doc["chan_psk"]  | chan_psk,  sizeof(chan_psk));

        slog("[cfg] loaded %s\r\n", GATEWAY_CONFIG_PATH);
        return true;
    }

    bool save() const {
        JsonDocument doc;
        doc["sta_enabled"]  = sta_enabled;
        doc["sta_ssid"]     = sta_ssid;
        doc["sta_pwd"]      = sta_pwd;
        doc["ap_enabled"]   = ap_enabled;
        doc["ap_ssid"]      = ap_ssid;
        doc["ap_pwd"]       = ap_pwd;
        doc["ap_channel"]   = ap_channel;
        doc["tcp_enabled"]  = tcp_enabled;
        doc["tcp_port"]     = tcp_port;
        doc["mdns_enabled"] = mdns_enabled;
        doc["mdns_host"]    = mdns_host;
        doc["portal_pwd"]   = portal_pwd;
        doc["path_req_rate_s"] = path_req_rate_s;
        doc["announce_rate_s"] = announce_rate_s;
        doc["prop_dests"]   = prop_dests;
        doc["chan_name"]    = chan_name;
        doc["chan_psk"]     = chan_psk;

        // Write to a temp file and rename, so an interrupted write (a brownout
        // mid-save is not hypothetical on a solar node) cannot leave a
        // half-written config that parses as garbage.
        const char* tmp = GATEWAY_CONFIG_PATH ".tmp";
        File f = SPIFFS.open(tmp, FILE_WRITE);
        if (!f) {
            slog("[cfg] cannot open %s for write\r\n", tmp);
            return false;
        }
        size_t written = serializeJson(doc, f);
        f.close();
        if (written == 0) {
            slog("[cfg] wrote 0 bytes to %s\r\n", tmp);
            SPIFFS.remove(tmp);
            return false;
        }
        SPIFFS.remove(GATEWAY_CONFIG_PATH);
        if (!SPIFFS.rename(tmp, GATEWAY_CONFIG_PATH)) {
            slog("[cfg] rename %s -> %s failed\r\n", tmp, GATEWAY_CONFIG_PATH);
            return false;
        }
        slog("[cfg] saved %s (%u bytes)\r\n", GATEWAY_CONFIG_PATH, (unsigned)written);
        return true;
    }

    // Factory reset. The AP and portal come back up on build-flag defaults.
    static bool clear() {
        bool ok = SPIFFS.remove(GATEWAY_CONFIG_PATH);
        slog("[cfg] factory reset: %s\r\n", ok ? "config cleared" : "nothing to clear");
        return ok;
    }
};

#endif // RNS_GATEWAY_CONFIG_H
