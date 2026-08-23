/*
 * MeshCoreInterface.h
 *
 * A microReticulum (RNS) Interface that tunnels Reticulum packets over a
 * MeshCore LoRa mesh, using the *exact wire format* of
 *   comms-engineer/RNS_Over_Meshcore  (MeshCore_Dynamic_Interface.py).
 *
 * Single-device port of the two-board bridge. There, the RNS stack ran on a
 * RAK4631 and drove a Heltec V4 companion radio across a UART. Here MeshCore
 * owns the radio in the same binary, so the companion protocol, the owned
 * drain state machine, the handshake/liveness watchdogs and the frame parser
 * are all gone — replaced by MeshCoreLink, four in-process calls.
 *
 *   [ RNS task, core 0 ]  --MeshCoreLink-->  [ MeshCore task, core 1 ]  --LoRa--> mesh
 *
 * Wire format helpers live in MeshCoreTunnelCodec (host-testable), unchanged.
 *
 * SCOPE: fragmentation, channel-broadcast TX, RX reassembly, sliding-window
 * dedup, stale cleanup, ANNOUNCE / PATH-REQUEST rate limiting with a
 * path-response bypass, demand-driven RNSBIND peer discovery, and unicast
 * DIRECT routing with ACK-confirmed delivery and CHANNEL fallback.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MESHCORE_INTERFACE_H
#define MESHCORE_INTERFACE_H

#include <deque>
#include <map>
#include <string>
#include <vector>
#include <stdint.h>

// microReticulum before any MeshCore header — MeshCore.h #defines PUB_KEY_SIZE
// and friends as bare macros that collide with RNS::Type's constants.
#include <microReticulum.h>

#include "MeshCoreLink.h"
#include "MeshCoreTunnelCodec.h"
#include "PropPolicy.h"

class MeshCoreInterface : public RNS::InterfaceImpl {
public:
    struct Config {
        const char* name          = "MeshCore";
        uint16_t    payload_size  = 64;
        uint32_t    fragment_delay_ms = 2500;    // between CHANNEL fragments
        uint32_t    direct_frag_delay_ms = 500;  // between DIRECT fragments
        uint32_t    fragment_timeout_ms = 300000; // 5 min
        uint32_t    dedup_ttl_ms  = 30000;

        // ── Announce / discovery policy (matches the Python reference) ──────
        uint32_t    announce_rate_ms = 600000;    // 10 min per destination
        // Per-destination path-request throttle. The reference default is
        // 1800 s; 0 (always allow) was a bring-up setting and is a hole on a
        // 300 bps channel — a fresh-boot client can storm hundreds of requests.
        uint32_t    path_req_rate_ms = 1800000;
        uint32_t    path_req_burst_window_ms = 60000;
        uint32_t    path_response_bypass_ms  = 15000;

        // ── Direct routing ─────────────────────────────────────────────────
        bool        allow_direct = true;
        bool        can_route    = true;    // advertise R (router) vs E (edge)
        uint32_t    direct_ack_timeout_ms     = 4000;   // min ACK wait
        uint32_t    direct_ack_timeout_max_ms = 8000;   // hard ACK-wait ceiling
        uint32_t    peer_ttl_ms  = 86400000;            // 24 h

        uint32_t    bitrate       = 300;
        size_t      max_outq      = 64;
        size_t      max_assembly  = 16;

        // ── Prop-node-restricted policy (the *_prop build variants) ─────────
        // When set, outbound mesh traffic must serve a whitelisted destination
        // (see prop_add_dest) or a link established with one. Runtime flag so
        // the policy code compiles — and host-tests — in every variant; the
        // RNS_GW_PROP_ONLY build flag just forces it on.
        bool        prop_only = false;
    };

    MeshCoreInterface(MeshCoreLink& link, const Config& cfg);
    virtual ~MeshCoreInterface();

    virtual bool start() override;
    virtual void stop()  override;
    virtual void loop()  override;

    // ── Inbound, called from the MeshCore task via the role class ──────────
    // 'text' is a whole channel message, still in "<sender>: <body>" form.
    void on_channel_text(const char* text, uint32_t timestamp);
    // A direct (contact) message carrying a tunnel fragment.
    void on_contact_text(const uint8_t* pub_key, const char* text, uint32_t timestamp);
    // MeshCore confirmed delivery of the in-flight direct fragment.
    void on_direct_ack(uint32_t ack_code);

    // ── Diagnostics ────────────────────────────────────────────────────────
    uint32_t    rns_tx_packets() const { return _rns_tx_packets; }
    uint32_t    rns_rx_packets() const { return _rns_rx_packets; }
    uint32_t    channel_msgs()   const { return _chan_msgs; }
    uint32_t    announce_suppressed() const { return _announce_suppressed; }
    size_t      outq_depth()     const { return _outq.size(); }
    size_t      peer_count()     const { return _peer_table.size(); }
    size_t      route_count()    const { return _rns_to_mc_map.size(); }
    uint32_t    direct_tx()      const { return _direct_tx_frames; }
    uint32_t    direct_fallbacks() const { return _direct_fallbacks; }
    uint32_t    bind_tx()        const { return _bind_tx; }
    uint32_t    bind_rx()        const { return _bind_rx; }
    uint32_t    prop_dropped()   const { return _prop_dropped; }

    // Whitelist a prop destination hash (2*RNS_DST_LEN hex chars). Call before
    // start(); returns false on malformed input. Only consulted when
    // Config::prop_only is set.
    bool prop_add_dest(const char* hex) {
        return hex != nullptr && _prop.add_dest_hex(hex);
    }

private:
    virtual bool send_outgoing(const RNS::Bytes& data) override;
    void on_incoming(const RNS::Bytes& data);

    // Outgoing queue element: how a fragment should be sent.
    enum TxMode { TX_CHANNEL, TX_DIRECT };
    struct OutFrag { TxMode mode; std::string target_hex; std::string frag; };

    // Outgoing worker. With the companion gone there is no RESP_SENT to wait
    // for, so the state machine loses its AWAIT_SENT leg: a fragment is either
    // idle or waiting on a delivery ACK.
    enum TxState { TXS_IDLE, TXS_AWAIT_ACK };
    void process_outq(uint32_t now);
    void direct_fallback_to_channel(uint32_t now);
    uint32_t sender_ts() const;

    void load_state();
    void save_state();
    inline void mark_state_dirty() {
        if (!_state_dirty) {
            _state_dirty   = true;
            _save_after_ms = millis() + SAVE_DEBOUNCE_MS;
        }
    }

    void enqueue_packet(const uint8_t* data, size_t len, uint32_t pkt_id,
                        TxMode mode, const std::string& target_hex);
    void process_tunnel_text(const std::string& text, const std::string& sender);
    void learn_token(const std::string& sender, const std::vector<uint8_t>& full);
    bool rate_limit_ok(const uint8_t* data, size_t len);
    void run_cleanup(uint32_t now);

    // Peer discovery.
    void ensure_peer_contact(const std::string& key_hex, const std::string& name);
    void handle_bind(const MeshCoreTunnel::BindMsg& b, uint32_t now);
    void bind_discovery_step(uint32_t now);
    void send_bind(bool is_req);
    std::string resolve_sender_key(const std::string& key_hex);

    Config            _cfg;
    MeshCoreLink&     _link;
    std::string       _own_pubkey_hex;   // our MeshCore identity (64 hex)
    bool              _have_pubkey = false;
    uint32_t          _pkt_id = 0;

    std::deque<OutFrag> _outq;
    uint32_t          _next_tx_ms = 0;

    TxState           _tx_state = TXS_IDLE;
    OutFrag           _cur;                 // in-flight fragment
    bool              _got_ack     = false;
    uint32_t          _ack_deadline_ms = 0;

    struct Asm { uint8_t total; uint32_t ts; std::map<uint8_t, std::vector<uint8_t>> frags; };
    std::map<std::string, Asm> _assembly;
    std::map<std::string, uint32_t> _seen;
    std::map<std::string, uint32_t> _announce_sent;
    std::map<std::string, std::pair<uint32_t,uint32_t>> _path_req_sent;
    std::map<std::string, uint32_t> _path_response_pending;   // dest10 hex -> expiry

    // Peer / route tables (all keyed by lowercase hex strings).
    std::map<std::string, std::string> _peer_table;    // name  -> pubkey hex
    std::map<std::string, std::string> _reverse_peers; // pubkey/prefix hex -> name
    std::map<std::string, bool>        _peer_caps;      // name  -> can_route
    std::map<std::string, uint32_t>    _peer_last_seen; // name  -> millis
    std::map<std::string, std::string> _rns_to_mc_map;  // token16 hex -> pubkey hex

    // Bind-discovery state machine.
    uint32_t          _bind_settle_deadline_ms = 0;
    uint32_t          _bind_next_ms = 0;
    int               _bind_retries = 0;
    uint32_t          _bind_pending_resp_ms = 0;  // 0 = none scheduled

    uint32_t          _last_cleanup_ms = 0;
    uint32_t          _rns_tx_packets = 0;
    uint32_t          _rns_rx_packets = 0;
    uint32_t          _chan_msgs = 0;
    uint32_t          _announce_suppressed = 0;
    uint32_t          _direct_tx_frames = 0;
    uint32_t          _direct_fallbacks = 0;
    uint32_t          _bind_tx = 0;
    uint32_t          _bind_rx = 0;
    uint32_t          _prop_dropped = 0;

    MeshCoreTunnel::PropPolicy _prop;

    bool              _state_dirty   = false;
    uint32_t          _save_after_ms = 0;

    static const uint8_t PTYPE_DATA     = 0x00;
    static const uint8_t PTYPE_ANNOUNCE = 0x01;
    static const uint8_t DTYPE_PLAIN    = 0x02;

    static const size_t   _RNS_MAP_MAX          = 512;
    static const uint32_t BIND_BACKOFF_MIN_MS   = 3000;
    static const uint32_t BIND_BACKOFF_MAX_MS   = 15000;
    static const uint32_t BIND_HEARTBEAT_MS     = 3600000;
    static const uint32_t BIND_RESP_WINDOW_MS   = 60000;
    static const uint32_t BIND_SETTLE_MS        = 5000;
    static const int      BIND_MAX_RETRIES      = 3;
    static const uint32_t SAVE_DEBOUNCE_MS      = 300000;
};

#endif // MESHCORE_INTERFACE_H
