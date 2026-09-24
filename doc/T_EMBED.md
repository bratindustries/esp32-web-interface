# T-Embed CAN dashboard

This target adds a local LCD view to the existing CAN web interface. It is
intended for the **original LILYGO T-Embed** (ESP32-S3, 320 × 170 ST7789,
encoder on GPIO1/2/0). The T-Embed CC1101 and T-Embed Plus have different
hardware and are not covered by this pin configuration.

The firmware defaults to CAN RX GPIO16, TX GPIO17 and 500 kbit/s, matching
the ZombieVerterDisplay reference. Pins and speed remain configurable in the
web interface.

## Build and first flash

From the repository root, with PlatformIO installed:

```sh
pio run -e esp32_tembed
pio run -e esp32_tembed -t buildfs
pio run -e esp32_tembed -t upload --upload-port COM19
pio run -e esp32_tembed -t uploadfs --upload-port COM19
```

Replace `COM19` with the T-Embed's actual serial port. The SPIFFS upload is
required for the web dashboard. The firmware starts in CAN mode on GPIO16/17,
node 1, 500 kbit/s. Join the board's `ESP-xxxxx` Wi-Fi network and open
`http://192.168.4.1/`. In **Settings → Interface**, scan for the ZombieVerter
and choose the right node as the boot default. Change CAN speed if necessary.
For a board that previously stored a different pinout in `settings.json`, use
the new T-Embed CAN preset in Settings and save it.

The main dashboard page mirrors all five values selected under **Settings →
Web Interface**. Its layout follows ZombieVerterDisplay: the first value is a
large cyan radial instrument on the right, while values two through five are
four compact, colour-coded radial gauges directly on the black display face,
without surrounding boxes. All gauge arcs use
smooth, rounded ends. Gauges whose range crosses zero use zero as their centre
point, so positive drive power and negative regenerative power fill in opposite
directions. Power draw is red and regenerative power is green; at zero the
power gauge returns to its neutral track. A thin header
shows mode, status, CAN connection and any active fault. The metrics page also
shows the selected values in a simple grid. The history page plots
the selected favorite value, with its observed minimum and maximum, across up
to 32 readings; turn the encoder there to choose another favorite. Additional
pages show three starred **Spot Values** each, with sparklines for numeric
readings. Press the encoder to advance pages; on other pages, turning also
changes pages. If there are no favorites yet, history shows the first metric.

Web selections come from `/favorites.json` and `/uiprefs.json` after saving;
the gauge uses matching numeric min/max values from `/gauges.json` when present.
Percentage and SOC readings otherwise use 0–100; other values use their
observed range, labeled `AUTO RANGE`. Up to 24 favorites and 32 unique polled
values are supported. History is stored in RAM and begins accumulating after
boot or a settings change. Enumerated state values show text without a trend.
Missing or stale readings show `--` after six seconds, and invalid reads clear
the trend. CAN firmware updates pause screen polling while the update task owns
the bus. The LCD uses the existing CAN SDO reads and decoded virtual CAN values.

This is an initial firmware implementation; screen, encoder, and CAN operation
need an on-vehicle hardware check before relying on the readings while driving.
