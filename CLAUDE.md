# RNS-over-MeshCore gateway — active project

This is the MeshCore fork hosting the RNS gateway role. All work happens in
**new files** — the fork is additive-only against upstream MeshCore.

## Hard rules
- **Never edit an upstream file.** Our entire diff is new directories plus one
  appended block in `variants/heltec_v4/platformio.ini`. Enforced by
  `scripts/check_parity.sh` — run it after any change.
- Secrets (WiFi, channel PSK) live in `platformio.local.ini` (gitignored),
  never in tracked files.
- Do NOT modify `/Users/gm/Documents/rns_meshcore_bridge` — that is the old
  two-board design, kept as reference only.

## Layout
- Role code: `examples/rns_gateway/` (main.cpp, MyMesh, MeshCoreInterface,
  TcpInterface, BleInterface + BleFragmentation, ConfigPortal, GatewayConfig,
  SerialLog, StatusScreen)
- Host tests: `./scripts/run_host_tests.sh` (no hardware needed)
- Spec: `../FORK_BRIEF.md`; BLE client access: `docs/BLE_CLIENT_ACCESS.md`

## Client access: WiFi or BLE (either/or)
- Portal setting `client_access` (GatewayConfig). BLE mode never starts
  WiFi. PRG held 10 s then released = one-off WiFi setup session (RTC
  flag); held 5 s then released = power off. Gestures act on RELEASE only
  (GPIO0 is the strapping pin).
- WiFi + BLE together (bring-up builds only) REQUIRES WiFi modem sleep or
  the BLE controller aborts in coex_core_enable; under modem sleep inbound
  unicast is unreliable, so bring-up logs are pushed over UDP (UdpLog.h).
- BLE = ble-reticulum peripheral, normative reference is the Python repo
  (torlando-tech/ble-reticulum @ 07d9413); phone apps are conformance
  subjects. Fragment codec is pure and golden-tested.
- Product variants: Stationary, Mobile, **BLE** (defaults to Bluetooth;
  boots into the WiFi setup session until a PSK is saved). Same firmware.
  Boards: Heltec V4 (`heltec_v4_rns_gateway_*`), T-Beam Supreme
  (`tbeam_supreme_rns_gateway[_ble]`); role flags shared via
  `variants/rns_gateway/platformio.ini` [rns_gateway_role].
- Logs without serial: portal `/log` serves the slog() ring. Local-ini
  `_ble_dbg` envs keep WiFi up + push UDP logs; `RNS_GW_BLE_DEBUG_WIFI`
  must never ship.

## Build / flash
- Board A (station + AP): `pio run -e heltec_v4_rns_gateway -t upload`
- Board B (AP-only):      `pio run -e heltec_v4_rns_gateway_b -t upload`
- Prop-restricted variants (same firmware + `RNS_GW_PROP_ONLY`, which forces
  the PropPolicy outbound gate on): `heltec_v4_rns_gateway_prop` /
  `heltec_v4_rns_gateway_b_prop` in `platformio.local.ini`, plus tracked
  release envs `heltec_v4_rns_gateway_stationary_prop` / `_mobile_prop`.
  Whitelist is the portal's "Prop destination hashes" field.
- Release variants (tracked, no secrets, boot AP-first for portal config):
  `heltec_v4_rns_gateway_stationary` / `heltec_v4_rns_gateway_mobile`.
  Product naming is **Stationary** and **Mobile** (decided 2026-08-23; avoids
  colliding with RNS's MODE_ROAMING terminology).

## Gotchas that cost real time (details in project memory)
- Opening OR closing USB serial **resets the board** (native USB CDC). Any
  serial read is a deliberate reboot; test over the network when possible.
- microReticulum headers must be included before any MeshCore header.
- `-w` is set repo-wide and hides printf format bugs; `slog()` carries the
  format attribute — use it, not `Serial.printf`, in role code.
- `RNS_PERSIST_PATHS` is deliberately OFF (microStore FileStore crashes on
  SPIFFS compaction). Do not re-enable.
