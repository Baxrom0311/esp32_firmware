# Burxon ESP32 Firmware — Security Notes

This document covers the security-sensitive parts of the firmware: signed OTA,
TLS for MQTT, OTA host allowlisting, and rollback. Follow it whenever you cut
a release or rotate keys.

## 1. Signed OTA (firmware signing)

OTA images **must** be signed in production so the bootloader / app loader
rejects unsigned or tampered binaries.

The relevant sdkconfig flags are currently *commented out* in
`sdkconfig.defaults` because development builds use unsigned images:

```
# CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y
# CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y
```

For a production release:

1. Generate a signing key **once**, on a machine that is not the build host,
   and store it in your secrets manager. Never commit it.

   ```bash
   espsecure.py generate_signing_key --version 1 secure_boot_signing_key.pem
   ```

2. Place the key at the path your build expects (e.g. `secure/`) and add the
   path to `.gitignore` (already covered by `secure/` and `*.pem` patterns).

3. Enable the signing flags in `sdkconfig.defaults` (uncomment the two lines
   above) and set the key path:

   ```
   CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y
   CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y
   CONFIG_SECURE_BOOT_SIGNING_KEY="secure/secure_boot_signing_key.pem"
   ```

4. Build with PlatformIO / `idf.py build`. The build system will sign the app
   image automatically. Verify with:

   ```bash
   espsecure.py verify_signature --version 1 \
       --keyfile secure/secure_boot_signing_key.pem \
       .pio/build/esp32/firmware.bin
   ```

5. Upload the signed binary to your OTA server. Devices that already run a
   signed firmware will refuse unsigned updates.

### Key rotation

Rotating the signing key requires:

1. Issuing a transitional firmware that trusts both the old and the new key
   (multi-key support; see `CONFIG_SECURE_BOOT_SIGNING_KEY`).
2. Once all devices have updated, ship a release that only trusts the new
   key and decommission the old one.

Plan rotation events ahead — devices that miss the transitional release will
be stuck on the old key.

## 2. MQTT over TLS (`mqtts://`)

The example config in `include/config.h.example` recommends:

```c
#define MQTT_BROKER_URI         "mqtts://bell.boos.uz:8883"
```

Live `include/config.h` may use `mqtt://` against an internal IP for
development; **do not ship that to fleets**. To enable TLS:

1. Update the broker on the server to listen on `8883` with a valid cert
   (Let's Encrypt or your internal CA).
2. Make sure `CONFIG_MQTT_TRANSPORT_SSL=y` is in `sdkconfig.defaults`
   (already set).
3. Verify the device trust store. We currently rely on the IDF certificate
   bundle (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`) for public CAs. For an
   internal CA, populate `include/ca_cert.h` with the PEM bundle and compile
   it into the binary.
4. Switch `MQTT_BROKER_URI` in `include/config.h` to `mqtts://...`.
5. Confirm session resumption is on (`CONFIG_MBEDTLS_SSL_SESSION_TICKETS=y`)
   to keep the TLS handshake under ~1 s on reconnect.

## 3. OTA host allowlist

`include/config.h` declares two allowed OTA hosts:

```c
#define OTA_ALLOWED_HOST        "bell.boos.uz"
#define OTA_ALLOWED_HOST_2      "146.190.210.125"
```

`src/ota_manager.c` validates the URL host against this allowlist before
fetching. **Do not** broaden the allowlist to include arbitrary CDNs unless
you also pin the TLS certificate of the new host.

## 4. Boot validation and rollback

The OTA manager calls `esp_ota_mark_app_invalid_rollback_and_reboot` on early
boot failure (see `src/ota_manager.c`). This means: if a freshly-written app
panics before it can mark itself valid, the bootloader rolls back to the
previous slot on the next boot.

Two consequences:

- **Always** run the post-OTA self-check (network reachable, MQTT connect,
  schedule load) before calling `esp_ota_mark_app_valid_cancel_rollback`.
- A bricked image will reboot loop until rollback fires; ensure the watchdog
  is short enough (`CONFIG_ESP_TASK_WDT_TIMEOUT_S=30`) that rollback happens
  in minutes, not hours.

## 5. Provisioning credentials

- WiFi credentials live in NVS (`NVS_NAMESPACE_WIFI`). They are not
  encrypted by default (`CONFIG_NVS_ENCRYPTION=n`). For high-risk
  deployments, enable NVS encryption with a key stored in eFuse.
- MQTT device passwords are issued by the backend via
  `/api/v1/device/credentials/`. The raw password is only returned at
  creation/regeneration; the backend stores a hash. Do not log raw passwords
  on the device side.

## 6. Things never to commit

- `secure/`, `*.pem`, `*.key`, `secure_boot_signing_key.*`
- `include/ca_cert.h` if it embeds a private internal CA
- Any file matching `.env*` (handled by repo `.gitignore`)
- Captured serial logs that include MQTT passwords

## 7. Quick checklist for a release

- [ ] `FW_VERSION` in `include/config.h` bumped
- [ ] Signing key available and signing flags enabled
- [ ] OTA URL points to `https://` host on the allowlist
- [ ] MQTT URI uses `mqtts://` if devices are not on a private network
- [ ] Verified rollback works (intentionally brick a build, confirm
      previous slot resumes)
- [ ] No unsigned image published to the OTA bucket
