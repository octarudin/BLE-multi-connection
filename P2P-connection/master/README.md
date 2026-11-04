# Smart IR BLE Master

Firmware ESP32-S3 N16R8 untuk mengelola maksimal tiga ESP32 BLE slave secara
bergantian. Konfigurasi dan kontrol dilakukan dari web app lokal di laptop
melalui Web Bluetooth; master tidak lagi membuka Wi-Fi AP atau HTTP server.

## Versi

- Firmware: `0.2.9-dev`
- Environment: `dev`
- ESP-IDF: `v5.5.4`
- Smart IR protocol: `v1`

Build `0.2.9-dev` menggunakan satu host NimBLE dual-role. Master menjadi GATT
peripheral terhadap laptop dan GATT central terhadap slave.

## Fitur versi ini

- NimBLE central+peripheral; link slave memakai bonding LE Secure Connections
  Just Works. Service laptop pada environment dev tidak memaksa pairing OS agar
  stabil dengan Web Bluetooth Windows.
- Filter advertising menggunakan manufacturer data dan BLE identity address.
- Provisioning manual maksimal tiga slave melalui Web Bluetooth UI di laptop.
- Antrean delapan command per slave; command state terbaru menggantikan state lama yang sejenis.
- Command konfigurasi, START, STOP/standby, status, dan complete AC state.
- Tidak mengaktifkan Wi-Fi AP, STA, DHCP, atau HTTP server pada master.
- Kredensial Wi-Fi tetap dapat disimpan di NVS dari laptop untuk versi STA/MQTT berikutnya.
- Factory reset dari BLE Web UI atau menahan tombol GPIO2 selama lima detik.
- RGB WS2812 default GPIO48; ubah ke GPIO38 melalui menuconfig untuk board v1.1.
- Partition table dua slot OTA sudah disiapkan; OTA belum diaktifkan pada versi ini.

MQTT dan driver IR Daikin tidak termasuk firmware master versi ini.

## Mode konfigurasi laptop

Master mengiklankan service konfigurasi dengan nama `Smart-IR-Master-XXXXXX`
selama 60 detik ketika laptop belum terhubung. Transport slave bersifat
demand-driven: saat idle master tidak memindai slave. Provision, Queue
configuration, START, STOP, Status, dan AC memulai targeted scan maksimal 15
detik untuk slave yang memiliki antrean. Web tidak perlu disconnect.
Konfigurasi NimBLE menyediakan dua koneksi: satu untuk laptop dan satu untuk
slave yang sedang dilayani.

Setelah ACK atau timeout, scan otomatis berhenti. Command yang belum berhasil
tetap tersimpan dan akan dicoba lagi pada aksi berikutnya atau setelah scan
manual. Jika laptop disconnect saat targeted scan aktif, master menyelesaikan
jendela demand tersebut lalu kembali membuka advertising laptop.

Web app laptop berada di `tools/ble_web_config`. Aktifkan Bluetooth dan jalankan:

```powershell
cd tools\ble_web_config
.\start_server.ps1
```

Kemudian gunakan Chrome/Edge pada `http://localhost:8000`.

## Build

```powershell
C:\esp\v5.5.4\esp-idf\export.ps1
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

## Manufacturer data slave

Semua integer multi-byte little-endian.

```text
Company ID  Magic  Protocol  Flags  Slave ID
FF FF       53 49  01        xx     xx
```

Flags: bit 0 provisioned, bit 1 running, bit 2 pending/error status.

Saat belum provisioned, slave harus menerima command `SET_CONFIG` dari MAC yang
sedang terhubung tanpa mensyaratkan kecocokan `SlaveID` pada header. Sesudah
ACK konfigurasi pertama, semua frame wajib menggunakan ID yang telah diberikan.

## GATT contract

```text
Service: 7f510001-6c6f-4b8d-9a65-534d41525449
Command: 7f510002-6c6f-4b8d-9a65-534d41525449 (write)
Status : 7f510003-6c6f-4b8d-9a65-534d41525449 (notify)
```

Frame:

```text
SOF[1] Version[1] Sequence[1] SlaveID[1] Command[1]
PayloadLengthLE[2] Payload[N] CRC16LE[2]
```

- SOF: `0xA5`
- Maximum payload: 128 byte
- CRC: CRC-16/CCITT-FALSE dari Version sampai akhir payload
- Polynomial `0x1021`, initial value `0xFFFF`

Command IDs dan payload didefinisikan di `main/domain/ble_protocol.h`.

## GATT konfigurasi laptop

```text
Service: 7f520001-6c6f-4b8d-9a65-534d41525449
RX     : 7f520002-6c6f-4b8d-9a65-534d41525449 (write)
TX     : 7f520003-6c6f-4b8d-9a65-534d41525449 (read/notify)
```

Payload konfigurasi adalah JSON UTF-8 yang dibagi menjadi chunk. Header setiap
chunk adalah `MessageID[1] ChunkIndex[1] ChunkCount[1]`. Transport ini menangani
ATT MTU minimum 23 byte dan respons yang lebih panjang dari satu paket.

## Alur koneksi

```text
advertise -> connect -> encrypt/bond -> discover -> subscribe
-> WAKE_STATUS -> pending commands -> ACK/NACK/AC_RESULT
-> PRE_SLEEP_STATUS -> disconnect -> scan
```

Master tidak melakukan retry pada sesi yang sama. Command yang belum menerima
ACK tetap tersedia untuk siklus bangun berikutnya dan disimpan di NVS agar
tidak hilang ketika master mati. Perubahan ID/config baru diterapkan master
setelah ACK dari slave. Connection timeout 3 detik,
WAKE_STATUS timeout 2 detik, dan transaction timeout 3 detik.

## Struktur

```text
main/domain          Entitas dan protokol tanpa ketergantungan transport
main/application     Registry slave, use case, dan command queue
main/infrastructure  NimBLE central/peripheral, NVS, RGB LED, factory reset
tools/ble_web_config Web server lokal dan Web Bluetooth UI untuk laptop
```

## Catatan keamanan

Characteristic konfigurasi dan slave hanya digunakan melalui link BLE yang
Link master-slave dienkripsi dan dibonding menggunakan LE Secure Connections
Just Works sehingga belum memberikan perlindungan MITM. Service konfigurasi
laptop pada environment dev tidak memaksa pairing Windows; akses produksi perlu
ditambah autentikasi aplikasi sebelum menyimpan rahasia. Manufacturer data tetap
bersifat publik.
