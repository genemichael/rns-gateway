/*
 * PropPolicy.h — outbound traffic policy for the prop-node-restricted variants.
 *
 * The prop-centric architecture puts a propagation node (RFed) at the home
 * site; the tunnel carries last-mile delivery and wake packets only. Columba's
 * delivery mode is a global *client* setting, so a client configured for
 * direct delivery would happily open client-to-client links over the 300 bps
 * channel. Enforcement therefore lives in the gateway: outbound mesh traffic
 * is dropped unless it serves a whitelisted destination (the prop node) or a
 * link that was established with one.
 *
 * Pure wire-format logic, no Arduino / RNS deps — host-testable alongside the
 * codec. The caller owns hashing: a LINK_REQUEST to a whitelisted destination
 * returns its link_id preimage, the caller SHA-256s it and registers the first
 * RNS_DST_LEN bytes via add_link() so the link's later PROOF / DATA packets
 * pass. Link ids from *inbound* LINK_REQUESTs (prop node dialing a client
 * behind us) must be registered the same way, or the client's reply proof is
 * dropped on its way out.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PROP_POLICY_H
#define PROP_POLICY_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "MeshCoreTunnelCodec.h"

namespace MeshCoreTunnel {

class PropPolicy {
public:
    enum Verdict { ALLOW, DROP };

    struct Decision {
        Verdict     verdict;
        const char* reason;   // static string for logging, never NULL
        // Non-empty only for an ALLOWed LINK_REQUEST: SHA-256 this, then
        // add_link(digest) so the link's subsequent traffic passes.
        std::vector<uint8_t> link_preimage;
    };

    // Whitelist a destination hash (RNS_DST_LEN bytes / 2*RNS_DST_LEN hex).
    void add_dest(const uint8_t dest[RNS_DST_LEN]);
    bool add_dest_hex(const std::string& hex);   // false if malformed

    // Register a link id established with a whitelisted destination. Accepts a
    // full SHA-256 digest or longer; only the first RNS_DST_LEN bytes matter.
    void add_link(const uint8_t* digest, size_t len);

    size_t dest_count() const { return _dests.size(); }
    size_t link_count() const { return _links.size(); }

    Decision check_outgoing(const uint8_t* data, size_t len);

private:
    bool is_dest(const std::string& tok_hex) const { return _dests.count(tok_hex) != 0; }
    bool is_link(const std::string& tok_hex) const { return _links.count(tok_hex) != 0; }

    std::set<std::string>           _dests;   // whitelisted dest hashes, hex
    std::map<std::string, uint32_t> _links;   // link id hex -> insertion seq
    uint32_t                        _link_seq = 0;

    // Enough for every plausible concurrent link through a 300 bps tunnel;
    // oldest is evicted, and a long-lived link that outlives eviction re-forms
    // (RNS re-establishes on link failure).
    static const size_t MAX_LINKS = 32;
};

} // namespace MeshCoreTunnel

#endif // PROP_POLICY_H
