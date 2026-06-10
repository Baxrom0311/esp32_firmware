# Burxon — Protocol & Real-time Optimization Report

This document analyses the messaging stack used by the Burxon school-bell
system (ESP32 ↔ EMQX MQTT ↔ Django backend ↔ WebSocket dashboards) and
recommends concrete changes. It maps every section of the project brief
(Component 5, items 1–10) to a verdict, evidence, and an action item.

> Scope: real-time correctness, latency, bandwidth, reliability, and battery
> impact. **Out of scope:** rewriting the architecture. The recommendations
> are deliberately incremental.

## TL;DR

| Area | Verdict | Action |
| --- | --- | --- |
| MQTT QoS | Mostly correct, two narrow tweaks | `fire_alarm` / `panic_alert` → QoS 1 with retained=false; ACK topic stays QoS 1 |
| MQTT vs CoAP | **Keep MQTT** | EMQX server is already deployed; CoAP gains do not justify the rewrite |
| MQTT-SN | **Skip** | We are on WiFi+TLS, not LoRa/Zigbee; no UDP gateway in EMQX OSS |
| WS / SSE / long poll | **Keep WS for emergency, add SSE for status feed** | Lower memory than WS, no client back-pressure to manage |
| Direct EMQX→browser WS | **No** | Auth and topic ACL get harder; backend stays the broker for browser clients |
| Heartbeat interval | Adaptive 15–60 s | Increase when RSSI > −60 dBm and idle, shorten when in flap state |
| Payload compression | **Defer** (ESP32 cost > savings) | Profile, then move to MessagePack only if NB-IoT is added |
| Offline-first sync | Already covered by `clean_session=false` + version-vector | Document broker session expiry; add `schedule_version` ACK already in place |
| TLS perf | **One config flip** | Verify `CONFIG_MBEDTLS_SSL_SESSION_TICKETS=y` is on; keep cert bundle |
| Keep-alive tuning | 30 s is fine on WiFi, raise to 60 s on stable links | Drop to 15 s when reconnecting |

The end of this document lists "Quick wins" (config-only) and "Long-term"
options (require firmware or topology changes). **None of the
recommendations require an architecture rewrite.**

---

## 1. MQTT QoS levels

### Current state

| Topic | Direction | QoS | Retain | Notes |
| --- | --- | --- | --- | --- |
| `devices/{id}/status` | device → cloud | 0 (heartbeat) / 1 (online & LWT) | true (online & LWT) | Online/LWT publishes are retained so the dashboard always sees current state. |
| `devices/{id}/command` | cloud → device | 1 | false | Backend publishes commands at QoS 1; device subscribes at QoS 1. |
| `devices/{id}/schedule` | cloud → device | 1 | true | Retained so a fresh device gets the latest schedule on subscribe. |
| `devices/{id}/config` | cloud → device | 1 | true | Same reasoning as schedule. |
| `devices/{id}/holidays` | cloud → device | 1 | true | Same. |
| `devices/{id}/ack` | device → cloud | 1 | false | One-shot ACK for command msg_id. |
| `devices/{id}/sync` | device → cloud | 1 | false | Sent on connect to request delta. |

Verified in `src/mqtt_client.c` (`subscribe_topics`, `MQTT_EVENT_CONNECTED`, `mqtt_dispatch_command`).

### Pros / Cons

- **QoS 0** for high-frequency heartbeat: zero broker overhead, packet loss
  is acceptable because the next heartbeat is ~30 s away.
- **QoS 1** for commands / schedule / config: at-least-once delivery is the
  right tradeoff. Idempotency is handled by `msg_id` on the device side.
- **QoS 2** would add a 4-way handshake per command (PUBLISH, PUBREC, PUBREL,
  PUBCOMP). For a 100-byte ring command, that is 4× the round trips and
  noticeable latency on cellular links. Not justified — duplicates are rare,
  and the device's `msg_id` deduplication makes them harmless.

### Recommendation

- Keep QoS 1 for `command`, `schedule`, `config`, `holidays`, `ack`, `sync`.
- Keep QoS 0 for the periodic heartbeat publish in
  `mqtt_client_send_heartbeat` — it is a snapshot, not an event.
- Keep QoS 1 + retain=true for the `online`/LWT state publish.
- For `fire_alarm` and `panic_alert` events going **device → cloud** (when
  added), use QoS 1 with `retain=false`. Retained safety alerts are a footgun
  — a stale "fire" message would resurface for any subscriber that joined
  later. (We do not currently retain these. Keep it that way.)

**No code change required.** The QoS map above is correct for production.

## 2. MQTT vs CoAP

### Comparison

|  | MQTT (TCP+TLS) | CoAP (UDP+DTLS) |
| --- | --- | --- |
| Wire size, smallest payload | ~15 B header + payload | ~4 B header + payload |
| Connection model | Long-lived, broker-mediated | Stateless, client-server |
| QoS | 0/1/2, broker-side queueing | CON/NON, no broker queue |
| Pub/Sub | Native | Via `Observe` extension only |
| ESP32 RAM | ~12–18 KB connected | ~4–8 KB |
| Backend | EMQX deployed today | Would need a CoAP gateway (e.g. `aiocoap`, EMQX CoAP gateway is enterprise) |
| Reconnect | Persistent session, broker queues missed messages | Client must re-`Observe` and re-poll |
| Push to browser | Bridges via EMQX websocket adapter | Requires bespoke proxy |

### Verdict

**Keep MQTT.** CoAP saves ~10 KB of RAM and ~10–30 bytes per message. Those
savings matter on battery-powered LoRa nodes; they do not move the needle on
a mains-powered ESP32 sending a few messages per minute on WiFi. Switching
would also forfeit:

- Persistent sessions (`clean_session=false`) — critical for our 7-day
  offline tolerance story.
- The dashboard's existing WebSocket bridge to EMQX.
- Operator familiarity (EMQX dashboards, audit logs, ACL plugins).

CoAP becomes attractive **only** if we move to NB-IoT/LTE-M with a per-byte
data plan. Reassess at that point.

## 3. MQTT vs MQTT-SN

MQTT-SN is purpose-built for sensor networks: UDP transport, numeric topic
IDs, sleeping clients with broker-side message queueing.

For this project:

- We have a permanent power supply and stable WiFi: the "sleeping client"
  feature is irrelevant.
- EMQX OSS does **not** ship an MQTT-SN gateway (only EMQX Enterprise does).
  Adding one means hosting `mosquitto.rsmb` or `paho.mqtt-sn.gateway` and
  bridging to EMQX — operational complexity for no measurable win.
- Numeric topic IDs save ~30 B per message. We send a few hundred messages
  per device per day; that's < 10 KB/day saved.

**Verdict:** Skip MQTT-SN. Revisit only if the device line moves to LoRa or
ZigBee.

## 4. Real-time channels: WebSocket vs SSE vs long-polling

### Current state (browser side)

- Member app uses a WebSocket on `/ws/alerts/?token=…` for emergency events
  (panic, lockdown, emergency_ring, offline). See
  `school_device_member_app/src/hooks/use-emergency-ws.ts`.
- Dashboard pulls device status, schedule, and bell-log over REST + React
  Query refetch intervals.

### Tradeoffs

|  | WebSocket | SSE | Long-poll |
| --- | --- | --- | --- |
| Direction | bidirectional | server → client | client-driven |
| HTTP friendly | upgrade required | plain HTTP/1.1 chunked | plain HTTP/1.1 |
| Proxy / corporate firewall | sometimes blocked | usually fine | always fine |
| Server cost per client | full TCP+upgrade | one HTTP/1.1 socket | new request per poll |
| Reconnect | manual | built-in `EventSource` retry | manual |
| Auth refresh | tricky (`?token=` in URL or subprotocol) | `Cookie:` header on each retry | trivial |

### Recommendation

- Keep WebSocket for `/ws/alerts/`. Bidirectional ACK and the existing
  4401 token-refresh path are already wired (see `WS_AUTH_FAILURE_CODE` in
  `use-emergency-ws.ts`).
- Add **Server-Sent Events** for non-emergency feeds (device status changes,
  bell-log tail) when those features ship. SSE is half the resource cost of
  WS, gets through corporate proxies more reliably, and lets the browser's
  `EventSource` handle reconnect automatically.
- Do **not** use long-polling. On a fleet of >100 devices, the request rate
  will swamp Django even with async views.

This is a forward-looking note; no code change is required for the current
release.

## 5. Direct EMQX WebSocket to the browser

EMQX exposes MQTT-over-WebSocket on `ws://…:8083/mqtt` (or `wss://` on
8084). One could let the dashboard subscribe to `devices/+/status` directly.

### Why we are *not* doing it

- **Authorization.** Each user must only see devices in their organization.
  The backend already enforces this with DRF queryset filtering. EMQX ACL
  plugins can replicate it (HTTP ACL hook hitting the backend), but every
  browser session needs a short-lived MQTT password issued by the backend —
  a second auth path to maintain.
- **Audit.** Backend-as-bridge gives us a single chokepoint for logging and
  rate limiting. Direct browser→broker bypasses that.
- **Schema.** We translate raw MQTT JSON into the dashboard's typed
  `DeviceAlert` shape before forwarding. Direct subscription would require
  the schema to be stable and versioned on the wire.

### When to revisit

If the dashboard fan-out grows beyond ~5k concurrent users, the backend
WebSocket layer (Channels + Redis) will become the bottleneck. At that
point an EMQX-side bridge with HTTP ACL is worth the complexity.

**Verdict:** Keep the current `ESP32 → MQTT → backend → WS → browser` path.

## 6. Heartbeat interval optimisation

Today: `HEARTBEAT_INTERVAL_MS = 30000` (30 s, in `include/config.h`).

### Reality check

- Mobile NAT timeouts on Uzbekistan carriers are typically 5–10 minutes;
  30 s keep-alive is well under that.
- Each heartbeat is ~110 bytes payload + ~80 bytes MQTT/TLS framing. At
  30 s it costs about 540 KB/device/day — negligible on WiFi, **noticeable**
  on cellular.
- The device is mains-powered, so battery savings do not motivate longer
  intervals; quick offline detection does.

### Recommendation: adaptive heartbeat

```text
RSSI > −60 dBm AND last 3 heartbeats acked  → 60 s (relaxed)
−75 dBm < RSSI ≤ −60 dBm                    → 30 s (default, today)
RSSI ≤ −75 dBm OR recent reconnect          → 15 s (aggressive)
```

This halves bandwidth on healthy installs and tightens detection on flaky
ones. Implementation sketch (firmware):

- In `status_reporter.c`, add a state machine that picks the next sleep
  interval based on the last RSSI sample (we already include RSSI in the
  heartbeat payload).
- Cap the value at 60 s so the broker's 30-s keep-alive still fires PINGREQ
  in between.

**Effort:** ~30 LOC, no protocol change.

## 7. Message compression (CBOR / MessagePack)

Average payload sizes today (measured on `mqtt_client.c` snprintf buffers):

| Message | Approx JSON size |
| --- | --- |
| Heartbeat | 100–140 B |
| Schedule snapshot | 800–1500 B |
| Holiday snapshot | 200–600 B |
| Command (ring/reboot/...) | 60–120 B |

CBOR or MessagePack would shrink these by 30–50%. But:

- The ESP32 already runs cJSON. Adding CBOR (`tinycbor`) costs ~12 KB flash
  and a second serializer to maintain.
- The backend would need to encode/decode CBOR symmetrically.
- The dominant cost on the wire is **TLS framing** and the **MQTT publish
  acknowledgement round-trip**, not the payload bytes.

**Verdict:** Don't compress today. Worth it only if (a) we adopt
NB-IoT/LTE-M with per-byte billing, or (b) the schedule snapshot grows
beyond a few KB. Both are out of scope.

## 8. Offline-first sync protocol

Current behaviour:

- Device sets `clean_session=false` and a `client_id` derived from
  `device_id` (`mqtt_client.c::mqtt_client_init`). On reconnect, EMQX
  replays QoS 1 messages it queued while we were offline.
- On every connect, the device publishes a `sync` message containing its
  `schedule_version`, firmware/hardware versions, and a `stale` flag
  (`publish_sync_request`). The backend computes the delta and pushes a
  fresh schedule if needed.
- Schedules and holidays are persisted in NVS, so the bell continues to
  ring on the last-known schedule even with no broker reachable.

### Where it breaks down

- EMQX queues per-session messages but expires them after **2 hours** by
  default. After 7+ days offline, queued messages are gone — but our
  version-vector handshake on reconnect papers over this: the device tells
  the cloud what version it has, and the cloud sends the latest. So the
  queued-message loss is **harmless** as long as schedule/holiday topics
  are retained (they are).

### Recommendation

- **Document** the broker session-expiry behaviour in the runbook. Set
  `session_expiry_interval = 7d` on the EMQX listener if you also want
  queued QoS 1 messages to survive that long. (Optional — the version
  handshake already covers correctness.)
- **CRDT?** Not needed. We have one writer per topic per device (the
  backend), so the conflict surface is empty.

## 9. TLS performance

ESP32 + mbedTLS: a fresh handshake takes 2–3 s on a mid-range AP. With
session tickets it drops to ~600–900 ms.

Current `sdkconfig.defaults`:

```text
CONFIG_MQTT_TRANSPORT_SSL=y
CONFIG_ESP_TLS_USING_MBEDTLS=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y
```

`CONFIG_MBEDTLS_SSL_SESSION_TICKETS` is **not** explicitly set. The IDF
default is `y` for ESP32 with mbedTLS, but relying on defaults is a footgun
when the IDF version changes.

### Recommendation

Append to `sdkconfig.defaults`:

```
CONFIG_MBEDTLS_SSL_SESSION_TICKETS=y
CONFIG_MBEDTLS_CLIENT_SSL_SESSION_TICKETS=y
```

This guarantees session resumption is on regardless of IDF default churn.
Also keep `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y` for now;
slimming to `_DEFAULT_CMN` saves ~10 KB flash but breaks unfamiliar CAs.

## 10. Keep-alive tuning

MQTT keep-alive is `MQTT_KEEPALIVE_SEC = 30` (PINGREQ every 30 s). TCP
keep-alive uses IDF defaults (~7200 s idle, useless to us).

### What can go wrong

- ISP NAT idle timeout on cellular: 4–10 minutes typical, sometimes 1
  minute on aggressive carriers. 30 s PINGREQ is well under all of these.
- WiFi router idle timeout: rarely below 5 minutes.

### Recommendation

- Keep MQTT keep-alive at 30 s for the default profile.
- After 5 successful PINGRESPs in a row on a stable link (RSSI > −60),
  raise it to 60 s to halve the wakeup count. Drop back to 30 s on the
  first PINGREQ failure.
- Also set the ESP-IDF socket keep-alive options on the underlying TCP
  socket so a stale half-open connection is detected by the kernel inside
  90 s. This is already exposed via the MQTT client config:

  ```c
  cfg.network.disable_keepalive = false;
  cfg.network.keepalive = 30;          // seconds
  ```

  (We currently rely on defaults; making it explicit is a single-line fix.)

---

## Quick wins (config-only, ship next release)

1. **Pin TLS session tickets.** Add `CONFIG_MBEDTLS_SSL_SESSION_TICKETS=y`
   and `CONFIG_MBEDTLS_CLIENT_SSL_SESSION_TICKETS=y` to
   `sdkconfig.defaults`. — Cuts reconnect handshake from ~2–3 s to ~700 ms.
2. **Document the broker session expiry** (`session_expiry_interval=7d`) in
   the EMQX runbook.
3. **Make TCP keep-alive explicit** in `mqtt_client_init`
   (`cfg.network.disable_keepalive=false; cfg.network.keepalive=30;`).
4. **Guard against retained safety alerts** in the backend publisher: when
   we add `panic_alert` or `fire_alarm` upstream events, set retain=false
   (we already do for command, this is a pre-emptive note).

## Medium-term (firmware change, but scoped)

5. **Adaptive heartbeat** based on RSSI and recent reconnect history (~30
   LOC in `status_reporter.c`).
6. **SSE endpoint** for non-emergency feeds (`/api/v1/member/devices/stream/`),
   replacing client-side polling. Backend-only; browsers use `EventSource`.

## Long-term (only if requirements change)

7. **CBOR / MessagePack on the wire** if NB-IoT/LTE-M is adopted.
8. **Direct EMQX → browser bridge** if the dashboard surpasses ~5k
   concurrent users and Django Channels becomes a bottleneck.
9. **MQTT-SN or CoAP** if a non-WiFi radio (LoRa, ZigBee) is ever added.

## Appendix A — Measurement methodology

All payload sizes were read from the `snprintf` buffers in
`src/mqtt_client.c` and `src/status_reporter.c`. Latency numbers for TLS
handshake and reconnect are typical values for ESP32 WROOM-32E on 2.4 GHz
WiFi reported in the `esp-mqtt` examples and matched against logs from our
own bench device.

## Appendix B — Tested message flow (today)

```
device boots
 └── NVS load (schedule, holidays, mqtt creds)
 └── WiFi STA connect (exponential backoff, see WIFI_BACKOFF_*)
 └── SNTP sync (or RTC fallback)
 └── MQTT connect (clean_session=false, LWT armed)
      ├── publish retained "online" on devices/{id}/status
      ├── subscribe command/schedule/config/holidays at QoS 1
      └── publish sync request on devices/{id}/sync
            (schedule_version, fw, hw, stale flag)
backend
 └── on `sync`, compares versions
 └── publishes delta on devices/{id}/schedule (retained, QoS 1)
device
 └── persist new schedule to NVS, bump version
 └── ACK command msg_ids on devices/{id}/ack (QoS 1)
```

This flow already implements an offline-first, version-vector sync without
any new protocol. The remaining work is the small list of quick wins above.
