# ESP32 School Bell Firmware

Maktab qo'ng'iroq qurilmasi firmware — PlatformIO + ESP-IDF.

## Tez boshlash

1. PlatformIO o'rnating: https://platformio.org/install
2. Config yarating:
```bash
cp include/config.h.example include/config.h
# config.h ni o'z server/WiFi ma'lumotlaringiz bilan to'ldiring
```
3. Kompilatsiya va yuklash:
```bash
pio run -t upload
```
4. Monitor:
```bash
pio device monitor
```

## Hardware ulash

| ESP32 Pin | Funksiya |
|-----------|----------|
| GPIO 13 | Relay (qo'ng'iroq) |
| GPIO 21 | RTC SCL (I2C) |
| GPIO 22 | RTC SDA (I2C) |
| GPIO 4 | RTC RST |

## Funksiyalar

- ✅ WiFi auto-connect + AP provisioning (ulanolmasa o'zi hotspot bo'ladi)
- ✅ MQTT (TLS) — server bilan real-time aloqa
- ✅ 7 kunlik jadval NVS'da — offline ishlaydi
- ✅ Bayram kunlari — avtomatik o'chirish
- ✅ OTA firmware yangilash
- ✅ RTC (DS1307) — hardware soat
- ✅ RTC diagnostika — batareya holati monitoring
- ✅ SNTP vaqt sinxronizatsiya
- ✅ Heartbeat (30s) — online/offline status

## WiFi Provisioning

ESP32 WiFi'ga ulanolmasa:
1. "SchoolBell_XXXX" nomli hotspot yoqiladi
2. 192.168.4.1 ga brauzerdan kiring
3. WiFi SSID va parolni kiriting
4. Save → ESP32 qayta ishga tushadi

## MQTT Topics

| Topic | Yo'nalish | Funksiya |
|-------|-----------|----------|
| `devices/{id}/command` | Server→ESP32 | ring, reboot, ota |
| `devices/{id}/schedule` | Server→ESP32 | Jadval sync |
| `devices/{id}/holidays` | Server→ESP32 | Bayram ro'yxati |
| `devices/{id}/status` | ESP32→Server | Heartbeat |
| `devices/{id}/bell_log` | ESP32→Server | Qo'ng'iroq tarixi |
| `devices/{id}/alert` | ESP32→Server | RTC drift alert |
