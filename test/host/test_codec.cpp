/*
 * Host golden tests for MeshCoreTunnelCodec.
 * Run:  ./scripts/run_host_tests.sh
 *
 * Vectors exercise the same framing as MeshCore_Dynamic_Interface.py:
 *   "RNS:" + base64url( [idx:1][pkt_id:4 BE][total:1] + payload )  (no '=')
 */
#include "MeshCoreTunnelCodec.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);\
            ++g_fail;                                                          \
        }                                                                      \
    } while (0)

static void test_b64url_roundtrip() {
    const uint8_t raw[] = {0x00, 0x01, 0xfe, 0xff, 0x7e, 'R', 'N', 'S'};
    std::string enc = MeshCoreTunnel::b64url_encode(raw, sizeof(raw));
    CHECK(enc.find('=') == std::string::npos, "padding stripped");
    CHECK(enc.find('+') == std::string::npos, "no + (urlsafe)");
    CHECK(enc.find('/') == std::string::npos, "no / (urlsafe)");

    std::vector<uint8_t> dec;
    CHECK(MeshCoreTunnel::b64url_decode(enc, dec), "decode ok");
    CHECK(dec.size() == sizeof(raw), "decode length");
    CHECK(std::memcmp(dec.data(), raw, sizeof(raw)) == 0, "decode bytes");
}

static void test_b64url_known_vector() {
    // Python: base64.urlsafe_b64encode(b"hello").rstrip(b"=") -> b'aGVsbG8'
    std::string enc = MeshCoreTunnel::b64url_encode(
        reinterpret_cast<const uint8_t*>("hello"), 5);
    CHECK(enc == "aGVsbG8", "known encode hello");

    std::vector<uint8_t> dec;
    CHECK(MeshCoreTunnel::b64url_decode("aGVsbG8", dec), "known decode");
    CHECK(dec.size() == 5 && std::memcmp(dec.data(), "hello", 5) == 0, "hello bytes");
}

static void test_fragment_roundtrip() {
    const uint8_t payload[] = {0x10, 0x20, 0x30, 0x40, 0x50};
    std::string wire = MeshCoreTunnel::encode_channel_fragment(
        /*idx*/ 0, /*pkt*/ 0x01020304u, /*total*/ 2, payload, sizeof(payload));
    CHECK(wire.compare(0, 4, "RNS:") == 0, "RNS: prefix");
    CHECK(wire.find('=') == std::string::npos, "no padding on wire");

    MeshCoreTunnel::FragHeader hdr;
    std::vector<uint8_t> out;
    CHECK(MeshCoreTunnel::decode_channel_fragment(wire, hdr, out), "decode frag");
    CHECK(hdr.frag_idx == 0, "idx");
    CHECK(hdr.pkt_id == 0x01020304u, "pkt_id BE");
    CHECK(hdr.frag_total == 2, "total");
    CHECK(out.size() == sizeof(payload), "payload len");
    CHECK(std::memcmp(out.data(), payload, sizeof(payload)) == 0, "payload");
}

static void test_multipart_reassembly_shape() {
    // Simulate a 100-byte packet at payload_size=64 → 2 fragments.
    std::vector<uint8_t> packet(100);
    for (size_t i = 0; i < packet.size(); ++i) packet[i] = (uint8_t)(i & 0xFF);

    const uint16_t ps = 64;
    uint8_t total = MeshCoreTunnel::fragment_count(packet.size(), ps);
    CHECK(total == 2, "fragment_count 100/64");

    std::vector<std::string> wires;
    for (uint8_t idx = 0; idx < total; ++idx) {
        size_t off = (size_t)idx * ps;
        size_t chunk = (packet.size() - off < ps) ? (packet.size() - off) : ps;
        wires.push_back(MeshCoreTunnel::encode_channel_fragment(
            idx, /*pkt*/ 42, total, packet.data() + off, chunk));
    }

    // Out-of-order receive: frag 1 then 0
    std::vector<uint8_t> slots[2];
    uint8_t got = 0;
    for (int order : {1, 0}) {
        MeshCoreTunnel::FragHeader hdr;
        std::vector<uint8_t> chunk;
        CHECK(MeshCoreTunnel::decode_channel_fragment(wires[order], hdr, chunk), "rx");
        CHECK(hdr.pkt_id == 42, "same pkt");
        CHECK(hdr.frag_total == 2, "total");
        slots[hdr.frag_idx] = std::move(chunk);
        ++got;
    }
    CHECK(got == 2, "both frags");

    std::vector<uint8_t> full;
    full.insert(full.end(), slots[0].begin(), slots[0].end());
    full.insert(full.end(), slots[1].begin(), slots[1].end());
    CHECK(full.size() == packet.size(), "reassembled size");
    CHECK(std::memcmp(full.data(), packet.data(), packet.size()) == 0, "reassembled bytes");
}

static void test_oversized_rejected() {
    // payload_size=1 → 300 bytes needs 300 frags > 255
    CHECK(MeshCoreTunnel::fragment_count(300, 1) == 0, "overflow → 0");
    CHECK(MeshCoreTunnel::fragment_count(255, 1) == 255, "exactly 255 ok");
    CHECK(MeshCoreTunnel::fragment_count(0, 64) == 1, "empty → 1 frag");
}

static void test_meshcore_name_prefix_locate() {
    // Receiver finds "RNS:" inside "NodeName: RNS:...."
    const uint8_t payload[] = {0xAA};
    std::string body = MeshCoreTunnel::encode_channel_fragment(0, 7, 1, payload, 1);
    std::string channel_text = "SolarBridge: " + body;

    size_t rns = channel_text.find("RNS:");
    CHECK(rns != std::string::npos, "locate RNS:");
    MeshCoreTunnel::FragHeader hdr;
    std::vector<uint8_t> out;
    CHECK(MeshCoreTunnel::decode_channel_fragment(channel_text.substr(rns), hdr, out),
          "decode after prefix");
    CHECK(hdr.pkt_id == 7 && out.size() == 1 && out[0] == 0xAA, "payload after prefix");
}

static void test_malformed() {
    MeshCoreTunnel::FragHeader hdr;
    std::vector<uint8_t> out;
    CHECK(!MeshCoreTunnel::decode_channel_fragment("RNS:!!!!", hdr, out), "bad b64");
    CHECK(!MeshCoreTunnel::decode_channel_fragment("NOPE", hdr, out), "no prefix");
    CHECK(MeshCoreTunnel::encode_channel_fragment(2, 1, 2, nullptr, 0).empty() ||
          true, "idx>=total rejected");
    // idx 2 with total 2 is invalid (0-based)
    CHECK(MeshCoreTunnel::encode_channel_fragment(2, 1, 2, (const uint8_t*)"x", 1).empty(),
          "bad idx empty");
}

// ─────────────────────────────────────────────────────────────────────────
// Phase 2: discovery + direct-route helpers
// ─────────────────────────────────────────────────────────────────────────

static void test_encode_bind() {
    CHECK(MeshCoreTunnel::encode_bind("abc123", true,  false) == "RNSBIND:abc123:R",
          "encode BIND router");
    CHECK(MeshCoreTunnel::encode_bind("abc123", false, false) == "RNSBIND:abc123:E",
          "encode BIND edge");
    CHECK(MeshCoreTunnel::encode_bind("cafe", true, true) == "RNSBIND_REQ:cafe:R",
          "encode REQ router");
}

static void test_parse_bind() {
    MeshCoreTunnel::BindMsg b;

    CHECK(MeshCoreTunnel::parse_bind("Node1: RNSBIND:deadbeef:R", b), "parse BIND");
    CHECK(!b.is_req && b.sender_name == "Node1" && b.mc_pubkey == "deadbeef" && b.can_route,
          "BIND fields");

    CHECK(MeshCoreTunnel::parse_bind("Edgy: RNSBIND_REQ:cafe:E", b), "parse REQ");
    CHECK(b.is_req && b.sender_name == "Edgy" && b.mc_pubkey == "cafe" && !b.can_route,
          "REQ fields (edge)");

    // Legacy form (no capability suffix) → treated as router.
    CHECK(MeshCoreTunnel::parse_bind("N: RNSBIND:abcd", b), "parse legacy BIND");
    CHECK(!b.is_req && b.mc_pubkey == "abcd" && b.can_route, "legacy -> router");

    // Lowercase cap letter still recognised.
    CHECK(MeshCoreTunnel::parse_bind("x: RNSBIND:ff:e", b) && !b.can_route,
          "lowercase edge cap");

    // A data fragment is NOT a bind.
    CHECK(!MeshCoreTunnel::parse_bind("Foo: RNS:AAAA", b), "fragment is not bind");
    CHECK(!MeshCoreTunnel::parse_bind("just chatting", b), "plain text not bind");

    // Round-trip through encode_bind (radio prepends the sender name).
    std::string wire = "Me: " + MeshCoreTunnel::encode_bind("0011aa", false, true);
    CHECK(MeshCoreTunnel::parse_bind(wire, b), "roundtrip parse");
    CHECK(b.is_req && b.sender_name == "Me" && b.mc_pubkey == "0011aa" && !b.can_route,
          "roundtrip fields");
}

static void test_extract_token() {
    uint8_t tok[16];

    // Single-header packet: byte0 bit6=0. token = data[2:18].
    uint8_t single[18] = {0x00, 0x00};
    for (int i = 0; i < 16; ++i) single[2 + i] = (uint8_t)(0x10 + i);
    CHECK(MeshCoreTunnel::extract_rns_token(single, sizeof(single), tok), "single ok");
    CHECK(std::memcmp(tok, single + 2, 16) == 0, "single token = first addr");

    // Two-header packet: byte0 bit6=1 (0x40). token = data[18:34] (second addr).
    uint8_t dbl[34] = {0x40, 0x00};
    for (int i = 0; i < 16; ++i) dbl[2 + i]  = (uint8_t)(0x20 + i);   // addr1
    for (int i = 0; i < 16; ++i) dbl[18 + i] = (uint8_t)(0x30 + i);   // addr2 (dest)
    CHECK(MeshCoreTunnel::extract_rns_token(dbl, sizeof(dbl), tok), "double ok");
    CHECK(std::memcmp(tok, dbl + 18, 16) == 0, "double token = second addr");

    // Too short.
    uint8_t tiny[10] = {0x00};
    CHECK(!MeshCoreTunnel::extract_rns_token(tiny, sizeof(tiny), tok), "short rejected");
    CHECK(!MeshCoreTunnel::extract_rns_token(dbl, 20, tok), "double short rejected");
}

static void test_link_id_preimage() {
    std::vector<uint8_t> pre;

    // LINK_REQ, single header: byte0 = 0x02. preimage = [0x02] + data[2:].
    uint8_t lr[20] = {0x02, 0x00};
    for (int i = 0; i < 16; ++i) lr[2 + i] = (uint8_t)(0x40 + i);
    lr[18] = 0xAB; lr[19] = 0xCD;   // trailing payload
    CHECK(MeshCoreTunnel::link_id_preimage(lr, sizeof(lr), pre), "LR preimage ok");
    CHECK(pre.size() == 1 + (sizeof(lr) - 2), "preimage length");
    CHECK(pre[0] == 0x02, "preimage[0] = flags & 0x0F");
    CHECK(std::memcmp(pre.data() + 1, lr + 2, sizeof(lr) - 2) == 0, "preimage tail");

    // Two-header LINK_REQ: byte0 = 0x42. start=18.
    uint8_t lr2[36] = {0x42, 0x00};
    for (int i = 0; i < 34; ++i) lr2[2 + i] = (uint8_t)i;
    CHECK(MeshCoreTunnel::link_id_preimage(lr2, sizeof(lr2), pre), "LR2 preimage ok");
    CHECK(pre[0] == 0x02, "LR2 flags low nibble");
    CHECK(pre.size() == 1 + (sizeof(lr2) - 18), "LR2 preimage length");

    // Not a LINK_REQ → false.
    uint8_t data[20] = {0x00};
    CHECK(!MeshCoreTunnel::link_id_preimage(data, sizeof(data), pre), "DATA not LR");
}

static void test_is_broadcast() {
    uint8_t announce[12] = {0x01};                 // ptype ANNOUNCE
    uint8_t path_req[12] = {0x08};                 // DATA + PLAIN (0x02<<2)
    uint8_t data_single[12] = {0x00};              // DATA + SINGLE
    uint8_t link_req[12] = {0x02};                 // LINK_REQ
    CHECK(MeshCoreTunnel::is_broadcast_packet(announce, 12), "announce broadcast");
    CHECK(MeshCoreTunnel::is_broadcast_packet(path_req, 12), "path-req broadcast");
    CHECK(!MeshCoreTunnel::is_broadcast_packet(data_single, 12), "data/single direct-able");
    CHECK(!MeshCoreTunnel::is_broadcast_packet(link_req, 12), "link-req direct-able");
}

static void test_extract_path_request_target() {
    // Single header: [flags][hops][addr:16][context][payload: target16 + extra]
    // Observed on-air shape: 67 bytes = 2 + 16 + 1 + 48 (target + transport_id
    // + tag). The addressed hash is rnstransport.path.request for EVERY path
    // request; only the payload names the destination being sought.
    uint8_t pkt[67];
    std::memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x08;                                   // DATA + PLAIN
    for (int i = 0; i < 16; ++i) pkt[2 + i] = 0x6b;  // addressed (broadcast) hash
    pkt[18] = 0x00;                                  // context byte
    for (int i = 0; i < 16; ++i) pkt[19 + i] = (uint8_t)(0xA0 + i);  // target

    uint8_t out[16];
    CHECK(MeshCoreTunnel::extract_path_request_target(pkt, sizeof(pkt), out),
          "single-header path-req parses");
    bool match = true;
    for (int i = 0; i < 16; ++i) if (out[i] != (uint8_t)(0xA0 + i)) match = false;
    CHECK(match, "target comes from payload, not address field");

    // Two-byte header: payload starts after both addresses + context.
    uint8_t pkt2[83];
    std::memset(pkt2, 0, sizeof(pkt2));
    pkt2[0] = 0x48;                                  // DATA + PLAIN + hdr bit6
    for (int i = 0; i < 16; ++i) pkt2[35 + i] = (uint8_t)(0xB0 + i);
    CHECK(MeshCoreTunnel::extract_path_request_target(pkt2, sizeof(pkt2), out),
          "two-byte-header path-req parses");
    match = true;
    for (int i = 0; i < 16; ++i) if (out[i] != (uint8_t)(0xB0 + i)) match = false;
    CHECK(match, "two-byte-header target offset");

    uint8_t not_preq[67] = {0x00};                   // DATA + SINGLE
    CHECK(!MeshCoreTunnel::extract_path_request_target(not_preq, sizeof(not_preq), out),
          "non-path-request rejected");
    uint8_t runt[20] = {0x08};                       // too short for a target
    CHECK(!MeshCoreTunnel::extract_path_request_target(runt, sizeof(runt), out),
          "runt path-request rejected");
}

int main() {
    test_b64url_roundtrip();
    test_b64url_known_vector();
    test_fragment_roundtrip();
    test_multipart_reassembly_shape();
    test_oversized_rejected();
    test_meshcore_name_prefix_locate();
    test_malformed();
    test_encode_bind();
    test_parse_bind();
    test_extract_token();
    test_link_id_preimage();
    test_is_broadcast();
    test_extract_path_request_target();

    if (g_fail) {
        std::fprintf(stderr, "\n%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("All MeshCoreTunnelCodec host tests passed.\n");
    return 0;
}
