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
  TcpInterface, ConfigPortal, GatewayConfig, SerialLog)
- Host tests: `./scripts/run_host_tests.sh` (no hardware needed)
- Spec: `../FORK_BRIEF.md`

## Build / flash
- Board A (station + AP): `pio run -e heltec_v4_rns_gateway -t upload`
- Board B (AP-only):      `pio run -e heltec_v4_rns_gateway_b -t upload`
- Prop-restricted variants (same firmware + `RNS_GW_PROP_ONLY`, which forces
  the PropPolicy outbound gate on): `heltec_v4_rns_gateway_prop` /
  `heltec_v4_rns_gateway_b_prop`. Envs live in `platformio.local.ini`.
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
