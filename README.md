# RNS Gateway — Reticulum over MeshCore, on one device

A [Reticulum](https://reticulum.network/) gateway that runs *inside* MeshCore
firmware on a single Heltec WiFi LoRa 32 V4: MeshCore owns the SX1262 radio,
an embedded [microReticulum](https://github.com/torlando-tech/microReticulum)
transport runs alongside it, and Reticulum clients (MeshChat, Sideband,
Columba, …) connect over WiFi to a TCP server on the device. The MeshCore
mesh is the long-haul transport between sites.

```
 RNS client apps ──WiFi/TCP:4242──► [ Heltec V4 ]                 [ Heltec V4 ] ◄──WiFi/TCP──  RNS client apps
 (MeshChat, Sideband,               RNS transport ◄─LoRa/MeshCore─► RNS transport
  Columba, rnsd ...)                + MeshCore node    channel      + MeshCore node
```

This repository is a fork of [MeshCore](https://github.com/meshcore-dev/MeshCore).
Everything gateway-related is **additive**: new directories plus one appended
block in the variant build file, enforced by a CI gate
(`scripts/check_parity.sh`) that fails if any upstream file is edited. The
gateway role lives in [`examples/rns_gateway/`](examples/rns_gateway/). That
directory (plus `lib/microreticulum-shim/`, `test/host/`, `scripts/`) is the
entire footprint of this project.

## Lineage and credit

The tunnel design is **not original to this project**. It is a C++ port of
[`MeshCore_Dynamic_Interface.py`](https://github.com/comms-engineer/RNS_Over_Meshcore)
by comms-engineer — the hybrid channel-broadcast / unicast-direct RNS
interface for MeshCore — and is **wire-format identical** to it, including
the fragment framing, the `RNSBIND` demand-driven peer discovery, and the
direct-with-channel-fallback routing strategy. A gateway running this
firmware interoperates with a Python node running that interface. The
original author's design decisions around airtime discipline (demand-driven
discovery instead of periodic push, per-destination rate limits, hard
bandwidth caps) are the backbone of this implementation; where we deviate,
it is documented in the source.

Also: [Reticulum](https://github.com/markqvist/Reticulum) by
Mark Qvist, [microReticulum](https://github.com/torlando-tech/microReticulum)
(the embedded RNS core), and the [MeshCore](https://github.com/meshcore-dev/MeshCore)
project itself, whose README this file displaced —
[read it upstream](https://github.com/meshcore-dev/MeshCore#readme).

## How it actually works

**What MeshCore sees.** The tunnel rides on a **private MeshCore group
channel**. Each RNS packet is split into fragments and sent as ordinary
channel text messages of the form `RNS:<base64url(header+payload)>` — to the
rest of the mesh this is opaque, PSK-encrypted channel traffic on a channel
nobody else has joined. The gateway does **not** read, bridge, or translate
MeshCore users' messages; MeshCore chat and RNS traffic pass through the
same radios without touching each other.

**Routing.** Broadcast traffic (announces, path requests) goes on the
channel. Everything else is upgraded to MeshCore **direct messages** —
routed unicast, end-to-end acknowledged — as soon as peer gateways discover
each other via `RNSBIND`, with automatic fallback to the channel when a
direct send goes unacknowledged. Direct routing exists specifically to keep
point-to-point traffic from being flooded mesh-wide.

**Interface modes.** The mesh-facing RNS interface runs `MODE_BOUNDARY`; the
client-facing TCP interface runs `MODE_GATEWAY`. The original interface's
README warns: never use gateway mode **on a LoRa interface** attached to a
high-connectivity backbone — that floods the channel with routes. This
firmware follows that rule: gateway-mode duties (answering and propagating
clients' path requests) face the WiFi side only, and everything those duties
put on the air passes through the per-destination throttles below.

## Network strain — honest numbers

A LoRa channel is a shared, half-duplex resource. At the default radio
settings (US 910.525 MHz, 62.5 kHz BW, SF7, CR5) the tunnel budgets roughly
**300 bit/s** of useful throughput, moved in ≤64-byte fragments paced 2.5 s
apart on the channel (0.5 s when direct). Concretely:

| Traffic | Size | Fragments | Tunnel occupancy |
|---|---|---|---|
| Path request | ~70 B | 2 | ~5 s |
| Announce | ~220 B | 4 | ~10 s |
| Short LXMF text message | 300–500 B | 5-8 | 15-25 s |
| Photo / large resource | 100 KB+ | 1,500+ | **the better part of an hour** |

Text messaging over the tunnel is practical. Images and file transfer are
technically possible and **socially inappropriate on a mesh you share**.
They monopolize airtime for everyone within RF range, on any channel. Limit attachments 
to 32kb or less and only for IP connections. No LXST over MeshCore.

What the firmware does to stay polite:

- **Announce throttle**: one rebroadcast per destination per 10 min
  (default; portal-tunable). Keyed on the *actual destination*, so each
  device gets its path through once and repeats are suppressed.
- **Path-request throttle**: one forwarded request per *queried*
  destination per 30 min (default; portal-tunable), with a 60 s burst window
  so a retry can survive a lost fragment. Demand-driven path responses
  bypass the announce throttle so recovery stays fast without staying loud.
- **RNS announce bandwidth cap** on the mesh interface, on top of ours.
- **Duplicate suppression** and **flood scoping** inherit from MeshCore
  itself — the gateway is a standard MeshCore node and obeys the same
  airtime budget factor as everything else on your mesh.
- **No periodic beacons.** Peer discovery (`RNSBIND`) is demand-driven with
  an hourly quiet heartbeat, matching the reference interface.
- **No medium or larger attachments,** and only for IP connections.

**What this asks of your mesh**: an idle gateway pair is near-silent. A pair
serving light messaging costs a few announce/message bursts per hour. The
worst case is a cold start (both gateways rebooted, all paths forgotten —
paths are deliberately RAM-only), which costs one path-request/announce
exchange per active destination, throttled as above.

## Getting started

Prebuilt images are on the [releases page](../../releases) — **Stationary**
(site gateway) and **Mobile** (AP-only, travels; currently a preview). Flash
at offset 0 with esptool, then:

1. Join the device's WiFi AP (`RNSGateway-Stationary`, password
   `rnsgateway`) and open the portal at `http://192.168.4.1/` (user `admin`,
   password `password`).
2. **Change both passwords** (AP and portal — both fields are in the portal).
3. Create your **own private channel** in the MeshCore app, and paste its
   name and PSK into the portal. No channel ships in the firmware, and the
   device bridges nothing until you do this. Use a channel dedicated to the
   tunnel — don't ride a channel humans chat on.
4. Set the LoRa parameters to match your mesh **exactly** (silent failure
   otherwise — the node simply hears nothing).
5. Point your Reticulum client at the device: `rnsgateway-stationary.local`
   port `4242` (TCP), up to 4 clients.

Repeat on a second gateway at the far site, same channel and radio
parameters. Two sites, one shared rnsd, is a mistake worth avoiding: if both
gateways also peer with a common internet transport, Reticulum will route
around your mesh and the tunnel will carry nothing.

## On AI-assisted development

Parts of this codebase were written with AI assistance (Anthropic's Claude),
under human direction and review. The community's skepticism
is warranted here, and we think the honest response is discipline 
you can verify rather than assurances:

- **The wire protocol is not AI-invented** — it's a port of a working,
  human-designed interface, kept wire-compatible, with the reference
  implementation's golden tests ported alongside it (`test/host/`, runnable
  on any desktop with `scripts/run_host_tests.sh`, no hardware needed).
- **Every routing and throttle behavior was validated on real hardware**,
  several of them the hard way — the git history and source comments record
  the actual on-air failures that drove each fix, including two mis-keyed
  rate limiters found by watching a live channel misbehave.
- **The fork discipline is machine-enforced** — CI fails if a single
  upstream MeshCore line is edited, so reviewing this project means
  reviewing `examples/rns_gateway/` and nothing else.
  (This behavior needs some corrections as it is pointing to my own local
  MeshCore folder)

Review, criticism, and testing on other meshes are genuinely welcome.

## Roadmap

- Validation of the **Mobile** variant (then it leaves preview).
- **Propagation-node sync tunnels**: build variants (in-tree now, unreleased)
  that restrict the tunnel to whitelisted LXMF propagation-node
  destinations — a dedicated, policy-enforced sync link instead of a
  general-purpose tunnel.
- 802.11ah (HaLow) client access.

## License

Gateway code (`examples/rns_gateway/`, `test/host/`, `scripts/`) is MIT
(SPDX headers in each file). MeshCore and all upstream components retain
their own licenses.
