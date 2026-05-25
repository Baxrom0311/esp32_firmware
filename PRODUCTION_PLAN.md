# ESP32 Production-Ready Implementation Plan

## 1. Goal & Non-Goals

### Goal
ESP32 firmware'ni production-ready qilish: WiFi AP provisioning (captive portal), RTC batareya diagnostikasi, offline xavfsizlik, va WiFi reconnect strategiyasi.

### Non-Goals
- BLE provisioning'ni o'zgartirish (allaqachon ishlaydi)
- OTA mexanizmini o'zgartirish
- MQTT client logikasini qayta yozish
- Dashboard backend (faqat MQTT alert format)
- Hardware dizayn o'zgarishlari

---

## 2. Architecture

### Hozirgi Holat (Mavjud Kod Tahlili)

| Modul | Holat | Kerak bo'lgan o'zgarishlar |
|-------|-------|---------------------------|
| `ap_provisioning.c` | ✅ Asosiy ishlaydi | AP+STA mode, brute-force himoya, reset tugma |
| `captive_dns.c` | ✅ Tayyor | O'zgarish kerak emas |
| `wifi_manager.c` | ✅ Fallback bor | AP+STA mode, background retry |
| `rtc_diagnostics.c` | ✅ Asosiy ishlaydi | Boot-time check yaxshilash |
| `schedule_manager.c` | ✅ Version-based sync bor | Offline lock qo'shish |
| `main.c` | ✅ AP retry bor | Reset tugma, AP+STA integration |

### Arxitektura Diagramma

```
Boot Flow:
┌─────────────────────────────────────────────────────────────┐
│ NVS init → RTC → Timezone → Bell GPIO → Schedule load      │
│                                                              │
│ WiFi creds NVS'da bormi?                                    │
│   ├── Yo'q → AP+STA mode (AP ko'rinadi)                    │
│   └── Bor → STA connect (5 retry)                          │
│              ├── OK → Normal mode (MQTT, SNTP, sync)        │
│              └── Fail → AP+STA mode (background STA retry)  │
│                                                              │
│ Normal mode: schedule_tick + rtc_diag_tick + SNTP resync    │
│ AP active: schedule NVS'dan ishlayveradi                    │
└─────────────────────────────────────────────────────────────┘
```

### Data Flow

```
Captive Portal:
  Phone → WiFi connect "SchoolBell_XXXX" → DNS redirect → 192.168.4.1
  → HTML form (SSID + Password only) → POST /save → NVS write → Reboot

Schedule Sync:
  MQTT → "devices/{id}/schedule" → version compare → NVS save → offline ready

RTC Diagnostics:
  Boot: RTC year < 2024 → battery_dead flag
  Daily 3:00: SNTP vs RTC → drift > 5min → MQTT alert → 3 days → dead flag
```

---

## 3. Milestones

### Milestone 1: WiFi AP+STA Mode va Brute-Force Himoya
**Complexity: HIGH**

Hozirgi muammo: AP mode yoqilganda STA to'liq o'chadi. Kerak: bir vaqtda AP+STA ishlashi.

**Fayllar:**
- `src/wifi_manager.c` — `WIFI_MODE_APSTA` qo'llash, background STA retry
- `src/wifi_manager.h` — yangi API: `wifi_manager_start_apsta()`, `wifi_manager_stop_ap()`
- `src/ap_provisioning.c` — brute-force himoya (3 noto'g'ri → 30s lock), AP auto-off 2 min after STA connect
- `src/main.c` — reset tugma (3s hold → AP yoqish), BLE provisioning o'rniga AP fallback

**O'zgarishlar:**

1. **wifi_manager.c** — `wifi_manager_start_apsta()` funksiyasi:
   - `esp_wifi_set_mode(WIFI_MODE_APSTA)` 
   - AP config: `SchoolBell_XXXX` SSID, MAC-based password
   - STA: NVS credentials bilan background retry (30s interval)
   - STA ulanganda: 2 daqiqadan keyin AP o'chirish

2. **ap_provisioning.c** — brute-force himoya:
   - Static counter: `s_failed_attempts`
   - 3 ta noto'g'ri POST → 30s lockout (HTTP 429 qaytarish)
   - Timer bilan lockout reset

3. **main.c** — reset tugma integratsiya:
   - GPIO0 (mavjud panic button) 3s hold detect
   - 3s hold → AP majburiy yoqish (WiFi qayta sozlash)
   - Short press → panic (mavjud logic)

**Acceptance Criteria:**
- [ ] WiFi 5 marta fail → AP+STA mode yoqiladi
- [ ] AP mode'da STA background'da retry qiladi (30s)
- [ ] STA ulanganda AP 2 daqiqadan keyin o'chadi
- [ ] 3 ta noto'g'ri form submit → 30s lock
- [ ] Reset tugma 3s → AP yoqiladi
- [ ] AP yoqiq bo'lsa ham jadval NVS'dan ishlaydi

**Verification:**
```bash
cd esp32_firmware && pio run  # Compile check
```

---

### Milestone 2: Captive Portal Xavfsizlik Mustahkamlash
**Complexity: LOW**

Hozirgi captive portal faqat WiFi SSID/password qabul qiladi — bu to'g'ri. Qo'shimcha himoya kerak.

**Fayllar:**
- `src/ap_provisioning.c` — input sanitization, CSRF token, response headers

**O'zgarishlar:**

1. **Input validation kuchaytirish:**
   - SSID: 1-32 char, printable ASCII only (no control chars)
   - Password: 8-64 char
   - Total POST body: max 200 bytes (allaqachon bor)
   - Reject any request with unexpected parameters

2. **Security headers:**
   - `X-Content-Type-Options: nosniff`
   - `X-Frame-Options: DENY`
   - `Cache-Control: no-store`

3. **Rate limiting (Milestone 1'dan):**
   - Allaqachon brute-force himoya qo'shilgan

**Acceptance Criteria:**
- [ ] Control characters SSID'da reject qilinadi
- [ ] Security headers barcha response'larda bor
- [ ] Faqat `/`, `/save`, `/generate_204`, `/hotspot-detect.html` URI'lar ishlaydi
- [ ] Boshqa URI'lar 302 → `/` redirect

**Verification:**
```bash
cd esp32_firmware && pio run
```

---

### Milestone 3: RTC Diagnostika Yaxshilash
**Complexity: LOW**

Hozirgi `rtc_diagnostics.c` 90% tayyor. Qo'shimcha: boot-time SNTP kutish logikasi va status reporting.

**Fayllar:**
- `src/rtc_diagnostics.c` — boot-time logic yaxshilash
- `src/rtc_diagnostics.h` — `rtc_diagnostics_get_status_json()` qo'shish
- `src/status_reporter.c` — RTC status MQTT heartbeat'ga qo'shish

**O'zgarishlar:**

1. **Boot-time logic (main.c'da):**
   - RTC year < 2024 → `battery_dead` flag set (allaqachon bor ✅)
   - SNTP sync kutish — agar 10s ichida sync bo'lmasa, offline jadval bilan ishlash
   - SNTP sync bo'lganda → RTC'ga yozish (allaqachon bor ✅)

2. **Status JSON:**
   ```c
   // rtc_diagnostics.h ga qo'shish:
   void rtc_diagnostics_get_status(bool *battery_dead, int32_t *last_drift);
   ```

3. **Heartbeat'ga qo'shish:**
   - `status_reporter.c` da RTC battery status qo'shish

**Acceptance Criteria:**
- [ ] Boot'da RTC year < 2024 → battery_dead flag
- [ ] MQTT alert: `{"type":"rtc_drift","drift_sec":312,"battery_status":"low"}`
- [ ] 3 kun ketma-ket drift > 5min → `"battery_status":"dead"`
- [ ] Heartbeat'da RTC status ko'rinadi

**Verification:**
```bash
cd esp32_firmware && pio run
```

---

### Milestone 4: Offline Mode Xavfsizlik
**Complexity: MEDIUM**

Offline'da hech narsa o'zgartirilmasligi kerak (AP WiFi creds'dan tashqari).

**Fayllar:**
- `src/schedule_manager.c` — offline lock flag
- `src/schedule_manager.h` — `schedule_manager_is_locked()` 
- `src/mqtt_client.c` — schedule update faqat online'da qabul qilish

**O'zgarishlar:**

1. **Schedule manager offline lock:**
   - `schedule_manager_update()` — faqat `wifi_manager_is_connected()` true bo'lganda ishlaydi
   - Offline'da MQTT message kelmaydi (MQTT disconnect), lekin defensive check

2. **Version-based sync (allaqachon bor ✅):**
   - `schedule_manager_update()` da version compare mavjud
   - Online bo'lganda server version yuboradi → yangi bo'lsa sync

3. **7 kunlik jadval limiti:**
   - NVS'da `schedule_saved_time` timestamp saqlash
   - Agar jadval 7 kundan eski bo'lsa va online bo'lmasa → log warning (lekin ishlayveradi — xavfsizlik uchun)

**Acceptance Criteria:**
- [ ] Offline'da schedule_manager_update() reject qiladi
- [ ] Online bo'lganda version compare ishlaydi
- [ ] Jadval 7 kundan eski → warning log
- [ ] AP mode'da faqat WiFi creds o'zgaradi, boshqa NVS namespace'lar read-only

**Verification:**
```bash
cd esp32_firmware && pio run
```

---

### Milestone 5: Vaqt Sinxronizatsiya Strategiya Finalize
**Complexity: LOW**

Hozirgi kod 80% tayyor. Faqat tartibga solish kerak.

**Fayllar:**
- `src/main.c` — boot sequence tartibga solish

**O'zgarishlar:**

1. **Boot sequence (allaqachon to'g'ri ✅):**
   - RTC → system time (darhol) ✅
   - WiFi → SNTP → RTC save ✅

2. **Daily 3:00 check (allaqachon bor ✅):**
   - `rtc_diagnostics_tick()` handles this ✅

3. **Hourly resync (allaqachon bor ✅):**
   - `sntp_periodic_resync()` in main loop ✅
   - `rtc_driver_save_from_system()` after sync ✅

**Acceptance Criteria:**
- [ ] Boot: RTC → system time → WiFi → SNTP → RTC update
- [ ] Hourly SNTP resync + RTC save
- [ ] Daily 3:00 RTC drift check + alert

**Verification:**
```bash
cd esp32_firmware && pio run
```

---

## 4. Implementation Order (Dependency Graph)

```
Milestone 5 (vaqt sync) ─── allaqachon tayyor, faqat verify
     │
Milestone 3 (RTC diag) ─── kichik qo'shimchalar
     │
Milestone 4 (offline) ──── schedule lock qo'shish
     │
Milestone 2 (portal sec) ── input validation
     │
Milestone 1 (AP+STA) ───── eng katta o'zgarish, oxirida
```

**Tavsiya etilgan tartib:**
1. **Milestone 1** (AP+STA) — eng muhim, eng katta
2. **Milestone 2** (Portal security) — Milestone 1 bilan birga
3. **Milestone 3** (RTC) — mustaqil, parallel qilsa bo'ladi
4. **Milestone 4** (Offline) — Milestone 1 keyin
5. **Milestone 5** (Time sync) — faqat verify

---

## 5. Test Strategy

### Unit Test (Host-based, qemu yoki native)
ESP-IDF da host-based test qiyin, shuning uchun integration test asosiy.

### Integration Test (Real Hardware)

| Test | Qanday | Kutilgan natija |
|------|--------|-----------------|
| AP mode trigger | WiFi router'ni o'chirish | 5 retry → AP+STA yoqiladi |
| Captive portal | Telefon bilan AP'ga ulanish | 192.168.4.1 ochiladi |
| WiFi save | Form to'ldirish, submit | NVS'ga yoziladi, reboot |
| Brute-force | 3 marta noto'g'ri submit | 30s lock, HTTP 429 |
| Reset button | GPIO0 3s bosish | AP yoqiladi |
| RTC drift | RTC vaqtini qo'lda o'zgartirish | Alert MQTT'ga keladi |
| RTC battery | RTC batareya chiqarish | Boot'da battery_dead flag |
| Offline schedule | WiFi o'chirish | Jadval ishlayveradi |
| Schedule version | Eski version yuborish | Reject qilinadi |
| AP timeout | 5 min kutish | AP o'chadi |
| AP+STA | AP yoqiq, STA retry | STA ulanganda AP 2min keyin o'chadi |

### Smoke Test Checklist
```
1. Flash firmware → Serial monitor
2. WiFi creds yo'q → AP mode yoqiladi (log: "AP provisioning started")
3. Telefon bilan ulanish → Captive portal ochiladi
4. WiFi sozlash → Reboot → Normal mode
5. Router o'chirish → 5 retry → AP mode
6. Router yoqish → STA auto-connect → AP o'chadi
7. 3s reset → AP yoqiladi
```

---

## 6. Security Checklist

| # | Tekshirish | Holat |
|---|-----------|-------|
| 1 | AP parol MAC-based (tahmin qilib bo'lmaydi) | ✅ Bor |
| 2 | Captive portal faqat WiFi creds | ✅ Bor |
| 3 | Input validation (SSID 1-32, pass 8-64) | ✅ Bor |
| 4 | Brute-force himoya (3 attempt → 30s lock) | ⬜ Qo'shish kerak |
| 5 | AP 5 min timeout | ✅ Bor |
| 6 | Offline'da NVS read-only (WiFi creds'dan tashqari) | ⬜ Enforce qilish |
| 7 | MQTT TLS (mqtts://) | ✅ Bor |
| 8 | OTA host whitelist | ✅ Bor |
| 9 | NVS namespace isolation | ✅ Bor |
| 10 | No debug endpoints in captive portal | ✅ Bor |
| 11 | URL decode injection prevention | ⬜ Sanitize qilish |
| 12 | POST body size limit | ✅ 200 bytes |
| 13 | Security response headers | ⬜ Qo'shish kerak |
| 14 | Schedule CRC32 integrity | ✅ Bor |
| 15 | Reset button debounce | ✅ ISR debounce bor |

---

## 7. Risks & Mitigations

| Risk | Impact | Mitigation |
|------|--------|-----------|
| AP+STA mode RAM usage (~50KB extra) | ESP32 memory pressure | Monitor heap, AP o'chirganda free |
| Captive portal iOS/Android detection farqi | Ba'zi telefonlar portal ko'rmaydi | `/generate_204` + `/hotspot-detect.html` (allaqachon bor) |
| NVS wear (ko'p yozish) | Flash eskirishi | Faqat o'zgarganda yozish, version check |
| RTC I2C hang | System freeze | I2C timeout (allaqachon bor), watchdog |
| WiFi channel conflict AP vs STA | STA ulanolmaydi | AP channel=1 fixed, STA auto |
| 5 min AP timeout qisqa | Foydalanuvchi ulgurmaydi | Timer reset on HTTP activity |
| Reset button panic bilan conflict | Noto'g'ri trigger | Short press=panic, 3s hold=reset (farqlash) |
| SNTP server unreachable | Vaqt sync bo'lmaydi | 2 ta server (pool.ntp.org + time.google.com) ✅ |

---

## 8. Kod O'zgarishlar Xulosa

### Yangi funksiyalar kerak:

**wifi_manager.h:**
```c
esp_err_t wifi_manager_start_apsta(void);  // AP+STA mode
void wifi_manager_stop_ap(void);           // Faqat AP o'chirish
```

**ap_provisioning.c:**
```c
// Ichki: brute-force counter + lockout timer
// Ichki: HTTP activity timer (AP timeout reset)
```

**main.c:**
```c
// Reset button: 3s hold detection (panic ISR'dan ajratish)
// AP+STA integration logic
```

**rtc_diagnostics.h:**
```c
void rtc_diagnostics_get_status(bool *dead, int32_t *drift);
```

### O'chiriladigan kod:
- `ble_provisioning.c` — AP provisioning bilan almashtiriladi (yoki parallel qoldiriladi)
  - **Qaror:** BLE qoldiriladi (first-time setup), AP faqat WiFi fail'da. Ikkalasi ham kerak.

---

## 9. Estimated Effort

| Milestone | Soat | Prioritet |
|-----------|------|-----------|
| 1. AP+STA Mode | 6-8h | P0 |
| 2. Portal Security | 2-3h | P0 |
| 3. RTC Diagnostics | 2-3h | P1 |
| 4. Offline Security | 3-4h | P1 |
| 5. Time Sync Verify | 1h | P2 |
| **Jami** | **14-19h** | |

---

## 10. Boshlash Uchun Birinchi Qadam

```bash
# 1. Hozirgi kodni compile qilish (baseline)
cd esp32_firmware && pio run

# 2. Milestone 1 dan boshlash:
#    - wifi_manager.c ga APSTA mode qo'shish
#    - ap_provisioning.c ga brute-force qo'shish  
#    - main.c da reset button logic

# 3. Har bir milestone'dan keyin: pio run + hardware test
```
