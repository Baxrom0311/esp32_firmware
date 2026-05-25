# ESP32 Firmware Test Checklist

Manual testing checklist for each milestone. Use serial monitor (`idf.py monitor`) to verify.

## Milestone 1: Holidays + Silent Mode

- [ ] Device boots, loads holidays from NVS (CRC validated)
- [ ] MQTT `devices/{id}/holidays` → holidays saved to NVS with CRC (check log: "Updated N holidays")
- [ ] On a holiday date, `schedule_manager_tick()` skips bell (log: "Today is holiday, skipping")
- [ ] MQTT `{"command": "silent", "state": true}` → silent flag set, bells muted
- [ ] MQTT `{"command": "silent", "state": false}` → silent cleared, bells resume
- [ ] After reboot, silent flag persists from NVS

## Milestone 2: Bell Log + Monitoring

- [ ] When bell rings (schedule or remote), MQTT publish to `devices/{id}/bell_log`
- [ ] Payload format: `{"time": "HH:MM", "duration": 3000, "source": "schedule|mqtt|emergency"}`
- [ ] Heartbeat includes `rssi`, `uptime`, `heap`, `relay2`, `silent`, `sched_ver` fields
- [ ] Heartbeat interval matches `HEARTBEAT_INTERVAL_MS` config (30000ms = 30s)

## Milestone 3: Emergency Commands

- [ ] MQTT `{"command": "ring", "duration": 30}` → relay ON for 30 seconds
- [ ] MQTT `{"command": "stop"}` → cancels ongoing ring immediately (bell OFF within 100ms)
- [ ] MQTT `{"command": "lockdown", "state": true}` → relay 2 (GPIO 14) ON, persists in NVS, publishes alert
- [ ] MQTT `{"command": "lockdown", "state": false}` → relay 2 OFF, publishes alert
- [ ] MQTT `{"command": "cancel_emergency"}` → stops bell + releases relay2 + publishes ack + bell_log
- [ ] After reboot, lockdown state restored from NVS
- [ ] Panic button (GPIO 0) press → MQTT alert published FIRST, then local bell rings 5s
- [ ] Panic button cooldown: 10s between triggers (rapid presses ignored after first)
- [ ] Panic button works offline (bell rings even without MQTT connection)
- [ ] Panic button debounce (200ms in ISR): rapid presses don't send multiple alerts
- [ ] Panic button fallback: if bell mutex stuck, GPIO forced directly with WDT reset
- [ ] Relay2 auto-release after 5 minutes (with alert notification)
- [ ] Silent mode auto-clear after 24 hours (with status notification)

## Milestone 4: Schedule Sync

- [ ] MQTT `devices/{id}/schedule` with entries → schedule saved to NVS
- [ ] CRC32 mismatch → schedule discarded and erased
- [ ] Version field tracked (from JSON `"version"` or auto-increment), persisted in NVS
- [ ] Version reported in heartbeat as `sched_ver`
- [ ] After reboot, schedule loaded from NVS correctly with version

## Milestone 5: BLE Provisioning

- [ ] BLE advertises on first boot (no WiFi credentials)
- [ ] Mobile app connects, sends WiFi SSID+password
- [ ] Device connects to WiFi, stops BLE advertising
- [ ] After reboot with valid credentials, BLE does not start

## Hardware Pin Verification

| Pin | Function | Test |
|-----|----------|------|
| GPIO 13 | Relay 1 (bell) | Ring command → relay clicks |
| GPIO 14 | Relay 2 (lockdown) | Lockdown command → relay clicks |
| GPIO 0 | Panic button | Press → bell rings 5s + alert published (200ms ISR debounce) |

## Serial Monitor Commands

```bash
# Monitor output
idf.py monitor

# Expected boot log
I (xxx) holiday_mgr: Loaded N holidays (CRC OK), silent=0
I (xxx) schedule: Loaded N schedule entries from NVS (CRC OK), version=X
I (xxx) mqtt: MQTT connected
I (xxx) mqtt: Subscribed to device topics
```
