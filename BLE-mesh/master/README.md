# Smart IR BLE Mesh master

ESP32-S3 provisioner and vendor-model client for Smart IR ESP32 slaves. It
automatically discovers UUIDs beginning with `53 49`, provisions matching
devices, installs the application key, binds the vendor server, and assigns
sequential slave IDs starting at 1.

The application has no hard-coded two-slave limit. Its capacity follows
`CONFIG_BLE_MESH_MAX_PROV_NODES` in ESP-IDF (`10` by default and configurable
through `idf.py menuconfig`). Because the P2P-compatible slave ID remains an
8-bit value, set this option no higher than 254.

The metadata display format follows the P2P master, while BLE Mesh keeps an
independent firmware version:

- Firmware: `0.2.3-dev`
- Environment: `dev`
- Protocol: `3`
- Transport: `BLE Mesh`

## Build and flash

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

After both nodes report `ready=yes`, use the serial monitor commands:

```text
list
timing 1 10000 5000
start 1
status 1
stop 2
ac 1 1 1 24 0 0
```

`timing` stores the sleep and awake durations independently from `start`.
While a cycle is active, `stop` and AC commands are always persisted before
transmission. The master retries queued work every 500 ms until it receives a
matching acknowledgment, independently of its last awake/sleep observation.
Once `stop` is applied, the slave stays awake in idle mode and does not enter
deep sleep again.

`link=awake` means the master most recently received a lifecycle WAKE event or
a valid command response. `link=sleeping` is used only after an explicit SLEEP
event. A command timeout produces `link=unknown`, since packet loss alone does
not prove that the slave is sleeping.

With `awake_ms=0`, the slave keeps only a short handshake window. It sleeps
immediately after the master reports an empty queue, or after queued work has
been handled.

The `ac` arguments are slave ID, power, mode, temperature, fan, and swing.
The sample validates and stores the state, but returns `UNSUPPORTED` until the
exact Daikin IR frame family is implemented, matching the current P2P sample.

Provisioning and the node table persist in NVS. Erase flash on the master and
slave boards when starting a completely new Mesh network.
