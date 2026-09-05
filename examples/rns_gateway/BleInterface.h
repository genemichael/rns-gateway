/*
 * BleInterface — an RNS InterfaceImpl that carries Reticulum over BLE GATT,
 * speaking the ble-reticulum PERIPHERAL protocol (v2.2 wire format plus the
 * v0.3.0 capability advertisement).
 *
 * Client-access twin of TcpInterface: phones (Columba Android / iOS) and a
 * Linux box running the ble-reticulum package act as BLE centrals, connect
 * in to this device, and reach the mesh through the gateway's transport.
 * Same shape as TcpInterface on purpose — RNS::InterfaceImpl, serviced from
 * the RNS task on core 0, Bluedroid's callbacks hand off through FreeRTOS
 * queues, echo prevention by source slot, §8b/§8c MTU discipline.
 *
 * NORMATIVE reference: github.com/torlando-tech/ble-reticulum @ 07d9413
 * (BLEGATTServer.py, BLEInterface.py, BLEFragmentation.py). The phone apps
 * are conformance subjects against it, not authorities. What the peripheral
 * side of that reference does, and this does:
 *
 *   GATT   service  37145b00-442d-4a94-917f-8f42c5da28e3
 *          RX       ...28e5  WRITE | WRITE_NO_RESPONSE   central -> us
 *          TX       ...28e4  READ  | NOTIFY (+CCCD)      us -> central
 *          Identity ...28e6  READ, 16 bytes              our Transport identity hash
 *   Advert flags + complete 128-bit service UUID + manufacturer data
 *          FF FF 03 01 (company 0xFFFF, protocol 0x03, PERIPHERAL_ONLY);
 *          the name goes in the scan response. Sent from a random static
 *          address ff:ff:ff:… so v2.2 MAC sorting always makes the central
 *          initiate (see start()).
 *   Handshake: the FIRST write a central makes to RX is exactly 16 bytes —
 *          its own Transport identity hash. Fragments before that are
 *          dropped (reference logs a warning and waits). After it, writes
 *          are fragments. No pairing, no bonding — Reticulum's crypto rides
 *          on top.
 *   Fragments: BleFragmentation.h. Our notifications are sized from the
 *          negotiated ATT MTU per connection: fragment = ATT_MTU - 3.
 *
 * Peripheral-only and point-to-point: no scanning, no central role, no
 * MAC-comparison role logic. Several centrals may be connected at once
 * (the reference allows it too); each gets its own reassembler and identity.
 *
 * Threading. Bluedroid runs its host on the BTC task (core 0, internal-RAM
 * stack). Its callbacks here do the minimum — copy the bytes or the event
 * into a static FreeRTOS queue — and loop(), on the RNS task, does all the
 * protocol work. Nothing in the client table is touched from a callback.
 * Counters that callbacks bump are volatile and diagnostic only.
 *
 * Stack choice: Bluedroid, because it is what the framework ships and what
 * upstream's SerialBLEInterface uses, so it costs no new lib_deps. If the
 * Milestone-1 heap figures say it does not fit next to the tunnel, the
 * alternative is NimBLE-Arduino (Pyxis uses it; see its patch_nimble.py) —
 * a stack swap is an ask-Gene decision, not something to do quietly.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RNS_GATEWAY_BLE_INTERFACE_H
#define RNS_GATEWAY_BLE_INTERFACE_H

// microReticulum before any MeshCore header — MeshCore.h #defines PUB_KEY_SIZE
// and friends as bare macros that collide with RNS::Type's constants.
#include <microReticulum/Interface.h>
#include <microReticulum/Transport.h>
#include <microReticulum/Identity.h>
#include <microReticulum/Bytes.h>
#include <microReticulum/Log.h>

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <esp_gatts_api.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "BleFragmentation.h"
#include "MeshCoreTunnelCodec.h"   // ptype_name/dtype_name for frame logging
#include "SerialLog.h"

// ─── BLE Interface Configuration ─────────────────────────────────────────────
#define BLE_IF_SERVICE_UUID    "37145b00-442d-4a94-917f-8f42c5da28e3"
#define BLE_IF_TX_UUID         "37145b00-442d-4a94-917f-8f42c5da28e4"
#define BLE_IF_RX_UUID         "37145b00-442d-4a94-917f-8f42c5da28e5"
#define BLE_IF_IDENTITY_UUID   "37145b00-442d-4a94-917f-8f42c5da28e6"

#define BLE_IF_HW_MTU          500     // RNS MTU; reference BLEInterface.HW_MTU = 500
#define BLE_IF_IDENTITY_LEN    16      // Transport identity hash (TRUNCATED_HASHLENGTH/8)
#ifndef BLE_IF_MAX_CLIENTS
  // Bluedroid's CONFIG_BT_ACL_CONNECTIONS is 4. Three centrals is already
  // more phones than one gateway will see; each slot costs a 500-byte
  // reassembly arena plus bookkeeping.
  #define BLE_IF_MAX_CLIENTS   3
#endif
// Offer the largest ATT MTU; the central negotiates down. Android asks for
// 517, iOS auto-negotiates, BlueZ 517.
#define BLE_IF_LOCAL_ATT_MTU   517
// Largest single write we accept as one fragment. The reference central at
// ATT MTU 517 builds 517-byte fragments (mtu - 5, see BleFragmentation.h)
// and delivers them as GATT long writes; Bluedroid reassembles those into
// one value (ESP_GATT_MAX_ATTR_LEN is 600), so allow for it.
#define BLE_IF_MAX_FRAGMENT    520
#define BLE_IF_RX_QUEUE        6       // fragments in flight BTC task -> RNS task
#define BLE_IF_EVT_QUEUE       8       // connect/disconnect/mtu/subscribe/congest
#define BLE_IF_TX_QUEUE        8       // whole packets awaiting notification
// Minimum spacing between notifications to one central. One notification
// per connection event is the physical ceiling; 10 ms lets the controller
// queue a couple without being flooded at MTU 23 (34 fragments per packet).
#define BLE_IF_TX_INTERVAL_MS  10
// Give up on a packet for a client that stays congested this long.
#define BLE_IF_TX_STALL_MS     5000
// Bluedroid stops advertising on connect; restart so more centrals can join.
#define BLE_IF_ADV_RESTART_MS  500

// Python RNS Interface.BITRATE_GUESS — what the reference BLEInterface
// inherits, since it never sets its own. Realistic for BLE at these MTUs,
// and it sits where we want it: far above the 300 bps mesh, far below TCP.
#define BLE_IF_BITRATE         62500

enum BleIfEventType : uint8_t {
    BLE_EVT_CONNECT = 1,
    BLE_EVT_DISCONNECT,
    BLE_EVT_MTU,
    BLE_EVT_SUBSCRIBE,
    BLE_EVT_CONGEST,
};

struct BleIfEvent {
    uint8_t  type;
    uint16_t conn_id;
    uint16_t value;         // mtu / subscribed / congested
    uint8_t  bda[6];        // connect only
};

struct BleRxItem {
    uint16_t conn_id;
    uint16_t len;
    uint8_t  data[BLE_IF_MAX_FRAGMENT];
};

struct BleTxItem {
    uint16_t len;
    int8_t   exclude;       // client slot not to echo back to, or -1
    uint8_t  data[BLE_IF_HW_MTU];
};

struct BleClient {
    bool     active;
    bool     subscribed;    // CCCD notifications enabled on TX
    bool     handshaken;    // 16-byte identity received
    bool     congested;
    uint16_t conn_id;
    uint16_t att_mtu;
    uint8_t  bda[6];
    uint8_t  identity[BLE_IF_IDENTITY_LEN];
    uint32_t connected_at;
    uint32_t last_tx_ms;
    uint32_t stall_since;   // first failed/congested send of the head packet
    uint16_t tx_frag_next;  // progress through the head TX packet
    uint32_t rx_pkts;
    uint32_t tx_pkts;
    BleRns::Reassembler* reasm;
    uint8_t* arena;
};

class BleInterface : public RNS::InterfaceImpl,
                     private BLEServerCallbacks,
                     private BLECharacteristicCallbacks {
public:
    explicit BleInterface(const char* name = "BLEInterface")
        : RNS::InterfaceImpl(name)
    {
        _IN = true;
        _OUT = true;
        _HW_MTU = BLE_IF_HW_MTU;
        // MICRORETICULUM_BUGS.md §8b, as in TcpInterface: a known fixed MTU
        // is what enables link MTU clamping on forwarded LINKREQUESTs.
        _FIXED_MTU = true;
        _bitrate = BLE_IF_BITRATE;
        _announce_cap = 2.0;
        memset(_clients, 0, sizeof(_clients));
        _rx_queue  = xQueueCreateStatic(BLE_IF_RX_QUEUE,  sizeof(BleRxItem),
                                        _rx_storage,  &_rx_qstate);
        _evt_queue = xQueueCreateStatic(BLE_IF_EVT_QUEUE, sizeof(BleIfEvent),
                                        _evt_storage, &_evt_qstate);
    }

    virtual ~BleInterface() { stop(); }

    // ─── Lifecycle ───────────────────────────────────────────────────────────
    // Call AFTER Reticulum::start(): the Identity characteristic is backed by
    // Transport's identity, which does not exist before that.
    virtual bool start() override {
        if (_started) return true;
        if (self_slot() != nullptr && self_slot() != this) {
            slogln("[BleIF] another BleInterface already owns the BLE stack");
            return false;
        }

        const RNS::Identity& ident = RNS::Transport::identity();
        if (!ident) {
            slogln("[BleIF] Transport identity not loaded — start() called before Reticulum::start()?");
            return false;
        }
        const RNS::Bytes& h = ident.hash();
        if (h.size() != BLE_IF_IDENTITY_LEN) {
            slog("[BleIF] identity hash is %u bytes, expected %d\r\n",
                 (unsigned)h.size(), BLE_IF_IDENTITY_LEN);
            return false;
        }
        memcpy(_identity, h.data(), BLE_IF_IDENTITY_LEN);

        // Per-connection reassembly arenas. PSRAM if it is there: nothing
        // but the RNS task touches them, and internal RAM is what the BLE
        // and WiFi stacks are fighting over.
        for (int i = 0; i < BLE_IF_MAX_CLIENTS; i++) {
            if (_clients[i].arena) continue;
            uint8_t* a = (uint8_t*)heap_caps_malloc(BLE_IF_HW_MTU, MALLOC_CAP_SPIRAM);
            if (!a) a = (uint8_t*)malloc(BLE_IF_HW_MTU);
            if (!a) { slogln("[BleIF] out of memory for reassembly arena"); return false; }
            _clients[i].arena = a;
            _clients[i].reasm = new BleRns::Reassembler(a, BLE_IF_HW_MTU);
        }
        if (!_tx_ring) {
            _tx_ring = (BleTxItem*)heap_caps_malloc(sizeof(BleTxItem) * BLE_IF_TX_QUEUE,
                                                    MALLOC_CAP_SPIRAM);
            if (!_tx_ring) _tx_ring = (BleTxItem*)malloc(sizeof(BleTxItem) * BLE_IF_TX_QUEUE);
            if (!_tx_ring) { slogln("[BleIF] out of memory for TX ring"); return false; }
        }

        // Name: "RNS-" + first four identity bytes. Not used for discovery
        // by any conformant central (service UUID only), but it makes the
        // right device recognisable in a scanner. Goes in the scan response
        // — the advertising packet is full (3 + 18 + 6 = 27 of 31 bytes).
        snprintf(_dev_name, sizeof(_dev_name), "RNS-%02x%02x%02x%02x",
                 _identity[0], _identity[1], _identity[2], _identity[3]);

        uint32_t heap_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        self_slot() = this;
        BLEDevice::init(_dev_name);
        BLEDevice::setMTU(BLE_IF_LOCAL_ATT_MTU);
        BLEDevice::setCustomGattsHandler(gatts_event_trampoline);

        _server = BLEDevice::createServer();
        _server->setCallbacks(this);
        _service = _server->createService(BLE_IF_SERVICE_UUID);

        _tx_char = _service->createCharacteristic(
            BLE_IF_TX_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
        _cccd = new BLE2902();
        _tx_char->addDescriptor(_cccd);

        _rx_char = _service->createCharacteristic(
            BLE_IF_RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
        _rx_char->setCallbacks(this);

        _id_char = _service->createCharacteristic(
            BLE_IF_IDENTITY_UUID, BLECharacteristic::PROPERTY_READ);
        _id_char->setValue(_identity, BLE_IF_IDENTITY_LEN);

        _service->start();
        _tx_handle   = _tx_char->getHandle();
        _cccd_handle = _cccd->getHandle();
        // _gatts_if is learned from the raw event stream (the wrapper keeps
        // its copy private); every GATTS event for our app carries it, and
        // service registration has already delivered several by now.

        // Advertising packet: flags, the full service UUID, and the v0.3.0
        // capability block. Name in the scan response.
        BLEAdvertising* adv = BLEDevice::getAdvertising();
        BLEAdvertisementData ad;
        ad.setFlags(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
        ad.setCompleteServices(BLEUUID(BLE_IF_SERVICE_UUID));
        {
            // BLE_PROTOCOL_v0.3.0.md §2: CID 0xFFFF LE, version 0x03,
            // flags bit0 = PERIPHERAL_ONLY.
            const char mfg[4] = { (char)0xFF, (char)0xFF, (char)0x03, (char)0x01 };
            ad.setManufacturerData(std::string(mfg, sizeof(mfg)));
        }
        adv->setAdvertisementData(ad);
        BLEAdvertisementData sr;
        sr.setName(_dev_name);
        adv->setScanResponseData(sr);
        adv->setScanResponse(true);

        // Advertise from a BLE random STATIC address that sorts above every
        // public MAC. Protocol v2.2 decides connection direction by address
        // order — the LOWER address initiates — and neither the Python
        // reference (at 07d9413) nor Columba implements the v0.3.0
        // PERIPHERAL_ONLY flag that was written to exempt devices like this
        // one. A peripheral-only device therefore wants to sort as high as
        // possible so that every central concludes "I initiate". ff:ff:ff:…
        // is above any assigned OUI; the low three bytes come from the
        // identity so the address is stable across reboots (the reference
        // caches identity by address for 60 s after a disconnect). Random
        // static format: top two bits set, not all ones. The BLE address is
        // not part of the Reticulum identity, so nothing else changes.
        // Android compares against its app-visible 02:00:00:00:00:00 and
        // iOS never compares, so those two were fine either way.
        memset(_ble_addr, 0xFF, 3);
        _ble_addr[3] = _identity[4];
        _ble_addr[4] = _identity[5];
        _ble_addr[5] = _identity[6];
        if (_ble_addr[3] == 0xFF && _ble_addr[4] == 0xFF && _ble_addr[5] == 0xFF) {
            _ble_addr[5] = 0xFE;
        }
        adv->setDeviceAddress(_ble_addr, BLE_ADDR_TYPE_RANDOM);
        adv->start();

        _started = true;
        _online = true;
        uint32_t heap_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        slog("[BleIF] advertising as '%s' from random static %02x:%02x:%02x:%02x:%02x:%02x "
             "(public %s), identity %02x%02x%02x%02x…, "
             "max %d clients (internal heap %u -> %u, BLE stack cost %d)\r\n",
             _dev_name, _ble_addr[0], _ble_addr[1], _ble_addr[2],
             _ble_addr[3], _ble_addr[4], _ble_addr[5],
             BLEDevice::getAddress().toString().c_str(),
             _identity[0], _identity[1], _identity[2], _identity[3],
             BLE_IF_MAX_CLIENTS, (unsigned)heap_before, (unsigned)heap_after,
             (int)(heap_before - heap_after));
        return true;
    }

    // Stops serving without deinitialising Bluedroid: a mode change is a
    // reboot, and tearing the stack down live is where Bluedroid is flaky.
    virtual void stop() override {
        if (!_started) return;
        BLEDevice::stopAdvertising();
        for (int i = 0; i < BLE_IF_MAX_CLIENTS; i++) {
            if (_clients[i].active) _server->disconnect(_clients[i].conn_id);
            _release_slot(i, "stop");
        }
        _started = false;
        _online = false;
    }

    // ─── Serviced from the RNS task, alongside Reticulum::loop() ─────────────
    virtual void loop() override {
        if (!_started) return;
        uint32_t now = millis();

        _drain_events(now);
        _drain_rx(now);

        for (int i = 0; i < BLE_IF_MAX_CLIENTS; i++) {
            if (_clients[i].active && _clients[i].reasm->expire(now)) {
                slog("[BleIF] client %d: reassembly timed out, partial packet dropped\r\n", i);
            }
        }

        _pump_tx(now);

        if (_adv_restart_at && (int32_t)(now - _adv_restart_at) >= 0) {
            _adv_restart_at = 0;
            if (_num_clients < BLE_IF_MAX_CLIENTS) {
                BLEDevice::startAdvertising();
            }
        }
    }

    virtual inline std::string toString() const override {
        return "BleInterface[" + _name + "/" + _dev_name + "]";
    }

    // ─── Diagnostics ─────────────────────────────────────────────────────────
    int  clientCount() const { return _num_clients; }
    bool isStarted()   const { return _started; }
    bool isConnected() const { return _num_clients > 0; }
    const char* deviceName() const { return _dev_name; }
    // Whole RNS packets across the BLE boundary — same role as TcpInterface's
    // counters: without them, "the phone never sent it" and "the board
    // swallowed it" produce identical logs.
    uint32_t rx_frames() const { return _rx_pkts; }
    uint32_t tx_frames() const { return _tx_pkts; }
    uint32_t rx_fragments() const { return _rx_frags; }
    uint32_t tx_fragments() const { return _tx_frags; }
    // Everything dropped, by cause, so a stall has a name.
    uint32_t drop_queue_full()   const { return _drop_rx_queue; }
    uint32_t drop_pre_handshake() const { return _drop_pre_hs; }
    uint32_t drop_reassembly()   const { return _drop_reasm; }
    uint32_t drop_tx()           const { return _drop_tx; }

    bool client_info(int i, char* ident_hex, size_t hex_len, uint16_t& mtu,
                     bool& handshaken, uint32_t& connected_at) const {
        if (i < 0 || i >= BLE_IF_MAX_CLIENTS || !_clients[i].active) return false;
        const BleClient& c = _clients[i];
        size_t n = 0;
        for (int k = 0; k < BLE_IF_IDENTITY_LEN && n + 3 <= hex_len; k++) {
            n += snprintf(ident_hex + n, hex_len - n, "%02x", c.identity[k]);
        }
        mtu = c.att_mtu;
        handshaken = c.handshaken;
        connected_at = c.connected_at;
        return true;
    }

protected:
    // ─── RNS InterfaceImpl: outgoing packet from RNS Transport ───────────────
    // Queued, not sent inline: at ATT MTU 23 a 500-byte packet is 34
    // notifications, and pacing them from loop() keeps this call — and the
    // RNS task — non-blocking. Returns bool on microReticulum cd0338e7.
    virtual bool send_outgoing(const RNS::Bytes& data) override {
        if (!_started) return false;
        if (data.size() == 0 || data.size() > BLE_IF_HW_MTU) {
            slog("[BleIF] DROPPED outgoing %u-byte packet (HW_MTU %d)\r\n",
                 (unsigned)data.size(), BLE_IF_HW_MTU);
            _drop_tx++;
            return false;
        }
        // Anyone to send to? Count eligible clients excluding the sender.
        bool any = false;
        for (int i = 0; i < BLE_IF_MAX_CLIENTS; i++) {
            if (i == _last_rx_client_idx) continue;
            if (_clients[i].active && _clients[i].handshaken && _clients[i].subscribed) { any = true; break; }
        }
        if (!any) return false;

        if (_tx_count >= BLE_IF_TX_QUEUE) {
            slogln("[BleIF] TX ring full, packet dropped");
            _drop_tx++;
            return false;
        }
        BleTxItem& it = _tx_ring[(_tx_head + _tx_count) % BLE_IF_TX_QUEUE];
        it.len = (uint16_t)data.size();
        // Echo prevention, as in TcpInterface: a packet Transport forwards
        // because client N sent it must not go back to client N.
        it.exclude = (int8_t)_last_rx_client_idx;
        memcpy(it.data, data.data(), data.size());
        _tx_count++;

        InterfaceImpl::handle_outgoing(data);
        return true;
    }

    // ─── RNS InterfaceImpl: incoming packet to RNS Transport ─────────────────
    virtual void handle_incoming(const RNS::Bytes& data) override {
        TRACEF("BleInterface.handle_incoming: (%u bytes)", data.size());
        InterfaceImpl::handle_incoming(data);
    }

    // ─── Bluedroid callbacks — BTC task. Queue and leave. ────────────────────
    void onConnect(BLEServer*) override {}
    void onConnect(BLEServer*, esp_ble_gatts_cb_param_t* param) override {
        BleIfEvent e = {};
        e.type = BLE_EVT_CONNECT;
        e.conn_id = param->connect.conn_id;
        memcpy(e.bda, param->connect.remote_bda, 6);
        _post(e);
    }
    void onDisconnect(BLEServer*) override {}
    void onDisconnect(BLEServer*, esp_ble_gatts_cb_param_t* param) override {
        BleIfEvent e = {};
        e.type = BLE_EVT_DISCONNECT;
        e.conn_id = param->disconnect.conn_id;
        e.value = (uint16_t)param->disconnect.reason;
        _post(e);
    }
    void onMtuChanged(BLEServer*, esp_ble_gatts_cb_param_t* param) override {
        BleIfEvent e = {};
        e.type = BLE_EVT_MTU;
        e.conn_id = param->mtu.conn_id;
        e.value = param->mtu.mtu;
        _post(e);
    }
    // RX characteristic written. For a GATT long write the wrapper delivers
    // this once, after EXEC_WRITE, with the whole value committed — and then
    // `param` is the exec_write union member. conn_id is the first field of
    // both, so reading it through `write` is correct either way.
    void onWrite(BLECharacteristic* ch, esp_ble_gatts_cb_param_t* param) override {
        if (ch != _rx_char) return;
        size_t len = ch->getLength();
        if (len > BLE_IF_MAX_FRAGMENT) {
            // Never truncate — §8c. Count it and let it go.
            _drop_rx_oversize++;
            return;
        }
        // Not a stack local: the BTC task's stack is 3 KB
        // (CONFIG_BT_BTC_TASK_STACK_SIZE) and this item is 524 bytes. Writes
        // are delivered one at a time on that task, so one scratch is safe.
        BleRxItem& item = _rx_scratch;
        item.conn_id = param->write.conn_id;
        item.len = (uint16_t)len;
        memcpy(item.data, ch->getData(), len);
        if (xQueueSend(_rx_queue, &item, 0) != pdTRUE) {
            _drop_rx_queue++;
        }
    }
    void onWrite(BLECharacteristic*) override {}
    void onRead(BLECharacteristic*, esp_ble_gatts_cb_param_t*) override {}
    void onRead(BLECharacteristic*) override {}

private:
    // Raw GATTS events the wrapper does not surface with a connection id:
    // CCCD writes (which central subscribed) and congestion.
    static void gatts_event_trampoline(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                       esp_ble_gatts_cb_param_t* param) {
        BleInterface* self = self_slot();
        if (!self) return;
        if (gatts_if != ESP_GATT_IF_NONE) self->_gatts_if = gatts_if;
        self->_on_gatts_event(event, param);
    }

    void _on_gatts_event(esp_gatts_cb_event_t event, esp_ble_gatts_cb_param_t* param) {
        if (event == ESP_GATTS_WRITE_EVT && _cccd_handle &&
            param->write.handle == _cccd_handle && param->write.len >= 1) {
            BleIfEvent e = {};
            e.type = BLE_EVT_SUBSCRIBE;
            e.conn_id = param->write.conn_id;
            e.value = (param->write.value[0] & 0x01) ? 1 : 0;   // bit0 = notifications
            _post(e);
        } else if (event == ESP_GATTS_CONGEST_EVT) {
            BleIfEvent e = {};
            e.type = BLE_EVT_CONGEST;
            e.conn_id = param->congest.conn_id;
            e.value = param->congest.congested ? 1 : 0;
            _post(e);
        }
    }

    void _post(const BleIfEvent& e) {
        if (xQueueSend(_evt_queue, &e, 0) != pdTRUE) _drop_evt++;
    }

    // ─── RNS-task side ───────────────────────────────────────────────────────
    int _slot_for(uint16_t conn_id) const {
        for (int i = 0; i < BLE_IF_MAX_CLIENTS; i++) {
            if (_clients[i].active && _clients[i].conn_id == conn_id) return i;
        }
        return -1;
    }

    void _drain_events(uint32_t now) {
        BleIfEvent e;
        while (xQueueReceive(_evt_queue, &e, 0) == pdTRUE) {
            int idx = _slot_for(e.conn_id);
            switch (e.type) {
            case BLE_EVT_CONNECT: {
                int free_idx = -1;
                for (int i = 0; i < BLE_IF_MAX_CLIENTS; i++) {
                    if (!_clients[i].active) { free_idx = i; break; }
                }
                if (free_idx < 0) {
                    slog("[BleIF] max clients reached, dropping conn %u\r\n", e.conn_id);
                    _server->disconnect(e.conn_id);
                    break;
                }
                BleClient& c = _clients[free_idx];
                c.active = true;
                c.subscribed = false;
                c.handshaken = false;
                c.congested = false;
                c.conn_id = e.conn_id;
                c.att_mtu = 23;         // until ESP_GATTS_MTU_EVT says otherwise
                memcpy(c.bda, e.bda, 6);
                memset(c.identity, 0, sizeof(c.identity));
                c.connected_at = now;
                c.last_tx_ms = 0;
                c.stall_since = 0;
                c.tx_frag_next = 0;
                c.rx_pkts = c.tx_pkts = 0;
                c.reasm->reset();
                _num_clients++;
                slog("[BleIF] client %d connected from %02x:%02x:%02x:%02x:%02x:%02x (conn %u), awaiting identity\r\n",
                     free_idx, e.bda[0], e.bda[1], e.bda[2], e.bda[3], e.bda[4], e.bda[5], e.conn_id);
                // Keep advertising so further centrals can find us. Bluedroid
                // stopped it on connect; give the link a moment to settle.
                _adv_restart_at = now + BLE_IF_ADV_RESTART_MS;
                break;
            }
            case BLE_EVT_DISCONNECT:
                if (idx >= 0) {
                    char why[24];
                    snprintf(why, sizeof(why), "disconnected (0x%02x)", e.value);
                    _release_slot(idx, why);
                }
                _adv_restart_at = now + BLE_IF_ADV_RESTART_MS;
                break;
            case BLE_EVT_MTU:
                if (idx >= 0) {
                    _clients[idx].att_mtu = e.value;
                    slog("[BleIF] client %d: ATT MTU %u -> %u-byte fragments, %u-byte payload\r\n",
                         idx, e.value,
                         (unsigned)BleRns::max_fragment_for_att_mtu(e.value),
                         (unsigned)BleRns::payload_size(BleRns::max_fragment_for_att_mtu(e.value)));
                }
                break;
            case BLE_EVT_SUBSCRIBE:
                if (idx >= 0) {
                    _clients[idx].subscribed = (e.value != 0);
                    slog("[BleIF] client %d: notifications %s\r\n", idx, e.value ? "ON" : "off");
                }
                break;
            case BLE_EVT_CONGEST:
                if (idx >= 0) _clients[idx].congested = (e.value != 0);
                break;
            }
        }
    }

    void _release_slot(int idx, const char* reason) {
        BleClient& c = _clients[idx];
        if (!c.active) return;
        uint32_t heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        slog("[BleIF] client %d %s after %lus: rx=%lu tx=%lu (internal heap %u)\r\n",
             idx, reason, (unsigned long)((millis() - c.connected_at) / 1000),
             (unsigned long)c.rx_pkts, (unsigned long)c.tx_pkts, (unsigned)heap);
        c.active = false;
        c.subscribed = false;
        c.handshaken = false;
        c.congested = false;
        c.reasm->reset();
        if (_num_clients > 0) _num_clients--;
        if (_last_rx_client_idx == idx) _last_rx_client_idx = -1;
    }

    void _drain_rx(uint32_t now) {
        BleRxItem item;
        while (xQueueReceive(_rx_queue, &item, 0) == pdTRUE) {
            int idx = _slot_for(item.conn_id);
            if (idx < 0) {
                // Write from a connection we have not seen CONNECT for (or
                // already released). Nothing to attribute it to.
                _drop_rx_unknown++;
                continue;
            }
            BleClient& c = _clients[idx];

            if (!c.handshaken) {
                // Reference BLEInterface._handle_peripheral_data: the first
                // write must be exactly 16 bytes — the central's identity.
                // Anything else before that is dropped with a warning.
                if (item.len == BLE_IF_IDENTITY_LEN) {
                    memcpy(c.identity, item.data, BLE_IF_IDENTITY_LEN);
                    c.handshaken = true;
                    slog("[BleIF] client %d: identity %02x%02x%02x%02x%02x%02x%02x%02x… (handshake complete, MTU %u)\r\n",
                         idx, c.identity[0], c.identity[1], c.identity[2], c.identity[3],
                         c.identity[4], c.identity[5], c.identity[6], c.identity[7], c.att_mtu);
                } else {
                    _drop_pre_hs++;
                    slog("[BleIF] client %d: %u-byte write before identity handshake, dropped\r\n",
                         idx, item.len);
                }
                continue;
            }

            _rx_frags++;
            BleRns::ReasmResult r = c.reasm->receive(item.data, item.len, now);
            switch (r) {
            case BleRns::REASM_NEED_MORE:
            case BleRns::REASM_DUPLICATE:
                break;
            case BleRns::REASM_COMPLETE: {
                const uint8_t* p = c.reasm->packet();
                size_t n = c.reasm->packet_len();
                if (n == 0) break;
                RNS::Bytes data(p, n);
                _rx_pkts++;
                c.rx_pkts++;
                // One line per inbound packet BEFORE Transport sees it —
                // Transport drops unknown destinations silently, and this
                // is the only record the packet existed. Same fields as
                // TcpIF's line so the two are comparable.
                if (n >= 6) {
                    INFOF("BleIF: rx#%u cli%d %s/%s %u bytes dst=%02x%02x%02x%02x",
                          (unsigned)_rx_pkts, idx,
                          MeshCoreTunnel::ptype_name(p[0]),
                          MeshCoreTunnel::dtype_name(p[0] >> 2),
                          (unsigned)n, p[2], p[3], p[4], p[5]);
                }
                // The whole chain handle_incoming -> Transport::inbound ->
                // transmit -> send_outgoing is synchronous, so the scoped
                // set/clear is safe — and send_outgoing records it in the
                // queued packet, so the exclusion survives the TX pump.
                _last_rx_client_idx = idx;
                handle_incoming(data);
                _last_rx_client_idx = -1;
                break;
            }
            default:
                _drop_reasm++;
                slog("[BleIF] client %d: fragment rejected (%s), %u bytes\r\n",
                     idx, _reasm_name(r), item.len);
                break;
            }
        }
    }

    static const char* _reasm_name(BleRns::ReasmResult r) {
        switch (r) {
        case BleRns::REASM_ERR_SHORT:        return "short";
        case BleRns::REASM_ERR_TYPE:         return "bad type";
        case BleRns::REASM_ERR_SEQ:          return "bad seq/total";
        case BleRns::REASM_ERR_TOO_MANY:     return "too many fragments";
        case BleRns::REASM_ERR_TOTAL:        return "total mismatch";
        case BleRns::REASM_ERR_DUP_MISMATCH: return "duplicate mismatch";
        case BleRns::REASM_ERR_OVERSIZE:     return "oversize";
        default:                             return "?";
        }
    }

    // Send the head packet to every eligible client, one fragment per
    // client per BLE_IF_TX_INTERVAL_MS. The packet is popped when every
    // eligible client has finished (or stalled out).
    void _pump_tx(uint32_t now) {
        if (_tx_count == 0) return;
        BleTxItem& pkt = _tx_ring[_tx_head];

        bool all_done = true;
        for (int i = 0; i < BLE_IF_MAX_CLIENTS; i++) {
            BleClient& c = _clients[i];
            if (!c.active || !c.handshaken || !c.subscribed) continue;
            if (i == pkt.exclude) continue;

            size_t   payload = BleRns::payload_size(BleRns::max_fragment_for_att_mtu(c.att_mtu));
            uint16_t total   = BleRns::fragment_count(pkt.len, payload);
            if (c.tx_frag_next >= total) continue;        // this client is done
            all_done = false;

            if ((uint32_t)(now - c.last_tx_ms) < BLE_IF_TX_INTERVAL_MS) continue;
            if (c.congested) {
                if (c.stall_since == 0) c.stall_since = now;
                else if ((uint32_t)(now - c.stall_since) > BLE_IF_TX_STALL_MS) {
                    slog("[BleIF] client %d congested for %d ms, packet abandoned\r\n",
                         i, BLE_IF_TX_STALL_MS);
                    c.tx_frag_next = total;
                    c.stall_since = 0;
                    _drop_tx++;
                }
                continue;
            }

            uint8_t frag[BLE_IF_MAX_FRAGMENT];
            size_t flen = BleRns::build_fragment(pkt.data, pkt.len, payload, c.tx_frag_next,
                                                 frag, sizeof(frag));
            if (flen == 0) { c.tx_frag_next = total; continue; }

            // Per-connection notify (the wrapper's notify() broadcasts to
            // every peer, which defeats both echo prevention and per-MTU
            // sizing). need_confirm=false makes it a notification.
            esp_err_t rc = esp_ble_gatts_send_indicate(_gatts_if, c.conn_id, _tx_handle,
                                                       (uint16_t)flen, frag, false);
            c.last_tx_ms = now;
            if (rc == ESP_OK) {
                c.tx_frag_next++;
                c.stall_since = 0;
                _tx_frags++;
                if (c.tx_frag_next >= total) {
                    c.tx_pkts++;
                    _tx_pkts++;
                }
            } else {
                // Controller buffers full, or the link is going away. Retry
                // on the next pass; give up after the stall window.
                if (c.stall_since == 0) c.stall_since = now;
                else if ((uint32_t)(now - c.stall_since) > BLE_IF_TX_STALL_MS) {
                    slog("[BleIF] client %d: notify failing (rc=%d) for %d ms, packet abandoned\r\n",
                         i, (int)rc, BLE_IF_TX_STALL_MS);
                    c.tx_frag_next = total;
                    c.stall_since = 0;
                    _drop_tx++;
                }
            }
        }

        if (all_done) {
            _tx_head = (_tx_head + 1) % BLE_IF_TX_QUEUE;
            _tx_count--;
            for (int i = 0; i < BLE_IF_MAX_CLIENTS; i++) {
                _clients[i].tx_frag_next = 0;
                _clients[i].stall_since = 0;
            }
        }
    }

    // ─── Member variables ────────────────────────────────────────────────────
    // One BLE stack per chip, so one instance — the trampoline needs a
    // static. Function-local rather than an inline member: the build is
    // gnu++11 and inline variables would only compile as an extension.
    static BleInterface*& self_slot() {
        static BleInterface* self = nullptr;
        return self;
    }

    BLEServer*         _server = nullptr;
    BLEService*        _service = nullptr;
    BLECharacteristic* _tx_char = nullptr;
    BLECharacteristic* _rx_char = nullptr;
    BLECharacteristic* _id_char = nullptr;
    BLE2902*           _cccd = nullptr;
    uint16_t           _tx_handle = 0;
    uint16_t           _cccd_handle = 0;
    volatile esp_gatt_if_t _gatts_if = ESP_GATT_IF_NONE;   // written by the BTC task

    char       _dev_name[20] = "RNS-";
    uint8_t    _identity[BLE_IF_IDENTITY_LEN] = {0};
    esp_bd_addr_t _ble_addr = {0};            // random static, see start()
    BleClient  _clients[BLE_IF_MAX_CLIENTS];
    int        _num_clients = 0;
    bool       _started = false;
    uint32_t   _adv_restart_at = 0;
    int        _last_rx_client_idx = -1;   // echo prevention, see TcpInterface

    // BTC task -> RNS task. Static storage: queues must never be in PSRAM
    // and must exist before the BLE stack does.
    StaticQueue_t _rx_qstate;
    uint8_t       _rx_storage[BLE_IF_RX_QUEUE * sizeof(BleRxItem)];
    QueueHandle_t _rx_queue = nullptr;
    BleRxItem     _rx_scratch;            // BTC task only (onWrite)
    StaticQueue_t _evt_qstate;
    uint8_t       _evt_storage[BLE_IF_EVT_QUEUE * sizeof(BleIfEvent)];
    QueueHandle_t _evt_queue = nullptr;

    // RNS task only.
    BleTxItem* _tx_ring = nullptr;
    int        _tx_head = 0;
    int        _tx_count = 0;

    uint32_t _rx_pkts = 0, _tx_pkts = 0;
    uint32_t _rx_frags = 0, _tx_frags = 0;
    volatile uint32_t _drop_rx_queue = 0;     // BTC task
    volatile uint32_t _drop_rx_oversize = 0;  // BTC task
    volatile uint32_t _drop_evt = 0;          // BTC task
    uint32_t _drop_rx_unknown = 0;
    uint32_t _drop_pre_hs = 0;
    uint32_t _drop_reasm = 0;
    uint32_t _drop_tx = 0;
};

#endif // RNS_GATEWAY_BLE_INTERFACE_H
