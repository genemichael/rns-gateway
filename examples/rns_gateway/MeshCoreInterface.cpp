/*
 * MeshCoreInterface.cpp — hybrid channel/direct path of
 * comms-engineer/RNS_Over_Meshcore (MeshCore_Dynamic_Interface.py).
 *
 * SPDX-License-Identifier: MIT
 */
#include "MeshCoreInterface.h"

#include <SHA256.h>   // rweather/Crypto — for LINK_REQUEST link_id pre-binding
#include <SPIFFS.h>   // state persistence (RNS_USE_FS is not enabled in this build)

namespace {

std::string to_hex(const uint8_t* p, size_t n) {
    static const char* h = "0123456789abcdef";
    std::string s; s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { s += h[p[i] >> 4]; s += h[p[i] & 0xF]; }
    return s;
}

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decode exactly `outlen` bytes from a 2*outlen hex string. false if malformed.
bool hex_to_bytes(const std::string& hex, uint8_t* out, size_t outlen) {
    if (hex.size() != outlen * 2) return false;
    for (size_t i = 0; i < outlen; ++i) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

} // namespace

MeshCoreInterface::MeshCoreInterface(MeshCoreLink& link, const Config& cfg)
    : RNS::InterfaceImpl(cfg.name),
      _cfg(cfg),
      _link(link) {
    _bitrate = cfg.bitrate;
}

MeshCoreInterface::~MeshCoreInterface() {}

bool MeshCoreInterface::start() {
    const char* pk = _link.selfPubKeyHex();
    _own_pubkey_hex = pk ? pk : "";
    _have_pubkey = !_own_pubkey_hex.empty();

    // Load flash state before anything else so learned peers/routes survive a
    // power-cycle even if the mesh identity hasn't loaded yet.
    load_state();
    _state_dirty   = false;
    _save_after_ms = 0;

    uint32_t now = millis();
    _bind_settle_deadline_ms = now + BIND_SETTLE_MS;
    _bind_next_ms            = _bind_settle_deadline_ms;
    randomSeed(now ^ (uint32_t)(uintptr_t)this);

    _IN = true;
    _OUT = true;
    _online = true;
    return true;
}

void MeshCoreInterface::stop() {
    _online = false;
    _outq.clear();
}

uint32_t MeshCoreInterface::sender_ts() const {
    uint32_t epoch = _link.nowEpoch();
    if (epoch != 0) return epoch;
    // Local stand-in only — stamped on the wire, never treated as wall-clock truth.
    return 1700000000UL + (millis() / 1000UL);
}

void MeshCoreInterface::loop() {
    uint32_t now = millis();

    // Mesh identity can load after start() returns — keep trying until we
    // have our own pubkey for RNSBIND / self-filtering.
    if (!_have_pubkey) {
        const char* pk = _link.selfPubKeyHex();
        if (pk && pk[0]) {
            _own_pubkey_hex = pk;
            _have_pubkey = true;
        }
    }

    bind_discovery_step(now);
    process_outq(now);

    if ((now - _last_cleanup_ms) >= 30000) {
        run_cleanup(now);
        _last_cleanup_ms = now;
    }

    // Debounced write-back: wait SAVE_DEBOUNCE_MS after the *dirty* event
    // (not after boot/last save) before touching flash.
    if (_state_dirty && (int32_t)(now - _save_after_ms) >= 0) {
        save_state();
        _state_dirty   = false;
        _save_after_ms = 0;
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Outgoing worker: single-threaded DIRECT ACK state machine + CHANNEL sends.
// ─────────────────────────────────────────────────────────────────────────

void MeshCoreInterface::process_outq(uint32_t now) {
    switch (_tx_state) {
    case TXS_IDLE: {
        if (_outq.empty()) return;
        if ((int32_t)(now - _next_tx_ms) < 0) return;

        OutFrag item = _outq.front();
        _outq.pop_front();
        _cur = item;
        _got_ack = false;

        if (item.mode == TX_CHANNEL) {
            uint32_t ts = sender_ts();
            INFOF("MeshCoreInterface: TX channel frag (%u chars) ts=%lu",
                  (unsigned)item.frag.size(), (unsigned long)ts);
            _link.sendChannelText(item.frag.c_str(), ts);
            _next_tx_ms = now + _cfg.fragment_delay_ms;
            return;
        }

        // DIRECT
        uint8_t pk[32];
        if (!hex_to_bytes(item.target_hex, pk, 32)) {
            // Malformed target — degrade to channel.
            _link.sendChannelText(item.frag.c_str(), sender_ts());
            _next_tx_ms = now + _cfg.fragment_delay_ms;
            return;
        }
        INFOF("MeshCoreInterface: TX DIRECT frag -> %.12s...", item.target_hex.c_str());
        if (!_link.sendDirectText(pk, item.frag.c_str(), sender_ts())) {
            direct_fallback_to_channel(now);
            return;
        }
        uint32_t to = _cfg.direct_ack_timeout_ms;
        if (to > _cfg.direct_ack_timeout_max_ms) to = _cfg.direct_ack_timeout_max_ms;
        _ack_deadline_ms = now + to;
        _tx_state = TXS_AWAIT_ACK;
        return;
    }

    case TXS_AWAIT_ACK: {
        if (_got_ack) {
            _direct_tx_frames++;
            _tx_state = TXS_IDLE;
            _next_tx_ms = now + _cfg.direct_frag_delay_ms;
            return;
        }
        if ((int32_t)(now - _ack_deadline_ms) >= 0) {
            direct_fallback_to_channel(now);
        }
        return;
    }
    }
}

void MeshCoreInterface::direct_fallback_to_channel(uint32_t now) {
    INFOF("MeshCoreInterface: DIRECT send failed -> CHANNEL fallback (%.12s...)",
          _cur.target_hex.c_str());
    // Re-send this fragment on the channel; front so ordering is preserved.
    _outq.push_front(OutFrag{TX_CHANNEL, std::string(), _cur.frag});
    _direct_fallbacks++;
    _tx_state = TXS_IDLE;
    _next_tx_ms = now;   // send the fallback promptly
}

// ─────────────────────────────────────────────────────────────────────────
// Peer discovery (RNSBIND / RNSBIND_REQ)
// ─────────────────────────────────────────────────────────────────────────

void MeshCoreInterface::send_bind(bool is_req) {
    if (!_have_pubkey) return;
    std::string text = MeshCoreTunnel::encode_bind(
        _own_pubkey_hex, _cfg.can_route, is_req);
    // Same outq as RNS frags — never bypass the TX state machine / pile onto
    // an in-flight CHANNEL send.
    if (_outq.size() >= _cfg.max_outq) {
        WARNING("MeshCoreInterface: outq full — dropping bind");
        return;
    }
    _outq.push_back(OutFrag{TX_CHANNEL, std::string(), std::move(text)});
    _bind_tx++;
    INFOF("MeshCoreInterface: queued %s cap=%c (outq=%u)",
          is_req ? "RNSBIND_REQ" : "RNSBIND",
          _cfg.can_route ? 'R' : 'E', (unsigned)_outq.size());
}

void MeshCoreInterface::bind_discovery_step(uint32_t now) {
    if (!_have_pubkey) return;
    if ((int32_t)(now - _bind_settle_deadline_ms) < 0) return;

    // Deferred (IGMP-style) response to an overheard REQ.
    if (_bind_pending_resp_ms != 0 && (int32_t)(now - _bind_pending_resp_ms) >= 0) {
        send_bind(false);
        _bind_pending_resp_ms = 0;
    }

    if ((int32_t)(now - _bind_next_ms) < 0) return;

    bool have_peers = !_peer_table.empty();
    if (!have_peers && _bind_retries < BIND_MAX_RETRIES) {
        send_bind(true);
        _bind_retries++;
        _bind_next_ms = now + BIND_RESP_WINDOW_MS;
    } else {
        _bind_retries = 0;
        send_bind(false);   // quiet heartbeat
        _bind_next_ms = now + BIND_HEARTBEAT_MS;
    }
}

void MeshCoreInterface::handle_bind(const MeshCoreTunnel::BindMsg& b, uint32_t now) {
    if (b.sender_name.empty() || b.mc_pubkey.empty()) {
        // Matches the reference (line 846): a nameless RNSBIND is dropped. Made
        // visible so we can tell "firmware didn't prepend a name" apart from
        // "no RNSBIND arrived at all".
        WARNINGF("MeshCoreInterface: RNSBIND dropped — %s (key=%.16s)",
                 b.sender_name.empty() ? "no sender name" : "no pubkey",
                 b.mc_pubkey.c_str());
        return;
    }
    if (b.mc_pubkey == _own_pubkey_hex) return;

    _bind_rx++;

    auto it = _peer_table.find(b.sender_name);
    bool changed = (it == _peer_table.end() || it->second != b.mc_pubkey);
    if (changed) {
        _peer_table[b.sender_name]   = b.mc_pubkey;
        _reverse_peers[b.mc_pubkey]  = b.sender_name;
        for (size_t pfx_len : {8u, 12u, 16u, 24u}) {
            if (b.mc_pubkey.size() >= pfx_len)
                _reverse_peers[b.mc_pubkey.substr(0, pfx_len)] = b.sender_name;
        }
        INFOF("MeshCoreInterface: %s '%s' -> %.16s... [%s]",
              b.is_req ? "REQ from" : "peer",
              b.sender_name.c_str(), b.mc_pubkey.c_str(),
              b.can_route ? "router" : "edge");
        mark_state_dirty();
    }
    if (_peer_caps.find(b.sender_name) == _peer_caps.end() ||
        _peer_caps[b.sender_name] != b.can_route) {
        mark_state_dirty();
    }
    _peer_caps[b.sender_name]      = b.can_route;
    _peer_last_seen[b.sender_name] = now;

    if (b.is_req && _have_pubkey && _bind_pending_resp_ms == 0) {
        long delay_ms = random((long)BIND_BACKOFF_MIN_MS, (long)BIND_BACKOFF_MAX_MS);
        _bind_pending_resp_ms = now + (uint32_t)delay_ms;
    }

    // Every valid bind — including heartbeats — re-ensures the MeshCore contact.
    // Contacts otherwise form only from adverts, so a peer whose advert we never
    // heard is routable in the peer map but unsendable in MeshCore, and every
    // DIRECT attempt drops into channel fallback. ensureContact() is a no-op on
    // the mesh task when the contact already exists.
    ensure_peer_contact(b.mc_pubkey, b.sender_name);
}

void MeshCoreInterface::ensure_peer_contact(const std::string& key_hex,
                                            const std::string& name) {
    uint8_t pk[32];
    if (!hex_to_bytes(key_hex, pk, sizeof(pk))) {
        WARNINGF("MeshCoreInterface: bad peer pubkey hex (len=%u) for '%s'",
                 (unsigned)key_hex.size(), name.c_str());
        return;
    }
    _link.ensureContact(pk, name.c_str());
}

std::string MeshCoreInterface::resolve_sender_key(const std::string& key_hex) {
    if (key_hex.empty()) return key_hex;
    auto it = _reverse_peers.find(key_hex);
    if (it != _reverse_peers.end()) return it->second;
    for (const auto& kv : _reverse_peers) {
        const std::string& k = kv.first;
        if (k.rfind(key_hex, 0) == 0 || key_hex.rfind(k, 0) == 0)
            return kv.second;
    }
    return key_hex;
}

// ─────────────────────────────────────────────────────────────────────────
// Outbound path
// ─────────────────────────────────────────────────────────────────────────

bool MeshCoreInterface::send_outgoing(const RNS::Bytes& data) {
    if (!_online) return false;

    const uint8_t* p = data.data();
    size_t len = data.size();
    if (len == 0) return false;

    // Prop-only policy gate. Runs before rate limiting so blocked traffic
    // never consumes an announce/path-request slot for a dest we won't serve.
    if (_cfg.prop_only) {
        auto d = _prop.check_outgoing(p, len);
        if (!d.link_preimage.empty()) {
            // ALLOWed LINK_REQUEST to a prop dest — register the link id so
            // the link's PROOF and DATA pass the policy from here on.
            SHA256 sha;
            sha.update(d.link_preimage.data(), d.link_preimage.size());
            uint8_t dig[32];
            sha.finalize(dig, sizeof(dig));
            _prop.add_link(dig, sizeof(dig));
        }
        if (d.verdict == MeshCoreTunnel::PropPolicy::DROP) {
            _prop_dropped++;
            WARNINGF("MeshCoreInterface: prop-only dropped %s (%s)",
                     MeshCoreTunnel::packet_kind(p[0]), d.reason);
            return false;
        }
    }

    if (!rate_limit_ok(p, len)) {
        WARNINGF("MeshCoreInterface: rate-limited %s (%s/%s)",
                 MeshCoreTunnel::packet_kind(p[0]),
                 MeshCoreTunnel::ptype_name(p[0] & 0x03),
                 MeshCoreTunnel::dtype_name((p[0] >> 2) & 0x03));
        return false;
    }

    uint8_t total = MeshCoreTunnel::fragment_count(len, _cfg.payload_size);
    if (total == 0) {
        HEAD("MeshCoreInterface: packet too large for fragment encoding", RNS::LOG_ERROR);
        return false;
    }
    if (_outq.size() + total > _cfg.max_outq) {
        HEAD("MeshCoreInterface: TX queue full — dropping packet", RNS::LOG_WARNING);
        return false;
    }

    // Route selection: DIRECT if we have a bound MeshCore contact for this
    // destination token, else CHANNEL. Broadcast packets always go CHANNEL.
    TxMode mode = TX_CHANNEL;
    std::string target_hex;
    if (!MeshCoreTunnel::is_broadcast_packet(p, len) && _cfg.allow_direct) {
        uint8_t tok[MeshCoreTunnel::RNS_DST_LEN];
        if (MeshCoreTunnel::extract_rns_token(p, len, tok)) {
            std::string tok_hex = to_hex(tok, MeshCoreTunnel::RNS_DST_LEN);
            auto it = _rns_to_mc_map.find(tok_hex);
            if (it != _rns_to_mc_map.end()) {
                mode = TX_DIRECT;
                target_hex = it->second;
            }
        }
    }

    uint32_t id = _pkt_id;
    _pkt_id = (_pkt_id + 1) & 0xFFFFFFFF;

    enqueue_packet(p, len, id, mode, target_hex);
    _rns_tx_packets++;
    INFOF("MeshCoreInterface: queued %s %u bytes -> %u frags [%s] (outq=%u)",
          MeshCoreTunnel::packet_kind(p[0]), (unsigned)len, (unsigned)total,
          mode == TX_DIRECT ? "DIRECT" : "CHANNEL", (unsigned)_outq.size());
    return true;
}

void MeshCoreInterface::enqueue_packet(const uint8_t* data, size_t len, uint32_t pkt_id,
                                       TxMode mode, const std::string& target_hex) {
    const uint16_t ps = _cfg.payload_size;
    uint8_t total = MeshCoreTunnel::fragment_count(len, ps);
    if (total == 0) return;

    for (uint8_t idx = 0; idx < total; ++idx) {
        size_t off = (size_t)idx * ps;
        size_t chunk = (len - off < ps) ? (len - off) : ps;
        std::string wire = MeshCoreTunnel::encode_channel_fragment(
            idx, pkt_id, total, data + off, chunk);
        if (wire.empty()) continue;
        _outq.push_back(OutFrag{mode, target_hex, std::move(wire)});
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Inbound path
// ─────────────────────────────────────────────────────────────────────────

void MeshCoreInterface::on_channel_text(const char* text_c, uint32_t timestamp) {
    std::string text(text_c);

    // Bring-up visibility: show the verbatim on-wire text. MeshCore firmware
    // prepends "<NodeName>: " before the body, so a peer's RNSBIND looks like
    // "Peer: RNSBIND:<pubkey>:R" here even though the chat app hides the name.
    INFOF("MeshCoreInterface: CHAN rx '%.100s'", text.c_str());

    // Discovery messages take precedence when they appear before any RNS:.
    size_t rns_idx  = text.find(MeshCoreTunnel::MSG_PREFIX);
    size_t bind_idx = text.find(MeshCoreTunnel::BIND_PREFIX);
    size_t req_idx  = text.find(MeshCoreTunnel::BIND_REQ_PREFIX);
    size_t eff_bind = std::string::npos;
    if (req_idx != std::string::npos &&
        (bind_idx == std::string::npos || req_idx <= bind_idx)) {
        eff_bind = req_idx;
    } else if (bind_idx != std::string::npos) {
        eff_bind = bind_idx;
    }

    if (eff_bind != std::string::npos &&
        (rns_idx == std::string::npos || eff_bind < rns_idx)) {
        MeshCoreTunnel::BindMsg b;
        if (MeshCoreTunnel::parse_bind(text, b)) handle_bind(b, millis());
        return;
    }

    if (rns_idx == std::string::npos) return;

    _chan_msgs++;
    std::string sender;
    if (rns_idx > 0) {
        sender = text.substr(0, rns_idx);
        while (!sender.empty() && (sender.back() == ' ' || sender.back() == ':'))
            sender.pop_back();
    }
    process_tunnel_text(text.substr(rns_idx), sender);
}

void MeshCoreInterface::on_contact_text(const uint8_t* pub_key, const char* text_c, uint32_t timestamp) {
    std::string text(text_c);
    if (text.rfind(MeshCoreTunnel::MSG_PREFIX, 0) != 0) return;   // not a tunnel fragment

    std::string key_hex = to_hex(pub_key, 32);
    std::string sender  = resolve_sender_key(key_hex);
    INFOF("MeshCoreInterface: DIRECT rx from %.12s...", key_hex.c_str());
    process_tunnel_text(text, sender);
}

void MeshCoreInterface::on_direct_ack(uint32_t ack_code) {
    if (_tx_state == TXS_AWAIT_ACK) _got_ack = true;
}

void MeshCoreInterface::process_tunnel_text(const std::string& text, const std::string& sender) {
    MeshCoreTunnel::FragHeader hdr;
    std::vector<uint8_t> chunk;
    if (!MeshCoreTunnel::decode_channel_fragment(text, hdr, chunk)) {
        WARNINGF("MeshCoreInterface: bad RNS: frag from '%s' (len=%u)",
                 sender.c_str(), (unsigned)text.size());
        return;
    }

    std::string key = sender;
    key.push_back('\0');
    key.append(reinterpret_cast<const char*>(&hdr.pkt_id), sizeof(hdr.pkt_id));

    uint32_t now = millis();

    auto seen = _seen.find(key);
    if (seen != _seen.end()) {
        if ((int32_t)(now - seen->second) < 0) return;
        _seen.erase(seen);
    }

    if (_assembly.find(key) == _assembly.end() && _assembly.size() >= _cfg.max_assembly) {
        HEAD("MeshCoreInterface: assembly table full — dropping fragment", RNS::LOG_WARNING);
        return;
    }

    Asm& a = _assembly[key];
    if (a.frags.empty()) a.total = hdr.frag_total;
    a.ts = now;
    if (a.frags.count(hdr.frag_idx)) return;
    a.frags[hdr.frag_idx] = std::move(chunk);

    INFOF("MeshCoreInterface: frag %u/%u pkt=0x%08lx from '%s' (%u/%u held)",
          (unsigned)hdr.frag_idx, (unsigned)hdr.frag_total,
          (unsigned long)hdr.pkt_id, sender.c_str(),
          (unsigned)a.frags.size(), (unsigned)a.total);

    if (a.frags.size() < a.total) return;

    std::vector<uint8_t> full;
    for (uint8_t i = 0; i < a.total; ++i) {
        auto it = a.frags.find(i);
        if (it == a.frags.end()) { _assembly.erase(key); return; }
        full.insert(full.end(), it->second.begin(), it->second.end());
    }
    _assembly.erase(key);
    _seen[key] = now + _cfg.dedup_ttl_ms;

    if (full.empty()) return;

    learn_token(sender, full);

    // An inbound DATA+PLAIN path request will make Transport emit a fresh
    // ANNOUNCE for that destination almost immediately — flag it so the
    // announce rate limiter lets that demand-driven response through.
    uint8_t ptype = full[0] & 0x03;
    uint8_t dtype = (full[0] >> 2) & 0x03;
    if (ptype == PTYPE_DATA && dtype == DTYPE_PLAIN && full.size() >= 12) {
        // The flag must key on the QUERIED destination (in the payload) — the
        // packet's address field is the shared path-request broadcast hash,
        // which no announce is ever keyed by, so the old data+2 key meant
        // this bypass never fired at all.
        uint8_t target[MeshCoreTunnel::RNS_DST_LEN];
        if (MeshCoreTunnel::extract_path_request_target(full.data(), full.size(), target)) {
            _path_response_pending[to_hex(target, 10)] =
                now + _cfg.path_response_bypass_ms;
        }
    }

    RNS::Bytes out(full.data(), full.size());
    _rns_rx_packets++;
    INFOF("MeshCoreInterface: reassembled %s %u bytes (%s/%s) from '%s'",
          MeshCoreTunnel::packet_kind(full[0]), (unsigned)full.size(),
          MeshCoreTunnel::ptype_name(ptype), MeshCoreTunnel::dtype_name(dtype),
          sender.c_str());
    on_incoming(out);
}

void MeshCoreInterface::learn_token(const std::string& sender,
                                    const std::vector<uint8_t>& full) {
    if (sender.empty()) return;
    auto pit = _peer_table.find(sender);
    if (pit == _peer_table.end()) return;      // unknown sender — no pubkey to bind
    const std::string& mc_key = pit->second;

    uint8_t tok[MeshCoreTunnel::RNS_DST_LEN];
    if (MeshCoreTunnel::extract_rns_token(full.data(), full.size(), tok)) {
        std::string tok_hex = to_hex(tok, MeshCoreTunnel::RNS_DST_LEN);
        if (_rns_to_mc_map.find(tok_hex) == _rns_to_mc_map.end()) {
            if (_rns_to_mc_map.size() >= _RNS_MAP_MAX) {
                // Trim half (oldest by map order) to bound heap use.
                size_t drop = _RNS_MAP_MAX / 2;
                for (auto it = _rns_to_mc_map.begin();
                     drop > 0 && it != _rns_to_mc_map.end(); --drop)
                    it = _rns_to_mc_map.erase(it);
            }
            _rns_to_mc_map[tok_hex] = mc_key;
            mark_state_dirty();
            INFOF("MeshCoreInterface: linked token %.8s -> '%s'",
                  tok_hex.c_str(), sender.c_str());
        }
    }

    // LINK_REQUEST: pre-bind the future link_id so the PROOF/DATA can go direct.
    std::vector<uint8_t> pre;
    if (MeshCoreTunnel::link_id_preimage(full.data(), full.size(), pre)) {
        SHA256 sha;
        sha.update(pre.data(), pre.size());
        uint8_t dig[32];
        sha.finalize(dig, sizeof(dig));
        std::string link_hex = to_hex(dig, MeshCoreTunnel::RNS_DST_LEN);
        // Inbound LINK_REQUEST (e.g. the prop node dialing a client behind
        // us): register with the prop policy too, or the client's reply proof
        // is dropped on its way out. Harmless when prop_only is off.
        _prop.add_link(dig, sizeof(dig));
        if (_rns_to_mc_map.find(link_hex) == _rns_to_mc_map.end()) {
            _rns_to_mc_map[link_hex] = mc_key;
            mark_state_dirty();
            INFOF("MeshCoreInterface: pre-bound link_id %.8s -> '%s'",
                  link_hex.c_str(), sender.c_str());
        }
    }
}

void MeshCoreInterface::on_incoming(const RNS::Bytes& data) {
    RNS::InterfaceImpl::handle_incoming(data);
}

bool MeshCoreInterface::rate_limit_ok(const uint8_t* data, size_t len) {
    if (len < 12) return true;
    uint8_t flags = data[0];
    uint8_t ptype = flags & 0x03;
    uint8_t dtype = (flags >> 2) & 0x03;
    uint32_t now = millis();
    // Per-destination key. NEVER data+2 blindly: a transport-relayed packet
    // has a two-byte header whose FIRST address is the relaying transport's
    // ID — identical for every announce this node forwards — so keying on it
    // collapses the "per destination" limiter into one global bucket: first
    // announce through, every other destination suppressed for the full
    // window. extract_rns_token() picks the real destination for either
    // header form. (Same bug family as the path-request keying fixed earlier
    // on 2026-08-23; both found on hardware, the hard way.)
    std::string dest;
    {
        uint8_t tok[MeshCoreTunnel::RNS_DST_LEN];
        if (MeshCoreTunnel::extract_rns_token(data, len, tok)) {
            dest = to_hex(tok, 10);
        } else {
            dest = to_hex(data + 2, 10);   // runt fallback, len>=12 guaranteed
        }
    }

    if (ptype == PTYPE_ANNOUNCE) {
        if (_cfg.announce_rate_ms > 0) {
            // Path-response bypass: consume any pending flag for this dest.
            bool answering = false;
            auto pit = _path_response_pending.find(dest);
            if (pit != _path_response_pending.end()) {
                answering = ((int32_t)(now - pit->second) < 0);
                _path_response_pending.erase(pit);
            }
            if (!answering) {
                auto it = _announce_sent.find(dest);
                if (it != _announce_sent.end() && (now - it->second) < _cfg.announce_rate_ms) {
                    _announce_suppressed++;
                    return false;
                }
            }
            _announce_sent[dest] = now;
        }
        return true;
    }

    if (_cfg.path_req_rate_ms > 0 && ptype == PTYPE_DATA && dtype == DTYPE_PLAIN) {
        // Key on the QUERIED destination from the payload, never the address
        // field: every path request is addressed to the same well-known
        // broadcast hash (rnstransport.path.request = 6b9f6601...), so keying
        // on `dest` collapses all path requests into ONE bucket — the first
        // forwarded request then silences discovery for every destination on
        // the network for path_req_rate_ms. Cost one evening of "nothing
        // routes in either direction" (2026-08-23).
        uint8_t target[MeshCoreTunnel::RNS_DST_LEN];
        if (MeshCoreTunnel::extract_path_request_target(data, len, target)) {
            dest = to_hex(target, 10);
        }
        auto it = _path_req_sent.find(dest);
        if (it == _path_req_sent.end()) {
            _path_req_sent[dest] = std::make_pair(now, now);
        } else {
            uint32_t first = it->second.first, last = it->second.second;
            if ((now - first) < _cfg.path_req_burst_window_ms) {
                it->second.second = now;
            } else if ((now - last) < _cfg.path_req_rate_ms) {
                return false;
            } else {
                _path_req_sent[dest] = std::make_pair(now, now);
            }
        }
    }
    return true;
}

void MeshCoreInterface::run_cleanup(uint32_t now) {
    for (auto it = _assembly.begin(); it != _assembly.end();) {
        if ((now - it->second.ts) > _cfg.fragment_timeout_ms) {
            WARNINGF("MeshCoreInterface: assembly timeout — had %u/%u frags",
                     (unsigned)it->second.frags.size(), (unsigned)it->second.total);
            it = _assembly.erase(it);
        } else ++it;
    }
    for (auto it = _seen.begin(); it != _seen.end();) {
        if ((int32_t)(now - it->second) >= 0) it = _seen.erase(it);
        else ++it;
    }
    if (_cfg.announce_rate_ms > 0) {
        for (auto it = _announce_sent.begin(); it != _announce_sent.end();) {
            if ((now - it->second) > (_cfg.announce_rate_ms * 2)) it = _announce_sent.erase(it);
            else ++it;
        }
    }
    if (_cfg.path_req_rate_ms > 0) {
        for (auto it = _path_req_sent.begin(); it != _path_req_sent.end();) {
            if ((now - it->second.second) > (_cfg.path_req_rate_ms * 2)) it = _path_req_sent.erase(it);
            else ++it;
        }
    }
    for (auto it = _path_response_pending.begin(); it != _path_response_pending.end();) {
        if ((int32_t)(now - it->second) >= 0) it = _path_response_pending.erase(it);
        else ++it;
    }

    // Expire silent peers and their bound routes.
    for (auto it = _peer_last_seen.begin(); it != _peer_last_seen.end();) {
        if ((now - it->second) > _cfg.peer_ttl_ms) {
            const std::string name = it->first;
            auto pt = _peer_table.find(name);
            if (pt != _peer_table.end()) {
                std::string mc_key = pt->second;
                _reverse_peers.erase(mc_key);
                for (size_t pfx_len : {8u, 12u, 16u, 24u}) {
                    if (mc_key.size() >= pfx_len) _reverse_peers.erase(mc_key.substr(0, pfx_len));
                }
                for (auto rt = _rns_to_mc_map.begin(); rt != _rns_to_mc_map.end();) {
                    if (rt->second == mc_key) rt = _rns_to_mc_map.erase(rt);
                    else ++rt;
                }
                _peer_table.erase(pt);
            }
            _peer_caps.erase(name);
            it = _peer_last_seen.erase(it);
            mark_state_dirty();
        } else {
            ++it;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Persistence — learned peers + rns->mc routes survive a power-cycle.
//
// Format (little-endian, versioned):
//   'M' 'C' 'S' <ver=1>
//   u32 saved_epoch          (MeshCore wall-clock at save, 0 if unknown)
//   u16 peer_count
//     { u8 name_len, name, u8 key_len, key_hex, u8 can_route } * peer_count
//   u16 route_count
//     { u8 tok_len, tok_hex, u8 key_len, key_hex } * route_count
//
// Hand-rolled rather than MsgPack: fully deterministic, host-verifiable, and
// avoids pulling a serializer into the RX hot path. All fields are < 256 bytes
// (device names, 64-char key hex, 32-char token hex), so u8 length prefixes are
// safe. Loaded peers get a fresh TTL window (millis resets across reboot); the
// saved epoch is retained only for diagnostics.
//
// Uses SPIFFS directly rather than RNS::Utilities::OS::write_file/read_file —
// that path needs RNS_USE_FS, which this build deliberately does not enable.
// ─────────────────────────────────────────────────────────────────────────

static const char* MC_STATE_PATH = "/mc_state";

void MeshCoreInterface::save_state() {
    RNS::Bytes buf;
    buf.append((uint8_t)'M'); buf.append((uint8_t)'C');
    buf.append((uint8_t)'S'); buf.append((uint8_t)1);

    uint32_t epoch = _link.nowEpoch();
    buf.append((uint8_t)(epoch & 0xFF));         buf.append((uint8_t)((epoch >> 8) & 0xFF));
    buf.append((uint8_t)((epoch >> 16) & 0xFF)); buf.append((uint8_t)((epoch >> 24) & 0xFF));

    uint16_t pc = 0;
    RNS::Bytes peers;
    for (const auto& kv : _peer_table) {
        const std::string& name = kv.first;
        const std::string& key  = kv.second;
        if (name.empty() || name.size() > 255 || key.size() > 255) continue;
        uint8_t caps = 0;
        auto cit = _peer_caps.find(name);
        if (cit != _peer_caps.end() && cit->second) caps = 1;
        peers.append((uint8_t)name.size()); peers.append(name);
        peers.append((uint8_t)key.size());  peers.append(key);
        peers.append(caps);
        pc++;
    }
    buf.append((uint8_t)(pc & 0xFF)); buf.append((uint8_t)(pc >> 8));
    buf.append(peers);

    uint16_t rc = 0;
    RNS::Bytes routes;
    for (const auto& kv : _rns_to_mc_map) {
        const std::string& tok = kv.first;
        const std::string& key = kv.second;
        if (tok.empty() || tok.size() > 255 || key.size() > 255) continue;
        routes.append((uint8_t)tok.size()); routes.append(tok);
        routes.append((uint8_t)key.size()); routes.append(key);
        rc++;
    }
    buf.append((uint8_t)(rc & 0xFF)); buf.append((uint8_t)(rc >> 8));
    buf.append(routes);

    size_t wrote = 0;
    File f = SPIFFS.open(MC_STATE_PATH, "w");
    if (f) {
        wrote = f.write(buf.data(), buf.size());
        f.close();
    } else {
        WARNING("MeshCoreInterface: failed to open state file for write");
    }
    INFOF("MeshCoreInterface: saved state — %u peers, %u routes, %u bytes",
          (unsigned)pc, (unsigned)rc, (unsigned)wrote);
}

void MeshCoreInterface::load_state() {
    if (!SPIFFS.exists(MC_STATE_PATH)) return;
    File f = SPIFFS.open(MC_STATE_PATH, "r");
    if (!f) return;

    RNS::Bytes data;
    size_t fsize = f.size();
    if (fsize > 0) {
        std::vector<uint8_t> tmp(fsize);
        size_t got = f.read(tmp.data(), fsize);
        data = RNS::Bytes(tmp.data(), got);
    }
    f.close();

    const uint8_t* d = data.data();
    size_t len = data.size();
    if (d == nullptr || len < 10) return;
    if (!(d[0] == 'M' && d[1] == 'C' && d[2] == 'S' && d[3] == 1)) {
        WARNING("MeshCoreInterface: state file bad magic/version — ignoring");
        return;
    }
    size_t o = 4 + 4;   // magic+ver + saved_epoch (unused for TTL)
    auto need = [&](size_t k) { return o + k <= len; };

    if (!need(2)) return;
    uint16_t pc = (uint16_t)(d[o] | (d[o + 1] << 8));
    o += 2;
    uint32_t now = millis();
    for (uint16_t i = 0; i < pc; ++i) {
        if (!need(1)) return;
        uint8_t nl = d[o++];
        if (!need(nl)) return;
        std::string name((const char*)(d + o), nl);
        o += nl;
        if (!need(1)) return;
        uint8_t kl = d[o++];
        if (!need(kl)) return;
        std::string key((const char*)(d + o), kl);
        o += kl;
        if (!need(1)) return;
        uint8_t caps = d[o++];
        if (name.empty() || key.empty()) continue;
        _peer_table[name]     = key;
        _peer_caps[name]      = (caps != 0);
        _peer_last_seen[name] = now;   // fresh window; millis reset at reboot
        _reverse_peers[key]   = name;
        for (size_t pfx : {8u, 12u, 16u, 24u})
            if (key.size() >= pfx) _reverse_peers[key.substr(0, pfx)] = name;
        // Contacts are RAM-only and start empty every boot, so a restored peer
        // has a route but no encryption target until its next advert. Recreate
        // the contact now or the first sends all drop to channel fallback.
        ensure_peer_contact(key, name);
    }

    if (!need(2)) return;
    uint16_t rc = (uint16_t)(d[o] | (d[o + 1] << 8));
    o += 2;
    for (uint16_t i = 0; i < rc; ++i) {
        if (!need(1)) return;
        uint8_t tl = d[o++];
        if (!need(tl)) return;
        std::string tok((const char*)(d + o), tl);
        o += tl;
        if (!need(1)) return;
        uint8_t kl = d[o++];
        if (!need(kl)) return;
        std::string key((const char*)(d + o), kl);
        o += kl;
        if (tok.empty() || key.empty()) continue;
        if (_rns_to_mc_map.size() < _RNS_MAP_MAX) _rns_to_mc_map[tok] = key;
    }

    INFOF("MeshCoreInterface: restored %u peers, %u routes from flash",
          (unsigned)_peer_table.size(), (unsigned)_rns_to_mc_map.size());
}
