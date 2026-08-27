#include "MeshCoreTunnelCodec.h"

namespace MeshCoreTunnel {

namespace {

const char* B64URL =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

} // namespace

std::string b64url_encode(const uint8_t* in, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t n = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out += B64URL[(n >> 18) & 63];
        out += B64URL[(n >> 12) & 63];
        out += B64URL[(n >> 6) & 63];
        out += B64URL[n & 63];
        i += 3;
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t n = (uint32_t)in[i] << 16;
        out += B64URL[(n >> 18) & 63];
        out += B64URL[(n >> 12) & 63];
    } else if (rem == 2) {
        uint32_t n = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out += B64URL[(n >> 18) & 63];
        out += B64URL[(n >> 12) & 63];
        out += B64URL[(n >> 6) & 63];
    }
    return out;
}

bool b64url_decode(const std::string& in, std::vector<uint8_t>& out) {
    out.clear();
    uint32_t buf = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=') break;
        int v = b64val(c);
        if (v < 0) return false;
        buf = (buf << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)((buf >> bits) & 0xFF));
        }
    }
    return true;
}

bool pack_fragment(uint8_t frag_idx, uint32_t pkt_id, uint8_t frag_total,
                   const uint8_t* payload, size_t payload_len,
                   std::vector<uint8_t>& out) {
    if (frag_total == 0 || frag_idx >= frag_total) return false;
    if (payload_len > 0 && payload == nullptr) return false;
    out.clear();
    out.reserve(HEADER_SIZE + payload_len);
    out.push_back(frag_idx);
    out.push_back((uint8_t)((pkt_id >> 24) & 0xFF));
    out.push_back((uint8_t)((pkt_id >> 16) & 0xFF));
    out.push_back((uint8_t)((pkt_id >> 8) & 0xFF));
    out.push_back((uint8_t)(pkt_id & 0xFF));
    out.push_back(frag_total);
    if (payload_len) out.insert(out.end(), payload, payload + payload_len);
    return true;
}

bool parse_fragment(const uint8_t* raw, size_t len, FragHeader& hdr,
                    const uint8_t*& payload, size_t& payload_len) {
    if (raw == nullptr || len < HEADER_SIZE) return false;
    hdr.frag_idx   = raw[0];
    hdr.pkt_id     = ((uint32_t)raw[1] << 24) | ((uint32_t)raw[2] << 16) |
                     ((uint32_t)raw[3] << 8)  |  (uint32_t)raw[4];
    hdr.frag_total = raw[5];
    if (hdr.frag_total == 0 || hdr.frag_idx >= hdr.frag_total) return false;
    payload      = raw + HEADER_SIZE;
    payload_len  = len - HEADER_SIZE;
    return true;
}

std::string encode_channel_fragment(uint8_t frag_idx, uint32_t pkt_id,
                                    uint8_t frag_total,
                                    const uint8_t* payload, size_t payload_len) {
    std::vector<uint8_t> packed;
    if (!pack_fragment(frag_idx, pkt_id, frag_total, payload, payload_len, packed))
        return {};
    return "RNS:" + b64url_encode(packed.data(), packed.size());
}

bool decode_channel_fragment(const std::string& rns_text, FragHeader& hdr,
                             std::vector<uint8_t>& payload) {
    if (rns_text.size() < 4 || rns_text.compare(0, 4, "RNS:") != 0) return false;
    std::string b64 = rns_text.substr(4);
    while (!b64.empty() &&
           (b64.back() == ' ' || b64.back() == '\r' || b64.back() == '\n'))
        b64.pop_back();

    std::vector<uint8_t> raw;
    if (!b64url_decode(b64, raw)) return false;

    const uint8_t* p = nullptr;
    size_t plen = 0;
    if (!parse_fragment(raw.data(), raw.size(), hdr, p, plen)) return false;
    payload.assign(p, p + plen);
    return true;
}

uint8_t fragment_count(size_t len, uint16_t payload_size) {
    if (payload_size == 0) return 0;
    if (len == 0) return 1;
    size_t n = (len + payload_size - 1) / payload_size;
    if (n > 255) return 0;
    return (uint8_t)n;
}

// ─────────────────────────────────────────────────────────────────────────
// Phase 2: peer discovery + direct-route helpers
// ─────────────────────────────────────────────────────────────────────────

const char* const MSG_PREFIX      = "RNS:";
const char* const BIND_PREFIX     = "RNSBIND:";
const char* const BIND_REQ_PREFIX = "RNSBIND_REQ:";

namespace {

std::string rstrip_colon_space(const std::string& s) {
    size_t end = s.size();
    while (end > 0 && (s[end - 1] == ':' || s[end - 1] == ' ')) --end;
    return s.substr(0, end);
}

std::string strip_ws(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

} // namespace

std::string encode_bind(const std::string& mc_pubkey_hex, bool can_route, bool is_req) {
    std::string out = is_req ? BIND_REQ_PREFIX : BIND_PREFIX;
    out += mc_pubkey_hex;
    out += ':';
    out += (can_route ? CAP_ROUTER : CAP_EDGE);
    return out;
}

bool parse_bind(const std::string& text, BindMsg& out) {
    size_t bind_idx = text.find(BIND_PREFIX);
    size_t req_idx  = text.find(BIND_REQ_PREFIX);
    bool has_bind = (bind_idx != std::string::npos);
    bool has_req  = (req_idx  != std::string::npos);
    if (!has_bind && !has_req) return false;

    // REQ wins if present and at/before any plain BIND (matches reference).
    bool is_req = has_req && (!has_bind || req_idx <= bind_idx);
    const char* prefix = is_req ? BIND_REQ_PREFIX : BIND_PREFIX;
    size_t pfx_idx = is_req ? req_idx : bind_idx;
    size_t prefix_len = std::string(prefix).size();

    std::string sender_name = (pfx_idx > 0)
        ? rstrip_colon_space(text.substr(0, pfx_idx)) : "";
    std::string raw_value = strip_ws(text.substr(pfx_idx + prefix_len));
    if (raw_value.empty()) return false;

    std::string mc_pubkey;
    bool can_route;
    size_t last_colon = raw_value.rfind(':');
    if (last_colon != std::string::npos) {
        mc_pubkey = raw_value.substr(0, last_colon);
        std::string cap = strip_ws(raw_value.substr(last_colon + 1));
        // upper-case single char compare against 'E'
        char c = cap.empty() ? 0 : cap[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        can_route = (c != CAP_EDGE);
    } else {
        mc_pubkey = raw_value;   // legacy form, no capability field
        can_route = true;
    }
    mc_pubkey = strip_ws(mc_pubkey);
    if (mc_pubkey.empty()) return false;

    out.is_req      = is_req;
    out.sender_name = sender_name;
    out.mc_pubkey   = mc_pubkey;
    out.can_route   = can_route;
    return true;
}

bool extract_rns_token(const uint8_t* data, size_t len, uint8_t out[RNS_DST_LEN]) {
    if (data == nullptr || len < 2) return false;
    uint8_t header_type = (uint8_t)((data[0] & 0x40) >> 6);
    if (header_type == 1) {
        size_t end = 2 + 2 * RNS_DST_LEN;
        if (len < end) return false;
        for (size_t i = 0; i < RNS_DST_LEN; ++i) out[i] = data[2 + RNS_DST_LEN + i];
    } else {
        size_t end = 2 + RNS_DST_LEN;
        if (len < end) return false;
        for (size_t i = 0; i < RNS_DST_LEN; ++i) out[i] = data[2 + i];
    }
    return true;
}

bool packet_hash_preimage(const uint8_t* data, size_t len, std::vector<uint8_t>& out) {
    out.clear();
    if (data == nullptr || len < 2) return false;
    uint8_t header_type = (uint8_t)((data[0] & 0x40) >> 6);
    size_t start = (header_type == 1) ? (2 + RNS_DST_LEN) : 2;
    if (len < start) return false;
    out.push_back((uint8_t)(data[0] & 0x0F));
    out.insert(out.end(), data + start, data + len);
    return true;
}

bool link_id_preimage(const uint8_t* data, size_t len, std::vector<uint8_t>& out) {
    out.clear();
    if (data == nullptr || len < 2) return false;
    if ((data[0] & 0x03) != RNS_PTYPE_LINK_REQ) return false;
    return packet_hash_preimage(data, len, out);
}

bool extract_path_request_target(const uint8_t* data, size_t len,
                                 uint8_t out[RNS_DST_LEN]) {
    if (data == nullptr || len < 2) return false;
    if ((data[0] & 0x03) != RNS_PTYPE_DATA) return false;
    if (((data[0] >> 2) & 0x03) != RNS_DTYPE_PLAIN) return false;
    uint8_t header_type = (uint8_t)((data[0] & 0x40) >> 6);
    // [flags][hops][addr:16]([addr2:16])[context:1][payload: dest16 + ...]
    size_t off = 2 + (header_type == 1 ? 2 * RNS_DST_LEN : RNS_DST_LEN) + 1;
    if (len < off + RNS_DST_LEN) return false;
    for (size_t i = 0; i < RNS_DST_LEN; ++i) out[i] = data[off + i];
    return true;
}

bool is_broadcast_packet(const uint8_t* data, size_t len) {
    if (data == nullptr || len < 1) return true;
    uint8_t ptype = data[0] & 0x03;
    uint8_t dtype = (data[0] >> 2) & 0x03;
    if (ptype == RNS_PTYPE_ANNOUNCE) return true;
    if (ptype == RNS_PTYPE_DATA && dtype == RNS_DTYPE_PLAIN) return true;
    return false;
}

} // namespace MeshCoreTunnel
