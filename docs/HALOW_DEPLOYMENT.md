# HaLow deployment for the RNS Gateway

How to extend the gateway's TCP server (and the whole Reticulum site network)
across 802.11ah HaLow links using Heltec HD-01 V2 units. No firmware changes
are involved anywhere in this document: the gateway's TCP server, portal, and
mDNS bind to the IP stack, so anything that delivers IP packets to the board
serves them automatically.

Two topologies, same parts. Option A is simpler and easier to reason about;
Option B trades simplicity for multi-hop resilience.

## Frequency plan (do this first)

Everything here shares the US 902–928 MHz ISM band. Three radio systems that
cannot hear each other are pure interference to each other, so the channel
plan is load-bearing:

| System | Frequency | Notes |
|---|---|---|
| MeshCore LoRa (tunnel) | 910.525 MHz | 62.5 kHz BW, SF7 |
| RNS LoRa (RNode) | 914.900 MHz | 125 kHz BW, SF7 |
| HaLow (HD-01) | 902.5 MHz | 1 MHz channel — verify the configured channel **and width** on the units |

Rules:

- **Lock the HaLow channel.** Never leave auto-channel on. A unit that
  hops onto ~910 or ~915 will flatten a LoRa system, and the LoRa side
  fails *silently* (nodes just go deaf).
- HaLow channels are 1–8 MHz wide. At 902.5 with a 1 MHz width the top
  edge sits near 903 MHz. This is a comfortable clearance below 910.525. 
  A wider width setting (2/4/8 MHz) grows toward the LoRa frequencies; 
  if you need more HaLow throughput, grow **downward** in the band, not up.
- Separate antennas physically at any site that hosts more than one of
  these systems. Near-field overload desensitizes receivers regardless of
  channel spacing (different masts or building corners, not adjacent
  ports on one bracket).
- **Coexistence proof**: after setup, run a MeshCore tunnel test while
  pushing sustained traffic through the HaLow link. Five minutes; do it
  once per site build-out.

## Option A — hub and spoke (AP + stations)

One HD-01 at the home site in AP mode; each remote site's HD-01 in
station/bridge mode. With four units: home + up to three remote sites.

```
HOME SITE                                 REMOTE SITE (x1-3)
---------                                 ------------------
Pi 5: transport + lxmd ---+               phones / laptops
Stationary gateway -------+ router -- HD-01 (AP) ~~802.11ah~~ HD-01 (STA) -- EAP225 / AC1200
(RNS Gateway, Heltec V4)  |                                        |
Pi Zero 2 W (utility) ----+                                   local WiFi for clients
```

- The remote HD-01's Ethernet feeds an outdoor AP (EAP225 / AC1200) giving
  phones ordinary 2.4/5 GHz WiFi — phones do not speak HaLow.
- Client RNS apps connect to `rnsgateway-stationary.local:4242` (or the
  Pi 5's rnsd) across the bridge as plain TCP.
- The MeshCore LoRa tunnel remains the beyond-HaLow layer: Reticulum
  prefers the fast IP path wherever HaLow reaches, and the tunnel carries
  the sites it doesn't. No configuration expresses this — RNS routes.
- Pi Zero 2 W: utility box — mDNS reflector if ever needed, monitoring, or
  a second lxmd later.

Properties: single hop to every remote (full HaLow throughput per link),
simple debugging (a link is up or it isn't), but the home AP unit is a
single point of failure and every remote needs line-of-sight-ish RF to it.

## Option B — 802.11s mesh (all four HD-01s as mesh nodes)

All four units join one 802.11s mesh (confirmed supported on HD-01 V2).
Any node relays for any other; geometry is free-form instead of star.

```
        HD-01 #2 (site B) ~~~ HD-01 #4 (site D)
       /                          /
HD-01 #1 (home) ~~~ HD-01 #3 (site C)
   |
 router -- gateway, Pi 5, ...
```

- Same L2 result: every site's Ethernet drop is a port on one bridged
  network. Clients, TCP, and mDNS behave exactly as in Option A.
- **Multi-hop resilience**: a site with no direct RF path to home reaches
  it through an intermediate site, and 802.11s HWMP re-routes around a
  failed node automatically. This is the reason to choose B.
- **The costs, honestly**:
  - Throughput divides per hop — one radio per node means each relay hop
    roughly halves usable bandwidth on that path. Irrelevant for RNS
    traffic (kbit/s against Mbit/s links), noticeable only if the mesh
    also carries bulk data.
  - Multicast/broadcast floods the whole mesh. mDNS still works; just
    don't put chatty non-RNS gear on this network.
  - Debugging is harder: path selection is dynamic, so "which link is my
    traffic on" needs the HD-01's mesh status page rather than intuition.
- All mesh nodes share **one** HaLow channel (902.5) — the frequency plan
  above applies unchanged, and total airtime is shared across all hops.

## Choosing

- Sites all within RF reach of home, want simplest ops → **A**.
- Any site shadowed from home but visible to another site, or you want the
  backhaul to survive a node loss → **B**.
- Mixed: B is a superset — a mesh of two nodes behaves like a bridge pair,
  so starting with B everywhere is legitimate if the extra configuration
  doesn't bother you.

## Verification checklist (either option)

1. HD-01s in **transparent L2 bridge** mode, not NAT/routed. (If a unit
   only routes: TCP still works by IP; run an mDNS reflector — avahi on
   the Pi Zero — to restore `.local` names.)
2. From a laptop behind a remote unit: `ping rnsgateway-stationary.local`.
   That one command proves L2 bridging + multicast forwarding + the
   gateway's mDNS responder end to end.
3. Fixed HaLow channel + width confirmed on every unit (screenshot the
   config page for the site records).
4. Tunnel coexistence test under HaLow load (see frequency plan).
5. For Option B: temporarily power off the home-adjacent relay and confirm
   a two-hop site re-routes (expect seconds of outage, then recovery).
