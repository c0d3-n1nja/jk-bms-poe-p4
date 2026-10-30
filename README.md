# JK-BMS (v15) → MQTT Bridge for M5Stack Unit PoE-P4 (U213, old revision)

This stack reads a **JK-BMS (firmware v15, 16S LiFePO4, 5 temperature sensors)** through a
dedicated **RS485 port** (SP3485 transceiver on GPIO19/20/21) and publishes all
balancer/status data via **MQTT** over **Ethernet/PoE**. The device is configured
(network, MQTT, web password, debug) at **runtime through a built-in web UI** - the
settings are stored in a dedicated NVS partition and survive firmware updates.
This is the native ESP-IDF implementation of the protocol from
[phinix-org/Multiple-JK-BMS-by-Modbus-RS485](https://github.com/phinix-org/Multiple-JK-BMS-by-Modbus-RS485)
(without ESPHome; `jkbms.h`/`jkbms.c` implement the **JK BMS RS485 Modbus V1.0**
(protocol 001) themselves: Modbus RTU, function 0x03, CRC16-MODBUS, cell block plus
status/config blocks, identity (0x1400) and protocol number (0x14B2)).

## Hardware

Device: M5Stack **Unit PoE-P4** (SKU U213, legacy/old revision - ESP32-P4 rev < 3.0,
16 MB flash, IP101GRI PHY on the internal RMII Ethernet MAC).

### Wiring SP3485 ↔ Unit PoE-P4

| SP3485 board | Unit PoE-P4 |
|---|---|
| `RO` (Data Out) | **GPIO20** (RX) |
| `DI` (Data In)  | **GPIO19** (TX) |
| `RE` + `DE` bridged | **GPIO21** (pulled HIGH = transmit while sending, then LOW = receive) |
| `VCC` | 3V3 |
| `GND` | GND |

### Wiring SP3485 ↔ JK-BMS

Connect to the **RS485-1 (port 1 / UART1) socket on the right side** of the BMS.

RJ45 on the BMS (T568B pinout, verified):

| BMS RJ45 pin | Color   | Signal | → SP3485   |
|---|---|---|---|
| Pin 1        | white/orange | **B**  | Pin 7 (B)   |
| Pin 2        | orange       | **A**  | Pin 6 (A)   |
| Pin 3        | white/green  | **GND**| Pin 5 (GND) |

(Cable: twisted pair). No response? → **swap A/B** (pin 1 ↔ pin 2).

Settings on the BMS:
- **UART1 protocol = 001** ("JK BMS RS485 Modbus V1.0") in the display menu (otherwise
  the BMS does not respond to Modbus; 013 would be 9600 baud - do not use it).
- **DIP switch = address 1** (switch 1 ON, the rest OFF) - must match
  `CONFIG_JKBMS_MODBUS_SLAVE_ADDR` (default 1).

UART parameters: 115200 baud, 8N1 (fixed value, in the polling task in main.c).

### Flashing (without ESP-IDF or other tools, directly from the browser)

1. Open Firefox, Chrome or Edge on a desktop computer
2. Connect a USB data cable to the USB-C port for firmware download/OTG (the port right next to the network port), not to the host port
3. Put the device into download mode following the M5Stack instructions: hold the side button for about 3 seconds until the green LED lights up.
4. Go to the [Web Installer](https://c0d3-n1nja.github.io/jk-bms-poe-p4/web-installer/), click the Connect button, select the matching USB device in the browser and grant access.
5. Wait for flashing to finish completely and let the device restart.

### Flashing (manually via download mode)

Connect the USB-C cable (download port) → **hold the side button for about 3 s until the green LED lights up** →
the device waits in download mode. Power can come from PoE or USB in the meantime (both are fine).
The console (`idf.py monitor`) runs over the same USB port (USB-Serial-JTAG).

## Software Stack

- **ESP-IDF 5.4.x** (this is how M5Stack builds the old revision; 5.5.x reportedly works too).
  The new revision *PoE-P4X* (silicon rev 3.x) requires ESP-IDF ≥ 6.1 and is **not** a target
  of this project. If a newer IDF version specifies a "Minimum supported
  ESP32-P4 revision" ≥ 3.0 in `menuconfig`, correct it (see below).

### Project Structure

```
|- CMakeLists.txt            # Project tooling
|- partitions.csv            # 16 MB flash: nvs/phy/otadata/cfg + ota_0/ota_1 (6 MB each)
|- sdkconfig.defaults        # Base config (target, flash, pins, polling)
`- main/
    |- CMakeLists.txt        # compiles + embeds config_page.html
    |- Kconfig.projbuild     # Build-time options (pins, polling, OTA default URL, reset GPIO)
    |- ca_cert.pem           # CA for mqtts://, OTA https and HTTP server (provided at build)
    |- config_page.html      # Web UI (embedded in the firmware)
    |- log_page.html         # Terminal page for the device log (embedded, /log)
    |- main.c                # RS485 UART, framing, polling task, reset button, ETH/MQTT/web wiring
    |- jkbms.h / jkbms.c     # JK Modbus RTU: CRC16, FC 0x03 frames, block decoder (all fields)
    |- app_config.h/.c       # Runtime config in its own NVS partition 'cfg' (incl. manual update URL)
    |- app_web.h/.c          # Web UI (HTTP server): configuration, OTA button, update check, /log endpoints
    |- app_log.h/.c          # Device log: vprintf hook (esp_log_set_vprintf), line ring, redirection
    |- app_eth.h/.c          # EMAC + IP101GRI, DHCP or static IP, events, network info cache
    |- app_mqtt.h/.c         # esp_mqtt: broker (from runtime config), JSON status, HA autodiscovery, device topic
    `- app_ota.h/.c          # OTA: MQTT trigger + web UI button, rollback confirmation, online update check
```

## Build & Flash

### Prerequisites

Install ESP-IDF 5.4.x (Windows: ESP-IDF installer, Linux: standard installation).
Then open the "ESP-IDF 5.4 PowerShell" (or `export.sh`/`activate.sh`).

```
cd jk-bms-poe-p4
idf.py set-target esp32p4
idf.py menuconfig     # (optional) - only pins/polling/OTA URL now, see below
idf.py build
idf.py -p COM5 flash monitor      # Windows; on Linux: /dev/ttyACM0 or similar
```

- Find the COM port in the Device Manager (USB Serial/JTAG).
- If `idf.py monitor` loses newly discovered bytes: restarting `idf.py -p <PORT> monitor` is enough.
- Activate download mode beforehand (hold button 3 s - green LED).
- After (re)flashing: start normally (RST button or unplug/replug). Logs:

```text
I (...) jkbms: App ... v0.0.9 running from partition ota_0
I (...) jkbms: Firmware v0.0.9 | IP ... 
I (...) app_cfg: Config loaded: IP DHCP, MQTT (off), debug off
I (...) app_eth: DHCP: IP 192.168.x.x, netmask ..., gateway ...
I (...) app_web: Web UI up on port 80 (no password)
I (...) jkbms: JK-BMS online (16 cells, ...)
```

## Configuration

There are two configuration levels:

1. **Runtime (web UI)**: MQTT broker including credentials/TLS, DHCP or static IP, optional
   web password, RS485 debug switch. Stored in the NVS partition **`cfg`** -
   **survives firmware updates (OTA)**. Takes effect only after the restart that the save operation triggers.
2. **Build time (menuconfig)**: only the technically fixed values (pins, polling intervals, Modbus address,
   topic prefix, HA discovery, default OTA URL, reset button GPIO).

### Web UI (runtime configuration)

First start: the device defaults to **DHCP**, MQTT off, web UI without password.
Look up the assigned IP in the router (lease table) or in the log (line `app_eth: DHCP: IP ...`);
the web UI line is in the log as well. Then simply open in the browser:

```
http://<device-ip>/
```

The page (embedded in the firmware, no internet required):

- **Web interface**: optional Basic Auth password. Leave empty = password unchanged;
  the "remove" checkbox deletes an existing password. Without a password the web UI
  is freely accessible on the network.
- **Network**: DHCP on/off. With DHCP switched off, IP/netmask/gateway/DNS are mandatory
  (validation: format `a.b.c.d`). In DHCP mode the DNS field serves as an additional
  fallback DNS (in case the DHCP server does not provide a usable resolver - relevant
  if the broker is only reachable by name).
- **MQTT**: broker URI (`mqtt://host:port`), user, password. An empty broker URI switches
  MQTT off completely (only the web UI remains active). The **TLS** checkbox converts `mqtt://`
  to `mqtts://` when connecting (and vice versa) and validates the server against the
  embedded `ca_cert.pem`. Leave password empty = unchanged, checkbox = remove.
- **Diagnostics**: RS485 debug (hex dump of raw data in the log) on/off at runtime.
  GPIO layouts and polling parameters remain build-time values.
  The **"Open device log"** button opens the terminal page
  `http://<device-ip>/log` (new tab, monospace): history + live output of the
  firmware via polling. As long as the tab is polling, log output is redirected there via
  `esp_log_set_vprintf` (the serial console pauses) and
  the log level can be selected dynamically there (`esp_log_level_set("*", ...)`).
  After 10 s without a poll (tab closed) the output returns to the
  console and the previous log level is restored. Size of the log ring:
  `JKBMS_LOG_RING_KB` (build time, default 16 KB).
- **Firmware update**: Manual update URL (empty = the default URL
  `CONFIG_JKBMS_OTA_DEFAULT_URL` set at build time is used) including the button
  "Reset manual URL" (clears the field - after saving, the default URL applies
  again). The button "Install update from the active URL" installs
  from this URL; the status is shown below it. The update check (regularly
  and automatically, or via "Check now") compares the installed version against
  a remote "version" file and shows whether a newer version is available.

After "Save" the web server responds and the device restarts; only then are the
new values effective (network, MQTT, debug ...). The running config remains
unchanged during operation (no pointer races in the MQTT client).

**Migrating from an older version**: enter the broker URI/user/password once in the
web UI (all build-time MQTT options have been removed; in return, the config now
survives every update).

**Factory reset (recovery)**: Press the BOOT button of the board (default **GPIO45**, see
`CONFIG_JKBMS_RESET_BUTTON_GPIO`, -1 = off) **10 times within 15 seconds**.
This erases the `cfg` partition completely (web password, IP settings, MQTT, debug,
manual update URL) and
restarts into the defaults: DHCP, web UI without password, MQTT off. This is the way back
if a static IP set no longer fits or the web password is lost - the device
then responds again on the fresh DHCP address from the router.

### Build-time configuration (menuconfig / sdkconfig.defaults)

All options are under **"Component config → JK-BMS to MQTT Bridge (M5Stack Unit PoE-P4)"**:

| Option | Default | Meaning |
|---|---|---|
| `APP_PROJECT_VER` | `v0.0.9` | **Software version** ("Application manager" menu, taken from the sdkconfig via `CONFIG_APP_PROJECT_VER_FROM_CONFIG=y` - see section "Software version / releases"); ends up in the app descriptor and is used in the log, web UI, MQTT and update check |
| `JKBMS_TOPIC_PREFIX` | `jkbms` | Topics `<prefix>/state`, `<prefix>/available`, `<prefix>/mqtt_available`, `<prefix>/device`, `<prefix>/ota(/status)` |
| `JKBMS_HA_DISCOVERY` | y | Publish Home Assistant autodiscovery (retained) |
| `JKBMS_POLL_INTERVAL_MS` | 5000 | "Read all registers" polling cycle |
| `JKBMS_CONFIG_READ_INTERVAL_MS` | 60000 | Re-read interval of the config block (protection parameters/switches) in ms; in between, the last values are kept |
| `JKBMS_STATE_HEARTBEAT_MS` | 60000 | The state topic is only sent on change; after this time at most one heartbeat publish follows |
| `JKBMS_NO_RESPONSE_TIMEOUT_MS` | 30000 | Timeout until "BMS offline" is reported |
| `JKBMS_MODBUS_SLAVE_ADDR` | 1 | Modbus RTU address of the BMS (must match the DIP switch) |
| `JKBMS_OTA_DEFAULT_URL` | (empty) | Default firmware URL for the web UI update button (used if no manual URL is set) |
| `JKBMS_VERSION_URL` | (empty) | URL of the "version" text file for the update check; empty = derive from the active update URL (replace the last path segment with `version`) |
| `JKBMS_VERSION_CHECK_INTERVAL_S` | 21600 | Automatic update check every x seconds (21600 = 6 h); 0 = manual only via the web UI or after each restart |
| `JKBMS_RESET_BUTTON_GPIO` | 45 | GPIO of the config reset button (press 10 times in a row); -1 = off |
| RS485: `JKBMS_RS485_UART_NUM` / `RX` / `TX` / `DE` | 1 / 20 / 19 / 21 | UART + pins (pin map as above) |
| Ethernet: `JKBMS_ETH_MDC/MDIO/PHY_RST/PHY_ADDR` | 31 / 52 / 51 / 1 | Pin map of the U213 board |

### Software version / releases

The version number is **in `sdkconfig.defaults`** (section "Software version"):

```
CONFIG_APP_PROJECT_VER_FROM_CONFIG=y
CONFIG_APP_PROJECT_VER="v0.0.9"
```

With `CONFIG_APP_PROJECT_VER_FROM_CONFIG=y` the value goes directly into the app descriptor
(`esp_app_get_description()->version`); git-describe/build-time alternatives are
ignored. For a release, adjust exactly this one line (`CONFIG_APP_PROJECT_VER`) -
the version then appears automatically in the boot log, in the web UI, in the MQTT device topic
(`<prefix>/device`) and as the reference of the online update check. After changing
sdkconfig.defaults, run once `idf.py fullclean && idf.py set-target esp32p4 && idf.py build`
(i.e. re-read partition/version values; with an existing `sdkconfig`
the new value is otherwise not picked up).

Fastest way: use `idf.py menuconfig` or edit `sdkconfig.defaults` and then
run `idf.py fullclean && idf.py set-target esp32p4 && idf.py build`.

## MQTT Data Format

`<prefix>/state` - JSON with (among others):

```
{"link_up":true,"cell_count":16,
 "cell_voltage_1":3.412, ... "cell_voltage_16":3.405,
 "min_cell_voltage":3.405,"min_cell_voltage_cell":14,
 "max_cell_voltage":3.421,"max_cell_voltage_cell":2,
 "delta_cell_voltage":0.016,"average_cell_voltage":3.412,
 "total_voltage":54.59,"current":-12.34,"total_power":-673.6,
 "charging_power":0,"discharging_power":673.6,
 "mos_temperature":29,"temperature_1":30,"temperature_2":28,
 "temperature_sensor_count":5,
 "state_of_charge":86,"capacity_remaining":120.1,"full_charge_capacity":140,
 "charging_cycles":42,"total_charging_cycle_capacity":9100,"battery_strings":16,
 "errors_bitmask":0,"errors":"","operation_mode_bitmask":7,"operation_mode":"...",
 "balancing_switch":true,"charging_switch":true,"discharging_switch":true,
 "additionally all protection/balance settings, battery_type, device_type,
 software_version, manufacturer, protocol_version, total_runtime_h, ..."}
```

In addition there is `<prefix>/device` (retained) - a snapshot status of the
**device itself**: software version (from the sdkconfig) as well as the current
network data (DHCP-leased or statically configured) and the result of the
online update check. The topic is republished after every MQTT (re)connect and after every
completed update check:

```
{"device_id":"ABCDEF123456","project":"jkbms_mqtt_poe_p4","sw_version":"v0.0.9",
 "idf_version":"v5.4.4","partition":"ota_0","ip_mode":"dhcp",
 "ip":"192.168.1.50","netmask":"255.255.255.0","gateway":"192.168.1.1",
 "dns":"192.168.1.1","dns_backup":"",
 "sw_update_available":false,"sw_update_version":""}
```

Availability is deliberately split in two:

- `<prefix>/mqtt_available`: connection device → broker. Retained `online` on connect,
  the broker sets `offline` (Last Will) when the MQTT connection dies.
- `<prefix>/available`: state of the RS485 link to the BMS. Retained `online` as soon as the
  first valid poll cycle has completed, `offline` after the quiet timeout
  (`JKBMS_NO_RESPONSE_TIMEOUT_MS`). `link_up:false` in the state JSON follows the same logic.

- `<prefix>/state` is only published when values have changed; after at most
  `JKBMS_STATE_HEARTBEAT_MS` a heartbeat publish follows so that the retained message
  stays current. After every MQTT (re)connect, a full publish is done once.
- Missing responses to individual reads are invisible: the affected registers keep
  their last values and are re-read in the next cycle; only after 60 silent
  polls in a row is there a warning in the log.

### Home Assistant (if `JKBMS_HA_DISCOVERY=y`)

`homeassistant/sensor|binary_sensor/<prefix>-<Device-ID>/<field>/config`
discovery messages (retained) are published automatically: all cell voltages (every cell), voltages,
currents, power, temperatures, SOC, capacities, cycles, all protection parameters, error and
operation bits. There are deliberately no control switches (read-only monitor: writing
registers is not implemented).

## OTA Firmware Update

Updatable independently of the web UI; three triggers, same transport:

1. **MQTT trigger**: publish a firmware URL (`http://...`/`https://...`) in the payload of
   `<prefix>/ota`. Procedure:
   - `idf.py build` → `cd build` → `python -m http.server 8000`
   - `mosquitto_pub -h <broker> -u <user> -P <pass> -t jkbms/ota -m "http://<PC-IP>:8000/jkbms_mqtt_poe_p4.bin"`
2. **Web UI button**: downloads from the **active update URL** - the manual URL
   entered in the web UI, otherwise (field empty) the build-time default URL
   `CONFIG_JKBMS_OTA_DEFAULT_URL` (`http://` is allowed, see below regarding
   TLS behavior). With "Reset manual URL" + Save, only the default URL
   applies again.
3. **Online update check** only displays, does not switch firmware:
   - On the first IP-up, a background task starts that regularly
     (`JKBMS_VERSION_CHECK_INTERVAL_S`, default 6 h) loads a plain-text
     "version" file via HTTP GET (for GitHub releases, for example, simply put a
     `version` file into the release, containing e.g. `v0.1.0`).
   - The URL is derived from the active update URL (last path segment →
     `version`) or from `JKBMS_VERSION_URL`. GitHub redirects are followed
     automatically.
   - If the remote version is numerically newer than the installed one
     (`CONFIG_APP_PROJECT_VER`), the web UI shows "**UPDATE AVAILABLE**"
     under "Update check" and `<prefix>/device` contains
     `sw_update_available:true` along with the version. You then have to install it
     yourself (button, or automated via script).
   - If the check fails (missing version file, no network, DNS ...),
     the web UI shows the cause ("Check failed: HTTP 404"
     or similar) and `<prefix>/device` lists it in `sw_update_error`.
     Failed attempts are repeated every 60 s until the first successful check,
     afterwards at the normal interval.

### TLS server verification currently DISABLED (temporary, insecure!)

The embedded `ca_cert.pem` is so far a **placeholder** (not a real
certificate). An https OTA attempt therefore aborts during TLS connection setup
(`mbedtls_x509_crt_parse of CA cert returned -0x112C`, i.e.
`PEM_INVALID_DATA + BASE64_INVALID_CHARACTER` - the file content is not
PEM base64 at all). Until a real CA chain is embedded, the firmware does
**not** validate TLS server certificates (encryption stays active, but the
server identity is not checked - MITM possible!):

- Allowed via `CONFIG_ESP_TLS_INSECURE=y` +
  `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y` (sdkconfig.defaults, both lines
  are marked "TEMPORARY").
- OTA (https and http) and MQTTS therefore run without certificate verification; both
  code paths log a `DISABLED` warning at startup.

**When secure again:** real server certificate or the signing chain
(the order in `ca_cert.pem`: server certificate + intermediate + root -
or only root/intermediate if the server sends them along; example:
`openssl s_client -showcerts -connect github.com:443 </dev/null 2>/dev/null |
sed -n '/-----BEGIN CERT/,/-----END CERT/p' > main/ca_cert.pem`), then
remove the two `ESP_TLS_*` lines from sdkconfig.defaults, run `idf.py fullclean`
and replace both `DISABLED` `#if` blocks in `main/app_ota.c` / `main/app_mqtt.c`
with the attach variant (they are marked "TEMPORARY" there).

The result ends up under `<prefix>/ota/status` (retained): `"downloading 30%"`,
`"success, rebooting"` or `"failed: ..."`; the same text is shown in the web UI.

Safety net: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` - the new image boots
as "pending verify" and only confirms itself once MQTT is running and a
poll cycle has delivered valid BMS data. Without confirmation, the
bootloader automatically boots the old slot again on the next restart (no
brick risk).

Migrating to the OTA partition table (one-time via USB): instead of
`factory`, the table now has `ota_0`/`ota_1` + `otadata`; `idf.py -p COM5 flash`
writes the image to `ota_0` and boots from it (blank `otadata` = slot 0).
Recommendation: run `idf.py -p COM5 erase-flash` once beforehand. The partitions
`nvs`/`cfg` are only reset by this - the runtime config in `cfg`
has to be entered once afterwards; later it is preserved across every
OTA (the partition does not appear in the app area).

## Important Notes / Troubleshooting

- **"Old revision"**: legacy U213 (ESP32-P4 rev < 3.0) + ESP-IDF 5.4.x. Revisions ≥ 3.0 (PoE-P4X)
  are **not** compatible with this version. If `idf.py menuconfig` shows a
  "Minimum supported ESP32-P4 revision" of ≥ 3.0 (newer IDF version), set it back
  to 1.0/2.0 in the menu *Component config → Hardware Settings → Chip revision* or
  stay with ESP-IDF 5.4.x.
- The RS485 port keeps the BMS awake; a sleep mode (deep sleep) of the BMS blocks
  communication - if necessary, configure the sleep wait time in the BMS accordingly.
- The 5 temperature sensors: the frame of this protocol transmits MOS temperature,
  battery temperatures T1/T2 and (v15 firmware) T3/T4/T5; the field
  `temperature_sensor_count` shows how many sensors the BMS reports.
- CRC errors in the log → usually A/B swapped, missing common GND reference or wrong
  baud rate (setting: 115200 8N1).
- `MQTT_ERROR` (DNS): The broker host must be resolvable via DNS. `.local` mDNS names
  only work if an mDNS base is available - when in doubt, use a real DNS name,
  the broker's IP, or the DNS field of the web UI (fallback in DHCP mode).
- **Device misconfigured?** (static IP no longer fits / web password forgotten):
  hold the BOOT button for 10 seconds → factory reset of the config (see above). Afterwards
  it is reachable again via its DHCP address.
