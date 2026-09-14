# Tigo TapTap Unraid Home Assistant Bridge

A practical guide for monitoring Tigo TS4 optimizer data locally in Home Assistant using an ESP32, an RS-485 transceiver, Unraid, MQTT, and the TapTap software stack.

This project was built to provide a local path from the Tigo CCA/GATEWAY bus to Home Assistant. It is intended for people who want useful optimizer data without making their Home Assistant installation depend on the Tigo cloud.

> This is a community project and is not affiliated with or endorsed by Tigo. It is provided as-is. Read the safety notes before connecting anything to a PV system.

## What you will build

The completed setup looks like this:

```text
Tigo CCA/GATEWAY RS-485 bus
          |
          v
MAX485 transceiver (receive-only tap)
          |
          v
ESP32 Wi-Fi bridge
          |
          v
TCP on port 7160
          |
          v
Unraid: taptap + taptap-mqtt
          |
          v
MQTT broker
          |
          v
Home Assistant MQTT auto-discovery
```

The ESP32 does not decode the Tigo protocol. It forwards the RS-485 traffic over TCP. TapTap performs the protocol work, and taptap-mqtt publishes the results to MQTT for Home Assistant.

## Before you start

You will need:

- A Tigo CCA or compatible controller with access to the GATEWAY RS-485 A/B connection.
- Tigo TS4 optimizers already installed and operating.
- An ESP32 development board with a USB data cable.
- A 3.3 V-compatible RS-485 transceiver. A MAX485-style board can work, but check the voltage requirements of the exact board you own.
- An Unraid server or another Docker host on the same LAN.
- An MQTT broker reachable by Home Assistant and the Unraid host.
- A computer with Arduino IDE for uploading the firmware.

### Safety first

PV equipment can contain dangerous voltages, even when the system appears to be switched off. Do not alter PV string wiring or work inside equipment that you are not qualified to service. This project only describes a parallel tap of the low-voltage communications connection; follow the equipment manufacturer's instructions and local electrical rules.

Keep the MAX485 receiver electrically isolated from any signal you do not understand. Do not add a termination resistor to the bus unless the bus design specifically requires it. The tap should not replace, disconnect, or re-terminate the existing Tigo wiring.

## Hardware wiring

The ESP32 firmware uses UART2 at 38400 baud, 8N1, with GPIO16 as RX and GPIO17 as TX. The MAX485 driver is disabled so the bridge listens without transmitting onto the Tigo bus.

### MAX485 wiring table

| MAX485 pin | Connects to | Notes |
|---|---|---|
| VCC | ESP32 3.3 V, or 5 V only when appropriate for the exact transceiver board | Check the board's datasheet and logic-level compatibility. |
| GND | ESP32 GND | A common ground is required. |
| DI | ESP32 GPIO17 / TX2 | Not used during receive-only monitoring, but wire it for completeness. |
| RO | ESP32 GPIO16 / RX2 | Carries RS-485 data into the ESP32. |
| DE | GND | Keeps the RS-485 driver permanently disabled. |
| RE | GND | Keeps the receiver enabled; RE is active-low. |
| A | Tigo CCA/GATEWAY A | Connect in parallel without disturbing existing wiring. |
| B | Tigo CCA/GATEWAY B | Connect in parallel without disturbing existing wiring. |

If no data appears, check A/B polarity first. Some transceiver boards label the differential pair differently, so compare the board markings with its documentation rather than assuming every board uses identical labels.

## Firmware upload with Arduino IDE

The firmware is already included in this repository at `firmware/taptap_esp32_bridge.ino`. You do not need to write the sketch yourself.

### 1. Install Arduino IDE

Install the current Arduino IDE from the official Arduino website and connect the ESP32 with a USB **data** cable. Charge-only cables will power the board but will not provide a serial port.

### 2. Add ESP32 board support

In Arduino IDE:

1. Open **File → Preferences**.
2. Find **Additional Boards Manager URLs**.
3. Add Espressif's ESP32 package URL:

   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`

4. Open **Tools → Board → Boards Manager**.
5. Search for **esp32**.
6. Install **esp32 by Espressif Systems**.

### 3. Open the firmware

Download or clone this repository, then open:

```text
firmware/taptap_esp32_bridge.ino
```

If Arduino asks to create a sketch folder or rename the file, allow it to do so. The sketch uses the ESP32's built-in `WiFi`, `WebServer`, and `Preferences` libraries; no separate library installation should be necessary.

### 4. Select board and port

Connect the ESP32 and choose:

- **Tools → Board**: select the model that matches your board. If unsure, start with **ESP32 Dev Module**.
- **Tools → Port**: select the new serial port that appears when the board is connected.
- Leave the other settings at their defaults unless your particular board requires a different flash or upload setting.

### 5. Compile and upload

1. Click **Verify** to compile the sketch.
2. Correct any board or port selection issue reported by Arduino IDE.
3. Click **Upload**.
4. If the upload pauses at `Connecting...`, hold the board's **BOOT** button while the upload begins, then release it when writing starts.
5. Wait for the upload to complete.

Open **Tools → Serial Monitor**, set the speed to **115200 baud**, and press the ESP32 reset button. You should see the public-release build name and the setup access point details.

## Configure the ESP32

On first boot, the firmware starts a temporary Wi-Fi access point:

- SSID: `TapTap-Setup`
- Password: `taptap123`
- Setup address: `http://192.168.4.1/wifi`

Connect your phone or computer to that network, open the setup address, enter the normal home Wi-Fi SSID and password, and choose **Save & Reboot**.

After reboot, the ESP32 tries to join the saved Wi-Fi. When connected, the setup AP turns off and the status page reports **Wi-Fi Connected**. If the saved network is unavailable for long enough, the setup AP is enabled again so the settings can be corrected.

> The default setup password is published because it is part of the example firmware. For a real installation, change it before distributing or deploying the firmware, and do not expose the ESP32's HTTP pages to the internet.

Open the ESP32's normal LAN IP in a browser. The status page shows the Wi-Fi state, IP address, TCP port, serial settings, readable byte count, last byte time, and uptime in `hours:minutes:seconds` format.

## Test the bridge

The firmware listens for TapTap on TCP port `7160`, which is the port expected by the TapTap TCP command in this setup.

From the Unraid host, or another computer with TapTap installed, run:

```bash
taptap observe --tcp <ESP32_IP_ADDRESS>
```

Replace `<ESP32_IP_ADDRESS>` with the address assigned by your router. Keep the command running for a while. On the ESP32 status page:

- **Bytes relayed** should increase when traffic is received.
- **Last byte seen** should update when the bus is active.
- **taptap client** should show connected while TapTap is running.

Solar and optimizer traffic can vary with system state and daylight. No immediate output does not always mean the wiring is wrong.

## Unraid and MQTT setup

The remaining software path is:

```text
ESP32 TCP bridge → taptap → taptap-mqtt → MQTT broker → Home Assistant
```

Use the public `config-example.ini` as your starting point. Copy it to a private `config.ini` and change the placeholders for your own installation:

- MQTT broker hostname or IP address.
- MQTT username and password.
- ESP32 IP address or hostname.
- Module names and serial numbers.
- Site/topic name.

Do not commit the real `config.ini`. It can contain private network addresses, MQTT credentials, and hardware identifiers. Keep it in Unraid appdata or another private location and add `config.ini` to `.gitignore`.

The important TapTap settings are:

```ini
[TAPTAP]
BINARY = /usr/local/bin/taptap
ADDRESS = CHANGE_ME_BRIDGE_HOST
PORT = 7160
UPDATE = 15
```

The `MODULES` value maps the string, friendly name, and module serial number. Use the format shown in the example file:

```ini
MODULES = B:Module_01:REPLACE_WITH_SERIAL_01, B:Module_02:REPLACE_WITH_SERIAL_02
```

Configure the container so the `taptap` executable is available at the path in `BINARY`, the config file is mounted read/write where required, and the container can reach both the ESP32 and MQTT broker. Restart the container after changing its configuration.

## Home Assistant

Home Assistant needs an MQTT integration connected to the same broker configured in `config.ini`. Give the bridge a dedicated MQTT account rather than using an administrator account.

When taptap-mqtt is running successfully, it publishes MQTT discovery information. The discovered Tigo devices and sensors should then appear under **Settings → Devices & services → MQTT**. The exact entities depend on the data available from the Tigo system and the module mapping in your configuration.

## Troubleshooting

### The ESP32 does not appear as a USB port

Try another USB cable, install the USB-to-serial driver required by your board, and check Windows Device Manager, macOS System Information, or the Linux serial devices list.

### Arduino gets stuck at Connecting

Confirm the board and port, close any program using the serial port, press reset, and hold BOOT briefly while uploading.

### The setup Wi-Fi network is not visible

Press reset and wait for the startup messages. If saved Wi-Fi credentials are present, the ESP32 may connect to the normal network and turn the setup AP off. If necessary, clear saved credentials from the Wi-Fi settings page or erase the board's flash.

### The dashboard works but Bytes relayed stays at zero

Check MAX485 power and common ground, confirm GPIO16 is connected to RO, verify A/B polarity, and check that DE is grounded and RE is grounded. Also confirm that you are tapping the intended Tigo GATEWAY RS-485 connection and not changing the existing bus termination.

### TapTap cannot connect

Confirm the ESP32 LAN IP, port `7160`, and that the Unraid host can reach it. Check that another client is not already using the single TCP bridge connection.

### No data appears immediately

Allow time for traffic and test when the PV system is active. Check the ESP32 dashboard's last-byte field and test the TCP connection before troubleshooting MQTT.

### MQTT connects but Home Assistant shows no devices

Check the broker address, port, MQTT credentials, discovery prefix, and container logs. Confirm that discovery messages are being published and that retained discovery topics from an older configuration are not causing confusion.

## Privacy and maintenance

Keep private installation details out of public commits:

- Wi-Fi passwords.
- MQTT passwords or tokens.
- Private IP addresses if you do not want to publish your network layout.
- Tigo module serial numbers if you consider them sensitive.
- Logs containing credentials or identifiable network details.

The ESP32 firmware is deliberately simple: it forwards bytes and provides a small local status/configuration page. It does not provide internet security or user authentication. Keep it on a trusted LAN and do not forward its HTTP or TCP ports to the public internet.

## Credits

This project builds on the TapTap protocol work and the `taptap-mqtt` bridge. Please consult their upstream repositories for protocol details, software updates, and issue reporting:

- [willglynn/taptap](https://github.com/willglynn/taptap)
- [litinoveweedle/taptap-mqtt](https://github.com/litinoveweedle/taptap-mqtt)

## License

Add the license that applies to your firmware, documentation, and original configuration examples. Also respect the licenses of the upstream projects linked above.