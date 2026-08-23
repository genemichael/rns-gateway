/*
 * Host tests for PropPolicy — the outbound gate of the prop-node-restricted
 * firmware variants.  Run:  ./scripts/run_host_tests.sh
 *
 * Packet layout (RNS single header): [flags][hops][dest:16][payload...]
 * Two-byte header (flags bit 6):     [flags][hops][addr1:16][addr2:16][payload...]
 * flags: bits 1-0 ptype, bits 3-2 dtype, bit 6 header type.
 */
#include "PropPolicy.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace MeshCoreTunnel;

static int g_fail = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);\
            ++g_fail;                                                          \
        }                                                                      \
    } while (0)

static const uint8_t PROP_DEST[RNS_DST_LEN] = {
    0xaa, 0xbb, 0xcc, 0xdd, 0x00, 0x11, 0x22, 0x33,
    0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xee, 0xff};
static const uint8_t OTHER_DEST[RNS_DST_LEN] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10};

static std::vector<uint8_t> pkt(uint8_t ptype, uint8_t dtype,
                                const uint8_t dest[RNS_DST_LEN],
                                bool two_addr = false,
                                const uint8_t* addr1 = nullptr) {
    std::vector<uint8_t> p;
    uint8_t flags = (uint8_t)((ptype & 0x03) | ((dtype & 0x03) << 2));
    if (two_addr) flags |= 0x40;
    p.push_back(flags);
    p.push_back(0x00);   // hops
    if (two_addr) {
        // First address is the transport id; the destination is the second.
        const uint8_t* a1 = addr1 ? addr1 : OTHER_DEST;
        p.insert(p.end(), a1, a1 + RNS_DST_LEN);
    }
    p.insert(p.end(), dest, dest + RNS_DST_LEN);
    for (int i = 0; i < 8; ++i) p.push_back((uint8_t)(0xC0 + i));  // payload
    return p;
}

static void test_broadcast_always_allowed() {
    PropPolicy pol;   // empty whitelist — strictest possible state
    auto ann = pkt(RNS_PTYPE_ANNOUNCE, RNS_DTYPE_SINGLE, OTHER_DEST);
    CHECK(pol.check_outgoing(ann.data(), ann.size()).verdict == PropPolicy::ALLOW,
          "announce allowed with empty whitelist");
    auto preq = pkt(RNS_PTYPE_DATA, RNS_DTYPE_PLAIN, OTHER_DEST);
    CHECK(pol.check_outgoing(preq.data(), preq.size()).verdict == PropPolicy::ALLOW,
          "path request allowed with empty whitelist");
}

static void test_data_single_whitelist() {
    PropPolicy pol;
    pol.add_dest(PROP_DEST);
    auto to_prop = pkt(RNS_PTYPE_DATA, RNS_DTYPE_SINGLE, PROP_DEST);
    CHECK(pol.check_outgoing(to_prop.data(), to_prop.size()).verdict == PropPolicy::ALLOW,
          "data to prop dest allowed");
    auto to_other = pkt(RNS_PTYPE_DATA, RNS_DTYPE_SINGLE, OTHER_DEST);
    CHECK(pol.check_outgoing(to_other.data(), to_other.size()).verdict == PropPolicy::DROP,
          "data to non-prop dest dropped");
    auto group = pkt(RNS_PTYPE_DATA, RNS_DTYPE_GROUP, PROP_DEST);
    CHECK(pol.check_outgoing(group.data(), group.size()).verdict == PropPolicy::DROP,
          "group data dropped even to whitelisted hash");
}

static void test_two_byte_header_dest_is_second_address() {
    PropPolicy pol;
    pol.add_dest(PROP_DEST);
    auto p = pkt(RNS_PTYPE_DATA, RNS_DTYPE_SINGLE, PROP_DEST, /*two_addr=*/true);
    CHECK(pol.check_outgoing(p.data(), p.size()).verdict == PropPolicy::ALLOW,
          "two-byte header: dest read from second address");
    auto q = pkt(RNS_PTYPE_DATA, RNS_DTYPE_SINGLE, OTHER_DEST, /*two_addr=*/true,
                 PROP_DEST);
    CHECK(pol.check_outgoing(q.data(), q.size()).verdict == PropPolicy::DROP,
          "two-byte header: first address (transport id) must not match");
}

static void test_add_dest_hex() {
    PropPolicy pol;
    CHECK(pol.add_dest_hex("AABBCCDD00112233445566778899EEFF"),
          "uppercase hex accepted");
    auto p = pkt(RNS_PTYPE_DATA, RNS_DTYPE_SINGLE, PROP_DEST);
    CHECK(pol.check_outgoing(p.data(), p.size()).verdict == PropPolicy::ALLOW,
          "uppercase hex normalised to match binary dest");
    CHECK(!pol.add_dest_hex("aabb"), "short hex rejected");
    CHECK(!pol.add_dest_hex("zzbbccdd00112233445566778899eeff"), "non-hex rejected");
    CHECK(pol.dest_count() == 1, "rejected inputs not stored");
}

static void test_link_lifecycle() {
    PropPolicy pol;
    pol.add_dest(PROP_DEST);

    // LINK_REQ to a non-prop destination: dropped, no preimage.
    auto lr_bad = pkt(RNS_PTYPE_LINK_REQ, RNS_DTYPE_SINGLE, OTHER_DEST);
    auto d_bad = pol.check_outgoing(lr_bad.data(), lr_bad.size());
    CHECK(d_bad.verdict == PropPolicy::DROP, "link-req to non-prop dropped");
    CHECK(d_bad.link_preimage.empty(), "dropped link-req yields no preimage");

    // LINK_REQ to the prop destination: allowed, preimage handed back.
    auto lr_ok = pkt(RNS_PTYPE_LINK_REQ, RNS_DTYPE_SINGLE, PROP_DEST);
    auto d_ok = pol.check_outgoing(lr_ok.data(), lr_ok.size());
    CHECK(d_ok.verdict == PropPolicy::ALLOW, "link-req to prop allowed");
    CHECK(!d_ok.link_preimage.empty(), "allowed link-req yields preimage");

    // Caller hashes the preimage; simulate with a fixed 32-byte digest.
    uint8_t digest[32];
    for (int i = 0; i < 32; ++i) digest[i] = (uint8_t)(0x80 + i);
    pol.add_link(digest, sizeof(digest));
    CHECK(pol.link_count() == 1, "link registered");

    // Link traffic addresses the link id (first 16 bytes of the digest).
    auto data_link = pkt(RNS_PTYPE_DATA, RNS_DTYPE_LINK, digest);
    CHECK(pol.check_outgoing(data_link.data(), data_link.size()).verdict == PropPolicy::ALLOW,
          "data on registered link allowed");
    auto proof = pkt(RNS_PTYPE_PROOF, RNS_DTYPE_LINK, digest);
    CHECK(pol.check_outgoing(proof.data(), proof.size()).verdict == PropPolicy::ALLOW,
          "proof for registered link allowed");
    auto data_unk = pkt(RNS_PTYPE_DATA, RNS_DTYPE_LINK, OTHER_DEST);
    CHECK(pol.check_outgoing(data_unk.data(), data_unk.size()).verdict == PropPolicy::DROP,
          "data on unregistered link dropped");
    auto proof_unk = pkt(RNS_PTYPE_PROOF, RNS_DTYPE_LINK, OTHER_DEST);
    CHECK(pol.check_outgoing(proof_unk.data(), proof_unk.size()).verdict == PropPolicy::DROP,
          "proof for unregistered link dropped");
}

static void test_link_eviction_keeps_newest() {
    PropPolicy pol;
    uint8_t digest[32] = {0};
    for (int i = 0; i < 40; ++i) {
        digest[0] = (uint8_t)i;
        pol.add_link(digest, sizeof(digest));
    }
    CHECK(pol.link_count() == 32, "link table capped");
    digest[0] = 39;
    auto newest = pkt(RNS_PTYPE_DATA, RNS_DTYPE_LINK, digest);
    CHECK(pol.check_outgoing(newest.data(), newest.size()).verdict == PropPolicy::ALLOW,
          "newest link survives eviction");
    digest[0] = 0;
    auto oldest = pkt(RNS_PTYPE_DATA, RNS_DTYPE_LINK, digest);
    CHECK(pol.check_outgoing(oldest.data(), oldest.size()).verdict == PropPolicy::DROP,
          "oldest link evicted");
}

static void test_malformed() {
    PropPolicy pol;
    pol.add_dest(PROP_DEST);
    CHECK(pol.check_outgoing(nullptr, 0).verdict == PropPolicy::DROP, "null dropped");
    uint8_t runt[2] = {0x00, 0x00};
    CHECK(pol.check_outgoing(runt, sizeof(runt)).verdict == PropPolicy::DROP,
          "packet too short for a dest token dropped");
}

int main() {
    test_broadcast_always_allowed();
    test_data_single_whitelist();
    test_two_byte_header_dest_is_second_address();
    test_add_dest_hex();
    test_link_lifecycle();
    test_link_eviction_keeps_newest();
    test_malformed();

    if (g_fail == 0) {
        std::printf("test_prop_policy: all tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "test_prop_policy: %d FAILURE(S)\n", g_fail);
    return 1;
}
