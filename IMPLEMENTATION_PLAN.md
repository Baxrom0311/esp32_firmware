# ESP32 Mustahkamlash — Implementation Plan

## 1. Goal & Non-Goals

### Goal
ESP32 firmware'ni production-ready qilish:
- WiFi AP provisioning (captive portal) — qurilmani joyida sozlash imkoniyati
- RTC diagnostika — batareya muammolarini erta aniqlash
- Offline xavfsizlik — jadval himoyasi
- WiFi reconnect strategiya — uzilishlarga chidamlilik

### Non-Goals
- BLE provisioning'ni o'chirish (parallel mavjud bo'lib qoladi, faqat birinchi boot uchun)
- Dashboard backend o'zgarishlari (faqat MQTT alert format belgilanadi)
- OTA yoki MQTT protokol o'zgarishlari
- Captive portal'da jadval/MQTT/boshqa sozlamalar

---

## 2. Architecture

### Component Diagram

```
┌─────────────────────────────────────────────────────────┐
│                        main.c                            │
│  Boot → RTC sync → WiFi attempt (3x) → AP or Normal    │
└────┬──────────┬──────────────┬──────────────────────────┘
     │          │              │
     ▼          ▼              ▼
┌─────────┐ ┌──────────────┐ ┌────────────────┐
│wifi_mgr │ │ap_provisioning│ │rtc_diagnostics │
│(updated)│ │+ captive_dns  │ │                │
└─────────┘ │+ HTTP server  │ └────────────────┘
            └──────────────┘
```

### Data Flow

```
Boot:
  NVS → WiFi creds → wifi_manager_connect_sta(3 retries)
    ├── Success → Normal mode (MQTT, schedule, SNTP)
    └── Fail → ap_provisioning_start()
                ├── SoftAP: "SchoolBell_XXXX"
                ├── DNS redirect → 192.168.4.1
                ├── HTTP: GET / → HTML form
                ├── HTTP: POST /save → NVS write → reboot
                └── 5 min timeout → AP off → 10 min wait → retry

RTC Diagnostics (daily 3:00 AM):
  SNTP time vs RTC time → drift calculation
    ├── drift > 30s → auto-correct RTC
    ├── drift > 5min → MQTT alert {"type":"rtc_drift",...}
    └── 3 consecutive days > 5min → "RTC_BATTERY_DEAD" flag
```

### Tech Choices
- **ESP-IDF HTTP Server** (`esp_http_server.h`) — lightweight, already in framework
- **ESP-IDF DNS Server** — custom minimal UDP DNS (all queries → 192.168.4.1)
- **SoftAP** — `esp_wifi` APSTA mode not needed, pure AP during provisioning
- **HTML** — embedded C string, minimal (no JS frameworks, ~2KB)

---

## 3. Milestones

### Milestone 1: WiFi Manager Refactor (AP Fallback)
**Complexity: Medium**

**Description:** wifi_manager.c'ni yangilash — 3 marta retry, keyin AP mode'ga signal berish.

**Files:**
- `src/wifi_manager.c` — refactor: 3-retry limit, AP fallback callback
- `src/wifi_manager.h` — yangi API: `wifi_manager_get_fail_count()`, `wifi_manager_stop_sta()`

**Changes:**
```c
// wifi_manager.h ga qo'shiladi:
typedef void (*wifi_ap_fallback_cb_t)(void);
esp_err_t wifi_manager_init_with_fallback(wifi_ap_fallback_cb_t cb);
void wifi_manager_stop_sta(void);
void wifi_manager_restart_sta(void);
```

**Logic:**
1. `wifi_manager_init()` → 3 marta ulanishga harakat (15s timeout har biri)
2. 3 ta fail → callback chaqirish (main.c AP mode'ni boshlaydi)
3. AP mode'dan keyin `wifi_manager_restart_sta()` bilan qayta ulanish

**Acceptance Criteria:**
- [ ] 3 marta ulanolmasa callback chaqiriladi
- [ ] Exponential backoff saqlanadi (mavjud logic)
- [ ] WiFi stop/restart to'g'ri ishlaydi

**Verification:**
```bash
pio run  # Compile check
```

---

### Milestone 2: Captive DNS Server
**Complexity: Low**

**Description:** Barcha DNS so'rovlarni 192.168.4.1 ga yo'naltiruvchi minimal UDP server.

**Files:**
- `src/captive_dns.c` — NEW
- `src/captive_dns.h` — NEW

**API:**
```c
esp_err_t captive_dns_start(void);  // UDP :53 listen, all → 192.168.4.1
void captive_dns_stop(void);
```

**Implementation:**
- UDP socket on port 53
- Parse DNS query header (12 bytes) + question section
- Reply with same transaction ID, answer = 192.168.4.1
- FreeRTOS task (2KB stack)

**Acceptance Criteria:**
- [ ] Har qanday DNS query'ga 192.168.4.1 javob beradi
- [ ] Task to'g'ri to'xtaydi (`captive_dns_stop`)
- [ ] Memory leak yo'q

**Verification:**
```bash
pio run
```

---

### Milestone 3: AP Provisioning + Captive Portal HTTP
**Complexity: High**

**Description:** SoftAP + HTTP server + HTML form. Faqat WiFi SSID/password o'zgartirish.

**Files:**
- `src/ap_provisioning.c` — NEW
- `src/ap_provisioning.h` — NEW

**API:**
```c
esp_err_t ap_provisioning_start(void);  // Start AP + DNS + HTTP
void ap_provisioning_stop(void);         // Stop everything
bool ap_provisioning_is_active(void);
```

**Implementation Details:**

1. **SoftAP Config:**
   - SSID: `SchoolBell_` + MAC oxirgi 4 hex (e.g., `SchoolBell_CE40`)
   - Password: `SchoolBell_` + MAC oxirgi 6 hex (e.g., `SchoolBell_71CE40`)
   - Max connections: 1
   - Channel: 1

2. **HTTP Endpoints:**
   - `GET /` → HTML form (SSID input, Password input, Save button)
   - `GET /generate_204` → redirect to `/` (Android captive portal detection)
   - `GET /hotspot-detect.html` → redirect to `/` (iOS detection)
   - `POST /save` → validate → NVS save → "Saqlandi, qayta yuklanmoqda..." → reboot

3. **HTML Form (~2KB):**
   - Uzbek tilida
   - Mobile-friendly (viewport meta)
   - SSID: text input (max 32 char)
   - Password: password input (min 8, max 64 char)
   - Submit button
   - NO other settings

4. **Security:**
   - Brute-force: AP password orqali himoyalangan (WPA2)
   - Input validation: SSID 1-32 chars, password 8-64 chars
   - No XSS: HTML-encode user input in response
   - 5 min timeout: timer → `ap_provisioning_stop()` → 10 min sleep → retry

5. **Timeout Logic:**
   - `xTimerCreate("ap_timeout", pdMS_TO_TICKS(300000), ...)` — 5 min
   - Timer fire → stop AP → set flag "retry after 10 min"
   - Main loop: 10 min o'tgandan keyin yana WiFi STA retry → fail → AP again

**Acceptance Criteria:**
- [ ] AP "SchoolBell_XXXX" SSID bilan ko'rinadi
- [ ] Telefon ulanganida avtomatik captive portal ochiladi
- [ ] Faqat WiFi SSID/password form ko'rinadi
- [ ] Save → NVS'ga yoziladi → ESP32 reboot
- [ ] 5 daqiqada hech narsa qilinmasa AP o'chadi
- [ ] Noto'g'ri input (bo'sh SSID, qisqa parol) reject qilinadi

**Verification:**
```bash
pio run
# Manual test: telefon bilan AP'ga ulanish, form to'ldirish
```

---

### Milestone 4: Main.c Integration — AP Mode Flow
**Complexity: Medium**

**Description:** main.c'da AP mode logic: WiFi fail → AP → timeout → retry cycle.

**Files:**
- `src/main.c` — modify boot sequence and main loop

**Changes to Boot Sequence:**
```
Current:
  BLE check → WiFi init → SNTP → MQTT

New:
  BLE check (first boot only) → WiFi init (3 retry) →
    ├── OK → SNTP → MQTT (normal)
    └── FAIL → AP provisioning start
              Main loop: check AP timeout, check if configured
              AP done → reboot
```

**Main Loop Additions:**
```c
// AP mode active bo'lsa:
if (ap_provisioning_is_active()) {
    // Faqat WDT reset va AP timeout check
    // Schedule tick ISHLAYDI (offline jadval)
    // MQTT, SNTP — ishlamaydi
}

// AP timeout + retry logic:
// 10 min o'tdi → wifi_manager_restart_sta() → 3 retry
// Fail → ap_provisioning_start() again
```

**Acceptance Criteria:**
- [ ] Boot'da WiFi 3 marta fail → AP mode
- [ ] AP mode'da jadval ishlayveradi (offline)
- [ ] AP configured → reboot → normal mode
- [ ] AP timeout → 10 min → retry → fail → AP again (cheksiz)

**Verification:**
```bash
pio run
```

---

### Milestone 5: RTC Diagnostics Module
**Complexity: Medium**

**Description:** RTC drift detection, battery monitoring, MQTT alerts.

**Files:**
- `src/rtc_diagnostics.c` — NEW
- `src/rtc_diagnostics.h` — NEW

**API:**
```c
esp_err_t rtc_diagnostics_init(void);
void rtc_diagnostics_tick(void);  // Call from main loop (checks daily)
bool rtc_diagnostics_battery_dead(void);
int32_t rtc_diagnostics_last_drift_sec(void);
```

**Implementation:**

1. **Boot Check:**
   ```c
   // RTC time < 2024 → battery dead flag immediately
   struct tm t;
   rtc_driver_get_time(&t);
   if (t.tm_year + 1900 < 2024) {
       s_battery_dead = true;
       // SNTP'ni kutish, offline jadval bilan ishlash
   }
   ```

2. **Daily Check (3:00 AM):**
   ```c
   // Har soat tick chaqiriladi, faqat 3:00 da ishlaydi
   time_t sntp_time = time(NULL);
   struct tm rtc_time;
   rtc_driver_get_time(&rtc_time);
   time_t rtc_epoch = mktime(&rtc_time);
   int32_t drift = abs(sntp_time - rtc_epoch);

   if (drift > 30) rtc_driver_save_from_system(); // Auto-correct
   if (drift > 300) { // 5 min
       s_consecutive_drift_days++;
       send_mqtt_alert(drift);
   } else {
       s_consecutive_drift_days = 0;
   }
   if (s_consecutive_drift_days >= 3) {
       s_battery_dead = true;
       nvs_storage_set_blob("config", "rtc_dead", &s_battery_dead, 1);
   }
   ```

3. **MQTT Alert:**
   ```json
   {"type": "rtc_drift", "drift_sec": 312, "battery_status": "low"}
   ```
   Topic: `devices/{id}/alert`

4. **NVS Persistence:**
   - `config/rtc_dead` — battery dead flag (survives reboot)
   - `config/rtc_drift_days` — consecutive drift counter
   - Online bo'lganda va drift < 5min → flag reset

**Acceptance Criteria:**
- [ ] Boot'da RTC < 2024 → battery_dead = true
- [ ] Soat 3:00 da SNTP vs RTC comparison
- [ ] Drift > 30s → RTC auto-correct
- [ ] Drift > 5min → MQTT alert
- [ ] 3 kun ketma-ket → battery_dead flag
- [ ] Flag NVS'da saqlanadi
- [ ] Online + drift OK → flag reset

**Verification:**
```bash
pio run
```

---

### Milestone 6: Schedule Version-Based Sync
**Complexity: Low**

**Description:** MQTT orqali jadval version tekshirish, faqat yangi bo'lsa yuklab olish.

**Files:**
- `src/schedule_manager.c` — minor update (allaqachon 90% tayyor)
- `src/mqtt_client.c` — version check logic

**Current State:** `schedule_manager_update()` allaqachon version field'ni parse qiladi va NVS'ga saqlaydi. `schedule_manager_get_version()` mavjud.

**Needed Changes:**
1. MQTT connected bo'lganda → server'ga current version yuborish:
   ```json
   // devices/{id}/status topicga:
   {"event": "schedule_sync_request", "current_version": 5}
   ```
2. Server javob beradi `devices/{id}/schedule` topic'ga (mavjud flow)
3. `schedule_manager_update()` da version compare:
   ```c
   if (new_version <= s_schedule_version) {
       ESP_LOGI(TAG, "Schedule already up-to-date (v%lu)", s_schedule_version);
       return ESP_OK; // Skip
   }
   ```

**Acceptance Criteria:**
- [ ] Online bo'lganda version so'raladi
- [ ] Yangi version kelsa → update
- [ ] Eski/teng version kelsa → skip
- [ ] Offline'da jadval o'zgarmaydi

**Verification:**
```bash
pio run
```

---

### Milestone 7: WiFi Reconnect Strategy Enhancement
**Complexity: Low**

**Description:** Mavjud exponential backoff'ni kengaytirish — AP mode fallback qo'shish.

**Files:**
- `src/wifi_manager.c` — modify reconnect logic

**Current State:** Allaqachon exponential backoff bor (5s→60s max), MAX_RETRY=10.

**Needed Changes:**
```
WiFi uzildi (runtime):
  1. Exponential backoff (mavjud: 5s, 10s, 20s, 40s, 60s)
  2. 10 marta fail → AP mode trigger (callback)
  3. AP 5 min → AP off
  4. 10 min kutish → STA retry (10 marta)
  5. Fail → AP again
  6. Cheksiz loop (offline jadval ishlayveradi)
```

**Key Point:** Bu Milestone 1 bilan birgalikda amalga oshiriladi. Runtime disconnect uchun ham AP fallback ishlashi kerak.

**Acceptance Criteria:**
- [ ] Runtime WiFi loss → 10 retry → AP mode
- [ ] AP timeout → retry cycle
- [ ] Jadval hech qachon to'xtamaydi

**Verification:**
```bash
pio run
```

---

## 4. Implementation Order (Dependencies)

```
Milestone 2 (Captive DNS) ─────────┐
                                    ├──→ Milestone 3 (AP Provisioning)
Milestone 1 (WiFi Manager Refactor)┘           │
                                               ▼
                                    Milestone 4 (Main.c Integration)
                                               │
                                               ▼
                                    Milestone 7 (Reconnect Strategy)

Milestone 5 (RTC Diagnostics) ← independent, parallel
Milestone 6 (Schedule Sync) ← independent, parallel
```

**Recommended order:**
1. Milestone 2 (Captive DNS) — dependency, low complexity
2. Milestone 1 (WiFi Manager Refactor) — dependency, medium
3. Milestone 3 (AP Provisioning) — core feature, high
4. Milestone 4 (Main.c Integration) — ties it together
5. Milestone 5 (RTC Diagnostics) — independent
6. Milestone 6 (Schedule Sync) — trivial change
7. Milestone 7 (Reconnect Strategy) — builds on M1+M4

---

## 5. Test Strategy

### Unit-Level (compile + logic verification)
| Test | Method |
|------|--------|
| DNS packet parsing | Hex dump comparison (known query → expected response) |
| HTML form generation | String contains expected elements |
| RTC drift calculation | Mock time values, verify threshold logic |
| Schedule version compare | Known versions, verify skip/update |

### Integration (on hardware)
| Test | Steps |
|------|-------|
| AP Mode Trigger | Remove WiFi AP from range → verify AP appears in 45s |
| Captive Portal | Connect phone to AP → verify auto-redirect |
| WiFi Save | Enter valid creds → verify reboot + connect |
| Invalid Input | Empty SSID → verify error message |
| AP Timeout | Wait 5 min → verify AP disappears |
| RTC Alert | Set RTC 10 min behind → verify MQTT alert |
| Offline Schedule | Disconnect WiFi → verify bells ring on time |
| Reconnect Cycle | Toggle WiFi AP on/off → verify reconnection |

### Coverage Goals
- All error paths logged (ESP_LOGE)
- All NVS operations have error handling
- All timers have stop/cleanup
- Memory: no malloc without free path

---

## 6. Security Checklist

| # | Item | Implementation |
|---|------|---------------|
| 1 | AP Password strength | MAC-based: `SchoolBell_XXXXXX` (6 hex = 16M combinations) |
| 2 | Captive portal scope | FAQAT WiFi creds — no schedule, no MQTT, no config |
| 3 | Input validation | SSID: 1-32 chars, Pass: 8-64 chars, no null bytes |
| 4 | XSS prevention | HTML-encode all user input in responses |
| 5 | AP timeout | 5 min auto-off (reduces attack window) |
| 6 | Single connection | `max_connection = 1` on SoftAP |
| 7 | Offline immutability | AP mode'da faqat WiFi creds o'zgaradi, jadval read-only |
| 8 | NVS integrity | Schedule CRC32 (allaqachon mavjud) |
| 9 | MQTT TLS | Allaqachon mavjud (ca_cert.h) |
| 10 | No secrets in logs | WiFi password log'ga yozilmaydi |

---

## 7. Risks & Mitigations

| Risk | Impact | Mitigation |
|------|--------|-----------|
| Flash size overflow (HTML + DNS + AP code) | Build fail | HTML minimal (~2KB), DNS ~1KB code. Monitor with `pio run -v` size output |
| SoftAP + STA simultaneous (APSTA) memory | Crash | Pure AP mode (STA off during provisioning), not APSTA |
| Captive portal not triggering on some phones | UX issue | Support Android (`/generate_204`), iOS (`/hotspot-detect.html`), Windows (`/connecttest.txt`) |
| RTC I2C bus conflict | Data corruption | I2C operations already serialized (single task) |
| NVS wear (frequent writes) | Flash degradation | RTC drift flag: max 1 write/day. WiFi creds: only on user action |
| BLE + AP conflict | Memory | BLE only on first boot (no WiFi creds). AP only after WiFi fail. Never simultaneous |
| Timer overflow (tick count) | Logic error | Use `(now - last) >= interval` pattern (handles 32-bit wrap) |

---

## 8. File Summary

| File | Action | Milestone |
|------|--------|-----------|
| `src/captive_dns.c` | CREATE | M2 |
| `src/captive_dns.h` | CREATE | M2 |
| `src/wifi_manager.c` | MODIFY | M1, M7 |
| `src/wifi_manager.h` | MODIFY | M1 |
| `src/ap_provisioning.c` | CREATE | M3 |
| `src/ap_provisioning.h` | CREATE | M3 |
| `src/rtc_diagnostics.c` | CREATE | M5 |
| `src/rtc_diagnostics.h` | CREATE | M5 |
| `src/main.c` | MODIFY | M4 |
| `src/mqtt_client.c` | MODIFY | M6 |
| `src/schedule_manager.c` | MODIFY | M6 |
| `src/CMakeLists.txt` | MODIFY | M2 (add new .c files) |
| `include/config.h` | MODIFY | M1 (add AP config defines) |

---

## 9. Config Defines to Add (config.h)

```c
/* AP Provisioning */
#define AP_MAX_CONNECTIONS      1
#define AP_CHANNEL              1
#define AP_SSID_PREFIX          "SchoolBell_"
#define AP_PASS_PREFIX          "SchoolBell_"
#define AP_TIMEOUT_MS           300000   /* 5 minutes */
#define AP_RETRY_DELAY_MS       600000   /* 10 minutes between AP attempts */
#define WIFI_STA_MAX_RETRY      3        /* Retries before AP mode */

/* RTC Diagnostics */
#define RTC_DRIFT_CORRECT_SEC   30       /* Auto-correct threshold */
#define RTC_DRIFT_ALERT_SEC     300      /* Alert threshold (5 min) */
#define RTC_DEAD_CONSECUTIVE    3        /* Days before "battery dead" */
#define RTC_CHECK_HOUR          3        /* Daily check at 3:00 AM */
```
