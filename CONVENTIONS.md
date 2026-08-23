# MeshCore host conventions — inventory for the RNS/LXMF gateway

Audited against this tree at `companion-v1.16.0-15-g219812b9`. Every entry below
is a pattern that already exists in MeshCore; the gateway follows these rather
than inventing its own. Items under [Gaps](#gaps--no-host-pattern-exists) have
no host pattern and need an agreed pattern before code is written.

File references are to this tree.

---

## Build system

- Root `platformio.ini` declares only shared bases; every board lives in
  `variants/<board>/platformio.ini`, pulled in by
  `[platformio] extra_configs = variants/*/platformio.ini`.
- Inheritance chain: `arduino_base` → `esp32_base` → `[Heltec_lora32_v4]` →
  a display flavour (`heltec_v4_oled` / `heltec_v4_tft`) → `[env:...]` per role.
- **Roles are `env:` targets, not `#ifdef`s.** One env per role per board:
  `heltec_v4_repeater`, `heltec_v4_room_server`, `heltec_v4_companion_radio_*`,
  `heltec_v4_sensor`, `heltec_v4_kiss_modem`.
- Feature selection is `-D` flags plus `build_src_filter`, which pulls the
  role's sources in from `examples/<role>/`.
- Radio defaults are build flags, not code:
  `LORA_FREQ` / `LORA_BW` / `LORA_SF` / `LORA_CR` in `arduino_base`
  (`platformio.ini:29-32`), overridden per board.

**Consequence for the gateway:** it is a new `examples/<role>/` app plus a new
`[env:heltec_v4_*]` section. No new top-level project layout.

**Headless template:** `[env:heltec_v4_kiss_modem]`
(`variants/heltec_v4/platformio.ini:435`) is the only heltec_v4 env that extends
`Heltec_lora32_v4` directly instead of a display flavour. It is the model for
the gateway's env — no `DISPLAY_CLASS`, no display sources in
`build_src_filter`.

## Board target — Heltec V4.3

- `boards/heltec_v4.json`: ESP32-S3, 16 MB flash, `memory_type: qio_qspi`,
  `psram_type: qspi`, `maximum_ram_size: 2097152`, default partitions
  `default_16MB.csv`. Matches Pyxis's independently-derived finding that the
  S3R2 needs `qio_qspi` (OPI config boot-loops).
- Pins come from `variants/heltec_v4/platformio.ini:14-45` — LoRa NSS=8,
  SCLK=9, MOSI=10, MISO=11, RESET=12, BUSY=13, DIO1=14; `PIN_VEXT_EN=36`,
  `P_LORA_TX_LED=35`, `PIN_USER_BTN=0`, `PIN_ADC_CTRL=37`, `PIN_VBAT_READ=1`.
- **V4.2 vs V4.3 is auto-detected at runtime, not by build flag.**
  `variants/heltec_v4/LoRaFEMControl.cpp:22-31` reads `P_LORA_KCT8103L_PA_CSD`
  (GPIO2) as an input at boot: HIGH ⇒ KCT8103L FEM ⇒ **V4.3**; LOW ⇒ GC1109
  ⇒ V4.2. Do not add a V4.3 build flag; the detection is the pattern.
- V4.3 FEM pins: `P_LORA_PA_POWER=7`, `P_LORA_KCT8103L_PA_CSD=2`,
  `P_LORA_KCT8103L_PA_CTX=5`.
- LNA control exists only on the V4.3 branch —
  `setLnaCanControl(true)` at `LoRaFEMControl.cpp:37`, with
  `setLNAEnable(bool)` storing the flag and `setRxModeEnable()` driving CTX
  LOW (LNA on) / HIGH (bypass).
- Board hooks into TX/RX are `HeltecV4Board::onBeforeTransmit()` /
  `onAfterTransmit()` (`HeltecV4Board.h:24-25`), which drive the FEM.

## Task model

**Single cooperative `loop()`. There are no FreeRTOS tasks in MeshCore.**

`examples/companion_radio/main.cpp:260-292`:

```cpp
void loop() {
  the_mesh.loop();
  sensors.loop();
  rtc_clock.tick();
  ...
  if (!the_mesh.hasPendingWork()) { board.sleep(0); }
}
```

`Mesh::loop()` is one line — `Dispatcher::loop()` (`src/Mesh.cpp:10`). Radio
service, packet dispatch, retransmit timing and ACK deadlines all advance from
that call. Anything that blocks the Arduino loop delays radio servicing.

Every subsystem exposes a non-blocking `loop()` and is polled from the top.
`hasPendingWork()` (`examples/companion_radio/MyMesh.cpp:2275`) gates sleep.

## Radio and SPI ownership

- `radio_init()` is declared per variant in `variants/<board>/target.h` and
  defined in `target.cpp`. The radio is a single global `radio_driver`
  (`WRAPPER_CLASS`, here `CustomSX1262Wrapper`).
- The `Mesh` subclass receives the radio by reference through the constructor
  chain — `BaseChatMesh(mesh::Radio& radio, ...)`
  (`src/helpers/BaseChatMesh.h:80`). Nothing else touches the SX1262.
- `SX126X_DIO2_AS_RF_SWITCH=true` is set for all heltec_v4 envs
  (`variants/heltec_v4/platformio.ini:36`) with a comment referring to the
  *GC1109* CTX line. On V4.3 the KCT8103L CTX is MCU-driven on GPIO5 by
  `LoRaFEMControl`. Flagged for hardware verification; not changed.

## Logging

`src/MeshCore.h:26-32`:

```cpp
#define MESH_DEBUG_PRINT(F, ...)   Serial.printf("DEBUG: " F, ##__VA_ARGS__)
#define MESH_DEBUG_PRINTLN(F, ...) Serial.printf("DEBUG: " F "\n", ##__VA_ARGS__)
```

Compiled out unless `MESH_DEBUG` is defined. `MESH_PACKET_LOGGING` is a
separate flag for packet traces. `WIFI_DEBUG_PRINTLN` is gated on
`WIFI_DEBUG_LOGGING` (`variants/heltec_v4/platformio.ini:236`).

Use these macros. No `Serial.print` in library or role code except in explicit
boot banners and the USB-debug heartbeat.

## Settings persistence

- `NodePrefs` (`src/helpers/CommonCLI.h:22-66`) is a flat POD struct written
  whole to a prefs file. Adding a setting means adding a field here.
- `CommonCLI::loadPrefs(FILESYSTEM*)` / `savePrefs(FILESYSTEM*)`.
- Roles implement `CommonCLICallbacks` (`CommonCLI.h:68-115`) — `savePrefs()`,
  `getRole()`, `formatFileSystem()`, `setTxPower()`, `formatStatsReply()` etc.
- Identity is separate: `IdentityStore`, surfaced via
  `DataStore::loadMainIdentity()` / `saveMainIdentity()`.

## Filesystem and blob storage

- ESP32 uses SPIFFS: `SPIFFS.begin(true)` then `DataStore store(SPIFFS, rtc_clock)`
  (`examples/companion_radio/main.cpp:32-35, 208-209`).
- `DataStore` (`examples/companion_radio/DataStore.h`) owns contacts, channels,
  prefs and identity, and provides a generic keyed blob store:

```cpp
uint8_t getBlobByKey(const uint8_t key[], int key_len, uint8_t dest_buf[]);
bool    putBlobByKey(const uint8_t key[], int key_len, const uint8_t src_buf[], uint8_t len);
bool    deleteBlobByKey(const uint8_t key[], int key_len);
```

  **`len` is `uint8_t` — a blob is at most 255 bytes.** Anything larger must be
  chunked across keys or written as its own file via `DataStore::openRead()` /
  `removeFile()`.
- `BaseChatMesh` declares matching virtuals `getBlobByKey` / `putBlobByKey`
  (`BaseChatMesh.h:126-127`) for the mesh subclass to wire through to `DataStore`.
- Contacts and channels persist through the `DataStoreHost` callback interface
  (`DataStore.h:7-13`) — the store calls back into the mesh to enumerate what
  to save, rather than holding its own copy.

## Channel API — the translation surface

`BaseChatMesh` (`src/helpers/BaseChatMesh.h`) is the layer the gateway
subclasses. **The bridge surface is a MeshCore group channel, not DMs.**

**Inbound (channel → gateway):**
```cpp
virtual void onChannelMessageRecv(const mesh::GroupChannel& channel, mesh::Packet* pkt,
                                  uint32_t timestamp, const char* text) = 0;   // line 121
```

**Outbound (gateway → channel):**
```cpp
bool sendGroupMessage(uint32_t timestamp, mesh::GroupChannel& channel,
                      const char* sender_name, const char* text, int text_len);  // line 160
```

**Addressing is already MeshCore's wire format — do not invent one.**
`sendGroupMessage` builds the prefix itself (`BaseChatMesh.cpp`):

```cpp
sprintf((char *) &temp[5], "%s: ", sender_name);   // <sender>: <msg>
if (text_len + prefix_len > MAX_TEXT_LEN) text_len = MAX_TEXT_LEN - prefix_len;
```

and `onChannelMessageRecv` delivers that same `"Name: body"` string. The sender
name round-trips through the host's own convention: the gateway passes the LXMF
sender's name as `sender_name` outbound, and inbound the name is already in
`text`.

**Channel management:**
```cpp
ChannelDetails* addChannel(const char* name, const char* psk_base64);   // line 179
bool getChannel(int idx, ChannelDetails& dest);                        // line 180
bool setChannel(int idx, const ChannelDetails& src);                   // line 181
int  findChannelIdx(const mesh::GroupChannel& ch);                     // line 182
```
Channel 0 is MeshCore public and takes no private PSK, so a private bridge
channel must be index 1-7. **The channel PSK is the access boundary on the mesh
side** — there is no separate peer allowlist to build.

**Size behaviour differs from the DM path.** `sendGroupMessage` *truncates* to
fit `MAX_TEXT_LEN` (160, `BaseChatMesh.h:8`), whereas `sendMessage` (line 158,
the DM path, unused here) hard-returns `MSG_SEND_FAILED` past the same limit
(`BaseChatMesh.cpp:463`). Effective channel payload is
`160 - strlen(sender_name) - 2`, so a long sender name eats message body.
Splitting a longer LXMF message across several `sendGroupMessage` calls is the
gateway's job; the host will otherwise truncate silently.

**No contact map.** MeshCore already owns and persists contacts —
`lookupContactByPubKey()` (line 172), `getContactByIdx()` (line 177),
`getNumContacts()` (line 176), `onDiscoveredContact()` (line 111) — and channel
messaging needs none of them.

Other constants: `MAX_PACKET_PAYLOAD` 184, `MAX_TRANS_UNIT` 255,
`MAX_PATH_SIZE` 64, `CIPHER_MAC_SIZE` 2 (`src/MeshCore.h:14-23`).
`MAX_CONTACTS` defaults to 32 in `BaseChatMesh.h:37`, raised to 100 in
`MyMesh.h` and to 350 by the heltec_v4 companion envs.

## Config / status surface

Two established patterns:

1. **`CommonCLI`** — used by the headless roles (`simple_repeater`,
   `simple_room_server`, `simple_sensor`). Text commands via
   `handleCommand(uint32_t sender_timestamp, char* command, char* reply)`,
   transport-agnostic; replies are capped at 160 chars throughout
   (`CommonCLI.cpp:978`, `:1021`).
2. **Companion protocol** — `companion_radio` does *not* use `CommonCLI`; it
   speaks the binary companion frame protocol over a `BaseSerialInterface`
   (`ArduinoSerialInterface` / `SerialBLEInterface` / `SerialWifiInterface`).

Being headless infrastructure, the gateway follows pattern 1.

## WiFi

MeshCore already has a WiFi pattern on ESP32
(`examples/companion_radio/main.cpp:109-112, 218-233, 284-292`):

- `board.setInhibitSleep(true)` while WiFi is active.
- `WiFi.setAutoReconnect(true)` plus a `WiFi.onEvent` handler that sets a
  `wifi_needs_reconnect` flag on `ARDUINO_EVENT_WIFI_STA_DISCONNECTED` and
  clears it on `..._GOT_IP`.
- A 10-second retry in `loop()` that calls `WiFi.disconnect(); WiFi.reconnect();`.
- Credentials arrive as `-D WIFI_SSID` / `-D WIFI_PWD` build flags.

Follow this rather than writing a new reconnect state machine. Whether
credentials stay build flags or move into `NodePrefs` is an open decision.

## Naming and style

- 2-space indent, `{` on the same line, `#pragma once` in headers.
- Classes `PascalCase`; methods `camelCase`; members plain or `_`-prefixed
  (`_prefs`, `_callbacks`) — helper classes prefix, `BaseChatMesh` mostly does not.
- Constants and build flags `SCREAMING_SNAKE_CASE` via `#define`.
- Role app classes are named `MyMesh` in every example, in
  `examples/<role>/MyMesh.{h,cpp}` with a thin `main.cpp`.
- Virtual hooks are `onSomething()`; senders are `sendSomething()`.
- Guarded optional features use `#ifdef FEATURE` around both declaration and use.

---

## The RNS/LXMF side — what we embed

Pyxis no longer vendors LXMF inside microReticulum. The live stack, taken from
`reference/pyxis/platformio.ini`, is three pieces:

| Piece | Pin | Notes |
|---|---|---|
| `torlando-tech/microReticulum` | `cd0338e7` (`pyxis-fixes-on-0.4.1`) | upstream attermann 0.4.1+16 + fixes; bounded transport tables, explicit 1 MiB RNS container pool |
| `torlando-tech/microLXMF` | `60fb7d6` | LXMF as its own repo |
| `reference/pyxis/lib/microreticulum-shim/` | — | `PSRAMAllocator`, `ObjectPool`, `BytesPool`, `MessageBase` |

**Do not use Pyxis's `deps/microReticulum` submodule.** It tracks
`pyxis-fixes-on-0.3.0` and contains no LXMF at all. The `ca355e5` monolith in
the older `~/Downloads/pyxis-heltec-v4` port kit is superseded by the above.

### One identity, one delivery destination — by design

`LXMRouter` holds a single `RNS::Destination _delivery_destination`
(`src/LXMF/LXMRouter.h:591`); `announce()` is hardcoded to it
(`LXMRouter.cpp:1105-1149`); the constructor takes an `Identity`, not a
`Destination`; and `ROUTER_REGISTRY_SIZE = 4` (`LXMRouter.cpp:159`) caps
router instances globally. Each instance costs roughly 28 KB in fixed pools.

This is not a limitation to route around. In Reticulum the identity *is* the
address, and `<identity>.lxmf.delivery` is its one delivery destination. The
gateway therefore runs **exactly one** `LXMRouter` and uses it as intended.

**Consequence:** no derived proxy identities, no per-contact destinations, no
HKDF master secret, and no reimplementation of the inbound link/resource path.
Inbound LXMF arrives through the router's own delivery callback, which already
handles OPPORTUNISTIC and DIRECT (link + resource) and is the path covered by
the C++↔Python interop tests.

---

## Grafting RNS into MeshCore — what the integration actually needed

Four things that are not obvious and will bite anyone repeating this:

1. **Include order is load-bearing.** `src/MeshCore.h:8-12` `#define`s
   `PUB_KEY_SIZE`, `PRV_KEY_SIZE`, `SEED_SIZE`, `SIGNATURE_SIZE` as bare
   macros. These collide with the identically-named constants inside
   `RNS::Type`. **Every microReticulum/microLXMF header must be included before
   any MeshCore header** in a translation unit that uses both.
2. **`Transport::register_interface()` takes `RNS::Interface`, not
   `InterfaceImpl`.** Wrap the impl: `new RNS::Interface(impl)` — the wrapper
   takes ownership via `shared_ptr`. `_online` is protected on the impl; read
   connection state through the wrapper's `online()`.
3. **microLXMF needs patched MsgPack.** `LXMessage`'s `dict[int, Any]` fields
   map uses `Packer::packRawBytes` and `Unpacker::indices`/`raw_data`, all
   private upstream in `hideakitai/MsgPack@0.4.2`. `patch_msgpack.py`
   (`pre:` script, ported from Pyxis) promotes them to public. Without it the
   build fails inside `LXMessage.cpp`.
4. **`RNS_USE_FS` / `RNS_PERSIST_PATHS` are deliberately off.** They pull in
   `microStore` plus two more upstream patch scripts, and buy only path-table
   persistence — cheap to re-learn over a TCP link. The LXMF identity, which
   genuinely must survive reboot, is written to SPIFFS `/rns_identity` (64 raw
   bytes) by `RnsBridge::setupReticulum()` instead.

Pinned deps live in `[heltec_v4_rns_gateway_base]`: microReticulum
`cd0338e7`, microLXMF `60fb7d6`, plus `ArduinoJson`, `MsgPack` and
`densaugeo/base64`. `lib/microreticulum-shim` and `lib/libbz2` are vendored
from Pyxis.

## Decisions taken

| Area | Decision |
|---|---|
| Task model | microReticulum + TCP + LXMF run in their own FreeRTOS task on core 0. MeshCore keeps core 1 and its untouched cooperative `loop()`. A bounded queue each way carries `(contact pubkey, text)` — the only shared state. |
| LoRa | This tree's defaults: 910.525 MHz / BW 62.5 / SF7 / CR5. |
| MeshCore role | Endpoint only. `allowPacketForward` stays false; the node does not repeat or flood. |
| Bridge surface | A MeshCore **group channel** (index 1-7, private PSK), not DMs. No contact map, no proxy destinations. |
| Addressing | MeshCore's own `"<sender>: <msg>"` channel format, built by `sendGroupMessage`. Nothing invented. |
| RNS interfaces | TCPInterface only, to `olymesh.duckdns.org:4243`. AutoInterface off. |
| Bridge channel | Existing `RNSTesting`, index 1, existing PSK — carried over from the two-board build for now. |
| Secrets | WiFi credentials, channel PSK and TCP endpoint live in `platformio.local.ini`, which is gitignored (`.gitignore:19`) and already pulled in by `extra_configs` (`platformio.ini:14`). Never in a tracked file. |
| Messages > 160 B | Split across several `sendGroupMessage` calls with a compact `1/3` marker, capped at N parts, remainder truncated and marked. |
| Config surface | `CommonCLI`, following the headless roles. |
| Partitions | Dual-slot OTA. |
| LNA | Follow the host: `board.setLoRaFemLnaEnabled(_prefs.radio_fem_rxgain)`. |

Still unanswered: WiFi SSID and password (to be written into
`platformio.local.ini` locally, not pasted into the repo or this transcript).

Note on the carried-over channel: its PSK appears in a committed README in the
old two-board repo, so treat it as known-compromised for anything sensitive.
It is also the channel the old bridge used for `RNS:` tunnel fragments — if any
of those nodes are still running, this channel will carry both tunnel traffic
and bridged chat. Worth a fresh channel before this leaves the bench.

---

## Gaps — no host pattern exists

Resolved since the first draft:

- **LNA on V4.3** — closed. 1.17.1 added a full path:
  `mesh::MainBoard::setLoRaFemLnaEnabled()` (`src/MeshCore.h:67`), the V4.3
  override (`variants/heltec_v4/HeltecV4Board.cpp:66`), a
  `NodePrefs.radio_fem_rxgain` field, CLI commands
  (`CommonCLI.cpp:548`, `:556`), and one line per role. Nothing to invent.
- **Headless env** — closed. `[env:heltec_v4_kiss_modem]` is the template.
- **Second network stack** — decided (own task on core 0), but MeshCore has no
  precedent for it, so the queue/locking boundary is new code that should be
  kept as narrow as the table above describes.

Still genuinely open:

### 1. Extending `CommonCLI` with gateway commands

`CommonCLI::handleCommand` (`src/helpers/CommonCLI.cpp:182`) is a fixed
if/else chain with no registration hook or unknown-command callback. Gateway
settings (TCP peer, WiFi, proxy-map inspection) need either a fork, an
upstreamable extension hook, or interception in the role class before
delegating. Interception is the least invasive and does not diverge from the
host; prefer it unless the hook is worth upstreaming.

### 2. Loop prevention

The gateway both reads from and writes to the same channel. Anything it emits
via `sendGroupMessage` will come back through `onChannelMessageRecv` if it is
flood-relayed back, and an LXMF message relayed to the channel must not then be
relayed to LXMF again. MeshCore's `_tables->hasSeen(pkt)` dedups at the packet
layer, but the gateway also needs to recognise its own `sender_name` prefix and
drop it. Cheap to get right, catastrophic to get wrong on a shared mesh.

### 3. Two consumers, one data partition

MeshCore uses SPIFFS on ESP32 for prefs, identity, contacts and channels;
microLXMF's `MessageStore` and the RNS identity also expect a filesystem. They
can share one partition with disjoint path prefixes, but nothing coordinates
quota, and MeshCore's `formatFileSystem()` would wipe both — including the
gateway master secret that every proxy identity derives from.
