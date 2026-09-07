# Getting the device onto the network

The hub URL is already baked into the current build
(`http://192.168.1.10:8787`), so only Wi-Fi is missing. Credentials go into
NVS, so they survive reflashing.

Open the console:

```bash
tools/idf.sh -p /dev/ttyACM0 monitor
```

Wait for the `beacon>` prompt (a couple of seconds after boot), then:

```
beacon-set ssid YOUR-NETWORK
beacon-set pass YOUR-PASSWORD
beacon-save
beacon-reboot
```

Exit the monitor with `Ctrl+]`.

Checks:

- `beacon-show` prints the configuration. The password is only ever shown as
  `(set)`.
- `beacon-preview` draws every screen on the panel and logs how long each
  refresh took — a display check that needs no network.
- The header shows signal bars and the **SYSTEM** screen (hold `OK` from the
  fleet screen, then `OK`) shows SSID, address, RSSI and last sync.

Notes:

- 2.4 GHz only — the ESP32-S3 has no 5 GHz radio. If the network is a
  band-steering SSID it will still associate, but only to the 2.4 GHz side.
- An SSID or password with spaces needs quoting: `beacon-set ssid "My Network"`.
- To point the device at a different hub later:
  `beacon-set hub http://host:8787` then `beacon-save` and `beacon-reboot`.
- If the hub uses a token, `beacon-set token <token>` matches
  `beacon hub --token <token>`.
