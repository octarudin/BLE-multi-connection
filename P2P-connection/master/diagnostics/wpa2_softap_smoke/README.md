# Isolated WPA2 SoftAP smoke test

Firmware diagnostik berdasarkan contoh resmi ESP-IDF v5.5.4. Firmware ini
tidak menjalankan BLE, Web Server, atau komponen aplikasi Smart IR.

- SSID: `smart-ir-wpa2-test`
- Password default: `change-me-1234` (ubah melalui `idf.py menuconfig`
  sebelum pengujian)
- Channel: `6`
- Security: WPA2-PSK (konfigurasi resmi, termasuk PMF required)

```powershell
cd diagnostics/wpa2_softap_smoke
idf.py set-target esp32s3
idf.py build flash monitor
```

Hasil interpretasi:

- SSID terlihat: masalah ada pada konfigurasi/integrasi firmware master.
- SSID tidak terlihat: masalah dapat direproduksi pada contoh minimal WPA2,
  sehingga perlu menguji versi ESP-IDF/board lain.

Firmware ini menggantikan aplikasi master pada flash selama pengujian. Flash
kembali project `master` setelah diagnosis selesai.
