# Bluetooth LE client access

The gateway can serve local Reticulum clients over **Bluetooth LE** instead of
WiFi. Columba on Android and iOS, and a Linux box running the
[`ble-reticulum`](https://github.com/torlando-tech/ble-reticulum) package,
connect to the device as BLE centrals and reach the MeshCore mesh through the
gateway's Reticulum transport — same tunnel, same multi-hop behaviour, no WiFi.

Status (2026-09-05): **running on hardware.** Columba Android connects,
handshakes, and passes Reticulum packets both ways over BLE; the board is
bound to the mesh at the same time. See [Milestones](#milestones).

## Either WiFi or BLE, never both

Client access is a single setting in the portal, **Client access → WiFi / Bluetooth
LE**. One is active per boot:

| Mode | Client path | WiFi radio | Portal reachable |
|---|---|---|---|
| WiFi (default) | AP + station, TCP server on port 4242 | on | yes |
| Bluetooth LE | ble-reticulum peripheral | **never started** | no — see below |

This is deliberate. The S3 shares one 2.4 GHz front end between WiFi and BLE,
and a site with two client paths lets Reticulum route around the mesh, which
silently invalidates every multi-hop test. Release builds therefore refuse to
run both. In BLE mode the WiFi driver is never initialised, so its buffers and
task are never allocated.

## Getting back to the portal from BLE mode

In BLE mode there is no WiFi, so there is no portal. Two ways back:

1. **Hold the PRG button for ten seconds, then release** (display or not).
   The device reboots into a one-off *setup session*: the normal WiFi/AP-first
   boot, portal at `http://192.168.4.1/` on the device's AP, with the stored
   configuration untouched. Change what you need and save, or just reboot —
   the next boot is back in BLE mode unless you switched the setting. The
   request lives in RTC memory, so it survives the software reset but not a
   power cycle. Feedback while holding: the TX LED comes on solid at five
   seconds and blinks at ten; the OLED, if fitted, says which release does
   what. Releasing between five and ten seconds **powers the device off**
   instead (MeshCore's deep-sleep power-off; press RST or power-cycle to
   start it again). Both act on release: PRG is the boot-strapping pin, and
   resetting while it is held would land in the serial bootloader.
2. **Reflash.** A `/rns_gateway.json` written by the portal survives a reflash
   and keeps its `client_access`, so reflashing alone does not change mode;
   use factory reset from a setup session for that.

If the BLE stack fails to start in BLE mode, the firmware does not sit dark: it
reboots into the same setup session and says why in the log.

## Reading logs without the serial port

Opening **or closing** USB serial resets this board (native USB CDC), so every
serial read is a reboot. Everything `slog()` prints is also kept in a 16 KB ring
in PSRAM, served by the portal at **`/log`** (same login as the portal). In
WiFi mode that is simply the log. In BLE mode WiFi is off, so for bring-up there
are dedicated builds that keep WiFi up **only** for the portal:

| Env | What it is |
|---|---|
| `heltec_v4_rns_gateway_ble_bringup` | tracked, no secrets; boots as AP `RNSGateway-BLE`, client access defaults to BLE, WiFi kept up for `/log` |
| `heltec_v4_rns_gateway_ble_dbg` / `_b_ble_dbg` | in `platformio.local.ini`; boards A and B with their usual credentials plus the same two flags |

The flags are `-D RNS_GW_CLIENT_ACCESS=1` (first-boot default = BLE; a stored
config wins once saved) and `-D RNS_GW_BLE_DEBUG_WIFI=1` (keep WiFi up in BLE
mode). The TCP server stays **off** in BLE mode even in these builds, so the
phone under test still has no path except Bluetooth. `RNS_GW_BLE_DEBUG_WIFI`
must never appear in a release env.

The log carries a `[mem]` line at boot, after Reticulum starts, after the BLE
stack starts, and every 60 s: internal heap free / largest block / minimum
ever, and PSRAM free / largest. Those numbers answer the Bluedroid-fit question
directly. The 10 s heartbeat gains a `[ble]` line with packet and fragment
counters and every drop cause.

## Protocol

The device implements the **peripheral** side of ble-reticulum, wire format
v2.2 plus the v0.3.0 capability advertisement. The normative reference is the
Python implementation, commit `07d9413`; the phone apps are conformance
subjects against it.

| | |
|---|---|
| Service UUID | `37145b00-442d-4a94-917f-8f42c5da28e3` |
| RX (central → device) | `…28e5`, WRITE and WRITE_WITHOUT_RESPONSE |
| TX (device → central) | `…28e4`, READ and NOTIFY with CCCD |
| Identity | `…28e6`, READ, 16 bytes: the gateway's Transport identity hash |
| Advertising | flags + complete 128-bit service UUID + manufacturer data `FF FF 03 01` (company 0xFFFF, protocol 0x03, PERIPHERAL_ONLY); name `RNS-<8 hex>` in the scan response |
| Handshake | the first write to RX is exactly 16 bytes — the central's Transport identity hash. Anything else before that is dropped. No pairing, no bonding. |
| Fragments | 5-byte header `[type u8][seq u16 BE][total u16 BE]`, types START 0x01 / CONTINUE 0x02 / END 0x03; a lone fragment is START with total 1. Payload per fragment = fragment size − 5. |
| Sizing | our notifications use fragment size = negotiated ATT MTU − 3; we offer ATT MTU 517. Received fragments of any size up to 520 bytes are accepted (long writes included). |
| RNS interface | `HW_MTU` 500, `FIXED_MTU`, bitrate 62 500, `MODE_GATEWAY`, echo prevention by source connection |
| Clients | up to 3 concurrent centrals, each with its own identity and reassembler |

The codec (`BleFragmentation.h/.cpp`) is pure logic with golden tests in
`test/host/test_ble_fragmentation.cpp`, run by `scripts/run_host_tests.sh`.
The tests pin the reference's reassembly semantics, including its quirks
(sequence 0 always restarts the packet, so fragments before START are lost),
and the two places we are stricter: a packet over 500 bytes is dropped whole,
never truncated, and `total` is capped at 64 fragments.

## Connecting a client

**Linux reference (the conformance gate).** Install ble-reticulum per its
README, then in `~/.reticulum/config`:

```
[[BLE Interface]]
  type = BLEInterface
  enabled = yes
  # Central only: the gateway cannot connect out, so advertising here
  # only adds noise to the test.
  enable_peripheral = no
  enable_central = yes
  # min_rssi = -85
```

`rnsd --verbose` should discover the gateway by service UUID, read its
identity, write its own, and `rnstatus` should list a BLE peer interface.

The reference decides connection direction by address order (lower
initiates) and does not implement the v0.3.0 PERIPHERAL_ONLY flag; the
gateway sidesteps that by advertising from `ff:ff:ff:…`, which sorts above
any adapter. See finding 7. If the reference still logs "they initiate" at
DEBUG level, the random static address did not take — check the
`[BleIF] advertising … from random static` log line.

Do **not** have any other interface enabled that could reach the far site
(AutoInterface on a shared LAN, a shared TCP peer) or the multi-hop test
proves nothing — confirm with `rnpath`.

**Columba Android.** Add an *AndroidBLE* interface in Interface Management
and enable it (BLE is off by default; the app asks for Bluetooth permissions).
The shipping Kotlin backend uses reticulum-kt, whose fragmenter matches the
reference byte for byte.

**Columba iOS.** Experimental. The shipping (embedded-Python) build has working
BLE through its SwiftBLEBridge; the native ReticulumSwift build does not yet.

## Bring-up findings on hardware (2026-09-05)

1. **WiFi + BLE together requires WiFi modem sleep, or the BLE controller
   aborts.** `esp_bt_controller_enable()` → `coex_enable()` →
   `coex_core_enable()` calls `abort()` when the WiFi driver is running with
   power save off, which `wifi_begin()` sets deliberately. Board C
   crash-looped on every boot until the bring-up build re-enabled modem
   sleep just before `BleInterface::start()`. Release BLE mode never starts
   WiFi and is unaffected.
2. **Under modem sleep the station is half-dead**, the same failure this
   project documented for AP+STA: it answers mDNS multicast and drops most
   inbound unicast, so `/log` could not be fetched and neither could the AP
   address. Outbound traffic is fine, so bring-up logging is now pushed as
   UDP datagrams to a host (`UdpLog.h`, `RNS_GW_UDP_LOG_HOST`, listen with
   `nc -ul 5140`). The `/log` endpoint remains useful in WiFi mode.
3. A static-filled OLED on this build was the crash loop, not the display:
   the panel initialised and the task that draws on it died before its first
   frame.
4. USB serial on the S3 re-enumerates across the reset that opening the port
   causes; a capture has to reopen the device after it comes back.

## Findings from the Phase-0 research (2026-09-04)

Nothing here blocks interop; each is either a documented quirk or something
to raise upstream. None is worked around silently.

1. **The reference's `mtu` is the ATT MTU, and it does not subtract the 3-byte
   ATT header.** `BLEFragmenter(mtu=517)` builds 517-byte fragments; those
   only fit a GATT *long write*, not a notification (max ATT MTU − 3 = 514).
   As a central, BlueZ long-writes them and we reassemble the value whole, so
   it works. As a peripheral the reference would have BlueZ truncate its own
   notifications at large MTUs — not our case, but worth an upstream note.
   Both phone shims size from the usable length (ATT MTU − 3) instead, which is
   what this firmware does.
2. **Columba Android ignores the v0.3.0 PERIPHERAL_ONLY flag** and still applies
   the v2.2 MAC-sorting rule (`KotlinBLEBridge.shouldConnect`, local MAC <
   peer MAC ⇒ connect). Android hides the phone's real MAC from apps and
   returns `02:00:00:00:00:00`, so in practice the phone always sorts lower
   than an Espressif address and always initiates — the right outcome by
   accident. We advertise the flag anyway (harmless to v2.2 centrals, correct
   for v0.3.0 ones). Upstream should honour the flag.
3. **Columba's `BleConstants.kt` defines a fragment type `LONE = 0x00`** that
   neither reticulum-kt nor the reference ever emits, and both reassemblers
   reject. Dead constant; our reassembler rejects 0x00 too and counts it.
4. **No keepalive exists in the protocol.** Unlike the TCP interface there is no
   idle reaper here either: the BLE link layer's supervision timeout reports a
   vanished central as a disconnect.
5. **iOS cannot see MACs** and returns a zero sentinel; its role resolution is
   post-connect by identity. It connects to any advertiser with the service
   UUID, which is all a peripheral-only device needs.
6. The rejected "real TCP over Bluetooth" idea was re-checked and stays
   rejected: the S3 has no Classic BT (no PAN/RFCOMM), neither phone OS
   exposes IPSP/6LoWPAN, iOS non-GATT transport needs MFi, and Android's
   L2CAP CoC (API 29+) is a raw byte stream, not a socket.
7. **The v2.2 MAC-sorting rule can deadlock a peripheral-only gateway against
   the Linux reference** (finding 2 is the same rule on Android, where the
   fake `02:00:00:00:00:00` local address happens to make it harmless). The
   reference has no flag support and no config override (`enable_central`
   only turns scanning on or off). This is precisely the problem v0.3.0 was
   written to fix, and it is unfixed in the reference. **Resolved on the
   gateway side (2026-09-04):** the device advertises from a BLE *random
   static* address `ff:ff:ff:xx:xx:xx` (low bytes from the identity, so it
   is stable across reboots), which sorts above every assigned OUI. Under
   v2.2 the higher address never initiates, which is exactly what a
   peripheral-only device wants: every central concludes it must connect.
   Protocol-conformant, and the BLE address is not part of the Reticulum
   identity. Columba was fine either way (Android's fake local MAC sorts
   lowest; iOS never compares). Implementing the flag upstream remains the
   right long-term fix and is worth an issue on ble-reticulum.

## Milestones

| # | Gate | Status |
|---|---|---|
| 1 | BLE stack up in BLE mode with WiFi down; heap figures at boot and every 60 s; logs readable without serial | **passed 2026-09-05 on board C**: advertising, nRF Connect connects with no PIN, identity reads back, OLED shows `CLI 1/3`. Bluedroid costs ~60 KB of internal heap (151 KB → 90 KB free with WiFi also up); ~150 KB free in real BLE mode. |
| 2 | BleInterface, fragmentation codec with golden tests, mode either/or, path back to portal | built; portal WiFi→BLE switch **observed** (save turned WiFi off, board came up in BLE mode). PRG path back not yet exercised. |
| 3.1 | Linux ble-reticulum ↔ gateway passes RNS traffic | not run — Columba is the target; Linux box optional |
| 3.2 | LXMF to a destination on the far side of the tunnel, multiple hops confirmed | pending |
| 3.3 | Columba Android connects, handshakes, passes traffic | **passed 2026-09-05**: `BLERX`/`BLETX` both climbing with the shipping Kotlin backend, no app-side or gateway-side workaround |
| 3.4 | WiFi → BLE → WiFi via portal, no reflash | pending |
| 3.5 | Columba iOS | pending |

Static build figures, 2026-09-04 (`heltec_v4_rns_gateway_stationary`, same
firmware either way — the BLE code is always compiled in):

| | before BLE | with BLE | delta |
|---|---|---|---|
| static RAM (data + bss) | 87,552 | 108,496 | +20,944 |
| flash | 1,389,589 | 1,970,385 | +580,796 |

The static RAM growth is the Bluedroid host's own tables plus this role's
queues (6 × 524-byte RX items, event queue). The runtime cost — controller
and host heap, task stacks in internal RAM — is what `[mem] after BLE start`
will report; nothing is known about it until a board runs this.
