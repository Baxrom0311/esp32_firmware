# ESP32 School Bell Firmware

ESP32-based school bell controller firmware using ESP-IDF framework via PlatformIO.

## Features

- **WiFi Manager** — Connects using NVS-stored credentials, auto-reconnects
- **MQTT Client** — TLS connection to EMQX broker, subscribes to command/schedule/config topics
- **Schedule Manager** — Stores bell schedule in NVS, triggers GPIO at configured times
- **Bell Controller** — GPIO relay/buzzer control with configurable duration
- **OTA Updates** — Receives OTA command via MQTT, downloads firmware over HTTPS
- **Device Registration** — Auto-registers with backend API on first boot
- **Status Reporter** — Publishes heartbeat (RSSI, uptime, firmware version) every 30s
- **Offline Mode** — Operates using NVS-cached schedule when WiFi is unavailable

## Architecture

```
app_main()
  ├── nvs_storage_init()        # Initialize NVS flash
  ├── bell_controller_init()    # Configure GPIO output
  ├── wifi_manager_init()       # Connect to WiFi (NVS or defaults)
  ├── schedule_manager_init()   # Load schedule from NVS
  ├── device_registration       # Register with API if first boot
  ├── mqtt_client_init()        # Connect to MQTT broker
  └── main loop (1s tick)
        ├── schedule_manager_tick()   # Check time, ring bell
        └── status_reporter_tick()    # Send heartbeat every 30s
```

## MQTT Topics

| Topic | Direction | Purpose |
|-------|-----------|---------|
| `devices/{id}/command` | Server → Device | Commands: ring, ota, reboot |
| `devices/{id}/schedule` | Server → Device | Schedule JSON payload |
| `devices/{id}/config` | Server → Device | Runtime configuration |
| `devices/{id}/status` | Device → Server | Heartbeat with RSSI/uptime |
| `devices/{id}/ota/status` | Device → Server | OTA progress reporting |

## Build

```bash
# Install PlatformIO CLI
pip install platformio

# Build firmware
cd esp32_firmware
pio run

# Flash to device
pio run -t upload

# Monitor serial output
pio device monitor
```

## Configuration

Edit `include/config.h` before building:

| Define | Description | Default |
|--------|-------------|---------|
| `DEFAULT_WIFI_SSID` | WiFi SSID (fallback) | `SchoolDevice` |
| `DEFAULT_WIFI_PASS` | WiFi password (fallback) | `changeme123` |
| `MQTT_BROKER_URI` | MQTT broker URI | `mqtts://mqtt.example.com:8883` |
| `API_BASE_URL` | Backend API URL | `https://api.example.com` |
| `BELL_GPIO_PIN` | Relay/buzzer GPIO | `GPIO_NUM_25` |
| `HEARTBEAT_INTERVAL_MS` | Status report interval | `30000` |

WiFi credentials can also be stored in NVS (namespace: `wifi`, keys: `ssid`, `pass`).

## Partition Table

Uses custom partition layout (`partitions.csv`) with dual OTA partitions:

| Name | Type | Size |
|------|------|------|
| nvs | data | 16KB |
| otadata | data | 8KB |
| phy_init | data | 4KB |
| ota_0 | app | 1.5MB |
| ota_1 | app | 1.5MB |

## Device Registration Flow

1. First boot: no device ID in NVS
2. Reads MAC address as unique identifier
3. POSTs to `/api/v1/devices/auto-register/` with MAC + firmware version
4. If approved: receives MQTT credentials, stores in NVS
5. If pending: retries on next boot
6. Subsequent boots: loads credentials from NVS, connects directly

## Command Format

```json
{"command": "ring", "duration": 5}
{"command": "ota", "url": "https://server/firmware/v1.2.3.bin"}
{"command": "reboot"}
```

## Schedule Format

```json
{
  "entries": [
    {"hour": 8, "minute": 0, "duration": 3000, "days": 31},
    {"hour": 8, "minute": 45, "duration": 3000, "days": 31}
  ]
}
```

`days` is a bitmask: bit0=Monday ... bit6=Sunday. `31` = Mon-Fri.

## Hardware

- **Board:** ESP32-WROOM-32 (4MB flash)
- **Bell output:** GPIO 25 → relay module → bell/buzzer
- **Power:** 5V USB or external supply
