# First-time setup

Two variants ship as prebuilt images. They are the same firmware with
different first-boot identities, so two gateways in one household never
collide:

| | Stationary | Mobile |
|---|---|---|
| Intended life | Lives at a site, usually joined to a local network | Travels; serves clients from its own AP |
| Setup AP | `RNSGateway-Stationary` | `RNSGateway-Mobile` |
| Hostname | `rnsgateway-stationary.local` | `rnsgateway-mobile.local` |
| Mesh node name | `RNS GW Stationary` | `RNS GW Mobile` |
| Status | Released | **Preview — not yet field-validated** |

Hardware: Heltec WiFi LoRa 32 **V4** (ESP32-S3 + SX1262). Power from a good
5 V / 2 A USB supply — WiFi + LoRa transmit spikes brown out weak supplies
and thin cables, and the failure looks like mystery reboots, not a clean
error. Treat any attached battery as ride-through backup and verify the
node actually runs on it for 10+ minutes before trusting it.

## 1. Flash

Download the image for your variant from the releases page and flash it at
offset **0**:

```bash
esptool.py --chip esp32s3 write_flash 0x0 rns-gateway-stationary-v0.1.0.bin
```

(Any ESP32 web flasher works too — single file, offset 0x0.)

Unplug and repower the board after flashing. Note: on this board, opening
or closing a USB serial connection **reboots it** — that's normal.

## 2. First contact — the setup AP

The gateway boots as a WiFi access point:

1. Join the network `RNSGateway-Stationary` (or `-Mobile`).
   Password: `rnsgateway`
2. A captive portal should open; if not, browse to `http://192.168.4.1/`.
   Log in as user `admin`, password `password`.

**3. Change both passwords now** — the AP password (Access point section)
and the portal password (Portal access section). They are shipped defaults
and everyone who reads this document knows them.

## 3. Create your MeshCore channel

The gateway bridges **nothing** until you give it a private MeshCore
channel. It does not ship with one, and it cannot use the public channel.

1. In the MeshCore phone app, create a new private channel. Give it a name
   and let the app generate the key.
2. Copy the channel's **PSK** (base64) and name into the portal's
   *MeshCore bridge channel* section.
3. Use a channel **dedicated to the tunnel** — don't ride a channel people
   chat on. Tunnel traffic is opaque `RNS:...` text; it would fill a human
   channel with noise, and human chat costs the tunnel airtime.

Every gateway that should talk to this one needs the **exact same** channel
name and PSK.

## 4. Set the radio

In the portal's *LoRa radio* section, set frequency / bandwidth / spreading
factor / coding rate to match your mesh **exactly**. Defaults are US
910.525 MHz / 62.5 kHz / SF7 / CR5.

> Wrong radio parameters are a **silent** failure: nothing errors, the node
> simply never hears the mesh. If two gateways refuse to see each other,
> check this first, on both, character by character.

## 5. Save & reboot, then verify

Click *Save & reboot*. What success looks like:

- The AP comes back up (with your new password).
- Within ~30 s of a second gateway being powered on the same channel, the
  two discover each other automatically (`RNSBIND` exchange) — nothing to
  configure.

## 6. Connect Reticulum clients

Point your RNS client (MeshChat, Sideband, Columba, `rnsd`, …) at the
gateway's TCP server — a `TCPClientInterface` to:

- **Host**: `rnsgateway-stationary.local` (or `-mobile.local`), or the
  device IP (`192.168.4.1` when you're on its AP)
- **Port**: `4242`
- Up to **4 clients** per gateway.

Example `rnsd` config stanza:

```ini
[[RNS Gateway]]
  type = TCPClientInterface
  enabled = true
  target_host = rnsgateway-stationary.local
  target_port = 4242
```

## Stationary only: join your local network

In the portal's *WiFi station* section, enter your home/site network's SSID
and password. After reboot the gateway joins it and is reachable at
`rnsgateway-stationary.local` from that network — portal and TCP server
both. Notes:

- **One radio serves both AP and station.** The AP follows the station's
  channel and drops its clients when the station roams. If all your clients
  are on the site network anyway, the simplest stable setup is to disable
  the AP entirely once station mode works — the portal stays reachable at
  the hostname.
- If `…local` doesn't resolve, use the device's IP (check your router's
  client list); hostname resolution needs mDNS/multicast, which some
  routers or guest networks filter.
- Do **not** join two mesh-linked sites to networks that can reach each
  other (or peer both into one shared internet transport). Reticulum will
  route around your mesh — everything still works, but the tunnel carries
  nothing and proves nothing.

## Mobile only

Mobile is AP-only by design: its clients join `RNSGateway-Mobile` and reach
the portal/TCP server at `192.168.4.1` (or the hostname). There is nothing
else to configure beyond channel + radio. Station mode exists in the portal
if you ever want it, but a mobile gateway that joins arbitrary networks can
create exactly the route-around-the-mesh situation described above.

## What to expect on the air

- **Text messages** across the mesh: seconds. Feels live.
- **Propagation-node sync** across the mesh: **minutes, by design** — it's
  a chatty protocol on a ~300 bit/s half-duplex link. Start it and let it
  finish; retrying tears down the link and starts over slower.
- **Attachments/images** across the mesh: impractical (a 32 KB file is
  15+ minutes of monopolized airtime). If a transfer completes instantly,
  it went over an IP path, not the radio — which is Reticulum choosing the
  better route, and a feature.
- **After a gateway reboots**: learned paths are deliberately RAM-only.
  The network re-converges on its own (throttled path requests and
  announces); first contact with a destination after a reboot can take up
  to a minute. Everything after that is normal speed.

## Troubleshooting quick hits

| Symptom | First thing to check |
|---|---|
| Gateways don't see each other | Radio parameters identical on both? Channel name **and** PSK identical? |
| `…local` hostname dead | mDNS filtered on your network — use the IP. |
| Client app keeps disconnecting | Gateway drops silent clients after 10 min; healthy apps reconnect automatically. Persistent flapping usually means weak WiFi to the AP. |
| Messages stopped after a reboot | Wait ~1 min for path re-convergence; send again. |
| Node reboots randomly on battery | It's the power supply, not the software — see the note at the top. |
| Portal password forgotten | Factory reset in the portal you can't reach is no help — hold-erase reflash via USB (`esptool.py erase_flash`, then reflash) returns everything to defaults. |
