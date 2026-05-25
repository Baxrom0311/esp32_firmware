# ESP32 Mustahkamlash — Implementation Plan

## 1. Goal & Non-Goals

### Goal
ESP32 firmware'ni production-ready qilish:
- WiFi AP provisioning (captive portal) — qurilmani dastlabki sozlash
- RTC diagnostika — batareya monitoring va drift detection
- Offline xavfsizlik — NVS jadval bilan ishonchli ishlash
- WiFi reconnect strategiya — AP+STA mode bilan uzluksiz ishlash

### Non-Goals
- BLE provisioning'ni o'zgartirish (allaqachon mavjud, parallel ishlaydi)
- OTA update logic'ni o'zgartirish
- Dashboard backend/frontend (faqat MQTT alert format belgilanadi)
- Jadval formati o'zgartirish (mavjud `schedule_entry_t` saqlanadi)
- Multi-AP (bir vaqtda bir nechta WiFi saqlash)

---

## 2. Architecture

### Component Diagram

```
┌─────────────────────────────────────────────────────────┐
│                        main.c                            │
│  Boot → RTC → WiFi(STA) → [fail?] → AP+STA mode        │
│  Main loop: schedule_tick, rtc_diag_tick, reconnect     │
└────────┬──────────┬──────────┬──────────┬───────────────┘
         │          │          │          │
    ┌────▼────┐ ┌───▼───┐ ┌───▼────┐ ┌───▼──────────┐
    │wifi_mgr │ │ap_prov│ │rtc_diag│ │schedule_mgr  │
    │(AP+STA) │ │+portal│ │+alerts │ │(version sync)│
    └────┬────┘ └───┬───┘ └───┬────┘ └───┬──────────┘
         │          │          │          │
    ┌────▼──────────▼──────────▼──────────▼───┐
    │              NVS Storage                  │
    │  wifi/ssid, wifi/pass, config/rtc_dead,  │
    │  schedule/data, schedule/ver, schedule/crc│
    └─────────────────────────────────────────┘
```

### Data Flow

1. **Boot**: RTC → system time → WiFi STA attempt (3x) → fail → AP+STA mode
2. **AP Mode**: Captive portal (faqat WiFi creds) → NVS save → reboot
3. **Online**: SNTP sync → RTC save → MQTT subscribe → jadval version check → sync
4. **Offline**: NVS jadval bilan ishlash, hech narsa o'zgarmaydi
5. **Daily 3:00**: SNTP vs RTC drift check → alert if > 5min

### Tech Choices

| Tanlangan | Sabab |
|-----------|-------|
| ESP-IDF HTTP server | Allaqachon ishlatilmoqda, lightweight |
| Captive DNS (custom) | Allaqachon mavjud, barcha DNS → 192.168.4.1 |
| AP+STA mode | STA background retry + AP provisioning parallel |
| NVS blob + CRC32 | Allaqachon mavjud pattern, ishonchli |
| FreeRTOS timer | AP timeout, reconnect — allaqachon ishlatilmoqda |

---

## 3. Milestones

### Milestone 1: WiFi AP+STA Reconnect Strategiya (AP fallback yaxshilash)

**Hozirgi holat:** `wifi_manager.c` da AP+STA mode mavjud, lekin `main.c` da faqat AP-only mode ishlatilmoqda (STA to'xtatiladi, keyin AP boshlanadi).

**Kerakli o'zgarish:** Boot'da AP+STA mode ishlatish — AP provisioning va STA retry parallel ishlashi kerak.

**Fayllar:**
- `src/main.c` — o'zgartirish: `wifi_manager_stop_sta()` + `ap_provisioning_start()` o'rniga `wifi_manager_start_apsta()` + captive portal
- `src/ap_provisioning.c` — o'zgartirish: AP+STA mode bilan ishlash (AP'ni o'zi yaratmaslik, faqat HTTP server + DNS)
- `src/ap_provisioning.h` — yangi: `ap_provisioning_start_portal_only()` (HTTP + DNS, AP'siz)
- `src/wifi_manager.c` — kichik fix: `s_ap_auto_off_timer` undeclared variable (hozir event_handler'da ishlatilmoqda lekin e'lon qilinmagan)

**Acceptance Criteria:**
- [ ] WiFi 3x fail → AP+STA mode yoqiladi (AP ko'rinadi, STA 30s retry)
- [ ] STA ulanganda → AP 2 daqiqadan keyin o'chadi
- [ ] AP yoqiq bo'lsa ham jadval NVS'dan ishlayveradi
- [ ] WiFi yana uzilsa → AP qayta yoqiladi
- [ ] Reset tugma 3s → AP majburiy yoqiladi

**Verification:**
```bash
cd esp32_firmware && pio run  # Compile check
```

**Complexity:** Medium

---

### Milestone 2: Captive Portal Xavfsizlik Mustahkamlash

**Hozirgi holat:** `ap_provisioning.c` da brute-force variables e'lon qilingan lekin ishlatilmayapti. HTML sahifada faqat WiFi creds — bu to'g'ri.

**Kerakli o'zgarish:**
1. Brute-force himoya: 3 noto'g'ri parol → 30s lockout (AP parolga emas, captive portal form'ga)
2. AP parol format: `SchoolBell_XXYYZZ` (MAC oxirgi 6 hex) — allaqachon to'g'ri
3. 5 daqiqa timeout — allaqachon mavjud
4. CSRF token qo'shish (oddiy random token session'da)
5. Input sanitization kuchaytirish

**Fayllar:**
- `src/ap_provisioning.c` — o'zgartirish: brute-force logic implement, CSRF token, rate limiting

**Acceptance Criteria:**
- [ ] 3 ta ketma-ket noto'g'ri form submit → 30s lockout (HTML xabar)
- [ ] CSRF token: har bir GET'da yangi token, POST'da tekshirish
- [ ] SSID/password input: maxlength enforce, XSS-safe (HTML entity encode)
- [ ] 5 daqiqa timeout ishlaydi (allaqachon bor)
- [ ] AP parol: `SchoolBell_XXYYZZ` format (allaqachon bor)

**Verification:**
```bash
cd esp32_firmware && pio run
```

**Complexity:** Low

---

### Milestone 3: RTC Diagnostika Kengaytirish

**Hozirgi holat:** `rtc_diagnostics.c` to'liq ishlaydi — drift detection, auto-correct, MQTT alert, battery dead flag, NVS persist. 

**Kerakli o'zgarish:** Minimal — faqat status_reporter orqali dashboard'ga battery status yuborish.

**Fayllar:**
- `src/status_reporter.c` — o'zgartirish: heartbeat payload'ga `rtc_battery_dead` va `rtc_drift_sec` qo'shish

**Acceptance Criteria:**
- [ ] Heartbeat MQTT message'da `"rtc_battery":"dead"` yoki `"rtc_battery":"ok"` bor
- [ ] Heartbeat'da `"rtc_drift":<seconds>` bor
- [ ] Dashboard bu ma'lumotni ko'rsata oladi

**Verification:**
```bash
cd esp32_firmware && pio run
```

**Complexity:** Low

---

### Milestone 4: Offline Mode Xavfsizlik Qoidalari

**Hozirgi holat:** `schedule_manager.c` allaqachon NVS'dan jadval yuklaydi va version-based sync qiladi. Offline'da jadval ishlaydi.

**Kerakli o'zgarish:**
1. Offline'da schedule_manager_update() ni bloklash (faqat online'da ruxsat)
2. 7 kunlik jadval limiti (NVS'da saqlangan jadval 7 kundan oshsa → alert)
3. Jadval sync logic: MQTT subscribe → version compare → yangilash

**Fayllar:**
- `src/schedule_manager.c` — o'zgartirish: online-only update guard, jadval age tracking
- `src/schedule_manager.h` — yangi: `schedule_manager_is_stale()` funksiya

**Acceptance Criteria:**
- [ ] `schedule_manager_update()` faqat WiFi connected bo'lganda ishlaydi
- [ ] Jadval 7 kundan eski bo'lsa → `schedule_manager_is_stale()` = true
- [ ] Stale jadval hali ham ishlaydi (bell chaladi), faqat alert yuboriladi
- [ ] Version compare: agar server version == NVS version → skip

**Verification:**
```bash
cd esp32_firmware && pio run
```

**Complexity:** Low

---

### Milestone 5: Vaqt Sinxronizatsiya Strategiya Yaxshilash

**Hozirgi holat:** SNTP har soatda resync (bor), RTC'ga yozish (bor), boot'da RTC → system time (bor).

**Kerakli o'zgarish:** Minimal — hozirgi logic allaqachon 80% to'g'ri. Faqat:
1. Boot'da RTC invalid bo'lsa (year < 2024) → SNTP kutish, lekin jadval offline ishlashi kerak
2. Bu allaqachon `rtc_diagnostics_init()` da handle qilingan

**Fayllar:** Hech qanday o'zgarish kerak emas — allaqachon to'liq implement qilingan.

**Acceptance Criteria:**
- [ ] Boot: RTC → system time (darhol)
- [ ] WiFi ulanganda → SNTP → RTC save
- [ ] Har soatda SNTP resync + RTC save
- [ ] Har kuni 3:00 da drift check

**Verification:**
```bash
cd esp32_firmware && pio run
```

**Complexity:** None (allaqachon tayyor)

---

### Milestone 6: Reset Button (AP Majburiy Yoqish)

**Hozirgi holat:** `main.c` da panic button ISR bor (GPIO 0), lekin u faqat panic alert uchun.

**Kerakli o'zgarish:** 3 soniya bosib turish → AP mode majburiy yoqish (WiFi qayta sozlash).

**Fayllar:**
- `src/main.c` — o'zgartirish: panic button'ga long-press detection qo'shish (3s = AP mode, short press = panic)

**Acceptance Criteria:**
- [ ] Tugma < 1s = panic alert (hozirgi behavior)
- [ ] Tugma >= 3s = AP mode yoqiladi (WiFi qayta sozlash)
- [ ] AP mode yoqilganda LED/buzzer signal (agar mavjud)

**Verification:**
```bash
cd esp32_firmware && pio run
```

**Complexity:** Medium

---

## 4. Implementation Order (Dependency Graph)

```
Milestone 1 (AP+STA reconnect) ← Milestone 2 (Portal security)
                                ← Milestone 6 (Reset button)
Milestone 3 (RTC status report) — independent
Milestone 4 (Offline security)  — independent  
Milestone 5 (Time sync)         — already done, skip
```

**Tavsiya etilgan tartib:**
1. **Milestone 1** — AP+STA mode (boshqa hamma narsa bunga bog'liq)
2. **Milestone 2** — Captive portal xavfsizlik (M1 bilan birga)
3. **Milestone 6** — Reset button (M1 keyin)
4. **Milestone 3** — RTC status (parallel, mustaqil)
5. **Milestone 4** — Offline guard (parallel, mustaqil)

---

## 5. Test Strategy

### Unit Tests (host-based, optional)
- `schedule_manager_update()` — invalid JSON, version skip, overflow
- URL decode — edge cases (`%00`, `%FF`, empty string)
- RTC drift calculation — boundary values (29s, 30s, 299s, 300s)

### Integration Tests (on hardware)
| Test | Qanday | Kutilgan natija |
|------|--------|-----------------|
| WiFi fail → AP mode | WiFi router'ni o'chirish | 45s ichida AP paydo bo'ladi |
| Captive portal | Telefon bilan AP'ga ulanish | 192.168.4.1 avtomatik ochiladi |
| WiFi save + reboot | Portal'da SSID/pass kiritish | Reboot, yangi WiFi'ga ulanadi |
| AP timeout | 5 daqiqa kutish | AP o'chadi |
| AP retry | 10 daqiqa kutish | WiFi retry, fail → AP qayta |
| STA reconnect | Router'ni yoqish | 30s ichida ulanadi, AP o'chadi |
| Reset button 3s | GPIO 0 ni 3s bosish | AP yoqiladi |
| RTC drift alert | RTC vaqtini 10 min orqaga surish | MQTT alert keladi |
| Offline jadval | WiFi o'chirish, vaqt kelganda | Bell chaladi |
| Schedule version | Eski version yuborish | Update skip qilinadi |

### Stress Tests
- AP'ga 3 ta qurilma bir vaqtda ulanish (max_connection=2, 3-chi reject)
- 50 ta schedule entry (MAX limit)
- WiFi flapping (har 10s connect/disconnect)

---

## 6. Security Checklist

| # | Tekshirish | Holat |
|---|-----------|-------|
| 1 | AP parol kuchli (MAC-based, 13+ char) | ✅ Bor |
| 2 | Captive portal faqat WiFi creds | ✅ Bor |
| 3 | MQTT, jadval, boshqa sozlamalar portal'da YO'Q | ✅ Bor |
| 4 | Input validation (SSID 1-32, pass 8-64) | ✅ Bor |
| 5 | Brute-force himoya (3 attempt → 30s lock) | ⚠️ E'lon qilingan, implement kerak |
| 6 | CSRF token | ❌ Qo'shish kerak |
| 7 | XSS himoya (HTML entity encode) | ⚠️ url_decode bor, lekin output encode yo'q |
| 8 | AP timeout (5 min) | ✅ Bor |
| 9 | Offline'da hech narsa o'zgarmaydi | ⚠️ Guard qo'shish kerak |
| 10 | NVS CRC32 integrity | ✅ Bor |
| 11 | MQTT TLS (mqtts://) | ✅ Bor |
| 12 | OTA host whitelist | ✅ Bor |
| 13 | Security headers (X-Frame, no-cache) | ✅ Bor |
| 14 | max_connection=1 (AP) | ✅ Bor (AP-only), ⚠️ AP+STA da 2 |
| 15 | WiFi creds NVS'da plaintext | ⚠️ Acceptable (ESP32 flash encrypted qo'shish mumkin) |

---

## 7. Risks & Mitigations

| Risk | Impact | Mitigation |
|------|--------|-----------|
| AP+STA mode RAM usage (~30KB extra) | ESP32 4MB flash, 520KB RAM — tight | AP auto-off 2 min keyin; captive DNS task 2KB stack |
| `s_ap_auto_off_timer` undeclared bug | Compile error | M1 da fix qilish (forward declaration yoki restructure) |
| Captive portal iOS/Android detection farqi | Ba'zi telefonlar portal'ni ko'rmaydi | `/generate_204` + `/hotspot-detect.html` + DNS redirect (allaqachon bor) |
| NVS wear (ko'p yozish) | NVS flash sector 100K write cycles | Faqat WiFi save va schedule update'da yozish (kam) |
| RTC batareya alert spam | Har kuni alert | 3 kun keyin "dead" flag, keyin faqat heartbeat'da status |
| WiFi flapping → AP on/off loop | Foydalanuvchi confused | AP faqat 5x STA fail keyin; STA ulanganda 2 min kutish |
| Long-press vs short-press timing | Noto'g'ri trigger | ISR'da faqat press detect, main loop'da duration hisoblash |
| Flash encryption yo'q | NVS plaintext o'qilishi mumkin | Production'da `CONFIG_SECURE_FLASH_ENC_ENABLED` yoqish (alohida task) |

---

## 8. Existing Code Issues (Fix Required)

### Bug: `s_ap_auto_off_timer` undeclared in `event_handler`

`wifi_manager.c` da `event_handler()` funksiyasi (line ~60) `s_ap_auto_off_timer` ni ishlatadi, lekin bu variable file'ning pastki qismida (line ~170) e'lon qilingan. C da forward reference ishlamaydi — compile error bo'lishi kerak.

**Fix:** `s_ap_auto_off_timer` ni file boshiga ko'chirish (boshqa static variables yoniga).

### Issue: AP-only vs AP+STA inconsistency

`main.c` hozir `wifi_manager_stop_sta()` → `ap_provisioning_start()` qiladi (AP-only mode). Lekin `wifi_manager.c` da `wifi_manager_start_apsta()` allaqachon mavjud. Bu ikki approach bir-biriga zid.

**Fix:** `main.c` ni AP+STA mode'ga o'tkazish, `ap_provisioning_start()` ni faqat HTTP+DNS server sifatida ishlatish.

---

## 9. File Change Summary

| Fayl | Action | Sabab |
|------|--------|-------|
| `src/main.c` | Modify | AP+STA fallback, reset button long-press |
| `src/ap_provisioning.c` | Modify | Brute-force, CSRF, portal-only mode |
| `src/ap_provisioning.h` | Modify | `ap_provisioning_start_portal_only()` qo'shish |
| `src/wifi_manager.c` | Modify | `s_ap_auto_off_timer` bug fix, forward decl |
| `src/schedule_manager.c` | Modify | Online-only update guard, stale check |
| `src/schedule_manager.h` | Modify | `schedule_manager_is_stale()` qo'shish |
| `src/status_reporter.c` | Modify | RTC status heartbeat'ga qo'shish |

**Yangi fayllar:** Yo'q. Barcha kerakli fayllar allaqachon mavjud.

---

## 10. Estimated Effort

| Milestone | Vaqt | Lines of Change |
|-----------|------|-----------------|
| M1: AP+STA reconnect | 2-3 soat | ~80 lines (main.c refactor) |
| M2: Portal security | 1-2 soat | ~60 lines (brute-force + CSRF) |
| M3: RTC status report | 30 min | ~15 lines |
| M4: Offline guard | 30 min | ~20 lines |
| M5: Time sync | 0 | Already done |
| M6: Reset button | 1-2 soat | ~50 lines |
| **Jami** | **5-8 soat** | **~225 lines** |
