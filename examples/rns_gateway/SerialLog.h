/*
 * SerialLog — one mutex in front of Serial, and a log ring behind it.
 *
 * Two tasks print concurrently: the Reticulum stack on core 0 and MeshCore's
 * cooperative loop on core 1. Unsynchronised they interleave mid-line, and the
 * result is a log that is unreadable exactly when you need it — during
 * bring-up. It cost a diagnosis here: the line explaining why the transport
 * identity failed to persist was being shredded by the heartbeat.
 *
 * Everything this role prints goes through slog()/slogln(), and
 * microReticulum's own output is routed in with RNS::set_log_callback(), so
 * both sides share the lock and whole lines land intact.
 *
 * The ring exists because reading the serial port is not free on this board:
 * opening OR closing USB CDC resets it (native USB), so every serial read is
 * a reboot and drops whatever client was connected. Everything slog() prints
 * is also appended to a ring buffer in PSRAM, which the config portal serves
 * at /log — logs over the network, no cable, no reset. In BLE client-access
 * mode the WiFi is normally off, so a bring-up build keeps WiFi up purely for
 * this (RNS_GW_BLE_DEBUG_WIFI). Ring lines carry a millis() prefix; the
 * serial output does not, so it stays byte-identical to before.
 *
 * MeshCore's upstream prints are not routed through this — doing so would mean
 * editing upstream files, which the fork's parity rule forbids. In this role
 * they are rare (MESH_DEBUG is off), so what remains is tolerable.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RNS_GATEWAY_SERIAL_LOG_H
#define RNS_GATEWAY_SERIAL_LOG_H

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef SLOG_RING_SIZE
  // 16 KB of PSRAM holds a few minutes of heartbeats plus whatever happened
  // in between; the portal serves the whole thing in one response.
  #define SLOG_RING_SIZE 16384
#endif

// Recursive so a locked helper may call another without deadlocking.
// Function-local static: initialised on first use, which is thread-safe in
// C++11 and later, and always after the FreeRTOS scheduler is up under Arduino.
inline SemaphoreHandle_t& _serial_mutex() {
    static SemaphoreHandle_t m = xSemaphoreCreateRecursiveMutex();
    return m;
}

inline void serial_lock() {
    SemaphoreHandle_t m = _serial_mutex();
    if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY);
}

inline void serial_unlock() {
    SemaphoreHandle_t m = _serial_mutex();
    if (m) xSemaphoreGiveRecursive(m);
}

// ── Log ring ─────────────────────────────────────────────────────────────────
struct SlogRing {
    char*    buf;      // SLOG_RING_SIZE bytes, PSRAM when available
    size_t   head;     // next write position
    size_t   total;    // bytes ever written (min(total, size) are valid)
    uint32_t dropped;  // lines lost because the ring was not allocated
};

inline SlogRing& _slog_ring() {
    static SlogRing r = { nullptr, 0, 0, 0 };
    if (!r.buf) {
        r.buf = (char*)heap_caps_malloc(SLOG_RING_SIZE, MALLOC_CAP_SPIRAM);
        if (!r.buf) r.buf = (char*)malloc(SLOG_RING_SIZE);
    }
    return r;
}

// Caller holds the serial lock.
inline void _slog_ring_put(const char* s, size_t n) {
    SlogRing& r = _slog_ring();
    if (!r.buf) { r.dropped++; return; }
    for (size_t i = 0; i < n; i++) {
        r.buf[r.head] = s[i];
        r.head = (r.head + 1) % SLOG_RING_SIZE;
    }
    r.total += n;
}

inline void _slog_ring_put_line(const char* s) {
    char pre[16];
    int n = snprintf(pre, sizeof(pre), "%8lu ", (unsigned long)millis());
    _slog_ring_put(pre, (size_t)n);
    _slog_ring_put(s, strlen(s));
}

// Copies the ring, oldest first, into `out` (NUL-terminated). Returns bytes
// copied. Safe from any task.
inline size_t slog_ring_read(char* out, size_t cap) {
    if (cap == 0) return 0;
    serial_lock();
    SlogRing& r = _slog_ring();
    size_t n = 0;
    if (r.buf) {
        size_t valid = r.total < SLOG_RING_SIZE ? r.total : SLOG_RING_SIZE;
        size_t start = r.total < SLOG_RING_SIZE ? 0 : r.head;
        if (valid > cap - 1) {           // keep the newest part
            start = (start + (valid - (cap - 1))) % SLOG_RING_SIZE;
            valid = cap - 1;
        }
        for (size_t i = 0; i < valid; i++) {
            out[n++] = r.buf[(start + i) % SLOG_RING_SIZE];
        }
    }
    out[n] = '\0';
    serial_unlock();
    return n;
}

inline size_t slog_ring_capacity() { return SLOG_RING_SIZE; }

// ── Optional line sink ───────────────────────────────────────────────────────
// A bring-up build can register a sink that receives every line (UDP push,
// see UdpLog.h). Called under the serial lock, so it must be quick and must
// not log. nullptr disables it.
typedef void (*SlogSinkFn)(const char* line, size_t len);
inline SlogSinkFn& _slog_sink() {
    static SlogSinkFn fn = nullptr;
    return fn;
}
inline void slog_set_sink(SlogSinkFn fn) {
    serial_lock();
    _slog_sink() = fn;
    serial_unlock();
}

// ── Printing ─────────────────────────────────────────────────────────────────
// The format attribute is what lets the compiler type-check these call sites.
// GCC only checks format strings for functions it knows are printf-like, so
// without it a "%s" fed an integer compiles clean and then dereferences that
// integer as a pointer at runtime, panicking the node. Serial.printf carries no
// such attribute, which is exactly how that bug reached a flashed board.
#define SLOG_PRINTF_FMT(fmt_idx, first_arg) \
    __attribute__((format(printf, fmt_idx, first_arg)))

// Format first, then take the lock for the write, so the critical section is
// as short as possible and never spans a vsnprintf.
SLOG_PRINTF_FMT(1, 2)
inline void slog(const char* fmt, ...) {
    char buf[384];   // the heartbeat line runs to ~240 chars; leave headroom
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    serial_lock();
    Serial.print(buf);
    _slog_ring_put_line(buf);
    if (_slog_sink()) _slog_sink()(buf, strlen(buf));
    serial_unlock();
}

inline void slogln(const char* msg) {
    serial_lock();
    Serial.println(msg);
    _slog_ring_put_line(msg);
    _slog_ring_put("\r\n", 2);
    if (_slog_sink()) _slog_sink()(msg, strlen(msg));
    serial_unlock();
}

inline void slogln() {
    serial_lock();
    Serial.println();
    _slog_ring_put("\r\n", 2);
    serial_unlock();
}

#endif // RNS_GATEWAY_SERIAL_LOG_H
