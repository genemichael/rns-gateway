#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <Mesh.h>
#include <RTClib.h>
#include <target.h>

#if defined(ESP32)
  #include <SPIFFS.h>
  using File = fs::File;
#endif

#include <helpers/AdvertDataHelpers.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/BaseChatMesh.h>
#include <helpers/ClientACL.h>
#include <helpers/CommonCLI.h>
#include <helpers/IdentityStore.h>
#include <helpers/RegionMap.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/StatsFormatHelper.h>
#include <helpers/TxtDataHelpers.h>

#include "MeshCoreLink.h"

#ifndef FIRMWARE_BUILD_DATE
  #define FIRMWARE_BUILD_DATE   "17 Aug 2026"
#endif

#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION   "v1.17.1"
#endif

#define FIRMWARE_ROLE "rns_gateway"

/* ---------------------------------- CONFIGURATION ------------------------------------- */

// No silent defaults for the radio parameters. Wrong LoRa settings are not an
// error at runtime — the node simply never hears the mesh — so a fallback here
// would turn a missing build flag into a week of debugging a radio that "works"
// on the wrong bandwidth. All four come from the env in
// variants/heltec_v4/platformio.ini and a missing one fails the build instead.
#if !defined(LORA_FREQ) || !defined(LORA_BW) || !defined(LORA_SF) || !defined(LORA_CR)
  #error "LORA_FREQ / LORA_BW / LORA_SF / LORA_CR must all be set by the build env. \
See [heltec_v4_rns_gateway_base] in variants/heltec_v4/platformio.ini."
#endif
#ifndef LORA_TX_POWER
  #define LORA_TX_POWER 20
#endif
#ifndef ADVERT_NAME
  #define ADVERT_NAME "RNS Gateway"
#endif
#ifndef ADMIN_PASSWORD
  #define ADMIN_PASSWORD "password"
#endif

// The MeshCore group channel the tunnel runs over. Index 0 is MeshCore public
// and cannot take a private PSK, so a private tunnel channel must be 1-7.
// Name and PSK come from platformio.local.ini.
#ifndef BRIDGE_CHANNEL_NAME
  #define BRIDGE_CHANNEL_NAME   "RNSTesting"
#endif
#ifndef BRIDGE_CHANNEL_PSK
  #define BRIDGE_CHANNEL_PSK    ""
#endif

// Text queued across the task boundary. MeshCore caps a channel message at
// MAX_TEXT_LEN (160) including the "<sender>: " prefix; the tunnel's
// payload_size keeps fragments well inside that.
#define TUNNEL_TEXT_MAX   176
#define TUNNEL_QUEUE_DEPTH 12

struct TunnelTx {
  bool     direct;
  uint8_t  pub_key[PUB_KEY_SIZE];
  uint32_t timestamp;
  char     text[TUNNEL_TEXT_MAX];
};

struct TunnelRx {
  bool     direct;
  uint8_t  pub_key[PUB_KEY_SIZE];
  uint32_t timestamp;
  char     text[TUNNEL_TEXT_MAX];
};

// ensureContact request crossing from the RNS task to the mesh task.
struct TunnelBind {
  uint8_t pub_key[PUB_KEY_SIZE];
  char    name[32];
};

/**
 * MeshCore role for the RNS gateway. Owns the radio, the mesh identity and the
 * tunnel channel, and implements MeshCoreLink so the RNS task (core 0) can push
 * and pull tunnel fragments without ever touching MeshCore directly — MeshCore
 * is not thread-safe, so everything crosses through the two queues below.
 */
class MyMesh : public BaseChatMesh, public CommonCLICallbacks, public MeshCoreLink {
  FILESYSTEM* _fs;
  uint32_t last_millis;
  uint64_t uptime_millis;
  unsigned long next_local_advert, next_flood_advert;
  bool _logging;
  NodePrefs _prefs;
  ClientACL acl;
  CommonCLI _cli;
  TransportKeyStore key_store;
  RegionMap region_map, temp_map;
  unsigned long set_radio_at, revert_radio_at;
  float pending_freq;
  float pending_bw;
  uint8_t pending_sf;
  uint8_t pending_cr;
  ChannelDetails* _bridge_channel;   // the tunnel channel; NULL until joined
  char _chan_name[32];               // seeded from build flags, overridden by config
  char _chan_psk[45];                // base64 of a 16- or 32-byte key
  char _self_pubkey_hex[PUB_KEY_SIZE * 2 + 1];

  QueueHandle_t _tx_queue;   // RNS task -> mesh task (fragments to transmit)
  QueueHandle_t _rx_queue;   // mesh task -> RNS task (fragments received)
  QueueHandle_t _bind_queue; // RNS task -> mesh task (ensureContact requests)
  uint32_t _tx_dropped, _rx_dropped;
  volatile uint32_t _ack_pending;   // set by processAck on the mesh task

  // Loop guard. MeshCore can hand our own transmission back through
  // onChannelMessageRecv (BaseChatMesh keeps a _pendingLoopback), and a tunnel
  // fragment fed back into handle_incoming would corrupt reassembly.
  static const uint8_t ECHO_RING_SIZE = 12;
  uint32_t _echo_ring[ECHO_RING_SIZE];
  uint8_t  _echo_next;
  void noteOwnEcho(const char* text);
  bool isOwnEcho(const char* text);

  mesh::Packet* createSelfAdvert();
  void joinBridgeChannel();
  void drainTxQueue();
  void drainBindQueue();

protected:
  float getAirtimeBudgetFactor() const override { return _prefs.airtime_factor; }

  // BaseChatMesh. The tunnel rides on channel messages; contact messages carry
  // direct-routed fragments. The rest are required by the base class and unused.
  void onChannelMessageRecv(const mesh::GroupChannel& channel, mesh::Packet* pkt, uint32_t timestamp, const char* text) override;
  void onMessageRecv(const ContactInfo& contact, mesh::Packet* pkt, uint32_t sender_timestamp, const char* text) override;
  void onDiscoveredContact(ContactInfo& contact, bool is_new, uint8_t path_len, const uint8_t* path) override { }
  ContactInfo* processAck(const uint8_t* data) override;
  void onContactPathUpdated(const ContactInfo& contact) override { }
  void onCommandDataRecv(const ContactInfo& contact, mesh::Packet* pkt, uint32_t sender_timestamp, const char* text) override { }
  void onSignedMessageRecv(const ContactInfo& contact, mesh::Packet* pkt, uint32_t sender_timestamp, const uint8_t* sender_prefix, const char* text) override { }
  uint32_t calcFloodTimeoutMillisFor(uint32_t pkt_airtime_millis) const override;
  uint32_t calcDirectTimeoutMillisFor(uint32_t pkt_airtime_millis, uint8_t path_len) const override;
  void onSendTimeout() override { }
  // Base BaseChatMesh floods with path_hash_size=1; match companion_radio and
  // use the configured path_hash_mode so our floods encode like the rest of
  // the mesh.
  void sendFloodScoped(const mesh::GroupChannel& channel, mesh::Packet* pkt, uint32_t delay_millis = 0) override;
  uint8_t onContactRequest(const ContactInfo& contact, uint32_t sender_timestamp, const uint8_t* data, uint8_t len, uint8_t* reply) override { return 0; }
  void onContactResponse(const ContactInfo& contact, const uint8_t* data, uint8_t len) override { }

public:
  MyMesh(mesh::MainBoard& board, mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables);

  // Call before begin(). Overrides the BRIDGE_CHANNEL_* build-flag defaults
  // with values from the stored config.
  void setBridgeChannel(const char* name, const char* psk);

  void begin(FILESYSTEM* fs);
  void loop();
  void handleCommand(uint32_t sender_timestamp, char* command, char* reply);
  bool hasPendingWork() const;
  NodePrefs* getNodePrefs() { return &_prefs; }

  // Called from the RNS task to collect a received fragment. False when idle.
  bool takeReceived(TunnelRx& out);
  // Consumes a pending delivery ACK, if one arrived since the last call.
  bool takeAck();
  uint32_t txDropped() const { return _tx_dropped; }
  uint32_t rxDropped() const { return _rx_dropped; }
  bool bridgeChannelJoined() const { return _bridge_channel != NULL; }

  // MeshCoreLink — called on the RNS task, never touches MeshCore directly.
  bool sendChannelText(const char* text, uint32_t timestamp) override;
  bool sendDirectText(const uint8_t* pub_key, const char* text, uint32_t timestamp) override;
  bool ensureContact(const uint8_t* pub_key, const char* name) override;
  const char* selfPubKeyHex() override { return _self_pubkey_hex; }
  uint32_t nowEpoch() override;

  // CommonCLICallbacks
  const char* getFirmwareVer() override { return FIRMWARE_VERSION; }
  const char* getBuildDate() override { return FIRMWARE_BUILD_DATE; }
  const char* getRole() override { return FIRMWARE_ROLE; }
  void savePrefs() override { _cli.savePrefs(_fs); }
  bool formatFileSystem() override;
  void sendSelfAdvertisement(int delay_millis, bool flood) override;
  void updateAdvertTimer() override;
  void updateFloodAdvertTimer() override;
  void setLoggingOn(bool enable) override { _logging = enable; }
  void eraseLogFile() override { }
  void dumpLogFile() override { }
  void setTxPower(int8_t power_dbm) override { radio_driver.setTxPower(power_dbm); }
  void formatNeighborsReply(char* reply) override { strcpy(reply, "(not supported)"); }
  void formatStatsReply(char* reply) override;
  void formatRadioStatsReply(char* reply) override;
  void formatPacketStatsReply(char* reply) override;
  mesh::LocalIdentity& getSelfId() override { return self_id; }
  void saveIdentity(const mesh::LocalIdentity& new_id) override;
  void clearStats() override;
  void applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr, int timeout_mins) override;
};

extern MyMesh the_mesh;
