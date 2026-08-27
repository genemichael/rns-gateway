/*
 * RNS-over-MeshCore access point — single-device firmware for the Heltec V4.3.
 *
 * Two RNS interfaces, transport enabled, so the node forwards between them:
 *
 *   iface A  TcpInterface      — TCP *server*. Local Reticulum clients on WiFi
 *                                connect in to us; we never dial out. That
 *                                direction is load-bearing: if two sites both
 *                                peered with one shared rnsd, Reticulum would
 *                                route around the mesh entirely and the
 *                                end-to-end test would prove nothing.
 *   iface B  MeshCoreInterface — the MeshCore mesh, in-process. MODE_BOUNDARY
 *                                so announces cross into the tunnel (AP on
 *                                both sides blocks every announce; GATEWAY
 *                                would flood the shared mesh). Announce
 *                                throttling is MeshCoreInterface's own.
 *
 * MeshCore owns the SX1262 and runs its cooperative loop() on core 1. The whole
 * Reticulum stack runs on the core-0 task below and reaches the radio only
 * through MeshCoreLink, which queues across the boundary.
 */
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <WiFi.h>

// microReticulum before any MeshCore header — MeshCore.h #defines PUB_KEY_SIZE
// and friends as bare macros that collide with RNS::Type's constants.
#include <microReticulum.h>
#include <microStore/Adapters/SPIFFSFileSystem.h>
#include "SerialLog.h"
#include "GatewayConfig.h"
#include "ConfigPortal.h"
#include "MeshCoreInterface.h"
#include "TcpInterface.h"

#include <Mesh.h>
#include "MyMesh.h"
#ifdef DISPLAY_CLASS
  #include "StatusScreen.h"
#endif

// ── Client access ────────────────────────────────────────────────────────────
// Port clients dial. 4242 is the Reticulum TCPServerInterface convention.
#ifndef RNS_TCP_LISTEN_PORT
  #define RNS_TCP_LISTEN_PORT  4242
#endif

// ── WiFi ─────────────────────────────────────────────────────────────────────
// Station credentials. Empty = no station, AP-only (standalone / field).
#ifndef WIFI_SSID
  #define WIFI_SSID  ""
#endif
#ifndef WIFI_PWD
  #define WIFI_PWD   ""
#endif
// softAP for client access. Empty = no AP, station-only.
#ifndef WIFI_AP_SSID
  #define WIFI_AP_SSID  "RNSGateway"
#endif
#ifndef WIFI_AP_PWD
  #define WIFI_AP_PWD   ""
#endif
// AP channel used only when there is no station to inherit one from.
#ifndef WIFI_AP_CHANNEL
  #define WIFI_AP_CHANNEL  1
#endif

#define RNS_TASK_STACK      16384
#define RNS_TASK_PRIORITY   2
#define RNS_TASK_CORE       0
#define WIFI_RETRY_MILLIS   10000
// How long to wait for the station to associate before raising the AP, so the
// AP can be born on the station's channel instead of being dragged to it later.
#define WIFI_STA_INITIAL_WAIT_MS  15000

StdRNG fast_rng;
SimpleMeshTables tables;

MyMesh the_mesh(board, radio_driver, *new ArduinoMillis(), fast_rng, rtc_clock, tables);

static RNS::Reticulum*     _reticulum = NULL;
static MeshCoreInterface*  _mc_impl = NULL;
static RNS::Interface*     _mc_iface = NULL;
static TcpInterface*       _tcp_impl = NULL;
static RNS::Interface*     _tcp_iface = NULL;
static volatile bool       _sta_up = false;   // station associated
static volatile bool       _ap_up  = false;   // softAP raised

static GatewayConfig g_cfg;
#ifdef DISPLAY_CLASS
static bool g_screen_ok = false;
static StatusScreen g_screen(display, user_btn, g_cfg);
#endif
static ConfigPortal  g_portal;

// ── Radio parameters across the task boundary ────────────────────────────────
// The portal runs on core 0; MeshCore and its NodePrefs belong to core 1 and
// are NOT thread-safe. So the web UI never touches the_mesh: it reads a
// snapshot the mesh task publishes, and parks a change request that the mesh
// task picks up in loop() and applies through the same CLI path as `set radio`.
// Single writer on each side, which is what makes the plain volatiles safe.
struct RadioParams { float freq; float bw; uint8_t sf; uint8_t cr; uint8_t txp; };
static RadioParams    _radio_now  = {0, 0, 0, 0, 0};   // written by loop() only
static RadioParams    _radio_req  = {0, 0, 0, 0, 0};   // written by portal only
static volatile bool  _radio_req_pending = false;

static void portal_read_radio(float& freq, float& bw, uint8_t& sf,
                              uint8_t& cr, uint8_t& txp) {
  RadioParams snap = _radio_now;
  freq = snap.freq; bw = snap.bw; sf = snap.sf; cr = snap.cr; txp = snap.txp;
}

static bool portal_apply_radio(float freq, float bw, uint8_t sf,
                               uint8_t cr, uint8_t txp) {
  // Mirror CommonCLI's own validation so the portal rejects nonsense before it
  // ever reaches the mesh task, rather than failing silently one hop later.
  if (freq < 150.0f || freq > 2500.0f) return false;
  if (bw   < 7.0f   || bw   > 500.0f)  return false;
  if (sf   < 5      || sf   > 12)      return false;
  if (cr   < 5      || cr   > 8)       return false;
  if (_radio_req_pending) return false;          // a change is already in flight
  _radio_req = {freq, bw, sf, cr, txp};
  _radio_req_pending = true;
  return true;
}

void halt() {
  while (1) ;
}

static char command[160];

// The board has one 2.4 GHz radio, so AP and STA must share a channel. Raising
// the AP only after the station has associated lets it adopt the station's
// channel; if the station later moves, esp_wifi drags the AP with it and any
// associated clients are dropped. That is the documented cost of running both.
static void wifi_begin() {
  const bool sta = g_cfg.sta_enabled && g_cfg.sta_ssid[0];
  const bool ap  = g_cfg.ap_enabled  && g_cfg.ap_ssid[0];

  if (sta && ap) {
    WiFi.mode(WIFI_AP_STA);
  } else if (sta) {
    WiFi.mode(WIFI_STA);
  } else if (ap) {
    WiFi.mode(WIFI_AP);
  } else {
    WiFi.mode(WIFI_OFF);
    slogln("[wifi] no station and no AP configured — no client access");
    return;
  }

  // Disable modem sleep. With AP+STA the station misses unicast frames the AP
  // buffered for it once power save settles in, and the node goes silently
  // half-dead: it still answers mDNS multicast, so it looks present on the
  // network, while every inbound TCP connection and even ICMP is dropped.
  // Observed exactly that — reachable right after boot, unreachable minutes
  // later, with the firmware still reporting the station as associated.
  // Costs idle power, which matters on a solar node, but an access point that
  // cannot accept connections is not an access point.
  WiFi.setSleep(false);

  if (sta) {
    WiFi.setAutoReconnect(true);
    WiFi.begin(g_cfg.sta_ssid, g_cfg.sta_pwd);
    slog("[wifi] station: joining '%s'\r\n", g_cfg.sta_ssid);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - t0) < WIFI_STA_INITIAL_WAIT_MS) {
      vTaskDelay(pdMS_TO_TICKS(250));
    }
    _sta_up = (WiFi.status() == WL_CONNECTED);
    if (_sta_up) {
      slog("[wifi] station: %s on ch %d\r\n",
                    WiFi.localIP().toString().c_str(), WiFi.channel());
    } else {
      slogln("[wifi] station: not associated yet, will keep retrying");
    }
  }

  if (ap) {
    uint8_t chan = _sta_up ? (uint8_t)WiFi.channel() : g_cfg.ap_channel;
    const char* pwd = g_cfg.ap_pwd[0] ? g_cfg.ap_pwd : NULL;
    _ap_up = WiFi.softAP(g_cfg.ap_ssid, pwd, chan, 0, TCP_IF_MAX_CLIENTS);
    if (_ap_up) {
      slog("[wifi] AP '%s' up on ch %d at %s (max %d clients)\r\n",
                    g_cfg.ap_ssid, chan, WiFi.softAPIP().toString().c_str(),
                    TCP_IF_MAX_CLIENTS);
      if (pwd == NULL) {
        slogln("[wifi] *** AP IS OPEN — anyone in range can inject "
                       "Reticulum traffic and reach the config portal. "
                       "Set an AP password. ***");
      }
      if (sta && !_sta_up) {
        slog("[wifi] note: AP started on ch %d before the station "
                      "associated; it will move to the station's channel and "
                      "drop AP clients when it does\r\n", chan);
      }
    } else {
      slogln("[wifi] AP failed to start");
    }
  }
}

static void service_wifi(unsigned long& last_retry) {
  if (!g_cfg.sta_enabled || !g_cfg.sta_ssid[0]) return;

  if (WiFi.status() == WL_CONNECTED) {
    if (!_sta_up) {
      _sta_up = true;
      slog("[wifi] station up: %s on ch %d\r\n",
                    WiFi.localIP().toString().c_str(), WiFi.channel());
    }
    return;
  }
  _sta_up = false;

  unsigned long now = millis();
  if (now - last_retry > WIFI_RETRY_MILLIS) {
    last_retry = now;
    WiFi.disconnect();
    WiFi.reconnect();
  }
}

// Clients can reach us over either link, so "network available" is either one.
static inline bool net_up() { return _sta_up || _ap_up; }

// Reticulum logs from core 0 while MeshCore prints from core 1. Routing RNS's
// output through the same mutex our own prints use is what keeps whole lines
// intact — see SerialLog.h.
static void rns_log_to_serial(const char* msg, RNS::LogLevel level) {
  slog("[rns/%s] %s\r\n", RNS::getLevelName(level), msg ? msg : "");
}

static void rns_task(void* arg) {
  unsigned long last_retry = 0;

  // Reticulum::start() calls OS::storage_size() unconditionally — it is NOT
  // gated on RNS_USE_FS — and that throws std::runtime_error when no
  // filesystem is registered, which terminates the task. MeshCore already
  // mounted SPIFFS in setup(); microStore's SPIFFS adapter shares it. Static
  // because SPIFFSFileSystem deletes operator new.
  RNS::set_log_callback(rns_log_to_serial);

  static microStore::Adapters::SPIFFSFileSystem rns_fs;
  RNS::Utilities::OS::register_filesystem(rns_fs);

  wifi_begin();

  // The portal (and mDNS with it) comes up as soon as ANY network exists —
  // before Reticulum, so a node with a broken RNS config is still
  // reconfigurable. Not AP-only: a station-only node (board A since
  // 2026-08-23) still needs the portal and rnsgateway.local on the home LAN;
  // gating this on the AP left such a node with no portal and a dead mDNS
  // name, so clients configured by hostname could never reconnect. A station
  // that associates late is handled in the loop below.
  bool portal_started = false;
  if (net_up()) {
    g_portal.begin(g_cfg, portal_read_radio, portal_apply_radio);
    portal_started = true;
  }

  _reticulum = new RNS::Reticulum();

  MeshCoreInterface::Config mccfg;
  mccfg.name = "MeshCore";
  mccfg.path_req_rate_ms = g_cfg.path_req_rate_s * 1000UL;
  mccfg.announce_rate_ms = g_cfg.announce_rate_s * 1000UL;
#ifdef RNS_GW_PROP_ONLY
  // Prop-restricted variant: policy is forced on at build time so it cannot
  // be disabled from the portal — restriction is a property of the firmware.
  mccfg.prop_only = true;
#endif
  _mc_impl = new MeshCoreInterface(the_mesh, mccfg);
#ifdef RNS_GW_PROP_ONLY
  {
    int prop_dest_count = 0;
    char buf[sizeof(g_cfg.prop_dests)];
    strlcpy(buf, g_cfg.prop_dests, sizeof(buf));
    for (char* tok = strtok(buf, ", "); tok; tok = strtok(NULL, ", ")) {
      if (_mc_impl->prop_add_dest(tok)) prop_dest_count++;
      else slog("RNS: prop dest '%s' malformed — ignored\r\n", tok);
    }
    slog("RNS: prop-only policy ON, %d whitelisted destination(s)\r\n",
         prop_dest_count);
    if (prop_dest_count == 0) {
      // Not fatal by design: announces and path requests still flow, so the
      // portal stays reachable to fix the config — but nothing else will pass.
      slogln("RNS: WARNING — no prop destinations configured; all link/data "
             "traffic onto the mesh will be dropped until one is set");
    }
  }
#endif
  _mc_iface = new RNS::Interface(_mc_impl);
  _mc_iface->mode(RNS::Type::Interface::MODE_BOUNDARY);
  RNS::Transport::register_interface(*_mc_iface);
  if (!_mc_impl->start()) {
    slogln("RNS: MeshCoreInterface failed to start");
  }

  // Client access. Registered unconditionally so Transport knows the interface
  // exists, but the listening socket only opens once a link is actually up —
  // WiFiServer::begin() on a down interface binds to nothing useful.
  bool tcp_started = false;
  if (g_cfg.tcp_enabled) {
    _tcp_impl = new TcpInterface(TCP_IF_MODE_SERVER, g_cfg.tcp_port,
                                 NULL, 0, "tcp0");
    _tcp_iface = new RNS::Interface(_tcp_impl);
    // The mode here is load-bearing twice over, and both failure modes are
    // silent (the blocks only log at TRACE level, which we compile out):
    //
    //  - It must not be left unset. When Transport decides whether to
    //    rebroadcast an announce onto the BOUNDARY mesh interface, it checks
    //    the mode of the interface the announce ARRIVED on and blocks if that
    //    is MODE_NONE ("next hop interface has no mode configured"). Unset =
    //    no client announce ever reaches the mesh, rns_tx stays 0 forever.
    //
    //  - It must not be MODE_ACCESS_POINT either, although the two-board
    //    bridge used that on its client-facing radio. AP semantics rebroadcast
    //    an announce to clients only when the destination is instance-local,
    //    so REMOTE announces arriving over the mesh were never forwarded to
    //    the TCP clients — MeshChat saw no peers and had no one to send to
    //    (verified on hardware 2026-08-19). AP mode exists to save airtime;
    //    this link is TCP over WiFi, where forwarding announces costs nothing.
    //
    //  - It must not be MODE_FULL either (it was, until 2026-08-23). FULL
    //    forwards announces both ways, but Transport only searches for UNKNOWN
    //    destinations when a path request arrives on an interface whose mode
    //    is in DISCOVER_PATHS_FOR = AP | GATEWAY (Interface.cpp:23, matching
    //    Python RNS Transport.py:2577 — MODE_FULL is in neither). Our TCP
    //    clients are remote interfaces, not shared-instance local clients, so
    //    the is_from_local_client door that saves desktop rnsd setups does not
    //    apply: on FULL, a client's path request for an unlearned destination
    //    dies here silently. Paths are RAM-only (RNS_PERSIST_PATHS is off), so
    //    EVERY reboot recreates that state until an announce survives a
    //    multi-fragment crossing — verified live 2026-08-23: a client stormed
    //    path requests for 17 min while rns_tx never moved.
    //
    // MODE_GATEWAY forwards announces both ways AND propagates unknown-dest
    // path requests onto the mesh. Safe now — and only now — because the
    // tunnel throttles path requests per QUERIED destination (see
    // rate_limit_ok); with the pre-2026-08-23 throttle this mode would have
    // flooded the 300 bps channel with request storms.
    _tcp_iface->mode(RNS::Type::Interface::MODE_GATEWAY);
    RNS::Transport::register_interface(*_tcp_iface);
  } else {
    slogln("RNS: TCP client access disabled in config");
  }

  _reticulum->transport_enabled(true);
  _reticulum->start();

#ifdef DISPLAY_CLASS
  if (g_screen_ok) {
    g_screen.begin(_mc_impl, _tcp_impl, &board, the_mesh.getNodePrefs());
  }
#endif

  while (true) {
    service_wifi(last_retry);

#ifdef DISPLAY_CLASS
    g_screen.loop();
#endif

    if (!portal_started && net_up()) {
      g_portal.begin(g_cfg, portal_read_radio, portal_apply_radio);
      portal_started = true;
    }

    if (_tcp_impl && !tcp_started && net_up()) {
      tcp_started = _tcp_impl->start();
      if (!tcp_started) {
        slogln("RNS: TCP server failed to start");
      }
    }

    // Hand the mesh task's received fragments to the tunnel interface.
    TunnelRx rx;
    while (the_mesh.takeReceived(rx)) {
      if (rx.direct) {
        _mc_impl->on_contact_text(rx.pub_key, rx.text, rx.timestamp);
      } else {
        _mc_impl->on_channel_text(rx.text, rx.timestamp);
      }
    }
    while (the_mesh.takeAck()) {
      _mc_impl->on_direct_ack(0);
    }

    _reticulum->loop();
    _mc_impl->loop();
    if (tcp_started) _tcp_impl->loop();

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// microReticulum signals errors by throwing. An escaped throw on this task hits
// std::terminate and reboots the whole node, taking the mesh side down with it
// — so the boundary is guarded and the RNS side fails alone.
static void rns_task_guarded(void* arg) {
  try {
    rns_task(arg);
  }
  catch (const std::exception& e) {
    // MESH_DEBUG_PRINTLN compiles to nothing without -D MESH_DEBUG=1, which
    // would make the single most important failure in this firmware silent.
    slog("RNS task died: %s\r\n", e.what());
  }
  catch (...) {
    slogln("RNS task died: unknown exception");
  }
  vTaskDelete(NULL);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  board.begin();

  if (!radio_init()) {
    slogln("Radio init failed!");
    halt();
  }

  fast_rng.begin(radio_driver.getRngSeed());

  FILESYSTEM* fs;
#if defined(ESP32)
  SPIFFS.begin(true);
  fs = &SPIFFS;

  // Boot-time filesystem inventory. The RNS transport identity and path store
  // live here, and "it regenerated again" is indistinguishable from "it was
  // never written" without seeing the directory. Cheap, and it has already
  // paid for itself once.
  slog("[fs] SPIFFS %u/%u bytes used\r\n",
                (unsigned)SPIFFS.usedBytes(), (unsigned)SPIFFS.totalBytes());
  {
    File root = SPIFFS.open("/");
    if (root) {
      for (File f = root.openNextFile(); f; f = root.openNextFile()) {
        slog("[fs]   %-28s %u bytes\r\n", f.name(), (unsigned)f.size());
        f.close();
      }
      root.close();
    } else {
      slogln("[fs]   <root not listable>");
    }
  }

  // Stored config wins over build flags. Loaded before the_mesh.begin() so the
  // bridge channel it joins is the configured one.
  g_cfg.load();
  the_mesh.setBridgeChannel(g_cfg.chan_name, g_cfg.chan_psk);

  IdentityStore store(SPIFFS, "/identity");
#else
  #error "RNS gateway targets ESP32 only"
#endif
  if (!store.load("_main", the_mesh.self_id)) {
    MESH_DEBUG_PRINTLN("Generating new keypair");
    the_mesh.self_id = radio_new_identity();   // create new random identity
    int count = 0;
    while (count < 10 && (the_mesh.self_id.pub_key[0] == 0x00 || the_mesh.self_id.pub_key[0] == 0xFF)) {  // reserved id hashes
      the_mesh.self_id = radio_new_identity(); count++;
    }
    store.save("_main", the_mesh.self_id);
  }

  // printHex writes straight to the Stream, so hold the lock across the whole
  // line rather than letting the hex dump interleave with the RNS task.
  serial_lock();
  Serial.print("Gateway ID: ");
  mesh::Utils::printHex(Serial, the_mesh.self_id.pub_key, PUB_KEY_SIZE);
  Serial.println();
  serial_unlock();

  command[0] = 0;

  sensors.begin();

  the_mesh.begin(fs);

#ifdef DISPLAY_CLASS
  // Probe fails harmlessly on boards with no OLED fitted; the screen (and
  // the PRG button driver) then stay dormant for the whole run. Serviced
  // from the RNS task, which owns every data source the pages read.
  if (display.begin()) {
    user_btn.begin();
    g_screen_ok = true;
    slogln("[oled] display up");
  } else {
    slogln("[oled] no display found");
  }
#endif

  if (xTaskCreatePinnedToCore(rns_task_guarded, "rns", RNS_TASK_STACK, NULL,
                              RNS_TASK_PRIORITY, NULL, RNS_TASK_CORE) != pdPASS) {
    slogln("Failed to start RNS task!");
  }

  // send out initial zero hop Advertisement to the mesh
#if ENABLE_ADVERT_ON_BOOT == 1
  the_mesh.sendSelfAdvertisement(16000, false);
#endif

  board.onBootComplete();
}

void loop() {
  // Handle Serial CLI
  int len = strlen(command);
  while (Serial.available() && len < sizeof(command)-1) {
    char c = Serial.read();
    if (c != '\n') {
      command[len++] = c;
      command[len] = 0;
      Serial.print(c);
    }
    if (c == '\r') break;
  }
  if (len == sizeof(command)-1) {  // command buffer full
    command[sizeof(command)-1] = '\r';
  }

  if (len > 0 && command[len - 1] == '\r') {  // received complete line
    Serial.print('\n');
    command[len - 1] = 0;  // replace newline with C string null terminator
    char reply[160];
    reply[0] = 0;
    the_mesh.handleCommand(0, command, reply);  // NOTE: there is no sender_timestamp via serial!
    if (reply[0]) {
      slog("  -> %s\r\n", reply);
    }

    command[0] = 0;  // reset command buffer
  }

  the_mesh.loop();
  sensors.loop();
  rtc_clock.tick();

  // Apply a radio change requested from the portal. This runs on core 1, the
  // only task allowed to touch MeshCore, and goes through the same CLI path as
  // `set radio` / `set tx` so validation and persistence stay in one place.
  if (_radio_req_pending) {
    RadioParams req = _radio_req;
    char cmd[64], reply[160];
    reply[0] = 0;
    snprintf(cmd, sizeof(cmd), "set radio %.4f,%.1f,%u,%u",
             req.freq, req.bw, (unsigned)req.sf, (unsigned)req.cr);
    the_mesh.handleCommand(0, cmd, reply);
    slog("[cfg] %s -> %s\r\n", cmd, reply[0] ? reply : "(no reply)");

    reply[0] = 0;
    snprintf(cmd, sizeof(cmd), "set tx %u", (unsigned)req.txp);
    the_mesh.handleCommand(0, cmd, reply);
    slog("[cfg] %s -> %s\r\n", cmd, reply[0] ? reply : "(no reply)");

    _radio_req_pending = false;
  }

  // Publish the current radio settings for the portal to display.
  {
    NodePrefs* p = the_mesh.getNodePrefs();
    _radio_now = { p->freq, p->bw, p->sf, p->cr, (uint8_t)p->tx_power_dbm };
  }

  // Bring-up heartbeat, same fields as the two-board build so the output is
  // comparable against it.
  static uint32_t last_hb = 0;
  uint32_t now = millis();
  if (now - last_hb >= 10000) {
    last_hb = now;
    // The station IP is DHCP and it is the first thing you need when a client
    // cannot reach the node, so print it rather than making someone go find it.
    // tcprx/tcptx are CUMULATIVE frames across the TCP boundary. tcprx counts
    // client->board frames handed to Transport; a client "send" that never
    // moves tcprx never left the phone. tcprx moving while nothing reaches the
    // mesh means Transport swallowed it. Cumulative so a test needs no timing
    // coordination — send whenever, read the totals later.
    slog("[hb] ip=%s up=%us sta=%d ap=%d tcpcli=%d tcprx=%u tcptx=%u chan=%d rns_tx=%u rns_rx=%u chan_msgs=%u "
                  "ann_drop=%u peers=%u routes=%u direct=%u dfall=%u bindtx=%u bindrx=%u "
                  "outq=%u txdrop=%u rxdrop=%u heap=%u psram=%u\n",
                  _sta_up ? WiFi.localIP().toString().c_str() : "-",
                  now / 1000, _sta_up ? 1 : 0, _ap_up ? 1 : 0,
                  _tcp_impl ? _tcp_impl->clientCount() : 0,
                  _tcp_impl ? _tcp_impl->rx_frames() : 0,
                  _tcp_impl ? _tcp_impl->tx_frames() : 0,
                  the_mesh.bridgeChannelJoined() ? 1 : 0,
                  _mc_impl ? _mc_impl->rns_tx_packets() : 0,
                  _mc_impl ? _mc_impl->rns_rx_packets() : 0,
                  _mc_impl ? _mc_impl->channel_msgs() : 0,
                  _mc_impl ? _mc_impl->announce_suppressed() : 0,
                  _mc_impl ? (unsigned)_mc_impl->peer_count() : 0,
                  _mc_impl ? (unsigned)_mc_impl->route_count() : 0,
                  _mc_impl ? _mc_impl->direct_tx() : 0,
                  _mc_impl ? _mc_impl->direct_fallbacks() : 0,
                  _mc_impl ? _mc_impl->bind_tx() : 0,
                  _mc_impl ? _mc_impl->bind_rx() : 0,
                  _mc_impl ? (unsigned)_mc_impl->outq_depth() : 0,
                  the_mesh.txDropped(), the_mesh.rxDropped(),
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  }
}
