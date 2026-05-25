# ESP32 Mustahkamlash — Implementation Roadmap

## 1. Goal & Non-Goals

### Goal
ESP32 firmware'ni production-ready qilish: WiFi AP provisioning, RTC diagnostika, offline xavfsizlik — barchasi allaqachon 70-90% tayyor kodga asoslangan holda to'liq ishga tushirish.

### Non-Goals
- BLE provisioning'ni o'zgartirish (allaqachon ishlaydi)
- OTA mexanizmini qayta yozish
- Dashboard backend/frontend (faqat MQTT alert format)
- Yangi hardware qo'shish

---

## 2. Architecture

### Komponentlar va Data Flow

```
┌─────────────────────────────────────────────────────────────┐
│                        BOOT SEQUENCE                         │
├─────────────────────────────────────────────────────────────┤
│ NVS init → RTC → Timezone → RTC Diagnostics → Bell GPIO    │
│     ↓                                                        │
│ WiFi creds NVS'da bormi?                                    │
│   ├── Yo'q → BLE Provisioning → WiFi connect               │
│   └── Bor → wifi_manager_init_with_fallback()               │
│              ├── 3x fail → AP+STA mode + Captive Portal     │
│              └── OK → SNTP → MQTT → Normal loop             │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│                       MAIN LOOP (1s)                         │
├─────────────────────────────────────────────────────────────┤
│ schedule_manager_tick()     — NVS jadval bilan qo'ng'iroq   │
│ rtc_diagnostics_tick()      — Soat 3:00 da drift tekshirish │
│ sntp_periodic_resync()      — Har soatda vaqt sync          │
│ AP timeout/retry logic      — 5min timeout, 10min retry     │
│ WiFi reconnect monitoring   — Uzilsa AP qayta yoqish        │
│ Panic button + relay logic  — ISR + cooldown                │
└─────────────────────────────────────────────────────────────┘
```

### Mavjud Holat Tahlili

| Komponent | Holat | Qolgan ish |
|-----------|-------|------------|
| `ap_provisioning.c` | 90% tayyor | AP parol format to'g'rilash, portal xavfsizlik |
| `wifi_manager.c` | 95% tayyor | AP+STA mode allaqachon bor, 5→3 retry fix |
| `rtc_diagnostics.c` | 95% tayyor | Deyarli to'liq, faqat status_reporter integratsiya |
| `schedule_manager.c` | 90% tayyor | Version-based sync bor, stale check bor |
| `captive_dns.c` | 100% tayyor | O'zgarish kerak emas |
| `main.c` | 85% tayyor | AP retry logic bor, bir nechta tuzatish kerak |

---

## 3. Milestones

### Milestone 1: WiFi AP Provisioning Mustahkamlash
**Complexity: LOW**

Mavjud kod allaqachon 90% tayyor. Faqat bir nechta xavfsizlik tuzatish kerak.

**Fayllar:**
- `src/wifi_manager.c` — `AP_FALLBACK_RETRIES` 3→5 ga o'zgartirish (brief'da 5 marta deyilgan)
- `src/ap_provisioning.c` — Brute-force himoya allaqachon bor ✅
- `src/main.c` — AP retry logic allaqachon bor ✅

**Aniq o'zgarishlar:**

1. **`wifi_manager.c`** — `AP_FALLBACK_RETRIES` ni 5 ga o'zgartirish (brief: "STA 5 marta fail"):
   ```c
   #define AP_FALLBACK_RETRIES 5  // was 3
   ```

2. **`ap_provisioning.c`** — AP parol formatini brief'ga moslashtirish (hozir `SchoolBell_%02X%02X%02X`, brief'da `SchoolBell_` + oxirgi 6 hex):
   - Hozirgi: `SchoolBell_71CE40` ← allaqachon to'g'ri format (mac[3], mac[4], mac[5])
   - SSID: `SchoolBell_%02X%02X` ← mac[4], mac[5] — to'g'ri ✅

3. **`main.c`** — AP+STA mode'da STA ulangandan keyin AP 2 daqiqadan keyin o'chishi:
   - Allaqachon `wifi_manager.c` da `s_ap_auto_off_timer` (120000ms) bor ✅
   - Lekin `ap_provisioning_stop()` ham chaqirilishi kerak

**Acceptance Criteria:**
- [ ] WiFi'ga 5 marta ulanolmasa → AP+STA mode yoqiladi
- [ ] AP SSID: "SchoolBell_XXXX" (oxirgi 4 hex MAC)
- [ ] AP parol: "SchoolBell_XXXXXX" (oxirgi 6 hex MAC)
- [ ] Captive portal: faqat WiFi SSID/password form
- [ ] 3 ta noto'g'ri urinish → 30s lockout
- [ ] 5 daqiqa ichida sozlanmasa → AP o'chadi
- [ ] 10 daqiqadan keyin yana AP yoqiladi (retry)
- [ ] WiFi sozlangandan keyin reboot

**Verification:**
```bash
cd esp32_firmware && pio run
# Hardware test: WiFi creds o'chirish → AP paydo bo'lishini tekshirish
# curl http://192.168.4.1/ → HTML form qaytishi
```

---

### Milestone 2: AP+STA Mode va WiFi Reconnect Strategiya
**Complexity: LOW**

Mavjud kod allaqachon to'liq implementatsiya qilingan. Faqat `ap_provisioning_stop()` integratsiyasi kerak.

**Fayllar:**
- `src/wifi_manager.c` — `ap_auto_off_cb` da portal ham to'xtatish
- `src/main.c` — Allaqachon bor: WiFi lost → AP+STA, WiFi restored → AP off

**Aniq o'zgarishlar:**

1. **`wifi_manager.c`** — `ap_auto_off_cb` da captive portal ham to'xtatish:
   ```c
   static void ap_auto_off_cb(TimerHandle_t timer)
   {
       if (s_connected) {
           ESP_LOGI(TAG, "AP+STA: STA connected, stopping AP after 2min");
           ap_provisioning_stop();  // ← qo'shish (HTTP + DNS to'xtatadi)
           wifi_manager_stop_ap();
       }
   }
   ```
   Muammo: `wifi_manager.c` da `ap_provisioning.h` include qilish kerak yoki callback orqali.

2. **Yechim:** Callback approach (circular dependency oldini olish):
   ```c
   // wifi_manager.h ga qo'shish:
   typedef void (*wifi_ap_stopped_cb_t)(void);
   void wifi_manager_set_ap_stopped_cb(wifi_ap_stopped_cb_t cb);
   ```
   `main.c` da: `wifi_manager_set_ap_stopped_cb(ap_provisioning_stop);`

**Acceptance Criteria:**
- [ ] WiFi uzilganda AP+STA mode yoqiladi
- [ ] STA background'da 30s intervalda retry qiladi
- [ ] STA ulanganda 2 daqiqadan keyin AP o'chadi
- [ ] AP o'chganda HTTP server va DNS ham to'xtaydi
- [ ] Reset tugma 3s → AP majburiy yoqiladi (allaqachon bor ✅)

**Verification:**
```bash
pio run
# Test: WiFi router'ni o'chirish → AP paydo bo'lishi → router yoqish → AP o'chishi
```

---

### Milestone 3: RTC Diagnostika Mustahkamlash
**Complexity: LOW**

Mavjud `rtc_diagnostics.c` allaqachon to'liq. Faqat `status_reporter` integratsiyasi kerak.

**Fayllar:**
- `src/rtc_diagnostics.c` — O'zgarish kerak emas ✅
- `src/status_reporter.c` — RTC battery status qo'shish

**Aniq o'zgarishlar:**

1. **`status_reporter.c`** — Heartbeat payload'ga RTC status qo'shish:
   ```c
   // Heartbeat JSON'ga qo'shish:
   "rtc_battery": "%s",  // rtc_diagnostics_battery_status()
   "rtc_drift": %ld      // rtc_diagnostics_last_drift_sec()
   ```

**Mavjud funksionallik (allaqachon ishlaydi):**
- ✅ Soat 3:00 da drift tekshirish
- ✅ Drift > 30s → auto-correct
- ✅ Drift > 5 min → MQTT alert `{"type":"rtc_drift","drift_sec":312,"battery_status":"low"}`
- ✅ 3 kun ketma-ket → "dead" flag
- ✅ Boot'da year < 2024 → battery dead
- ✅ NVS'da state persist

**Acceptance Criteria:**
- [ ] Heartbeat'da RTC battery status ko'rinadi
- [ ] Dashboard qurilma kartasida battery status
- [ ] RTC drift > 5 min → MQTT alert yuboriladi
- [ ] 3 kun ketma-ket → "RTC_BATTERY_DEAD" flag

**Verification:**
```bash
pio run
# Test: RTC vaqtini qo'lda noto'g'ri qo'yish → alert kelishini tekshirish
```

---

### Milestone 4: Offline Mode Xavfsizlik
**Complexity: LOW**

Mavjud kod allaqachon himoyalangan. Faqat tekshirish va bir nechta guard qo'shish.

**Fayllar:**
- `src/schedule_manager.c` — Allaqachon `wifi_manager_is_connected()` guard bor ✅
- `src/main.c` — Offline'da MQTT command'larni reject qilish

**Mavjud himoyalar:**
- ✅ `schedule_manager_update()` — `wifi_manager_is_connected()` check
- ✅ Version-based sync: `schedule_manager_should_sync(server_version)`
- ✅ Stale schedule detection (7 kun)
- ✅ CRC32 validation on NVS load
- ✅ AP mode'da faqat WiFi creds o'zgartiriladi

**Qo'shimcha guard (MQTT handler'da):**
```c
// mqtt_client.c da schedule update handler'da:
if (!wifi_manager_is_connected()) return;  // allaqachon bor
// Version check:
cJSON *ver = cJSON_GetObjectItem(root, "version");
if (ver && !schedule_manager_should_sync(ver->valueint)) return;  // allaqachon bor
```

**Acceptance Criteria:**
- [ ] Offline: NVS jadval bilan ishlaydi, hech narsa o'zgarmaydi
- [ ] Online: version tekshirish, yangilangan bo'lsa sync
- [ ] Jadval 7 kundan eski → stale alert
- [ ] NVS corrupt → graceful recovery (CRC check)

**Verification:**
```bash
pio run
# Test: WiFi o'chirish → jadval ishlashini tekshirish
# Test: Eski version yuborish → reject bo'lishini tekshirish
```

---

### Milestone 5: Vaqt Sinxronizatsiya Strategiya
**Complexity: LOW**

Allaqachon 95% tayyor. Faqat tekshirish.

**Mavjud implementatsiya:**
- ✅ Boot: RTC → system time (`rtc_driver_init()` in main.c)
- ✅ WiFi ulanganda: SNTP sync → RTC'ga yozish (`init_sntp()` + `rtc_driver_save_from_system()`)
- ✅ Har soatda: SNTP re-sync (`sntp_periodic_resync()` + `rtc_driver_save_from_system()`)
- ✅ Soat 3:00: RTC diagnostika (`rtc_diagnostics_tick()`)

**O'zgarish kerak emas.** Barcha strategiya allaqachon implementatsiya qilingan.

**Acceptance Criteria:**
- [ ] Boot'da RTC'dan vaqt olinadi (offline ishlashi uchun)
- [ ] WiFi ulanganda SNTP sync bo'ladi
- [ ] Har soatda re-sync
- [ ] Soat 3:00 da diagnostika

**Verification:**
```bash
pio run
# Log'da tekshirish: "SNTP time synced", "Periodic SNTP re-sync OK"
```

---

## 4. Test Strategy

### Unit Test (Host-based, qmock/fff)
ESP-IDF da host-based test qiyin, lekin quyidagilarni tekshirish mumkin:

| Test | Usul |
|------|------|
| URL decode | `url_decode()` ni alohida test |
| Schedule CRC | Corrupt data → graceful reject |
| Version compare | `should_sync()` logic |
| RTC drift logic | Turli drift qiymatlari |

### Integration Test (Hardware)
| Senariy | Kutilgan natija |
|---------|-----------------|
| WiFi creds yo'q + boot | AP+STA mode, captive portal |
| WiFi 5x fail | AP yoqiladi |
| AP'da form to'ldirish | NVS'ga saqlash, reboot |
| AP 5 min timeout | AP o'chadi |
| 10 min keyin | AP qayta yoqiladi |
| WiFi uzilish | AP yoqiladi, STA retry |
| WiFi qayta ulanish | AP 2 min keyin o'chadi |
| Reset tugma 3s | AP majburiy yoqiladi |
| RTC batareya olib tashlash | Alert, "dead" flag |
| Server'dan eski version | Reject, sync bo'lmaydi |
| 7 kun offline | Stale alert |

### Stress Test
- AP'ga 3 ta client bir vaqtda ulanish (max_connection=1 himoya)
- Brute-force: 3 ta noto'g'ri → lockout
- WiFi flapping (tez-tez uzilish/ulanish)
- NVS to'lishi (partition size check)

---

## 5. Security Checklist

| # | Tekshirish | Holat |
|---|-----------|-------|
| 1 | AP parol kuchli (MAC-based, 13+ char) | ✅ `SchoolBell_XXXXXX` |
| 2 | Captive portal'da faqat WiFi form | ✅ Boshqa endpoint yo'q |
| 3 | Brute-force himoya (3 attempt → 30s) | ✅ Implementatsiya bor |
| 4 | HTTP security headers (X-Frame, nosniff) | ✅ `set_security_headers()` |
| 5 | Input validation (SSID 1-32, pass 8-64) | ✅ Server-side check |
| 6 | Offline'da schedule o'zgarmaydi | ✅ `wifi_manager_is_connected()` guard |
| 7 | NVS CRC32 integrity | ✅ Load'da tekshiriladi |
| 8 | MQTT TLS (mqtts://) | ✅ `config.h` da |
| 9 | AP auto-timeout (5 min) | ✅ Timer bor |
| 10 | max_connection=1 (AP) | ✅ Faqat 1 client |
| 11 | URL decode injection | ⚠️ Tekshirish kerak |
| 12 | POST body size limit | ✅ 200 byte buf |

### Qo'shimcha xavfsizlik tavsiyalari:
1. **SSID/password sanitization** — NUL byte, control char filter
2. **HTTPS for captive portal** — ESP32 da self-signed cert bilan mumkin (lekin captive portal detection buzilishi mumkin, shuning uchun HTTP qoldiriladi)
3. **AP parolni NVS'da saqlash** — Hozir har safar MAC'dan generatsiya qilinadi (yaxshi — o'zgarmas)

---

## 6. Risks

| Risk | Ehtimollik | Ta'sir | Yechim |
|------|-----------|--------|--------|
| NVS partition to'lishi | Past | Yuqori | `nvs_storage_erase_namespace()` + partition size tekshirish |
| AP+STA mode RAM yetishmasligi | O'rta | O'rta | AP o'chganda RAM qaytadi; `max_connection=1` |
| Captive portal iOS/Android detect fail | O'rta | Past | `/generate_204` + `/hotspot-detect.html` allaqachon bor |
| RTC I2C hang | Past | Yuqori | Timeout bor (`rtc_driver.c`), WDT himoya |
| WiFi flapping → AP on/off loop | O'rta | Past | `ap_stopped_tick` + 10 min cooldown bor |
| SNTP server unreachable | Past | Past | 2 ta server, RTC fallback |
| Flash wear (NVS yozish) | Past | Yuqori | Faqat o'zgarganda yozish (version check) |
| Concurrent HTTP requests | Past | Past | `max_connection=1` + single-threaded httpd |

---

## 7. Implementation Order (Dependency Graph)

```
[Milestone 1: AP Provisioning] ← Eng muhim, boshqa hamma narsa bunga bog'liq
       ↓
[Milestone 2: AP+STA Reconnect] ← M1 ga bog'liq (portal stop callback)
       ↓
[Milestone 3: RTC Diagnostika] ← Mustaqil, parallel qilsa bo'ladi
       ↓
[Milestone 4: Offline Xavfsizlik] ← Mustaqil, faqat tekshirish
       ↓
[Milestone 5: Vaqt Sync] ← Allaqachon tayyor, faqat verify
```

### Umumiy baholash:
- **Jami o'zgarish hajmi:** ~50-80 qator kod (mavjud kod juda yaxshi)
- **Vaqt:** 2-3 soat (barcha milestone'lar)
- **Xavf darajasi:** Past (mavjud arxitektura to'g'ri)

---

## 8. Aniq Kod O'zgarishlari Ro'yxati

### Fayl: `src/wifi_manager.c`
1. `AP_FALLBACK_RETRIES` → 5 (1 qator)
2. `ap_auto_off_cb` → `ap_provisioning_stop()` chaqirish (callback orqali)
3. `wifi_manager_set_ap_stopped_cb()` funksiya qo'shish (~10 qator)

### Fayl: `src/wifi_manager.h`
1. `wifi_ap_stopped_cb_t` typedef qo'shish
2. `wifi_manager_set_ap_stopped_cb()` deklaratsiya

### Fayl: `src/main.c`
1. `wifi_manager_set_ap_stopped_cb(ap_provisioning_stop)` chaqirish (1 qator)

### Fayl: `src/status_reporter.c`
1. `#include "rtc_diagnostics.h"` qo'shish
2. Heartbeat JSON'ga `rtc_battery` va `rtc_drift` qo'shish (~5 qator)

### Fayl: `src/ap_provisioning.c`
1. SSID/password input sanitization (NUL, control chars) (~10 qator)

**Jami: ~30 qator yangi kod, ~5 qator o'zgarish**
