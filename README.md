# JK-BMS (v15) → MQTT Bridge für M5Stack Unit PoE-P4 (U213, alte Revision)

Dieser Stack liest einen **JK-BMS (Firmware v15, 16 LiFePO4-Zellen, 4 Temperatursensoren)** über
einen dedizierten **RS485-Port** (SP3485-Wandler an GPIO19/20/21) aus und veröffentlicht alle
Balancer-/Statusdaten per **MQTT** über **Ethernet/PoE** (DHCP). Das ist die native ESP-IDF-
Umsetzung des Protokolls von [phinix-org/Multiple-JK-BMS-by-Modbus-RS485](https://github.com/phinix-org/Multiple-JK-BMS-by-Modbus-RS485)
(ohne ESPHome – die Dateien jkbms.h/jkbms.c implementieren das **JK BMS RS485 Modbus V1.0**
(Protokoll 001) selbst: Modbus RTU, Funktion 0x03, CRC16-MODBUS, Registerblöcke
0x1000/0x1200/0x128A/0x1400 inkl. aller Detail-Offsets).

## Hardware

Gerät: M5Stack **Unit PoE-P4** (SKU U213, legacy/alte Revision – ESP32-P4 Rev < 3.0,
16 MB Flash, IP101GRI-PHY am internen RMII-Ethernet-MAC).

### Verdrahtung SP3485 ↔ Unit PoE-P4

| SP3485-Board | Unit PoE-P4 |
|---|---|
| `RO` (Data Out) | **GPIO20** (RX) |
| `DI` (Data In)  | **GPIO19** (TX) |
| `RE` + `DE` gebrückt | **GPIO21** (wird beim Senden auf HIGH = Senden gezogen, danach LOW = Empfang) |
| `VCC` | 3V3 |
| `GND` | GND |

### Verdrahtung SP3485 ↔ JK-BMS

Angeschlossen wird die Buchse **RS485-1 (Port 1 / UART1) auf der rechten Seite** des BMS
(diese ist immer Modbus-Slave; NICHT die linken Inverter-CAN/RS485-Ports).

RJ45 am BMS (T568B-Belegung, verifiziert):

| BMS RJ45-Pin | Farbe   | Signal | → SP3485     |
|---|---|---|---|
| Pin 1        | weiß/orange | **B**  | Pin 7 (B)    |
| Pin 2        | orange      | **A**  | Pin 6 (A)    |
| Pin 3        | weiß/grün   | **GND**| Pin 5 (GND)  |

(Kabel: verdrilltes Paar). Keine Antwort? → **A/B tauschen** (Pin 1 ↔ Pin 2).

Am BMS einstellen:
- **UART1-Protokoll = 001** ("JK BMS RS485 Modbus V1.0") im Display-Menü (sonst antwortet
  der BMS nicht auf Modbus; 013 wäre 9600 Baud – nicht verwenden).
- **DIP-Switch = Adresse 1** (Schalter 1 ON, Rest OFF) – muss zu
  `CONFIG_JKBMS_MODBUS_SLAVE_ADDR` (Default 1) passen.

UART-Parameter: 115200 Baud, 8N1 (Fixwert, im Polling-Task in main.c).

### Flashen (Download-Modus)

USB-C-Kabel (Download-Port) verbinden → **Seitenknopf ca. 3 s halten, bis die grüne LED leuchtet** →
Gerät wartet im Download-Modus. Power läuft währenddessen über PoE oder USB (beides ist ok).
Die Konsole (`idf.py monitor`) läuft über denselben USB-Port (USB-Serial-JTAG).

## Software-Stack

- **ESP-IDF 5.4.2** (so baut M5Stack die alte Revision; 5.5.x funktioniert erfahrungsgemäß auch).
  Die neue Revision *PoE-P4X* (Silicon-Rev 3.x) benötigt ESP-IDF ≥ 6.1 und ist **nicht** Zielsystem
  dieses Projekts. Falls eine jüngere IDF-Version im `menuconfig` eine "Minimum supported
  ESP32-P4 revision" ≥ 3.0 vorgibt, korrigiere sie (siehe unten).

### Projektstruktur

```
├── CMakeLists.txt          # Projekt-Werkzeug
├── partitions.csv          # 16 MB Flash, 6 MB App-Partition
├── sdkconfig.defaults      # Grund-Konfig (Ziel, Flash, MQTT-Defaults)
└── main/
    ├── CMakeLists.txt
    ├── Kconfig.projbuild   # *** alle Buildzeit-Optionen (MQTT, Pins, Polling) ***
    ├── ca_cert.pem         # Broker-CA (nur für mqtts:// relevant)
    ├── main.c              # RS485-UART (handverkoppeltes DE/RE), Framing, Polling-Task, Verknüpfung ETH/MQTT
    ├── jkbms.h / jkbms.c   # JK-Modbus RTU: CRC16, FC-0x03-Frames, Block-Decoder (alle Felder)
    ├── app_eth.h/.c        # ESP-IDF EMAC + IP101GRI, DHCP-Client, Ereignisse
    └── app_mqtt.h/.c       # esp_mqtt: Broker, JSON-Status, HA-Autodiscovery
```

## Build & Flash

### Voraussetzung

ESP-IDF 5.4.2 installieren (Windows: ESP-IDF-Installer, Linux: Standard-Installation).
Danach die "ESP-IDF 5.4 PowerShell" (bzw. `export.sh`/`activate.sh`) öffnen.

```
cd jk-bms-poe-p4
idf.py set-target esp32p4
idf.py menuconfig     # (optional) — MQTT-Settings siehe unten
idf.py build
idf.py -p COM6 flash monitor      # Windows; unter Linux: /dev/ttyACM0 o.ä.
```

- COM-Port im Gerätemanager (USB Serial/JTAG) ermitteln.
- Falls `idf.py monitor` neuentdeckte Bytes verliert: `idf.py -p <PORT> monitor` neu starten reicht.
- Download-Modus vorher aktivieren (Knopf 3 s – grüne LED).
- Nach dem (Re)Flashen: normal starten (RST-Knopf oder Stecker). Logs:

```
I (… ) jkbms: JK-BMS to MQTT bridge started
I (…) app_eth: IPv4 …, netmask …, gateway …
I (…) app_mqtt: MQTT connected to mqtt://…:1883
I (…) jkbms: JK-BMS online (16 cells, …)
```

## Buildzeit-Konfiguration (menuconfig / sdkconfig.defaults)

Alle Optionen unter **"Component config → JK-BMS to MQTT Bridge (M5Stack Unit PoE-P4)"**:

| Option | Default | Bedeutung |
|---|---|---|
| `JKBMS_MQTT_URI` | `mqtt://homeassistant.local:1883` | Broker-URL: Scheme `mqtt://` oder `mqtts://` (mit TLS) + Name/IP + Port. **Der Name wird bei jedem Verbindungsversuch via DNS aufgelöst** (auch nach Netzwerk-/Broker-Ausfall) |
| `JKBMS_MQTT_USERNAME` | (leer) | MQTT-Benutzer (leer = anonym) |
| `JKBMS_MQTT_PASSWORD` | leer | MQTT-Passwort |
| `JKBMS_TOPIC_PREFIX` | `jkbms` | Topics `<prefix>/state` (JSON, retained) + `<prefix>/available` (LWT) |
| `JKBMS_HA_DISCOVERY` | y | Home-Assistant-Autodiscovery publishen (retained) |
| `JKBMS_MQTT_TLS` | n | Bei `mqtts://…` das eingebettete `ca_cert.pem` als Broker-CA validieren |
| `JKBMS_FALLBACK_DNS` | leer | Fallback-DNS, z. B. `192.168.1.1` (wenn DHCP keinen (brauchbaren) DNS liefert, für Broker-Namen) |
| `JKBMS_POLL_INTERVAL_MS` | 5000 | Lesezyklus "Read all registers" |
| `JKBMS_CONFIG_READ_INTERVAL_MS` | 60000 | Reread-Intervall des Config-Blocks (Schutzparameter/Switches) in ms; dazwischen bleiben die letzten Werte stehen |
| `JKBMS_STATE_HEARTBEAT_MS` | 60000 | State-Topic wird nur bei Änderung gesendet; nach dieser Zeit folgt höchstens ein Heartbeat-Publish |
| `JKBMS_NO_RESPONSE_TIMEOUT_MS` | 30000 | Timeout bis "BMS offline" meldet |
| RS485: `JKBMS_RS485_UART_NUM` / `RX` / `TX` / `DE` | 1 / 19 / 20 / 21 | UART + Pins (Pinmap wie oben) |
| Ethernet: `JKBMS_ETH_MDC/MDIO/PHY_RST/PHY_ADDR` | 31 / 52 / 51 / 1 | Pinmap des U213-Boards |

Am schnellsten: Werte direkt in **`sdkconfig.defaults`** editieren und danach
`idf.py fullclean && idf.py set-target esp32p4 && idf.py build` ausführen
(oder `idf.py menuconfig` benutzen — die MQTT-Passwort-Eingabe wird dort maskiert angezeigt).

## MQTT-Datenformat

`<prefix>/state` — JSON mit (u. a.):

```
{"link_up":true,"cell_count":16,
 "cell_voltage_1":3.412, … "cell_voltage_16":3.405,
 "min_cell_voltage":3.405,"min_cell_voltage_cell":14,
 "max_cell_voltage":3.421,"max_cell_voltage_cell":2,
 "delta_cell_voltage":0.016,"average_cell_voltage":3.412,
 "total_voltage":54.59,"current":-12.34,"total_power":-673.6,
 "charging_power":0,"discharging_power":673.6,
 "mos_temperature":29,"temperature_1":30,"temperature_2":28,
 "temperature_sensor_count":4,
 "state_of_charge":86,"capacity_remaining":120.1,"full_charge_capacity":140,
 "charging_cycles":42,"total_charging_cycle_capacity":9100,"battery_strings":16,
 "errors_bitmask":0,"errors":"","operation_mode_bitmask":7,"operation_mode":"…",
 "balancing_switch":true,"charging_switch":true,"discharging_switch":true,
 "zusätzlich alle Schutz-/Balance-Einstellungen, battery_type, device_type,
 software_version, manufacturer, protocol_version, total_runtime_h, …"}
```

- `<prefix>/available`: `online`/`offline` — Last Will & Testament: der Broker markiert offline,
  wenn das Gerät die Verbindung verliert.
- `link_up:false` wird gesetzt, wenn wiederholt keine gültigen Frames ankommen
  (Timeout: `JKBMS_NO_RESPONSE_TIMEOUT_MS`).
- Das State-JSON enthält kein `uptime_s` mehr: `<prefix>/state` wird nur
  publiziert, wenn sich Werte geaendert haben; nach höchstens
  `JKBMS_STATE_HEARTBEAT_MS` folgt ein Heartbeat-Publish, damit die retained
  Message aktuell bleibt. Nach jedem MQTT-(Re)Connect wird einmal voll gepublisht.

### Home Assistant (falls `JKBMS_HA_DISCOVERY=y`)

Es werden automatisch `homeassistant/sensor|binary_sensor/<prefix>-<Device-ID>/<Feld>/config`-
Discovery-Meldungen (retained) veröffentlicht: sämtliche Zellspannungen (alle Zellen), Spannungen,
Ströme, Leistung, Temperaturen, SOC, Kapazitäten, Zyklen, alle Schutzparameter, Fehler- und
Betriebsbits. Steuer-Switches gibt es bewusst nicht (Read-only-Monitor: das Schreiben von
Registern ist nicht implementiert).

## Wichtige Hinweise / Troubleshooting

- **"alte Revision"**: legacy U213 (ESP32-P4 Rev < 3.0) + ESP-IDF 5.4.2. Revisionen ≥ 3.0 (PoE-P4X)
  sind mit dieser Version **nicht** kompatibel. Falls `idf.py menuconfig` eine
  "Minimum supported ESP32-P4 revision" von ≥ 3.0 anzeigt (jüngere IDF-Version), setze sie
  im Menü *Component config → Hardware Settings → Chip revision* auf 1.0/2.0 zurück oder
  bleibe bei ESP-IDF 5.4.2.
- Der RS485-Port hält den BMS wach; ein Sleep-Modus (Deep Sleep) des BMS blockiert die
  Kommunikation – im BMS ggf. die Sleep-Wartezeit entsprechend konfigurieren.
- Die 4 Temperatursensoren: Der Frame dieses Protokolls überträgt MOS-Temperatur + 2
  Batterie-Temperatursensoren (T1/T2) — genau wie syssi/esphome-jk-bms es dekodiert. Das Feld
  `temperature_sensor_count` zeigt, wie viele Sensoren der BMS meldet (z. B. 4). Falls deine
  v15-Firmware T3/T4 als eigene Felder schickt, wären die Offsets um diese Bytes zu erweitern —
  im Zweifel mit `esp_log_level` DEBUG prüfen.
- CRC-Fehler im Log → meist A/B vertauscht, fehlende gemeinsame GND-Referenz oder falsche
  Baudrate (must: 115200 8N1).
- `MQTT_ERROR` (DNS): Der Broker-Host muss via DHCP/DNS auflösbar sein. `.local`-mDNS-Namen
  funktionieren nur, wenn eine mDNS-Basis vorhanden ist — im Zweifel einen echten DNS-Namen,
  die IP des Brokers oder `JKBMS_FALLBACK_DNS` verwenden.
