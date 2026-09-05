/*
 * Host golden tests for BleFragmentation — the ble-reticulum fragment codec.
 * Run:  ./scripts/run_host_tests.sh
 *
 * Vectors are derived from the NORMATIVE Python reference
 * (torlando-tech/ble-reticulum @ 07d9413, BLEFragmentation.py) and the worked
 * examples in BLE_PROTOCOL_v2.2.md. Where a test pins a stricter behaviour
 * than the reference (oversize drop, fragment cap) the comment says so.
 */
#include "BleFragmentation.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace BleRns;

static int g_fail = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);\
            ++g_fail;                                                          \
        }                                                                      \
    } while (0)

static std::vector<uint8_t> pattern(size_t n, uint8_t seed = 0) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = (uint8_t)((i * 7 + seed) & 0xFF);
    return v;
}

// Fragment a whole packet into wire fragments at `payload` bytes each.
static std::vector<std::vector<uint8_t>> fragment_all(const std::vector<uint8_t>& pkt,
                                                      size_t payload) {
    std::vector<std::vector<uint8_t>> out;
    uint16_t n = fragment_count(pkt.size(), payload);
    for (uint16_t i = 0; i < n; ++i) {
        uint8_t buf[600];
        size_t len = build_fragment(pkt.data(), pkt.size(), payload, i, buf, sizeof(buf));
        out.emplace_back(buf, buf + len);
    }
    return out;
}

struct Rig {
    uint8_t     arena[500];
    Reassembler r;
    Rig() : r(arena, sizeof(arena)) {}
};

// ── Sizing ───────────────────────────────────────────────────────────────────
static void test_sizing() {
    // Reference vocabulary: BLEFragmenter(mtu=23).payload_size == 18,
    // BLEFragmenter(mtu=517).payload_size == 512 (mtu - 5).
    CHECK(payload_size(23)  == 18,  "reference mtu=23 -> payload 18");
    CHECK(payload_size(517) == 512, "reference mtu=517 -> payload 512");
    // Reference clamps mtu to >= 20: BLEFragmenter(mtu=5).payload_size == 15.
    CHECK(payload_size(5)  == 15, "floor: mtu<20 clamps to 20 -> 15");
    CHECK(payload_size(0)  == 15, "floor at zero");
    // Our vocabulary: what one ATT notification carries is ATT_MTU - 3.
    CHECK(max_fragment_for_att_mtu(23)  == 20,  "ATT 23 -> 20 usable");
    CHECK(max_fragment_for_att_mtu(517) == 514, "ATT 517 -> 514 usable");
    CHECK(payload_size(max_fragment_for_att_mtu(23))  == 15,  "ATT 23 -> 15 payload");
    CHECK(payload_size(max_fragment_for_att_mtu(517)) == 509, "ATT 517 -> 509 payload");
    CHECK(payload_size(max_fragment_for_att_mtu(185)) == 177, "ATT 185 -> 177 payload");

    CHECK(fragment_count(0, 18) == 0,     "empty packet -> 0 fragments");
    CHECK(fragment_count(18, 18) == 1,    "exact fit -> 1");
    CHECK(fragment_count(19, 18) == 2,    "one over -> 2");
    CHECK(fragment_count(500, 15) == 34,  "500 B at 15/frag -> 34");
    CHECK(fragment_count(500, 509) == 1,  "500 B at ATT 517 -> 1");
    CHECK(fragment_count(100, 64) == 2,   "100/64 -> 2");
}

// ── Golden bytes ─────────────────────────────────────────────────────────────
static void test_golden_lone_fragment() {
    // A packet that fits in one fragment is START, seq 0, total 1 — the
    // reference's fragment_packet() tests i == 0 first, so there is no bare
    // END and no lone type.
    const uint8_t pkt[] = {'h', 'e', 'l', 'l', 'o'};
    uint8_t out[32];
    size_t len = build_fragment(pkt, sizeof(pkt), 18, 0, out, sizeof(out));
    const uint8_t want[] = {0x01, 0x00, 0x00, 0x00, 0x01, 'h', 'e', 'l', 'l', 'o'};
    CHECK(len == sizeof(want), "lone fragment length");
    CHECK(std::memcmp(out, want, sizeof(want)) == 0, "lone fragment bytes: START/0/1");
    CHECK(build_fragment(pkt, sizeof(pkt), 18, 1, out, sizeof(out)) == 0, "idx past end -> 0");
    CHECK(build_fragment(pkt, sizeof(pkt), 18, 0, out, 4) == 0, "out too small -> 0");
}

static void test_golden_spec_example_517() {
    // BLE_PROTOCOL_v2.2.md sequence diagram, MTU=517: an 847-byte packet
    // becomes [0x01][0x00][0x02][512 bytes] and [0x03][0x01][0x02][335 bytes].
    // (The reference uses payload 512 there; we pass 512 explicitly.)
    auto pkt = pattern(847);
    auto fr = fragment_all(pkt, 512);
    CHECK(fr.size() == 2, "847 B at 512 -> 2 fragments");
    const uint8_t h0[] = {0x01, 0x00, 0x00, 0x00, 0x02};
    const uint8_t h1[] = {0x03, 0x00, 0x01, 0x00, 0x02};
    CHECK(fr[0].size() == 517 && std::memcmp(fr[0].data(), h0, 5) == 0, "frag 0 header START/0/2, 512 B");
    CHECK(fr[1].size() == 340 && std::memcmp(fr[1].data(), h1, 5) == 0, "frag 1 header END/1/2, 335 B");
    CHECK(std::memcmp(fr[0].data() + 5, pkt.data(), 512) == 0, "frag 0 payload");
    CHECK(std::memcmp(fr[1].data() + 5, pkt.data() + 512, 335) == 0, "frag 1 payload");
}

static void test_golden_three_fragments_big_endian() {
    // 40 bytes at payload 18 -> 18 + 18 + 4. Middle is CONTINUE. Check the
    // big-endian u16 fields with a seq that has a non-zero high byte too.
    auto pkt = pattern(40);
    auto fr = fragment_all(pkt, 18);
    CHECK(fr.size() == 3, "40 B at 18 -> 3");
    CHECK(fr[0][0] == 0x01 && fr[1][0] == 0x02 && fr[2][0] == 0x03, "START/CONTINUE/END types");
    CHECK(fr[1][1] == 0x00 && fr[1][2] == 0x01, "seq 1 big-endian");
    CHECK(fr[1][3] == 0x00 && fr[1][4] == 0x03, "total 3 big-endian");
    CHECK(fr[2].size() == 5 + 4, "last fragment carries the remainder");

    // A synthetic header with high bytes set: seq 0x0102 total 0x0304.
    const uint8_t raw[] = {0x02, 0x01, 0x02, 0x03, 0x04, 0xAA};
    FragHeader h;
    CHECK(parse_header(raw, sizeof(raw), h), "parse ok");
    CHECK(h.type == 0x02 && h.seq == 0x0102 && h.total == 0x0304, "parse big-endian fields");
    CHECK(!parse_header(raw, 4, h), "parse rejects < 5 bytes");
}

// ── Reassembly ───────────────────────────────────────────────────────────────
static bool roundtrip(size_t packet_len, size_t payload, bool reverse, const char* what) {
    Rig rig;
    auto pkt = pattern(packet_len, (uint8_t)packet_len);
    auto fr = fragment_all(pkt, payload);
    uint16_t n = (uint16_t)fr.size();
    bool got = false;
    for (uint16_t k = 0; k < n; ++k) {
        uint16_t i = reverse ? (uint16_t)(n - 1 - k) : k;
        ReasmResult r = rig.r.receive(fr[i].data(), fr[i].size(), 1000);
        if (k + 1 < n) {
            if (r != REASM_NEED_MORE) {
                std::fprintf(stderr, "  %s: fragment %u -> %d, expected NEED_MORE\n", what, i, (int)r);
                return false;
            }
        } else {
            got = (r == REASM_COMPLETE);
        }
    }
    if (!got) { std::fprintf(stderr, "  %s: never completed\n", what); return false; }
    if (rig.r.packet_len() != pkt.size()) return false;
    if (std::memcmp(rig.r.packet(), pkt.data(), pkt.size()) != 0) return false;
    if (rig.r.in_progress()) return false;
    return rig.r.packets_complete() == 1 && rig.r.packets_dropped() == 0;
}

static void test_reassembly_mtu23_and_mtu512() {
    // ATT MTU 23 -> 15-byte payload; a full 500-byte RNS packet is 34 frags.
    CHECK(roundtrip(500, 15, false, "500@15 in order"),  "500 B over ATT 23, in order");
    CHECK(roundtrip(17, 15, false,  "17@15"),            "two fragments, 15+2");
    CHECK(roundtrip(15, 15, false,  "15@15"),            "exact single fragment");
    CHECK(roundtrip(1, 15, false,   "1@15"),             "one byte");
    // ATT MTU 512 -> 504 payload (rounded from 509 to a non-power for variety):
    CHECK(roundtrip(500, payload_size(max_fragment_for_att_mtu(512)), false, "500@ATT512"),
          "500 B over ATT 512 is a lone START fragment");
    // Reference-vocabulary sizes too: mtu=23 -> 18, mtu=185 -> 180, mtu=517 -> 512.
    CHECK(roundtrip(500, 18, false, "500@18"),           "500 B at reference mtu=23 payload");
    CHECK(roundtrip(500, 180, false, "500@180"),         "500 B at reference mtu=185 payload");
    CHECK(roundtrip(499, 512, false, "499@512"),         "499 B at reference mtu=517 payload");
}

static void test_out_of_order_after_start() {
    // Reference: once START (seq 0) is in, the remaining sequences may
    // arrive in any order.
    Rig rig;
    auto pkt = pattern(100, 9);
    auto fr = fragment_all(pkt, 18);          // 6 fragments
    CHECK(fr.size() == 6, "100 B at 18 -> 6");
    const uint16_t order[] = {0, 3, 5, 1, 4, 2};
    ReasmResult r = REASM_NEED_MORE;
    for (uint16_t i : order) r = rig.r.receive(fr[i].data(), fr[i].size(), 1000);
    CHECK(r == REASM_COMPLETE, "shuffled-after-START completes");
    CHECK(rig.r.packet_len() == 100 && std::memcmp(rig.r.packet(), pkt.data(), 100) == 0,
          "shuffled-after-START reassembles correctly");

    // Reference quirk, pinned: fully reversed (START last) can never complete,
    // because seq 0 discards everything held before it. 34 fragments of a
    // 500-byte packet at ATT MTU 23, sent 33..0.
    CHECK(!roundtrip(500, 15, true, "500@15 reversed (expected to fail)"),
          "START-last is NOT reassembled — reference semantics");
    Rig rev;
    auto p2 = pattern(40, 4);
    auto f2 = fragment_all(p2, 18);           // 3 fragments
    CHECK(rev.r.receive(f2[2].data(), f2[2].size(), 0) == REASM_NEED_MORE, "END first buffered");
    CHECK(rev.r.receive(f2[1].data(), f2[1].size(), 0) == REASM_NEED_MORE, "CONTINUE buffered");
    CHECK(rev.r.receive(f2[0].data(), f2[0].size(), 0) == REASM_NEED_MORE, "START last restarts");
    CHECK(rev.r.received() == 1 && rev.r.packets_dropped() == 1, "held fragments were dropped");
}

static void test_missing_start_tolerated() {
    // Reference: a non-zero seq with no buffer live starts one anyway.
    Rig rig;
    auto pkt = pattern(40, 3);
    auto fr = fragment_all(pkt, 18);          // 3 fragments
    CHECK(rig.r.receive(fr[1].data(), fr[1].size(), 0) == REASM_NEED_MORE, "CONTINUE first is buffered");
    CHECK(rig.r.in_progress() && rig.r.expected() == 3, "buffer opened from seq 1");
    CHECK(rig.r.receive(fr[2].data(), fr[2].size(), 0) == REASM_NEED_MORE, "END buffered");
    // Now seq 0 arrives — reference semantics: seq 0 ALWAYS starts a new
    // buffer, so the two already held are discarded and this is a fresh start.
    CHECK(rig.r.receive(fr[0].data(), fr[0].size(), 0) == REASM_NEED_MORE, "START restarts");
    CHECK(rig.r.received() == 1 && rig.r.packets_dropped() == 1, "previous partial counted as dropped");
    CHECK(rig.r.receive(fr[1].data(), fr[1].size(), 0) == REASM_NEED_MORE, "re-sent seq 1");
    CHECK(rig.r.receive(fr[2].data(), fr[2].size(), 0) == REASM_COMPLETE, "re-sent seq 2 completes");
    CHECK(std::memcmp(rig.r.packet(), pkt.data(), 40) == 0, "content intact after restart");
}

static void test_duplicates() {
    Rig rig;
    auto pkt = pattern(40, 5);
    auto fr = fragment_all(pkt, 18);
    CHECK(rig.r.receive(fr[0].data(), fr[0].size(), 0) == REASM_NEED_MORE, "seq0");
    CHECK(rig.r.receive(fr[1].data(), fr[1].size(), 0) == REASM_NEED_MORE, "seq1");
    // Exact duplicate: ignored, buffer untouched.
    CHECK(rig.r.receive(fr[1].data(), fr[1].size(), 0) == REASM_DUPLICATE, "exact dup ignored");
    CHECK(rig.r.received() == 2 && rig.r.in_progress(), "dup did not disturb buffer");
    CHECK(rig.r.receive(fr[2].data(), fr[2].size(), 0) == REASM_COMPLETE, "completes after dup");
    CHECK(std::memcmp(rig.r.packet(), pkt.data(), 40) == 0, "content intact after dup");

    // Same non-zero seq, different bytes: the reference discards the whole
    // buffer.
    CHECK(rig.r.receive(fr[0].data(), fr[0].size(), 0) == REASM_NEED_MORE, "second packet seq0");
    CHECK(rig.r.receive(fr[1].data(), fr[1].size(), 0) == REASM_NEED_MORE, "second packet seq1");
    std::vector<uint8_t> bad = fr[1];
    bad[7] ^= 0xFF;
    CHECK(rig.r.receive(bad.data(), bad.size(), 0) == REASM_ERR_DUP_MISMATCH, "dup mismatch rejected");
    CHECK(!rig.r.in_progress(), "buffer discarded on mismatch");
    CHECK(rig.r.packets_dropped() == 1, "mismatch counted as drop");
    // Different LENGTH with the same seq is also a mismatch.
    CHECK(rig.r.receive(fr[0].data(), fr[0].size(), 0) == REASM_NEED_MORE, "third packet seq0");
    CHECK(rig.r.receive(fr[1].data(), fr[1].size(), 0) == REASM_NEED_MORE, "third packet seq1");
    CHECK(rig.r.receive(fr[1].data(), fr[1].size() - 1, 0) == REASM_ERR_DUP_MISMATCH, "shorter dup is mismatch");

    // Reference quirk, pinned: a repeated seq 0 — even with different bytes —
    // is not a duplicate, it is a restart.
    CHECK(rig.r.receive(fr[0].data(), fr[0].size(), 0) == REASM_NEED_MORE, "fourth packet seq0");
    CHECK(rig.r.receive(fr[1].data(), fr[1].size(), 0) == REASM_NEED_MORE, "fourth packet seq1");
    std::vector<uint8_t> bad0 = fr[0];
    bad0[7] ^= 0xFF;
    CHECK(rig.r.receive(bad0.data(), bad0.size(), 0) == REASM_NEED_MORE, "changed seq0 restarts, not rejected");
    CHECK(rig.r.received() == 1, "restart holds only the new seq0");
}

static void test_total_mismatch() {
    Rig rig;
    auto pkt = pattern(40, 6);
    auto fr = fragment_all(pkt, 18);      // total 3
    CHECK(rig.r.receive(fr[0].data(), fr[0].size(), 0) == REASM_NEED_MORE, "seq0/3");
    std::vector<uint8_t> other = fr[1];
    other[4] = 4;                          // claims total 4
    CHECK(rig.r.receive(other.data(), other.size(), 0) == REASM_ERR_TOTAL, "total mismatch rejected");
    CHECK(!rig.r.in_progress() && rig.r.packets_dropped() == 1, "buffer discarded on total mismatch");
}

static void test_header_validation() {
    Rig rig;
    uint8_t f[8] = {0x01, 0x00, 0x00, 0x00, 0x01, 'x', 'y', 'z'};
    CHECK(rig.r.receive(f, 4, 0) == REASM_ERR_SHORT, "4 bytes: too short");
    // A header with no payload is legal (5 bytes) — reference accepts it.
    CHECK(rig.r.receive(f, 5, 0) == REASM_COMPLETE && rig.r.packet_len() == 0, "header-only lone fragment = empty packet");

    uint8_t t0[6] = {0x00, 0x00, 0x00, 0x00, 0x01, 'a'};
    CHECK(rig.r.receive(t0, 6, 0) == REASM_ERR_TYPE, "type 0x00 (LONE) is NOT in the reference — rejected");
    uint8_t t4[6] = {0x04, 0x00, 0x00, 0x00, 0x01, 'a'};
    CHECK(rig.r.receive(t4, 6, 0) == REASM_ERR_TYPE, "type 0x04 rejected");

    uint8_t s[6] = {0x02, 0x00, 0x03, 0x00, 0x03, 'a'};   // seq 3 of total 3
    CHECK(rig.r.receive(s, 6, 0) == REASM_ERR_SEQ, "seq >= total rejected");
    uint8_t z[6] = {0x01, 0x00, 0x00, 0x00, 0x00, 'a'};   // total 0
    CHECK(rig.r.receive(z, 6, 0) == REASM_ERR_SEQ, "total 0 rejected");
    CHECK(!rig.r.in_progress(), "rejections leave no buffer");

    // Our bound, not the reference's: total above MAX_FRAGMENTS.
    uint8_t m[6] = {0x01, 0x00, 0x00, 0x00, 0x41, 'a'};   // total 65
    CHECK(rig.r.receive(m, 6, 0) == REASM_ERR_TOO_MANY, "total 65 > MAX_FRAGMENTS rejected");
    uint8_t m2[6] = {0x01, 0x00, 0x00, 0x00, 0x40, 'a'};  // total 64
    CHECK(rig.r.receive(m2, 6, 0) == REASM_NEED_MORE, "total 64 accepted");
}

static void test_oversize_dropped_not_truncated() {
    // §8c: a packet that would exceed the bound is dropped whole. Build a
    // 600-byte packet (legal for the reference, over our 500-byte HW_MTU).
    Rig rig;
    auto pkt = pattern(650, 1);
    auto fr = fragment_all(pkt, 100);     // 7 fragments: 6 x 100 + 50
    ReasmResult r = REASM_NEED_MORE;
    int delivered = 0;
    for (auto& f : fr) {
        r = rig.r.receive(f.data(), f.size(), 0);
        if (r == REASM_COMPLETE) delivered++;
    }
    CHECK(delivered == 0, "oversize packet never delivered");
    CHECK(rig.r.packets_dropped() == 1, "oversize counted once");
    // seq 5 pushed the buffer past 500 and was dropped whole; seq 6 then
    // arrived with no buffer live, so the reference semantics open a fresh
    // buffer for it — it then waits (and would expire).
    CHECK(rig.r.in_progress() && rig.r.received() == 1, "trailing fragment opened a new buffer");

    // Exactly 500 bytes must pass.
    Rig ok;
    auto p500 = pattern(500, 2);
    auto f500 = fragment_all(p500, 100);
    for (auto& f : f500) r = ok.r.receive(f.data(), f.size(), 0);
    CHECK(r == REASM_COMPLETE && ok.r.packet_len() == 500, "500 B exactly is delivered");
    CHECK(std::memcmp(ok.r.packet(), p500.data(), 500) == 0, "500 B content");
}

static void test_timeout() {
    Rig rig;
    auto pkt = pattern(40, 8);
    auto fr = fragment_all(pkt, 18);
    CHECK(rig.r.receive(fr[0].data(), fr[0].size(), 1000) == REASM_NEED_MORE, "seq0 at t=1000");
    CHECK(!rig.r.expire(1000 + REASSEMBLY_TIMEOUT_MS), "not expired at exactly the timeout (strict >)");
    CHECK(rig.r.expire(1000 + REASSEMBLY_TIMEOUT_MS + 1), "expired one ms later");
    CHECK(!rig.r.in_progress() && rig.r.packets_expired() == 1, "expiry cleared the buffer");
    CHECK(!rig.r.expire(999999), "nothing to expire");
    // Wraparound-safe: started near UINT32_MAX, now past zero.
    CHECK(rig.r.receive(fr[0].data(), fr[0].size(), 0xFFFFFF00u) == REASM_NEED_MORE, "seq0 near wrap");
    CHECK(rig.r.expire(0xFFFFFF00u + REASSEMBLY_TIMEOUT_MS + 1), "expiry across millis wrap");
}

static void test_back_to_back_packets() {
    // Two packets in a row through one reassembler, second one lone.
    Rig rig;
    auto a = pattern(30, 11);
    auto b = pattern(10, 12);
    auto fa = fragment_all(a, 18);
    auto fb = fragment_all(b, 18);
    CHECK(rig.r.receive(fa[0].data(), fa[0].size(), 0) == REASM_NEED_MORE, "a0");
    CHECK(rig.r.receive(fa[1].data(), fa[1].size(), 0) == REASM_COMPLETE, "a1 completes");
    CHECK(rig.r.packet_len() == 30 && std::memcmp(rig.r.packet(), a.data(), 30) == 0, "a content");
    CHECK(rig.r.receive(fb[0].data(), fb[0].size(), 0) == REASM_COMPLETE, "lone b completes");
    CHECK(rig.r.packet_len() == 10 && std::memcmp(rig.r.packet(), b.data(), 10) == 0, "b content");
    CHECK(rig.r.packets_complete() == 2, "two completions");
}

int main() {
    test_sizing();
    test_golden_lone_fragment();
    test_golden_spec_example_517();
    test_golden_three_fragments_big_endian();
    test_reassembly_mtu23_and_mtu512();
    test_out_of_order_after_start();
    test_missing_start_tolerated();
    test_duplicates();
    test_total_mismatch();
    test_header_validation();
    test_oversize_dropped_not_truncated();
    test_timeout();
    test_back_to_back_packets();

    if (g_fail) {
        std::fprintf(stderr, "%d BLE fragmentation check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("BLE fragmentation tests: all passed\n");
    return 0;
}
