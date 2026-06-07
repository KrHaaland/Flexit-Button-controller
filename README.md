# Flexit-Button-controller

ESP32-C5 button panel for the **Flexit Tradition-S** ventilation unit. Replaces or parallels the original 5-button control panel with an isolated emulator that adds **Wi-Fi (2.4 GHz + 5 GHz)** and **MQTT** for smart-home integration.

The ESP32-C5 is currently one of the few cheap ESP modules with dual-band Wi-Fi 6, which lets the panel live on either band depending on your home network and signal coverage.

## What it does

The board exposes 5 channels, each mirroring one button + indicator LED on the original Flexit Tradition-S panel:

- **5 button outputs** drive AQY212S solid-state opto-couplers in parallel with on-board tactile switches. A short pulse on a channel looks electrically identical to a finger press, in galvanic isolation from the Flexit unit.
- **5 LED inputs** read the indicator-LED state back from the panel net.

You get three ways to interact with the panel:

1. The physical tactile buttons on the PCB (works without firmware).
2. A self-served web UI (Wi-Fi setup, manual trigger, live LED dots, pulse tuning, MQTT setup).
3. MQTT — subscribe to LED state changes, publish to fire buttons.

## Hardware

- **Module:** ESP32-C5-Mini (USB-C, dual-band Wi-Fi 6, 4 MB flash, no PSRAM)
- **Isolation:** 5x AQY212S solid-state relays (LED-driven MOSFET output, ~3 ms switching)
- **Connector to Flexit:** Micro-MaTch 2x6 (J1), carrying 5 button signals + 5 LED signals + 5 V supply + GND, mating with the cable on the back of the original Flexit panel.
- **PCB:** KiCad files under [`PCB/`](PCB/) (Gerbers/BOM/positions for the fab in [`PCB/production/`](PCB/production/)).

### Pin mapping (set in `FW/ESP32-C5-Flexit/src/main.cpp`)

| Channel | Trigger out | LED in |
|--------:|:-----------:|:------:|
| 1       | IO4         | IO5    |
| 2       | IO3         | IO6    |
| 3       | IO2         | IO7    |
| 4       | IO1         | IO8    |
| 5       | IO0         | IO9    |

Trigger outputs idle LOW and pulse HIGH for ~300 ms (configurable from the UI) to emulate a press. LED inputs are read with `INPUT_PULLDOWN`; the Flexit drives them HIGH when an indicator is lit.

## Build & flash

Requires [PlatformIO](https://platformio.org/) (`pip install platformio` or the VSCode extension).

```bash
cd FW/ESP32-C5-Flexit
pio run                       # compile
pio run -t upload             # flash via USB-C
pio device monitor -b 115200  # serial log
```

The C5 enumerates as `/dev/ttyACM*` (built-in USB Serial JTAG, no driver needed).

## First-time setup

1. Power the ESP32-C5 over USB-C, or via the J1 connector once mounted.
2. With no saved Wi-Fi, the board comes up as an access point:
   - SSID: `Flexit-Setup`
   - Password: `fibonacci` (default; the Wi-Fi form lets you set a custom one if you don't want the documented value).
3. Join that AP and open <http://192.168.4.1/> (also reachable as `http://flexit.local/` via mDNS).
4. In the **Wi-Fi** section, enter your home SSID + password and click *Save & reboot*. You can optionally set a custom AP fallback password in the same form — it persists in NVS. The board joins your network and is then reachable on its STA IP or via `flexit.local`.

Wi-Fi credentials and the AP-password override are stored in NVS and survive a reflash. *Forget & reboot to AP* wipes the Wi-Fi creds but keeps the AP password override.

If the home network is briefly unreachable at boot, the board lands in AP mode but periodically retries (every 5 min) so a transient router outage doesn't strand it.

## Web interface

| Section            | What it does |
|--------------------|--------------|
| **Knapper**        | 5 trigger buttons + 5 live LED indicators (polled every 400 ms). |
| **Pulse duration** | Length of the simulated button press (20–2000 ms, default 300 ms). Useful if a Flexit function requires a long-press. |
| **MQTT**           | Broker host/port, optional user/password, base topic, heartbeat republish interval, live connection status, MAC-derived client ID. |
| **Wi-Fi**          | SSID + password, *Save & reboot* into STA, *Forget* clears creds. Banner at top shows current STA/AP state, RSSI, IP. |

Everything except Wi-Fi changes applies live without reboot.

## MQTT

When the board is in STA mode (joined to your Wi-Fi) and a broker host is configured, it connects to the broker and stays connected with auto-reconnect and a Last Will. With base topic `flexit` (default — configurable in the UI):

| Topic                      | Direction | Payload                  | Notes |
|----------------------------|-----------|--------------------------|-------|
| `flexit/led/1..5/state`    | publish   | `ON` / `OFF`             | Retained. Sent on every transition; also republished periodically if heartbeat > 0. |
| `flexit/sw/1..5/trigger`   | subscribe | any (optional `<ms>`)    | Any payload triggers a pulse on that channel. A numeric payload overrides the configured pulse duration for that single trigger. |
| `flexit/status`            | publish   | `online` / `offline`     | LWT, retained. |

Client ID is `flexit-<MAC>` (12 hex chars from the chip's eFuse MAC) — globally unique per ESP32. Use it in your broker's ACL.

### Home Assistant example

```yaml
mqtt:
  binary_sensor:
    - name: "Flexit LED 1"
      state_topic: "flexit/led/1/state"
      payload_on: "ON"
      payload_off: "OFF"
    # ... repeat for 2..5
  button:
    - name: "Flexit button 1"
      command_topic: "flexit/sw/1/trigger"
    # ... repeat for 2..5
```

## Firmware updates (OTA)

Once the device is on your Wi-Fi (STA mode), you don't need USB again — there are two ways to push a new build:

### Web upload

1. Build locally: `cd FW/ESP32-C5-Flexit && pio run`. The artifact is `.pio/build/esp32-c5-devkitc1-n4/firmware.bin`.
2. Open the device's web UI (`http://flexit.local/` or its STA IP).
3. Scroll to the **Firmware** section. The current version, build timestamp and free slot space are shown.
4. Pick the new `firmware.bin`, click **Upload & reboot**. The progress bar tracks the upload, then the board reboots into the new image. The page auto-reloads after ~8 s.

### PlatformIO push

```bash
cd FW/ESP32-C5-Flexit
pio run -e esp32-c5-devkitc1-n4-ota -t upload
```

This uses the espota protocol over mDNS at `flexit.local:3232`.

### Partition layout

Switched from the Arduino default (`default.csv`, 1.25 MB per app slot) to `min_spiffs.csv` (1.875 MB per slot, two slots, plus 128 KB SPIFFS). NVS stays at offset `0x9000` so saved Wi-Fi / MQTT / pulse settings survive the migration when you flash the OTA-enabled image for the first time.

### Robustness

The HTTP OTA path is hardened against the common failure modes:

- The 5 button-trigger pins are forced LOW at the start of every upload, and `/trigger` + MQTT-fired triggers are ignored during the upload — a press in flight can't keep the opto-coupler latched through the multi-second multipart parse.
- Idle timeout (10 s) and total timeout (120 s) abort slow-loris uploads.
- A sticky abort flag short-circuits the rest of the upload as soon as `Update.begin`, a short write, or `Update.end` reports an error — no point in streaming 1 MB just to fail at the end.
- MQTT publishes `offline` retained to `<base>/status` and disconnects gracefully before the reboot, so subscribers see the flap immediately instead of waiting out the 30 s keepalive.
- Wi-Fi disconnect + `server.close()` + an 800 ms settle before `ESP.restart()` give the TCP FIN and the HTTP 200 response enough time to flush on a marginal link.
- A Wi-Fi event handler re-binds mDNS and ArduinoOTA whenever the STA interface gets a new IP, so wireless reflash keeps working after a router reboot or AP roam.

### Security model

There's **no authentication** on either OTA path or on the other config endpoints. The assumption is that your Wi-Fi LAN is trusted (this is a home device behind your router, not on the open internet). Anyone with route to the device on TCP 80 or 3232 can re-flash it.

The AP fallback PSK is `fibonacci` by default — yes, the same one this README mentions, on purpose. The Wi-Fi form lets you override it per device if you'd rather not advertise it in the source. Override is persisted in NVS.

If you need stricter controls (HTTP Basic auth on `/update`, an ArduinoOTA password, signed firmware images, broker ACLs, VLANs), open an issue.

## Repo layout

```
.
├── FW/ESP32-C5-Flexit/      PlatformIO project (Arduino framework)
│   ├── src/main.cpp         Firmware
│   └── src/index_html.h     Embedded web UI (single page)
├── PCB/                     KiCad schematic + PCB design
│   └── production/          Gerbers, BOM, positions for fab
└── knappepanel_flexit.kicad_pro
```

## Notes & quirks

- The board has 5 on-board tactile switches in parallel with the opto outputs, and 5 indicator LEDs on the LED nets, so it doubles as a stand-alone bench panel.
- ESP32-C5 boot prints `MSPI Timing: Failed to allocate dummy cacheline for PSRAM memory barrier!` — harmless, the C5-Mini has no PSRAM but the SDK looks for it.
- Verified against a Flexit Tradition-S. Other Flexit models with a different button matrix or LED drive scheme may need pin or polarity changes (see `LED_ACTIVE_HIGH` in `src/main.cpp`).

## License

No license set yet — treat as "all rights reserved" until one is added. Open an issue if you'd like a specific one (MIT / Apache-2.0 / CC-BY-SA for the hardware, etc.).
