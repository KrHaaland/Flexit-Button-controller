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
   - Password: `fibonacci`
3. Join that AP and open <http://192.168.4.1/> (also reachable as `http://flexit.local/` via mDNS when on the same LAN).
4. In the **Wi-Fi** section, enter your home SSID + password and click *Save & reboot*. The board joins your network and is then reachable on its STA IP, or via `flexit.local`.

Wi-Fi credentials are stored in the ESP32 NVS and survive a reflash. *Forget & reboot to AP* wipes them.

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
