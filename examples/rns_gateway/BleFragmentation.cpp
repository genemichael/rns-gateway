/*
 * BleFragmentation — implementation. See the header for the wire format and
 * the reference it mirrors (ble-reticulum BLEFragmentation.py @ 07d9413).
 *
 * SPDX-License-Identifier: MIT
 */
#include "BleFragmentation.h"

#include <string.h>

namespace BleRns {

// ── Sizing ───────────────────────────────────────────────────────────────────
size_t payload_size(size_t max_fragment_bytes) {
    // Reference: self.mtu = max(mtu, 20); payload = mtu - HEADER_SIZE.
    if (max_fragment_bytes < MIN_FRAGMENT_BYTES) max_fragment_bytes = MIN_FRAGMENT_BYTES;
    return max_fragment_bytes - HEADER_SIZE;
}

uint16_t fragment_count(size_t packet_len, size_t payload) {
    if (packet_len == 0 || payload == 0) return 0;
    size_t n = (packet_len + payload - 1) / payload;
    if (n > 65535) return 0;           // reference raises ValueError here
    return (uint16_t)n;
}

// ── Fragmenting ──────────────────────────────────────────────────────────────
size_t build_fragment(const uint8_t* packet, size_t packet_len, size_t payload,
                      uint16_t idx, uint8_t* out, size_t out_cap) {
    uint16_t total = fragment_count(packet_len, payload);
    if (total == 0 || idx >= total) return 0;

    size_t off   = (size_t)idx * payload;
    size_t chunk = packet_len - off;
    if (chunk > payload) chunk = payload;
    if (out_cap < HEADER_SIZE + chunk) return 0;

    // Reference fragment_packet(): i == 0 -> START, i == n-1 -> END, else
    // CONTINUE. The i == 0 test comes first, so a lone fragment is START.
    uint8_t type;
    if (idx == 0)               type = FRAG_START;
    else if (idx == total - 1)  type = FRAG_END;
    else                        type = FRAG_CONTINUE;

    out[0] = type;
    out[1] = (uint8_t)(idx >> 8);     // struct.pack("!BHH") — big-endian
    out[2] = (uint8_t)(idx & 0xFF);
    out[3] = (uint8_t)(total >> 8);
    out[4] = (uint8_t)(total & 0xFF);
    memcpy(out + HEADER_SIZE, packet + off, chunk);
    return HEADER_SIZE + chunk;
}

// ── Header ───────────────────────────────────────────────────────────────────
bool parse_header(const uint8_t* buf, size_t len, FragHeader& h) {
    if (len < HEADER_SIZE) return false;
    h.type  = buf[0];
    h.seq   = (uint16_t)((buf[1] << 8) | buf[2]);
    h.total = (uint16_t)((buf[3] << 8) | buf[4]);
    return true;
}

// ── Reassembler ──────────────────────────────────────────────────────────────
Reassembler::Reassembler(uint8_t* arena, size_t max_packet)
    : _arena(arena), _max_packet(max_packet),
      _total(0), _received(0), _used(0), _started_ms(0), _complete_len(0),
      _n_complete(0), _n_dropped(0), _n_expired(0) {
    memset(_slots, 0, sizeof(_slots));
}

void Reassembler::reset() {
    _total = 0;
    _received = 0;
    _used = 0;
    _started_ms = 0;
    _complete_len = 0;
    memset(_slots, 0, sizeof(_slots));
}

void Reassembler::start_new(uint16_t total, uint32_t now_ms) {
    reset();
    _total = total;
    _started_ms = now_ms;
}

void Reassembler::discard() {
    _n_dropped++;
    reset();
}

bool Reassembler::expire(uint32_t now_ms) {
    if (_total == 0) return false;
    // Reference: `now - start_time > timeout` (strict).
    if ((uint32_t)(now_ms - _started_ms) > REASSEMBLY_TIMEOUT_MS) {
        _n_expired++;
        reset();
        return true;
    }
    return false;
}

ReasmResult Reassembler::receive(const uint8_t* frag, size_t len, uint32_t now_ms) {
    _complete_len = 0;

    FragHeader h;
    if (!parse_header(frag, len, h)) return REASM_ERR_SHORT;
    if (h.type != FRAG_START && h.type != FRAG_CONTINUE && h.type != FRAG_END) {
        return REASM_ERR_TYPE;
    }
    // Reference order: "sequence >= total" is checked before "total == 0",
    // but both are rejections, so one result covers them.
    if (h.total == 0 || h.seq >= h.total) return REASM_ERR_SEQ;
    if (h.total > MAX_FRAGMENTS) return REASM_ERR_TOO_MANY;

    const uint8_t* data = frag + HEADER_SIZE;
    const size_t   dlen = len - HEADER_SIZE;

    if (h.seq == 0) {
        // Reference: sequence 0 unconditionally creates a fresh buffer,
        // replacing anything in progress from this sender — no duplicate
        // check, no total check. Fragments already held are lost.
        if (_total != 0) _n_dropped++;
        start_new(h.total, now_ms);
    } else if (_total == 0) {
        // Reference: a non-zero seq with no live buffer starts one anyway.
        start_new(h.total, now_ms);
    } else if (_total != h.total) {
        // Reference "MEDIUM #7": total mismatch discards the buffer.
        discard();
        return REASM_ERR_TOTAL;
    }

    Slot& s = _slots[h.seq];
    if (s.present) {
        // Reference: identical bytes -> benign duplicate, ignored; different
        // bytes -> data-integrity failure, whole buffer discarded.
        if (s.len == dlen && memcmp(_arena + s.off, data, dlen) == 0) {
            return REASM_DUPLICATE;
        }
        discard();
        return REASM_ERR_DUP_MISMATCH;
    }

    // §8c discipline: never let a packet grow past the bound. Drop it whole.
    if (_used + dlen > _max_packet) {
        discard();
        return REASM_ERR_OVERSIZE;
    }

    memcpy(_arena + _used, data, dlen);
    s.off = (uint16_t)_used;
    s.len = (uint16_t)dlen;
    s.present = true;
    _used += dlen;
    _received++;

    if (_received < _total) return REASM_NEED_MORE;
    return stitch();
}

// All fragments are present. They sit in the arena in ARRIVAL order; put
// them in SEQUENCE order. In-place would need a permutation cycle walk, so
// stitch through a small stack copy instead — max_packet is 500 bytes.
ReasmResult Reassembler::stitch() {
    // Sized for the RNS MTU. A caller that passes a larger max_packet gets
    // the packet dropped rather than a stack overrun.
    uint8_t tmp[512];
    if (_used > sizeof(tmp)) {
        discard();
        return REASM_ERR_OVERSIZE;
    }
    size_t pos = 0;
    for (uint16_t i = 0; i < _total; i++) {
        const Slot& s = _slots[i];
        memcpy(tmp + pos, _arena + s.off, s.len);
        pos += s.len;
    }
    memcpy(_arena, tmp, pos);
    _complete_len = pos;
    _n_complete++;
    // Leave the arena holding the packet; clear the in-progress state so the
    // next fragment starts fresh.
    _total = 0;
    _received = 0;
    _used = 0;
    _started_ms = 0;
    memset(_slots, 0, sizeof(_slots));
    return REASM_COMPLETE;
}

} // namespace BleRns
