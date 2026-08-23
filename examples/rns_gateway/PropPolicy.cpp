/*
 * PropPolicy.cpp — see PropPolicy.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "PropPolicy.h"

namespace MeshCoreTunnel {

namespace {

std::string hex_of(const uint8_t* p, size_t n) {
    static const char* h = "0123456789abcdef";
    std::string s; s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { s += h[p[i] >> 4]; s += h[p[i] & 0xF]; }
    return s;
}

int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

void PropPolicy::add_dest(const uint8_t dest[RNS_DST_LEN]) {
    _dests.insert(hex_of(dest, RNS_DST_LEN));
}

bool PropPolicy::add_dest_hex(const std::string& hex) {
    if (hex.size() != RNS_DST_LEN * 2) return false;
    std::string lower;
    lower.reserve(hex.size());
    for (char c : hex) {
        if (nibble(c) < 0) return false;
        lower += (char)((c >= 'A' && c <= 'F') ? c - 'A' + 'a' : c);
    }
    _dests.insert(lower);
    return true;
}

void PropPolicy::add_link(const uint8_t* digest, size_t len) {
    if (digest == nullptr || len < RNS_DST_LEN) return;
    std::string id = hex_of(digest, RNS_DST_LEN);
    if (_links.count(id)) return;
    if (_links.size() >= MAX_LINKS) {
        auto oldest = _links.begin();
        for (auto it = _links.begin(); it != _links.end(); ++it)
            if (it->second < oldest->second) oldest = it;
        _links.erase(oldest);
    }
    _links[id] = _link_seq++;
}

PropPolicy::Decision PropPolicy::check_outgoing(const uint8_t* data, size_t len) {
    if (data == nullptr || len < 2) return {DROP, "runt", {}};

    uint8_t ptype = data[0] & 0x03;
    uint8_t dtype = (data[0] >> 2) & 0x03;

    // Announces are broadcast, already rate-limited upstream, and load-bearing
    // both ways: the prop node learns client identities from them.
    if (ptype == RNS_PTYPE_ANNOUNCE) return {ALLOW, "announce", {}};

    // Path requests are broadcast discovery, throttled by path_req_rate_ms.
    // The queried destination rides in the payload, not the address field, so
    // filtering them needs payload parsing — deliberately out of scope here.
    if (ptype == RNS_PTYPE_DATA && dtype == RNS_DTYPE_PLAIN)
        return {ALLOW, "path-request", {}};

    uint8_t tok[RNS_DST_LEN];
    if (!extract_rns_token(data, len, tok)) return {DROP, "no-token", {}};
    std::string tok_hex = hex_of(tok, RNS_DST_LEN);

    if (ptype == RNS_PTYPE_LINK_REQ) {
        if (!is_dest(tok_hex)) return {DROP, "link-req to non-prop dest", {}};
        Decision d{ALLOW, "link-req to prop", {}};
        // Preimage failure just means the link's later packets won't match a
        // registered id; the request itself is still legitimate.
        link_id_preimage(data, len, d.link_preimage);
        return d;
    }

    if (ptype == RNS_PTYPE_PROOF) {
        if (is_link(tok_hex) || is_dest(tok_hex)) return {ALLOW, "proof", {}};
        return {DROP, "proof for non-prop link", {}};
    }

    // DATA
    if (dtype == RNS_DTYPE_LINK)
        return is_link(tok_hex) ? Decision{ALLOW, "link data", {}}
                                : Decision{DROP, "data on non-prop link", {}};
    if (dtype == RNS_DTYPE_SINGLE)
        return is_dest(tok_hex) ? Decision{ALLOW, "data to prop", {}}
                                : Decision{DROP, "data to non-prop dest", {}};

    // GROUP destinations have no place in the prop-only tunnel.
    return {DROP, "group data", {}};
}

} // namespace MeshCoreTunnel
