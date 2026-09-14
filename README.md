# tigo-taptap-unraid-ha
# Tigo Optimizer Local Monitoring with ESP32, TapTap & Home Assistant

This project documents an end-to-end setup for **fully local, offline monitoring** of Tigo TS4 optimizers using:

- A **MAX485 + ESP32** RS-485 bridge tapping the Tigo CCA GATEWAY bus.
- The **litinoveweedle fork** of [`taptap`](https://github.com/litinoveweedle/taptap).
- The [`taptap-mqtt`](https://github.com/litinoveweedle/taptap-mqtt) bridge to Home Assistant MQTT.
- An **Unraid** host running the `taptap-mqtt` container.
- **Home Assistant** with the MQTT integration and auto-discovery.

The goal is to get per-module power/voltage/temperature data into Home Assistant with a 100% local path (Tigo cloud optional), and to provide a practical, step-by-step guide for others.

> This is a personal project, not affiliated with Tigo or the upstream authors. Use at your own risk and respect electrical safety.

## 1. Hardware overview

- Tigo CCA (or compatible controller) with GATEWAY RS-485 port.
- One or more Tigo TS4-A-02 optimizers (example here: 9 modules on String B).
- MAX485 (or 3.3 V RS-485 transceiver like MAX3485/SP3485) module.
- ESP32 dev board (e.g. devkit-style) with WiFi.
- Unraid server on the same LAN as the ESP32 and Home Assistant.

Basic signal path:

- Tigo CCA GATEWAY RS-485 A/B → MAX485 → ESP32 UART2 → WiFi → Unraid → `taptap` → `taptap-mqtt` → MQTT → Home Assistant.

> The ESP32 acts purely as a **transparent RS-485 to TCP bridge**. It does not interpret or inject protocol frames; all decoding happens in `taptap`.

## 2. RS-485 tap wiring (receive-only)

This setup follows the recommendations in the upstream `taptap` docs: tap the existing RS-485 bus in parallel and avoid adding a third termination.[cite:1]

**MAX485 → ESP32 (receive-only):**

- RO → ESP32 RX (UART2, e.g. GPIO16).
- DI → ESP32 TX (UART2, e.g. GPIO17) — not used for transmit, but wired.
- RE → GND (active-low receive enable).
- DE → GND (driver disabled).
- VCC → 3.3 V (or 5 V if using a genuine 5 V MAX485 and level-tolerant board).
- GND → ESP32 GND.

**MAX485 A/B:**

- Connect A/B **in parallel** to the Tigo CCA GATEWAY A/B terminals.
- Do not add another termination resistor; keep the CCA and last TAP as the only terminations.[cite:1]

## 3. ESP32 firmware – RS-485 to TCP bridge

The ESP32 firmware should:

- Configure UART2 at 38400 baud, 8N1 (matching Tigo GATEWAY bus).
- Continuously forward bytes between UART2 and a TCP socket:
  - TCP server listening on e.g. `192.168.1.190:7160`.
- Handle one client at a time (the Unraid host running `taptap`/`taptap-mqtt`).

There are many examples of "ESP32 UART to TCP bridge" sketches; this project assumes you already have one running and verified by:

```bash
taptap observe --tcp 192.168.1.190 --port 7160
```

from a machine on the same LAN as the ESP32.

## 4. Building the taptap-mqtt Docker image

This project uses the **litinoveweedle fork** of `taptap` because `taptap-mqtt` depends on its extended features (module serial discovery, etc.).[cite:2][cite:24]

Example multi-stage Dockerfile:

```Dockerfile
FROM rust:1-bookworm AS builder
RUN apt-get update && apt-get install -y --no-install-recommends \
    pkg-config libudev-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /build
RUN git clone [https://github.com/litinoveweedle/taptap.git](https://github.com/litinoveweedle/taptap.git) .
RUN cargo build --release

FROM python:3.11-slim-bookworm
RUN apt-get update && apt-get install -y --no-install-recommends \
    git ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /app
RUN git clone [https://github.com/litinoveweedle/taptap-mqtt.git](https://github.com/litinoveweedle/taptap-mqtt.git) .
RUN pip install --no-cache-dir -r requirements.txt
COPY --from=builder /build/target/release/taptap /usr/local/bin/taptap
CMD ["python3", "/app/taptap-mqtt.py"]
```

Build on Unraid (or any build host):

```bash
docker build -t taptap-mqtt:latest .
```

## 5. Unraid container configuration

On Unraid:

1. Add a folder for config:

   ```bash
   mkdir -p /mnt/user/appdata/taptap-mqtt
   ```

2. In the Docker tab → **Add Container**:
   - Name: `TapTap-Mqtt`
   - Repository: `taptap-mqtt:latest`
   - Network: `Host`
   - Restart policy: `unless-stopped`
   - Command:
     - Command: `python3`
     - Post Arguments: `/app/taptap-mqtt.py`

3. Paths:
   - Bind only `config.ini` from appdata:

     - Host path: `/mnt/user/appdata/taptap-mqtt/config.ini`
     - Container path: `/app/config.ini`
     - Access: `Read/Write`

> Note: Binding the whole `/app` directory will hide the `taptap-mqtt.py` script; bind just `config.ini` instead.

## 6. config.ini example

Copy `config.ini.example` from the repo, rename to `config.ini`, and adjust:

```ini
[MQTT]
SERVER = 192.168.1.14
PORT = 1883
QOS = 1
TIMEOUT = 5
USER = taptapmqtt
PASS = taptapmqtt

[TAPTAP]
LOG_LEVEL = warning
BINARY = /usr/local/bin/taptap
SERIAL =
ADDRESS = 192.168.1.190
PORT = 7160
MODULES = B:Roof_SW_1:4-E51A7FL, B:Roof_SW_2:4-E24114Z, B:Roof_SW_3:4-E51A85Z,
          B:Roof_SW_4:4-E513FEP, B:Roof_SW_5:4-E246C2J, B:Roof_SW_6:4-E23F5AJ,
          B:Roof_SW_7:4-996FD3J, B:Roof_SW_8:4-996F8AR, B:Roof_SW_9:4-8E2929Y
TOPIC_PREFIX = taptap
TOPIC_NAME = tigo1
TIMEOUT = 180
UPDATE = 15
STATE_FILE = ./taptap.json

[HA]
DISCOVERY_PREFIX = homeassistant
DISCOVERY_LEGACY = false
BIRTH_TOPIC = homeassistant/status
NODES_AVAILABILITY_ONLINE = false
NODES_AVAILABILITY_IDENTIFIED = false
STRINGS_AVAILABILITY_ONLINE = false
STRINGS_AVAILABILITY_IDENTIFIED = false
STATS_AVAILABILITY_ONLINE = false
STATS_AVAILABILITY_IDENTIFIED = false
NODES_SENSORS_RECORDER = energy
STRINGS_SENSORS_RECORDER = energy
STATS_SENSORS_RECORDER = energy

[RUNTIME]
MAX_ERROR = 15
RUN_FILE = /run/taptap/taptap.run
```

Key points:

- `[MQTT]`: points to Home Assistant’s MQTT broker and uses a dedicated HA user `taptapmqtt`.[cite:43][cite:48]
- `[TAPTAP]`:
  - `ADDRESS`/`PORT`: ESP32 RS-485 bridge (TCP).
  - `BINARY`: path to the `taptap` binary from the builder image.
  - `MODULES`: `STRING:NAME:SERIAL` per optimizer; used for naming and mapping.[cite:24]
- `[HA]`: controls HA auto-discovery topics and availability behavior.

Restart the container after changes:

```bash
docker restart TapTap-Mqtt
```

## 7. Home Assistant integration

Requirements:

- MQTT integration in HA pointing at the same broker (`SERVER`, `PORT`, `USER`, `PASS`).
- HA user `taptapmqtt` (Settings → People → Users) with MQTT access.[cite:43][cite:48]

Once the container is running:

- Check MQTT topics from a terminal:

  ```bash
  mosquitto_sub -h 192.168.1.14 -p 1883 \
    -u taptapmqtt -P taptapmqtt \
    -t 'taptap/#' -v
  ```

- In HA:
  - Go to **Settings → Devices & Services → MQTT**.
  - A device like `taptap tigo1` should appear with per-module nodes and overall stats.[cite:24][cite:87]

You can then:

- Rename devices (e.g. `PV Roof – B1`, `PV Roof – B2`) and key entities (power, voltage, temperature).
- Build Lovelace dashboards for single module and string/overall views.

## 8. Troubleshooting

- `ConnectionRefusedError`:
  - Check MQTT `SERVER`, `PORT`, `USER`, `PASS`.
  - Verify with `mosquitto_sub` from the Unraid host.[cite:49][cite:51]

- `taptap observe --tcp ...` errors:
  - Confirm ESP32 IP and port.
  - Check RS-485 wiring (A/B not swapped, RE/DE tied low, no extra termination).[cite:28][cite:30]

- No `observe` output but `peek-bytes` works:
  - Some frames (especially power reports) are sparse; leave `observe` running for several minutes or test under daylight conditions.[cite:24][cite:29]

- Old HA entities hanging around:
  - Delete/disable old devices in **Settings → Devices & Services → MQTT**.
  - Optionally clear retained MQTT discovery topics using MQTT Explorer or `mosquitto_pub` to remove obsolete `homeassistant/.../config` topics.[cite:75][cite:87]

## 9. Unraid polish (icon & WebUI)

- Icon:
  - Copy a PNG into:

    ```bash
    cp /mnt/user/appdata/taptap-mqtt/tigo.png \
       /boot/config/plugins/dockerMan/images/taptap-mqtt.png
    ```

  - In Unraid Docker Edit (Advanced):
    - Icon URL:

      ```text
      /plugins/dynamix.docker.manager/images/taptap-mqtt.png
      ```

- WebUI:
  - Set WebUI URL to your HA dashboard, e.g.:

    ```text
    http://192.168.1.14:8123/dashboard/solar
    ```

---

This README reflects a working setup based on an ESP32 RS-485 bridge, the litinoveweedle taptap fork and taptap-mqtt, Unraid, and Home Assistant, with all configuration kept in `/mnt/user/appdata/taptap-mqtt/config.ini` on the Unraid host.
