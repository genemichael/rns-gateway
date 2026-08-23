/*
 * Pure wire-format helpers for the RNS-over-MeshCore tunnel.
 * Identical framing to comms-engineer/RNS_Over_Meshcore
 * (MeshCore_Dynamic_Interface.py). No Arduino / MeshCore deps —
 * host-testable on desktop.
 *
 *   "RNS:" + base64url( [frag_idx:1][pkt_id:4 BE][frag_total:1] + payload )
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MESHCORE_TUNNEL_CODEC_H
#define MESHCORE_TUNNEL_CODEC_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace MeshCoreTunnel {

static constexpr size_t HEADER_SIZE = 6; // idx + pkt_id + total

struct FragHeader {
    uint8_t  frag_idx   = 0;
    uint32_t pkt_id     = 0;
    uint8_t  frag_total = 0;
};

std::string b64url_encode(const uint8_t* in, size_t len);
bool        b64url_decode(const std::string& in, std::vector<uint8_t>& out);

// Pack one fragment body (header + payload chunk). Returns false if args invalid.
bool pack_fragment(uint8_t frag_idx, uint32_t pkt_id, uint8_t frag_total,
                   const uint8_t* payload, size_t payload_len,
                   std::vector<uint8_t>& out);

// Parse header from a decoded fragment body. Returns false if too short / bad.
bool parse_fragment(const uint8_t* raw, size_t len, FragHeader& hdr,
                    const uint8_t*& payload, size_t& payload_len);

// Full channel string for one fragment: "RNS:" + base64url(packed).
std::string encode_channel_fragment(uint8_t frag_idx, uint32_t pkt_id,
                                    uint8_t frag_total,
                                    const uint8_t* payload, size_t payload_len);

// Decode a string that starts with "RNS:" (or contains it at offset 0 after
// the caller stripped the MeshCore "Name: " prefix down to "RNS:...").
// Returns false on malformed input.
bool decode_channel_fragment(const std::string& rns_text, FragHeader& hdr,
                             std::vector<uint8_t>& payload);

// How many fragments for a packet of `len` with the given payload_size.
// Returns 0 if len==0 or if the fragment count would exceed 255.
uint8_t fragment_count(size_t len, uint16_t payload_size);

// ─────────────────────────────────────────────────────────────────────────
// Phase 2: peer discovery + direct-route helpers.
// Wire-compatible with MeshCore_Dynamic_Interface.py.
// ─────────────────────────────────────────────────────────────────────────

extern const char* const MSG_PREFIX;      // "RNS:"
extern const char* const BIND_PREFIX;     // "RNSBIND:"
extern const char* const BIND_REQ_PREFIX; // "RNSBIND_REQ:"

static constexpr char   CAP_ROUTER  = 'R';
static constexpr char   CAP_EDGE    = 'E';
static constexpr size_t RNS_DST_LEN = 16;

// RNS packet-type (byte0 & 0x03) and dest-type ((byte0 >> 2) & 0x03).
static constexpr uint8_t RNS_PTYPE_DATA     = 0x00;
static constexpr uint8_t RNS_PTYPE_ANNOUNCE = 0x01;
static constexpr uint8_t RNS_PTYPE_LINK_REQ = 0x02;
static constexpr uint8_t RNS_PTYPE_PROOF    = 0x03;
static constexpr uint8_t RNS_DTYPE_SINGLE   = 0x00;
static constexpr uint8_t RNS_DTYPE_GROUP    = 0x01;
static constexpr uint8_t RNS_DTYPE_PLAIN    = 0x02;
static constexpr uint8_t RNS_DTYPE_LINK     = 0x03;

// Human-readable names for console logs (byte0 & 0x03 / (byte0 >> 2) & 0x03).
inline const char* ptype_name(uint8_t ptype) {
    switch (ptype & 0x03) {
        case RNS_PTYPE_DATA:     return "DATA";
        case RNS_PTYPE_ANNOUNCE: return "ANNOUNCE";
        case RNS_PTYPE_LINK_REQ: return "LINK_REQ";
        case RNS_PTYPE_PROOF:    return "PROOF";
        default:                 return "?";
    }
}
inline const char* dtype_name(uint8_t dtype) {
    switch (dtype & 0x03) {
        case RNS_DTYPE_SINGLE: return "SINGLE";
        case RNS_DTYPE_GROUP:  return "GROUP";
        case RNS_DTYPE_PLAIN:  return "PLAIN";
        case RNS_DTYPE_LINK:   return "LINK";
        default:               return "?";
    }
}
// Convenience: DATA+PLAIN is a path request in Transport terms.
inline const char* packet_kind(uint8_t flags) {
    uint8_t ptype = flags & 0x03;
    uint8_t dtype = (flags >> 2) & 0x03;
    if (ptype == RNS_PTYPE_ANNOUNCE) return "ANNOUNCE";
    if (ptype == RNS_PTYPE_DATA && dtype == RNS_DTYPE_PLAIN) return "PATH_REQ";
    if (ptype == RNS_PTYPE_LINK_REQ) return "LINK_REQ";
    if (ptype == RNS_PTYPE_PROOF) return "PROOF";
    if (ptype == RNS_PTYPE_DATA) return "DATA";
    return "UNKNOWN";
}

struct BindMsg {
    bool        is_req    = false;
    std::string sender_name;     // MeshCore node name (radio-prepended prefix)
    std::string mc_pubkey;       // hex, exactly as carried on the wire
    bool        can_route = true;
};

// Build a discovery message body (no sender-name prefix; radio prepends that):
//   "RNSBIND:<pubkey>:<R|E>"  or  "RNSBIND_REQ:<pubkey>:<R|E>"
std::string encode_bind(const std::string& mc_pubkey_hex, bool can_route, bool is_req);

// Parse a received channel text as a BIND / BIND_REQ. Returns false if the text
// is not a discovery message. Mirrors _on_channel_msg precedence + _handle_bind.
bool parse_bind(const std::string& text, BindMsg& out);

// Extract the 16-byte RNS destination token for direct-route mapping. For a
// two-byte-header packet (byte0 bit6 set) the destination is the *second*
// address. Returns false if the packet is too short. Mirrors _extract_rns_token.
bool extract_rns_token(const uint8_t* data, size_t len, uint8_t out[RNS_DST_LEN]);

// Build the SHA-256 preimage for a LINK_REQUEST link_id:
//   [ data[0] & 0x0F ] + (bytes after the destination address(es))
// Caller hashes it (SHA-256) and takes the first 16 bytes. Returns false if the
// packet is not a LINK_REQUEST or is too short. Mirrors _link_id_from_lr_packet.
bool link_id_preimage(const uint8_t* data, size_t len, std::vector<uint8_t>& out);

// True if the packet MUST go on the shared channel (ANNOUNCE, or DATA+PLAIN
// path request) rather than a unicast direct message. Mirrors _is_broadcast_packet.
bool is_broadcast_packet(const uint8_t* data, size_t len);

// For a path request (DATA+PLAIN), extract the QUERIED destination hash — the
// first RNS_DST_LEN bytes of the payload, after the address field(s) and the
// context byte. The ADDRESSED destination of every path request is the same
// well-known broadcast hash (rnstransport.path.request), so per-destination
// logic keyed on the address field degenerates to one global bucket; the
// destination being sought only exists in the payload. Returns false if the
// packet is not a DATA+PLAIN path request or is too short.
bool extract_path_request_target(const uint8_t* data, size_t len,
                                 uint8_t out[RNS_DST_LEN]);

} // namespace MeshCoreTunnel

#endif
