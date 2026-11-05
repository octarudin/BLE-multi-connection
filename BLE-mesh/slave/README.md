# Smart IR BLE Mesh slave

Flash this same ESP-IDF project to every ESP32 slave board. The device UUID is
made unique from the Bluetooth MAC address, so the ESP32-S3 master can
provision multiple copies and assign sequential IDs.

The metadata display format follows the P2P slave, while BLE Mesh keeps an
independent firmware version:

- Firmware: `0.2.2-dev`
- Environment: `dev`
- Protocol: `3`
- Transport: `BLE Mesh`

```powershell
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

The node persists its Mesh provisioning data, assigned ID, running state, and
last AC state in NVS. It also persists the cycle timing. `start` enables
timer-based deep-sleep cycling; `stop` disables cycling and leaves the node
awake in idle mode. The default timing is 10,000 ms asleep and 5,000 ms awake.
After every deep-sleep reset, the slave repeats its wake announcement every
500 ms until `WAKE_SYNC` arrives or the wake window ends. BLE Mesh sequence
numbers are reserved in persistent blocks so these announcements are not
rejected as replayed packets after a reset.
GPIO 5 is the default IR output and can be changed in
`menuconfig` under **Smart IR BLE Mesh slave**.

The Daikin encoder remains intentionally unimplemented because Daikin uses
multiple incompatible frame families. AC commands are validated and stored,
then answered with `NACK_UNSUPPORTED`, just as in the existing P2P sample.
