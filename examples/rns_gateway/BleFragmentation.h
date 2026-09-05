/*
 * BleFragmentation — the ble-reticulum fragment codec, as pure logic.
 *
 * Reticulum packets are up to 500 bytes; a BLE ATT notification or write
 * carries at most (ATT_MTU - 3) bytes. ble-reticulum splits packets into
 * fragments with a 5-byte header and reassembles them on the far side.
 *
 * NORMATIVE reference: the Python implementation at
 *   github.com/torlando-tech/ble-reticulum, commit 07d9413,
 *   src/ble_reticulum/BLEFragmentation.py (BLEFragmenter / BLEReassembler).
 * The spec documents (BLE_PROTOCOL_v2.2.md, BLE_PROTOCOL_v0.3.0.md) agree with
 * it; where they would not, the source wins. Wire format, verbatim:
 *
 *   [Type: 1][Sequence: 2 BE][Total: 2 BE][Data: payload_size]
 *
 *   Type  0x01 START     first fragment (seq 0) — ALSO a lone fragment (total 1)
 *         0x02 CONTINUE  middle fragments
 *         0x03 END       last fragment (seq total-1)
 *
 * There is no CRC, no length field, no lone-fragment type: a one-fragment
 * packet is START with seq=0, total=1 (reference fragment() never emits a
 * bare END). Reassembly completes when all `total` sequence numbers have
 * arrived.
 *
 * Reassembly semantics mirror the reference's receive_fragment() exactly,
 * including its quirks, because the phone apps are conformance subjects
 * against the SAME reference (reticulum-kt's BLEFragmentation.kt, which is
 * what Columba Android ships, was checked and behaves the same way):
 *   - seq 0 UNCONDITIONALLY starts a new buffer, replacing whatever was in
 *     progress — there is no duplicate check on seq 0. Consequently fragments
 *     that arrive BEFORE their START are lost when START arrives, and a
 *     retransmitted START silently restarts the packet. Only non-zero
 *     sequences may arrive in any order.
 *   - a non-zero seq with no buffer in progress starts one anyway (a missing
 *     START is tolerated, as long as it never turns up).
 *   - an exact duplicate (same non-zero seq, same bytes) is ignored; the same
 *     seq with DIFFERENT bytes or length discards the whole buffer.
 *   - seq >= total, total == 0, an unknown type, or a fragment shorter than
 *     the header are rejected without touching the buffer.
 *   - an incomplete buffer expires after REASSEMBLY_TIMEOUT_MS.
 *
 * None of that matters on a healthy link — BLE delivers writes in order and
 * without loss on one connection — but it decides what happens when an app
 * retransmits, so it is pinned by tests.
 *
 * Three things here are deliberately STRICTER than the reference. The first
 * two are for the same reason as MICRORETICULUM_BUGS.md §8c in TcpInterface:
 * a truncated packet delivered to Transport as if whole corrupts resource
 * transfers and stalls them rather than failing loudly.
 *   - a packet that would exceed `max_packet` (HW_MTU, 500) is dropped
 *     ENTIRELY with a distinct error, never truncated; the reference has no
 *     bound at all (65535 fragments).
 *   - `total` is capped at MAX_FRAGMENTS, the most a 500-byte packet can need
 *     at the smallest legal payload, plus slack.
 *   - one buffer per connection. The reference keys buffers by (sender,
 *     total) and so can hold several half-packets from one sender with
 *     different totals; a non-zero seq whose total differs from the live
 *     buffer opens a second buffer there, and is REASM_ERR_TOTAL here
 *     (discarding the live one). On an ordered link a mid-packet total
 *     change can only be a sender restart without START, or corruption.
 *
 * MTU vocabulary — this cost real confusion, so it is pinned here:
 * the reference's `mtu` parameter is what BlueZ/Bleak report, which is the
 * ATT MTU (23..517), and it computes payload = mtu - 5. That does NOT leave
 * room for the 3-byte ATT opcode+handle: at ATT MTU 517 the reference builds
 * 517-byte fragments, which only fit a GATT *long write*, not a notification.
 * The Android and iOS shims instead size fragments from the usable value
 * length (ATT MTU - 3). This codec therefore takes `max_fragment_bytes` — the
 * largest single write/notification the link can carry — and derives the
 * payload from that. main/BleInterface pass (ATT_MTU - 3). The receive side
 * is agnostic: it accepts fragments of any length up to the packet bound, so
 * a central that sized its fragments differently still reassembles.
 *
 * No Arduino or RNS includes: this compiles on the host and is covered by
 * test/host/test_ble_fragmentation.cpp.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RNS_GATEWAY_BLE_FRAGMENTATION_H
#define RNS_GATEWAY_BLE_FRAGMENTATION_H

#include <stddef.h>
#include <stdint.h>

namespace BleRns {

// ── Wire constants (BLEFragmentation.py) ─────────────────────────────────────
static const uint8_t  FRAG_START    = 0x01;
static const uint8_t  FRAG_CONTINUE = 0x02;
static const uint8_t  FRAG_END      = 0x03;
static const size_t   HEADER_SIZE   = 5;

// BLEFragmenter clamps mtu to >= 20 and then subtracts the header, so the
// smallest payload the reference will ever produce is 15 bytes. We size from
// the ATT payload instead (see header comment), and the same floor applies:
// ATT MTU 23 -> 20 usable -> 15 bytes of payload per fragment.
static const size_t   MIN_FRAGMENT_BYTES = 20;
static const size_t   MIN_PAYLOAD        = MIN_FRAGMENT_BYTES - HEADER_SIZE;   // 15

// Reference: BLEReassembler.DEFAULT_TIMEOUT = 30 seconds.
static const uint32_t REASSEMBLY_TIMEOUT_MS = 30000;

// Our bound, not the reference's: 500 bytes at 15 bytes/fragment is 34
// fragments. 64 leaves slack for a central using an even smaller payload.
static const uint16_t MAX_FRAGMENTS = 64;

// ── Sizing helpers ───────────────────────────────────────────────────────────
// Usable payload per fragment for a link that can carry `max_fragment_bytes`
// in one write/notification. Floors at MIN_PAYLOAD, matching the reference's
// mtu >= 20 clamp.
size_t payload_size(size_t max_fragment_bytes);

// The (ATT_MTU - 3) convenience: what one notification/write can carry.
inline size_t max_fragment_for_att_mtu(uint16_t att_mtu) {
    return att_mtu > 3 ? (size_t)(att_mtu - 3) : 0;
}

// Fragments needed for `packet_len` bytes at `payload` bytes each. 0 means
// the packet cannot be carried (empty, or more than 65535 fragments).
uint16_t fragment_count(size_t packet_len, size_t payload);

// ── Fragmenting (BLEFragmenter.fragment_packet) ──────────────────────────────
// Writes fragment `idx` of `packet` into `out`. Returns the fragment length,
// or 0 if idx is out of range or `out` is too small. Type is START for idx 0
// (including a one-fragment packet), END for the last, CONTINUE between.
size_t build_fragment(const uint8_t* packet, size_t packet_len, size_t payload,
                      uint16_t idx, uint8_t* out, size_t out_cap);

// ── Header parsing ───────────────────────────────────────────────────────────
struct FragHeader {
    uint8_t  type;
    uint16_t seq;
    uint16_t total;
};

// Parses the 5-byte header. Returns false if `len` < HEADER_SIZE. Does NOT
// validate type/seq/total — the reassembler does, with distinct results.
bool parse_header(const uint8_t* buf, size_t len, FragHeader& h);

// ── Reassembling (BLEReassembler) ────────────────────────────────────────────
// One reassembler per connection: the gateway is point-to-point, so there is
// no per-sender keying. Storage is a fixed arena — fragments land in arrival
// order and are stitched in sequence order on completion, so variable-length
// fragments and any arrival order work without knowing the sender's payload
// size.
enum ReasmResult {
    REASM_NEED_MORE = 0,   // buffered, packet not yet complete
    REASM_COMPLETE,        // packet() / packet_len() hold a whole packet
    REASM_ERR_SHORT,       // fragment shorter than the header
    REASM_ERR_TYPE,        // type not START/CONTINUE/END
    REASM_ERR_SEQ,         // seq >= total, or total == 0
    REASM_ERR_TOO_MANY,    // total > MAX_FRAGMENTS (our bound)
    REASM_ERR_TOTAL,       // total differs from the packet in progress (buffer discarded)
    REASM_ERR_DUP_MISMATCH,// same seq, different bytes (buffer discarded)
    REASM_ERR_OVERSIZE,    // packet would exceed max_packet (buffer discarded)
    REASM_DUPLICATE,       // exact duplicate fragment, ignored
};

class Reassembler {
public:
    // `arena` must hold max_packet bytes and outlive the reassembler. Passing
    // the buffer in keeps this class allocation-free and lets the caller put
    // it in PSRAM or a static, whichever the firmware prefers.
    Reassembler(uint8_t* arena, size_t max_packet);

    // Feed one fragment as received off the air. `now_ms` is the caller's
    // clock (millis() on the board, anything monotonic on the host).
    ReasmResult receive(const uint8_t* frag, size_t len, uint32_t now_ms);

    // Valid only after REASM_COMPLETE, until the next receive()/reset().
    const uint8_t* packet() const     { return _arena; }
    size_t         packet_len() const { return _complete_len; }

    // Drops an in-progress packet older than the timeout. Returns true if
    // something was dropped. Call periodically (BLEReassembler.cleanup_stale_buffers).
    bool expire(uint32_t now_ms);

    void reset();

    bool     in_progress() const { return _total != 0; }
    uint16_t expected()    const { return _total; }
    uint16_t received()    const { return _received; }

    // Cumulative counters for the heartbeat: what arrived, what was dropped
    // and why. "The phone never sent it" and "the board discarded it" are the
    // same log without these.
    uint32_t packets_complete() const { return _n_complete; }
    uint32_t packets_dropped()  const { return _n_dropped; }
    uint32_t packets_expired()  const { return _n_expired; }

private:
    struct Slot { uint16_t off; uint16_t len; bool present; };

    void start_new(uint16_t total, uint32_t now_ms);
    void discard();
    ReasmResult stitch();

    uint8_t* _arena;
    size_t   _max_packet;
    // In-progress packet: fragments appended to the arena at _used, indexed
    // by seq through _slots. On completion the arena is compacted in order.
    Slot     _slots[MAX_FRAGMENTS];
    uint16_t _total;
    uint16_t _received;
    size_t   _used;
    uint32_t _started_ms;
    size_t   _complete_len;
    uint32_t _n_complete;
    uint32_t _n_dropped;
    uint32_t _n_expired;
};

} // namespace BleRns

#endif // RNS_GATEWAY_BLE_FRAGMENTATION_H
