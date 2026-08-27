#include "MyMesh.h"

#define SEND_TIMEOUT_BASE_MILLIS        500
#define FLOOD_SEND_TIMEOUT_FACTOR       16.0f
#define DIRECT_SEND_PERHOP_FACTOR       6.0f
#define DIRECT_SEND_PERHOP_EXTRA_MILLIS 250

MyMesh::MyMesh(mesh::MainBoard& board, mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables)
    : BaseChatMesh(radio, ms, rng, rtc, *new StaticPoolPacketManager(32), tables),
      region_map(key_store), temp_map(key_store),
      _cli(board, rtc, sensors, region_map, acl, &_prefs, this)
{
  last_millis = 0;
  uptime_millis = 0;
  next_local_advert = next_flood_advert = 0;
  set_radio_at = revert_radio_at = 0;
  _logging = false;
  _bridge_channel = NULL;
  memset(_echo_ring, 0, sizeof(_echo_ring));
  _echo_next = 0;
  _tunnel_flood = false;
  _tx_queue = NULL;
  _rx_queue = NULL;
  _bind_queue = NULL;
  _tx_dropped = _rx_dropped = 0;
  _ack_pending = 0;
  _self_pubkey_hex[0] = 0;

  // Build-flag defaults; setBridgeChannel() overrides them from stored config.
  StrHelper::strncpy(_chan_name, BRIDGE_CHANNEL_NAME, sizeof(_chan_name));
  StrHelper::strncpy(_chan_psk,  BRIDGE_CHANNEL_PSK,  sizeof(_chan_psk));

  // defaults — loadPrefs() only overwrites these if a prefs file exists, so
  // without them a fresh install configures the radio with zeros and nothing
  // ever leaves the antenna.
  _prefs.airtime_factor = 1.0;
  _prefs.rx_delay_base = 0.0f;
  _prefs.tx_delay_factor = 0.5f;
  _prefs.direct_tx_delay_factor = 0.3f;
  StrHelper::strncpy(_prefs.node_name, ADVERT_NAME, sizeof(_prefs.node_name));
  _prefs.node_lat = 0.0;
  _prefs.node_lon = 0.0;
  StrHelper::strncpy(_prefs.password, ADMIN_PASSWORD, sizeof(_prefs.password));
  _prefs.freq = LORA_FREQ;
  _prefs.sf = LORA_SF;
  _prefs.bw = LORA_BW;
  _prefs.cr = LORA_CR;
  _prefs.tx_power_dbm = LORA_TX_POWER;
  _prefs.advert_interval = 1;        // 2 minutes
  _prefs.flood_advert_interval = 47; // 47 hours
  _prefs.flood_max = 64;
  _prefs.flood_max_unscoped = 64;
  _prefs.flood_max_advert = 8;
  _prefs.interference_threshold = 0; // disabled
  _prefs.adc_multiplier = 0.0f;      // 0.0f means use the board default

#if defined(USE_SX1262) || defined(USE_SX1268)
#ifdef SX126X_RX_BOOSTED_GAIN
  _prefs.rx_boosted_gain = SX126X_RX_BOOSTED_GAIN;
#else
  _prefs.rx_boosted_gain = 1;
#endif
#endif
  _prefs.radio_fem_rxgain = 1;
  _prefs.radio_fem_txgain = 0;
}

mesh::Packet* MyMesh::createSelfAdvert() {
  uint8_t app_data[MAX_ADVERT_DATA_SIZE];
  uint8_t app_data_len = _cli.buildAdvertData(ADV_TYPE_CHAT, app_data);

  return createAdvert(self_id, app_data, app_data_len);
}

// Join the channel this gateway bridges. The PSK is base64 (16 or 32 bytes
// decoded) — same encoding the phone app uses when sharing a channel.
void MyMesh::joinBridgeChannel() {
  // Name and PSK come from the stored config when the portal has written one,
  // falling back to the build flags via setBridgeChannel()'s defaults.
  if (_chan_psk[0] == 0) {
    Serial.println("[ch] no channel PSK set — not bridging any channel");
    return;
  }
  _bridge_channel = addChannel(_chan_name, _chan_psk);
  if (_bridge_channel == NULL) {
    Serial.printf("[ch] FAILED to join '%s' (bad PSK?)\n", _chan_name);
  } else {
    Serial.printf("[ch] joined '%s' idx=%d\n", _chan_name,
                  findChannelIdx(_bridge_channel->channel));
  }
}

void MyMesh::setBridgeChannel(const char* name, const char* psk) {
  StrHelper::strncpy(_chan_name, name ? name : "", sizeof(_chan_name));
  StrHelper::strncpy(_chan_psk,  psk  ? psk  : "", sizeof(_chan_psk));
}

void MyMesh::begin(FILESYSTEM* fs) {
  BaseChatMesh::begin();
  _fs = fs;
  _cli.loadPrefs(_fs);
  acl.load(_fs, self_id);

  radio_driver.setParams(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
  radio_driver.setTxPower(_prefs.tx_power_dbm);

  Serial.printf("[radio] freq=%.3f bw=%.1f sf=%d cr=%d tx=%ddBm name='%s'\n",
                _prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr,
                _prefs.tx_power_dbm, _prefs.node_name);

  radio_driver.setRxBoostedGainMode(_prefs.rx_boosted_gain);
  board.setLoRaFemLnaEnabled(_prefs.radio_fem_rxgain);

  board.setAdcMultiplier(_prefs.adc_multiplier);

  updateAdvertTimer();
  updateFloodAdvertTimer();

  joinBridgeChannel();

  mesh::Utils::toHex(_self_pubkey_hex, self_id.pub_key, PUB_KEY_SIZE);
  for (char* p = _self_pubkey_hex; *p; p++) *p = tolower(*p);

  _tx_queue = xQueueCreate(TUNNEL_QUEUE_DEPTH, sizeof(TunnelTx));
  _rx_queue = xQueueCreate(TUNNEL_QUEUE_DEPTH, sizeof(TunnelRx));
  _bind_queue = xQueueCreate(TUNNEL_QUEUE_DEPTH, sizeof(TunnelBind));
}

// A tunnel fragment arrived on the bridge channel. 'text' is still in
// MeshCore's "<sender>: <body>" form; the interface parses it, exactly as it
// did when the companion protocol delivered the same string.
void MyMesh::onChannelMessageRecv(const mesh::GroupChannel& channel, mesh::Packet* pkt, uint32_t timestamp, const char* text) {
  if (_bridge_channel == NULL || findChannelIdx(channel) != findChannelIdx(_bridge_channel->channel)) return;
  if (isOwnEcho(text)) return;   // our own transmission looped back

  TunnelRx rx;
  rx.direct = false;
  memset(rx.pub_key, 0, sizeof(rx.pub_key));
  rx.timestamp = timestamp;
  StrHelper::strncpy(rx.text, text, sizeof(rx.text));
  if (_rx_queue == NULL || xQueueSend(_rx_queue, &rx, 0) != pdTRUE) _rx_dropped++;
}

// A direct-routed tunnel fragment from a known contact.
void MyMesh::onMessageRecv(const ContactInfo& contact, mesh::Packet* pkt, uint32_t sender_timestamp, const char* text) {
  TunnelRx rx;
  rx.direct = true;
  memcpy(rx.pub_key, contact.id.pub_key, PUB_KEY_SIZE);
  rx.timestamp = sender_timestamp;
  StrHelper::strncpy(rx.text, text, sizeof(rx.text));
  if (_rx_queue == NULL || xQueueSend(_rx_queue, &rx, 0) != pdTRUE) _rx_dropped++;
}

// MeshCore delivered a direct message we sent. The interface is waiting on this
// to decide between confirmed delivery and channel fallback.
ContactInfo* MyMesh::processAck(const uint8_t* data) {
  _ack_pending++;
  return NULL;
}

bool MyMesh::takeAck() {
  if (_ack_pending == 0) return false;
  _ack_pending--;
  return true;
}

bool MyMesh::takeReceived(TunnelRx& out) {
  if (_rx_queue == NULL) return false;
  return xQueueReceive(_rx_queue, &out, 0) == pdTRUE;
}

// ---- MeshCoreLink: called on the RNS task, queued to the mesh task ---------

bool MyMesh::sendChannelText(const char* text, uint32_t timestamp) {
  if (_tx_queue == NULL) return false;
  TunnelTx tx;
  tx.direct = false;
  memset(tx.pub_key, 0, sizeof(tx.pub_key));
  tx.timestamp = timestamp;
  StrHelper::strncpy(tx.text, text, sizeof(tx.text));
  if (xQueueSend(_tx_queue, &tx, 0) != pdTRUE) { _tx_dropped++; return false; }
  return true;
}

bool MyMesh::sendDirectText(const uint8_t* pub_key, const char* text, uint32_t timestamp) {
  if (_tx_queue == NULL) return false;
  TunnelTx tx;
  tx.direct = true;
  memcpy(tx.pub_key, pub_key, PUB_KEY_SIZE);
  tx.timestamp = timestamp;
  StrHelper::strncpy(tx.text, text, sizeof(tx.text));
  if (xQueueSend(_tx_queue, &tx, 0) != pdTRUE) { _tx_dropped++; return false; }
  return true;
}

bool MyMesh::ensureContact(const uint8_t* pub_key, const char* name) {
  if (_bind_queue == NULL) return false;
  TunnelBind b;
  memcpy(b.pub_key, pub_key, PUB_KEY_SIZE);
  StrHelper::strncpy(b.name, name ? name : "", sizeof(b.name));
  return xQueueSend(_bind_queue, &b, 0) == pdTRUE;
}

// Create contacts requested via ensureContact(). Pathless — sendMessage flood-
// routes until the first ACK teaches the return path — but that is already a
// direct-encrypted unicast instead of a channel broadcast, which is the whole
// point: without the contact, every DIRECT send drops and falls back to
// flooding the channel (the dfall storms).
void MyMesh::drainBindQueue() {
  if (_bind_queue == NULL) return;

  TunnelBind b;
  if (xQueueReceive(_bind_queue, &b, 0) != pdTRUE) return;

  if (lookupContactByPubKey(b.pub_key, PUB_KEY_SIZE) != NULL) return;

  ContactInfo c;
  memset(&c, 0, sizeof(c));
  c.id = mesh::Identity(b.pub_key);
  StrHelper::strncpy(c.name, b.name[0] ? b.name : "rns-peer", sizeof(c.name));
  c.type = ADV_TYPE_CHAT;
  c.out_path_len = OUT_PATH_UNKNOWN;
  c.lastmod = getRTCClock()->getCurrentTime();
  bool ok = addContact(c);
  Serial.printf("[bind] contact %s for '%s' %02x%02x%02x%02x%02x%02x (contacts=%d)\n",
                ok ? "created" : "FAILED (table full?)", c.name,
                b.pub_key[0], b.pub_key[1], b.pub_key[2],
                b.pub_key[3], b.pub_key[4], b.pub_key[5], getNumContacts());
}

uint32_t MyMesh::nowEpoch() {
  return getRTCClock()->getCurrentTime();
}

// Drain fragments the RNS task queued for transmission. One per loop keeps a
// burst from monopolising the mesh loop or the radio.
void MyMesh::drainTxQueue() {
  if (_tx_queue == NULL) return;

  TunnelTx tx;
  if (xQueueReceive(_tx_queue, &tx, 0) != pdTRUE) return;

  if (tx.direct) {
    ContactInfo* c = lookupContactByPubKey(tx.pub_key, PUB_KEY_SIZE);
    if (c == NULL) {
      // Which pubkey matters: a restored peer map can point at a board that is
      // no longer powered (names moved between physical boards during bring-up)
      // and such a contact can never re-form from adverts. The prefix printed
      // here either matches the live peer's Gateway ID — auto-add problem — or
      // it doesn't: stale mc_state. The fragment is not lost either way; the
      // RNS side times out its ACK wait and falls back to CHANNEL (dfall).
      Serial.printf("[tx] DIRECT dropped: no contact for pubkey %02x%02x%02x%02x%02x%02x (contacts=%d)\n",
                    tx.pub_key[0], tx.pub_key[1], tx.pub_key[2],
                    tx.pub_key[3], tx.pub_key[4], tx.pub_key[5],
                    getNumContacts());
      return;
    }
    uint32_t expected_ack, est_timeout;
    int rc = sendMessage(*c, tx.timestamp, 0, tx.text, expected_ack, est_timeout);
    Serial.printf("[tx] DIRECT rc=%d len=%u\n", rc, (unsigned)strlen(tx.text));
    return;
  }

  if (_bridge_channel == NULL) {
    Serial.printf("[tx] CHANNEL dropped: no bridge channel\n");
    return;
  }
  noteOwnEcho(tx.text);
  bool ok = sendGroupMessage(tx.timestamp, _bridge_channel->channel,
                             _prefs.node_name, tx.text, strlen(tx.text));
  Serial.printf("[tx] CHANNEL ok=%d name='%s' len=%u ts=%u\n",
                ok ? 1 : 0, _prefs.node_name, (unsigned)strlen(tx.text),
                (unsigned)tx.timestamp);
}

// Remember what we put on the channel so the loopback can be dropped. The hash
// covers the fully-formatted "<name>: <text>" line, since that is what comes
// back through onChannelMessageRecv.
void MyMesh::noteOwnEcho(const char* text) {
  uint32_t h = 2166136261u;
  for (const char* p = _prefs.node_name; *p; p++) { h = (h ^ (uint8_t)*p) * 16777619u; }
  h = (h ^ (uint8_t)':') * 16777619u;
  h = (h ^ (uint8_t)' ') * 16777619u;
  for (const char* p = text; *p; p++) { h = (h ^ (uint8_t)*p) * 16777619u; }

  _echo_ring[_echo_next] = h;
  _echo_next = (_echo_next + 1) % ECHO_RING_SIZE;
}

bool MyMesh::isOwnEcho(const char* text) {
  uint32_t h = 2166136261u;
  for (const char* p = text; *p; p++) { h = (h ^ (uint8_t)*p) * 16777619u; }

  for (uint8_t i = 0; i < ECHO_RING_SIZE; i++) {
    if (_echo_ring[i] == h) {
      _echo_ring[i] = 0;   // one-shot
      return true;
    }
  }
  return false;
}

void MyMesh::sendFloodScoped(const mesh::GroupChannel& channel, mesh::Packet* pkt, uint32_t delay_millis) {
  if (!_tunnel_flood) {
    sendZeroHop(pkt, delay_millis);
    return;
  }
  sendFlood(pkt, delay_millis, _prefs.path_hash_mode + 1);
}

void MyMesh::sendFloodScoped(const ContactInfo& recipient, mesh::Packet* pkt, uint32_t delay_millis) {
  if (!_tunnel_flood) {
    sendZeroHop(pkt, delay_millis);
    return;
  }
  BaseChatMesh::sendFloodScoped(recipient, pkt, delay_millis);
}

uint32_t MyMesh::calcFloodTimeoutMillisFor(uint32_t pkt_airtime_millis) const {
  return SEND_TIMEOUT_BASE_MILLIS + (FLOOD_SEND_TIMEOUT_FACTOR * pkt_airtime_millis);
}

uint32_t MyMesh::calcDirectTimeoutMillisFor(uint32_t pkt_airtime_millis, uint8_t path_len) const {
  uint8_t path_hash_count = path_len & 63;
  return SEND_TIMEOUT_BASE_MILLIS +
         ((pkt_airtime_millis * DIRECT_SEND_PERHOP_FACTOR + DIRECT_SEND_PERHOP_EXTRA_MILLIS) *
          (path_hash_count + 1));
}

bool MyMesh::formatFileSystem() {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return InternalFS.format();
#elif defined(RP2040_PLATFORM)
  return LittleFS.format();
#elif defined(ESP32)
  return SPIFFS.format();
#else
  #error "need to implement file system erase"
  return false;
#endif
}

void MyMesh::sendSelfAdvertisement(int delay_millis, bool flood) {
  mesh::Packet* pkt = createSelfAdvert();
  if (pkt) {
    if (flood) {
      sendFlood(pkt, delay_millis, _prefs.path_hash_mode + 1);
    } else {
      sendZeroHop(pkt, delay_millis);
    }
  } else {
    MESH_DEBUG_PRINTLN("ERROR: unable to create advertisement packet!");
  }
}

void MyMesh::updateAdvertTimer() {
  if (_prefs.advert_interval > 0) {
    next_local_advert = futureMillis(((uint32_t)_prefs.advert_interval) * 2 * 60 * 1000);
  } else {
    next_local_advert = 0;
  }
}

void MyMesh::updateFloodAdvertTimer() {
  if (_prefs.flood_advert_interval > 0) {
    next_flood_advert = futureMillis(((uint32_t)_prefs.flood_advert_interval) * 60 * 60 * 1000);
  } else {
    next_flood_advert = 0;
  }
}

void MyMesh::formatStatsReply(char* reply) {
  StatsFormatHelper::formatCoreStats(reply, board, *_ms, _err_flags, _mgr);
}

void MyMesh::formatRadioStatsReply(char* reply) {
  StatsFormatHelper::formatRadioStats(reply, _radio, radio_driver, getTotalAirTime(), getReceiveAirTime());
}

void MyMesh::formatPacketStatsReply(char* reply) {
  StatsFormatHelper::formatPacketStats(reply, radio_driver, getNumSentFlood(), getNumSentDirect(),
                                       getNumRecvFlood(), getNumRecvDirect());
}

void MyMesh::saveIdentity(const mesh::LocalIdentity& new_id) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  IdentityStore store(*_fs, "");
#elif defined(ESP32)
  IdentityStore store(*_fs, "/identity");
#elif defined(RP2040_PLATFORM)
  IdentityStore store(*_fs, "/identity");
#else
  #error "need to define saveIdentity()"
#endif
  store.save("_main", new_id);
}

void MyMesh::clearStats() {
  radio_driver.resetStats();
  resetStats();
  ((SimpleMeshTables *)getTables())->resetStats();
}

void MyMesh::applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr, int timeout_mins) {
  set_radio_at = futureMillis(2000);   // give CLI reply time to send first
  pending_freq = freq;
  pending_bw = bw;
  pending_sf = sf;
  pending_cr = cr;
  revert_radio_at = futureMillis(2000 + timeout_mins * 60 * 1000);
}

void MyMesh::handleCommand(uint32_t sender_timestamp, char* command, char* reply) {
  while (*command == ' ') command++;   // skip leading spaces

  // Drop the tunnel's persisted peer/route tables. For when mc_state has
  // rotted — e.g. names remapped to pubkeys of boards that no longer exist
  // ('RNS Gateway' -> 9fc9bb... after the 2026-08 bring-up reshuffles) — and
  // the restored routes would aim DIRECT sends at ghosts. Takes effect on
  // reboot; the tables rebuild from live RNSBIND within seconds.
  if (strcmp(command, "clear mcstate") == 0) {
    bool ok = _fs != NULL && _fs->remove("/mc_state");
    strcpy(reply, ok ? "mc_state cleared - reboot to rebuild from live binds"
                     : "mc_state not present (or fs not ready)");
    return;
  }

  _cli.handleCommand(sender_timestamp, command, reply);
}

bool MyMesh::hasPendingWork() const {
  return false;
}

void MyMesh::loop() {
  BaseChatMesh::loop();

  drainTxQueue();
  drainBindQueue();

  if (next_flood_advert && millisHasNowPassed(next_flood_advert)) {
    mesh::Packet* pkt = createSelfAdvert();
    if (pkt) sendFlood(pkt, (uint32_t)0, _prefs.path_hash_mode + 1);
    updateFloodAdvertTimer();
    updateAdvertTimer();
  } else if (next_local_advert && millisHasNowPassed(next_local_advert)) {
    mesh::Packet* pkt = createSelfAdvert();
    if (pkt) sendZeroHop(pkt);
    updateAdvertTimer();
  }

  if (set_radio_at && millisHasNowPassed(set_radio_at)) {
    set_radio_at = 0;
    radio_driver.setParams(pending_freq, pending_bw, pending_sf, pending_cr);
    MESH_DEBUG_PRINTLN("Temp radio params");
  }

  if (revert_radio_at && millisHasNowPassed(revert_radio_at)) {
    revert_radio_at = 0;
    radio_driver.setParams(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
    MESH_DEBUG_PRINTLN("Radio params restored");
  }

  uint32_t now = millis();
  uptime_millis += now - last_millis;
  last_millis = now;
}
