# BLE Mesh: one master and scalable slaves

This directory contains an ESP-IDF BLE Mesh implementation of the roles used
by `P2P-connection`:

- `master`: ESP32-S3 provisioner and Smart IR vendor client.
- `slave`: ESP32 node and Smart IR vendor server; flash the same firmware to
  every slave board.

The master automatically provisions matching slaves and assigns sequential IDs
starting at 1. Its capacity follows ESP-IDF's
`CONFIG_BLE_MESH_MAX_PROV_NODES` setting (10 by default), rather than the
number of boards currently available. Commands use acknowledged vendor
messages and preserve the P2P command meanings: configure, start, stop, set AC
state, and get status.

The master owns a persistent application command queue for cycling slaves.
`stop` and AC requests are saved before transmission, then retried periodically
until the slave acknowledges them. Delivery therefore does not depend on the
master receiving a WAKE/SLEEP event. Each slave also repeats its wake
announcement until the master replies with queued work (or an empty queue).
A stopped slave stays awake in idle mode.

## First run

1. Build and flash `slave` to every ESP32 slave board.
2. Build and flash `master` to the ESP32-S3.
3. Monitor the master until the available slaves are provisioned and configured.
4. Configure the cycle with `timing 1 10000 5000`, then use `start 1`.
5. Enter `list` to inspect awake/sleep and queue state.

For `awake_ms=0`, the slave opens a short 3-second Mesh handshake window after
boot. If the queue is empty it sleeps again immediately after synchronization;
if `stop` is queued it remains awake in idle mode.

If boards contain provisioning data from an older run, erase their flash
before repeating the initial provisioning.
