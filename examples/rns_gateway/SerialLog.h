/*
 * SerialLog — one mutex in front of Serial.
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
 * MeshCore's upstream prints are not routed through this — doing so would mean
 * editing upstream files, which the fork's parity rule forbids. In this role
 * they are rare (MESH_DEBUG is off), so what remains is tolerable.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RNS_GATEWAY_SERIAL_LOG_H
#define RNS_GATEWAY_SERIAL_LOG_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdarg.h>
#include <stdio.h>

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
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    serial_lock();
    Serial.print(buf);
    serial_unlock();
}

inline void slogln(const char* msg) {
    serial_lock();
    Serial.println(msg);
    serial_unlock();
}

inline void slogln() {
    serial_lock();
    Serial.println();
    serial_unlock();
}

#endif // RNS_GATEWAY_SERIAL_LOG_H
