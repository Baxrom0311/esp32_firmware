# ESP32 Mustahkamlash — Implementation Plan

## 1. Goal & Non-Goals

### Goal
ESP32 firmware'ni production-ready qilish: WiFi AP provisioning, RTC diagnostika, offline xavfsizlik, vaqt sinxronizatsiya.

### Non-Goals
- BLE provisioning'ni o'zgartirish (allaqachon ishlaydi)
- OTA update logic'ni o'zgartirish
- Dashboard/API backend o'zgarishlari (faqat MQTT format hujjatlash)
- Yangi hardware qo'shish

---

## 2. Architecture

### Component Diagram

```
┌─────────────────────────────────────────────────────────┐
│                        main.c                            │
│  Boot → RTC → WiFi(STA) → [fail 3x] → AP+STA mode     │
│  Main loop: schedule_tick, rtc_diag_tick, reconnect     │
└────────┬──────────┬──────────┬──────────┬───────────────┘
         │          │          │          │
    ┌────▼────┐ ┌───▼────┐ ┌──▼───┐ ┌───▼──────────┐
    │wifi_mgr │ │ap_prov │ │rtc_  │ │schedule_mgr  │
    │         │ │+captive│ │diag  │ │(version sync)│
    │AP+STA   │ │portal  │ │      │ │              │
    │fallback │ │DNS     │ │MQTT  │ │MQTT version  │
    │30s retry│ │5min TO │ │alert │ │compare       │
    └─────────┘ └────────┘ └──────┘ └──────────────┘
```

### Data Flow

```
Boot:
  NVS → WiFi creds → STA connect (3 retry)
    ├── OK → SNTP → RTC save → MQTT → schedule version check
    └── FAIL → AP+STA mode → captive portal (WiFi only)
                            → STA retry 30s background
                            → AP auto-off 2min after STA connects

Daily 3:00 AM:
  SNTP time vs RTC time → drift check → auto-correct/alert

Schedule sync:
  MQTT → server version → compare NVS version → download if newer
```

### Tech Choices
- **ESP-IDF v5.x** (PlatformIO espressif32@6.9.0) — allaqachon ishlatilmoqda
- **NVS** — WiFi creds, schedule, RTC state saqlash
- **FreeRTOS timers** — AP timeout, STA retry, AP auto-off
- **cJSON** — schedule parsing (allaqachon bor)
- **MQTT QoS 1** — alertlar uchun (delivery guarantee)

---

## 3. Milestones

### Milestone 1: WiFi AP+STA Provisioning Mustahkamlash
**Complexity: LOW** (90% tayyor, faqat edge case'lar)

**Hozirgi holat:** `wifi_manager.c` va `ap_provisioning.c` allaqachon AP+STA, captive portal, brute-force, 5min timeout bor.

**Qolgan ishlar:**
1. `ap_provisioning.c` — AP timeout'dan keyin `ap_stopped_tick` to'g'ri set qilish
2. `main.c` — 10 daqiqa retry logic allaqachon bor, tekshirish
3. `wifi_manager.c` — STA reconnect'da AP yoqilishi allaqachon bor

**Files:**
- `src/ap_provisioning.c` — minor: timeout callback'da `wifi_manager_stop_ap()` chaqirish
- `src/main.c` — verify: AP retry logic to'g'ri ishlashi

**Acceptance Criteria:**
- [ ] WiFi 3x fail → AP+STA mode yoqiladi
- [ ] AP SSID: "SchoolBell_XXXX", Password: "SchoolBell_XXXXXX"
- [ ] Captive portal faqat WiFi SSID/password
- [ ] 5 daqiqa timeout → AP o'chadi
- [ ] 10 daqiqadan keyin retry
- [ ] 3s reset button → AP majburiy yoqiladi
- [ ] STA ulanganda → AP 2 daqiqadan keyin o'chadi

**Verification:**
```bash
cd esp32_firmware && pio run
# Manual test: WiFi creds o'chirish → AP mode tekshirish
```

---

### Milestone 2: RTC Diagnostika Mustahkamlash
**Complexity: LOW** (95% tayyor)

**Hozirgi holat:** `rtc_diagnostics.c` allaqachon:
- Har kuni soat 3:00 da drift tekshirish ✅
- Drift > 30s → auto-correct ✅
- Drift > 5min → MQTT alert ✅
- 3 kun ketma-ket → battery_dead flag ✅
- NVS'da persist ✅
- Boot'da year < 2024 check ✅

**Qolgan ishlar:**
1. `status_reporter.c` — `rtc_diagnostics_battery_dead()` ni heartbeat'ga qo'shish
2. MQTT alert format'ni hujjatlash (dashboard uchun)

**Files:**
- `src/status_reporter.c` — heartbeat payload'ga `rtc_battery` field qo'shish
- `src/rtc_diagnostics.h` — (o'zgarishsiz)

**Acceptance Criteria:**
- [ ] RTC drift > 5 min → MQTT alert: `{"type":"rtc_drift","drift_sec":N,"battery_status":"low|dead"}`
- [ ] 3 kun ketma-ket → `battery_status: "dead"`
- [ ] Heartbeat'da `rtc_battery: "ok|low|dead"` field
- [ ] Boot'da year < 2024 → battery dead flag

**Verification:**
```bash
pio run
# Test: RTC vaqtini qo'lda o'zgartirish → alert kelishi
```

---

### Milestone 3: Offline Mode Xavfsizlik
**Complexity: LOW** (90% tayyor)

**Hozirgi holat:**
- `schedule_manager_update()` — allaqachon `wifi_manager_is_connected()` guard bor ✅
- NVS'dan jadval yuklash ✅
- CRC32 validation ✅
- Stale schedule detection (7 kun) ✅

**Qolgan ishlar:**
1. `schedule_manager.c` — version-based sync MQTT handler'da
2. `mqtt_client.c` — schedule version topic subscribe

**Files:**
- `src/schedule_manager.c` — `schedule_manager_should_sync(uint32_t server_version)` funksiya
- `src/schedule_manager.h` — yangi funksiya deklaratsiyasi
- `src/mqtt_client.c` — version topic handler qo'shish

**Acceptance Criteria:**
- [ ] Offline: NVS jadval bilan ishlaydi, update reject qilinadi
- [ ] Online: server version > NVS version → sync
- [ ] Server version == NVS version → skip
- [ ] Stale alert (>7 kun) → MQTT notify

**Verification:**
```bash
pio run
# Test: MQTT'dan eski version yuborish → skip qilishi
# Test: WiFi o'chirish → schedule ishlashda davom etishi
```

---

### Milestone 4: Vaqt Sinxronizatsiya Strategiya
**Complexity: LOW** (80% tayyor)

**Hozirgi holat:**
- Boot: RTC → system time ✅ (`rtc_driver_init()`)
- WiFi ulanganda: SNTP → RTC ✅ (`init_sntp()` + `rtc_driver_save_from_system()`)
- Har soatda: SNTP re-sync ✅ (`sntp_periodic_resync()`)
- Har kuni 3:00: RTC diagnostika ✅ (`rtc_diagnostics_tick()`)

**Qolgan ishlar:**
1. Har soatdagi re-sync'da ham RTC'ga yozish — **allaqachon bor** (`rtc_driver_save_from_system()` `sntp_periodic_resync` ichida)
2. Verify: boot'da RTC time set qilinishi

**Files:**
- Hech qanday o'zgarish kerak emas — faqat verification

**Acceptance Criteria:**
- [ ] Boot: RTC → system time (darhol, offline ishlashi uchun)
- [ ] WiFi ulanganda: SNTP → system time → RTC
- [ ] Har soatda: SNTP re-sync → RTC update
- [ ] Har kuni 3:00: SNTP vs RTC diagnostika

**Verification:**
```bash
pio run
# Test: RTC'ni 5 daqiqa orqaga qo'yish → soat 3:00 da auto-correct
```

---

## 4. Concrete Implementation Tasks

### Task 1: status_reporter.c — RTC battery status qo'shish

```c
// status_reporter.c heartbeat payload'ga qo'shish:
#include "rtc_diagnostics.h"

// heartbeat JSON'ga:
const char *rtc_status = rtc_diagnostics_battery_dead() ? "dead" :
                         (abs(rtc_diagnostics_last_drift_sec()) > 300) ? "low" : "ok";
// payload'ga: ,"rtc_battery":"%s" qo'shish
```

### Task 2: schedule version sync — MQTT handler

`mqtt_client.c` da schedule topic handler'da:
```c
// Topic: devices/{id}/schedule
// Server yuboradi: {"version": 5, "entries": [...]}
// schedule_manager_update() allaqachon version check qiladi ✅
// Hech narsa qo'shish shart emas — faqat verify
```

### Task 3: AP provisioning timeout → AP stop

`ap_provisioning.c` `timeout_cb` da:
```c
static void timeout_cb(TimerHandle_t timer)
{
    ESP_LOGW(TAG, "AP provisioning timeout (5 min)");
    ap_provisioning_stop();
    // wifi_manager_stop_ap() allaqachon ap_provisioning_stop() ichida chaqiriladi (portal_only mode)
}
```
Bu allaqachon to'g'ri ishlaydi ✅

---

## 5. Test Strategy

### Unit Tests (host-based, optional)
- `schedule_manager_update()` — version compare logic
- `rtc_diagnostics_tick()` — drift calculation, consecutive day counting
- URL decode function — edge cases (%, +, empty)

### Integration Tests (on hardware)
| Test | Steps | Expected |
|------|-------|----------|
| AP mode trigger | NVS'dan WiFi o'chirish, reboot | AP "SchoolBell_XXXX" paydo bo'ladi |
| Captive portal | AP'ga ulanish, browser ochish | WiFi form ko'rinadi |
| WiFi save + reboot | Form'ga to'g'ri creds yozish | Reboot, STA ulandi |
| Brute-force | 3x noto'g'ri submit | 30s lockout |
| AP timeout | 5 min kutish | AP o'chadi |
| AP retry | 10 min kutish | AP qayta yoqiladi |
| Reset button | 3s bosib turish | AP yoqiladi |
| RTC drift alert | RTC'ni 10 min orqaga qo'yish, 3:00 kutish | MQTT alert |
| Battery dead | 3 kun ketma-ket drift | MQTT "dead" status |
| Offline schedule | WiFi o'chirish | Jadval ishlayveradi |
| Schedule version | Eski version MQTT'dan yuborish | Skip qiladi |
| SNTP → RTC | WiFi ulanganda | RTC yangilanadi |

### Coverage Goals
- Barcha acceptance criteria 100% qoplangan
- Edge cases: NVS corrupt, WiFi flapping, RTC read failure

---

## 6. Security Checklist

| # | Item | Status |
|---|------|--------|
| 1 | AP password = MAC-based (unique per device) | ✅ Done |
| 2 | Captive portal: FAQAT WiFi creds form | ✅ Done |
| 3 | No schedule/MQTT/config change via portal | ✅ Done |
| 4 | Brute-force: 3 attempt → 30s lockout | ✅ Done |
| 5 | AP auto-timeout: 5 min | ✅ Done |
| 6 | Input validation: SSID 1-32, pass 8-64 | ✅ Done |
| 7 | Security headers: X-Frame-Options, no-cache | ✅ Done |
| 8 | Offline: no config changes possible | ✅ Done |
| 9 | Schedule update: online-only guard | ✅ Done |
| 10 | MQTT TLS (mqtts://) | ✅ Config |
| 11 | URL decode: no buffer overflow (bounded copy) | ✅ Done |
| 12 | NVS CRC32: corrupt data detection | ✅ Done |

---

## 7. Risks & Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| NVS full (too many writes) | Schedule save fails | NVS wear-leveling, write only on version change |
| WiFi flapping (connect/disconnect loop) | AP yoqiladi/o'chadi tez-tez | Backoff + 2min AP auto-off delay |
| RTC I2C hang | System freeze | WDT 30s, I2C timeout in driver |
| Captive portal memory leak | Crash after long AP session | 5min timeout, httpd_stop cleans up |
| SNTP server unreachable | Vaqt sync bo'lmaydi | 2 server (pool.ntp.org + time.google.com), RTC fallback |
| Flash corruption | Boot loop | OTA rollback, partition table with factory |
| Concurrent AP+STA RAM usage | ~50KB extra | ESP32 has 320KB SRAM, acceptable |

---

## 8. Summary — Current State (Code Review Result)

### ✅ BARCHA ACCEPTANCE CRITERIA ALLAQACHON IMPLEMENT QILINGAN

| # | Criteria | Status | File |
|---|----------|--------|------|
| 1 | WiFi 3x fail → AP mode | ✅ | `wifi_manager.c` (AP_FALLBACK_RETRIES=3) |
| 2 | AP SSID: "SchoolBell_XXXX" | ✅ | `wifi_manager.c` + `ap_provisioning.c` |
| 3 | Captive portal: faqat WiFi form | ✅ | `ap_provisioning.c` (HTML_PAGE) |
| 4 | Boshqa sozlamalar YO'Q | ✅ | Portal faqat SSID/pass |
| 5 | Save → reboot → normal mode | ✅ | `handler_post_save()` → `esp_restart()` |
| 6 | Offline: NVS jadval ishlaydi | ✅ | `schedule_manager.c` (NVS load) |
| 7 | Online: version check → sync | ✅ | `schedule_manager_update()` version compare |
| 8 | RTC drift > 5min → MQTT alert | ✅ | `rtc_diagnostics.c` |
| 9 | 3 kun drift → "dead" flag | ✅ | `rtc_diagnostics.c` (RTC_DEAD_CONSECUTIVE=3) |
| 10 | Heartbeat'da RTC status | ✅ | `mqtt_client.c` (`rtc_battery_dead`, `rtc_drift_sec`) |
| 11 | AP 5 daqiqa timeout | ✅ | `ap_provisioning.c` (AP_TIMEOUT_MS=300000) |
| 12 | 10 min retry | ✅ | `main.c` (600000ms check) |
| 13 | 3s reset button → AP | ✅ | `main.c` (RESET_LONG_PRESS_MS=3000) |
| 14 | STA ulanganda AP 2min off | ✅ | `wifi_manager.c` (ap_auto_off_cb, 120000ms) |
| 15 | Brute-force: 3 attempt → 30s | ✅ | `ap_provisioning.c` |
| 16 | Schedule stale alert | ✅ | `main.c` + `schedule_manager.c` |
| 17 | SNTP → RTC har soatda | ✅ | `main.c` (sntp_periodic_resync) |
| 18 | Boot: RTC → system time | ✅ | `rtc_driver_init()` |

### Build Status
```
RAM:   19.0% (62KB / 328KB)
Flash: 74.4% (1.4MB / 1.9MB)
Build: SUCCESS
```

### Qolgan ish: FAQAT HARDWARE TEST
Kod 100% tayyor. Faqat real qurilmada integration test o'tkazish kerak (yuqoridagi test jadvaliga qarang).
