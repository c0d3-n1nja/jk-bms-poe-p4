# JK-BMS (v15) â†’ MQTT Bridge fÃ¼r M5Stack Unit PoE-P4 (U213, alte Revision)

Dieser Stack liest einen **JK-BMS (Firmware v15, 16 LiFePO4-Zellen, 4 Temperatursensoren)** Ã¼ber
einen dedizierten **RS485-Port** (SP3485-Wandler an GPIO19/20/21) aus und verÃ¶ffentlicht alle
Balancer-/Statusdaten per **MQTT** Ã¼ber **Ethernet/PoE** (DHCP). Das ist die native ESP-IDF-
Umsetzung des Protokolls von [phinix-org/Multiple-JK-BMS-by-Modbus-RS485](https://github.com/phinix-org/Multiple-JK-BMS-by-Modbus-RS485)
(ohne ESPHome â€“ die Dateien jkbms.h/jkbms.c implementieren das **JK BMS RS485 Modbus V1.0**
(Protokoll 001) selbst: Modbus RTU, Funktion 0x03, CRC16-MODBUS, Registerbloecke
0x1000/0x1108/0x1200/0x128A/0x1400 inkl. aller Detail-Offsets).

## Hardware

GerÃ¤t: M5Stack **Unit PoE-P4** (SKU U213, legacy/alte Revision â€“ ESP32-P4 Rev < 3.0,
16 MB Flash, IP101GRI-PHY am internen RMII-Ethernet-MAC).

### Verdrahtung SP3485 â†” Unit PoE-P4 (Hat2-Bus / Verdrahtung wie von dir beschrieben)

| SP3485-Board | Unit PoE-P4 |
|---|---|
| `RO` (Data Out) | **GPIO20** (RX) |
| `DI` (Data In)  | **GPIO19** (TX) |
| `RE` + `DE` gebrÃ¼ckt | **GPIO21** (wird beim Senden auf HIGH = Senden gezogen, danach LOW = Empfang) |
| `VCC` | 3V3 |
| `GND` | GND |

### Verdrahtung SP3485 â†” JK-BMS

Angeschlossen wird die Buchse **RS485-1 (Port 1 / UART1) auf der rechten Seite** des BMS
(diese ist immer Modbus-Slave; NICHT die linken Inverter-CAN/RS485-Ports).

RJ45 am BMS (T568B-Belegung, verifiziert):

| BMS RJ45-Pin | Farbe   | Signal | â†' SP3485     |
|---|---|---|---|
| Pin 1        | weiÃŸ/orange | **B**  | Pin 7 (B)    |
| Pin 2        | orange      | **A**  | Pin 6 (A)    |
| Pin 3        | weiÃŸ/grÃ¼n   | **GND**| Pin 5 (GND)  |

(Kabel: verdrilltes Paar). Keine Antwort? â†' **A/B tauschen** (Pin 1 â†" Pin 2).

Am BMS einstellen:
- **UART1-Protokoll = 001** ("JK BMS RS485 Modbus V1.0") im Display-Menue (sonst antwortet
  der BMS nicht auf Modbus; 013 waere 9600 Baud â€“ nicht verwenden).
- **DIP-Switch = Adresse 1** (Schalter 1 ON, Rest OFF) â€“ muss zu
  `CONFIG_JKBMS_MODBUS_SLAVE_ADDR` (Default 1) passen.

UART-Parameter: 115200 Baud, 8N1 (Fixwert, im Polling-Task in main.c).

### Flashen (Download-Modus)

USB-C-Kabel (Download-Port) verbinden â†’ **Seitenknopf ca. 3 s halten, bis die grÃ¼ne LED leuchtet** â†’
GerÃ¤t wartet im Download-Modus. Power lÃ¤uft wÃ¤hrenddessen Ã¼ber PoE oder USB (beides ist ok).
Die Konsole (`idf.py monitor`) lÃ¤uft Ã¼ber denselben USB-Port (USB-Serial-JTAG).

## Software-Stack

- **ESP-IDF 5.4.2** (so baut M5Stack die alte Revision; 5.5.x funktioniert erfahrungsgemÃ¤ÃŸ auch).
  Die neue Revision *PoE-P4X* (Silicon-Rev 3.x) benÃ¶tigt ESP-IDF â‰¥ 6.1 und ist **nicht** Zielsystem
  dieses Projekts. Falls eine jÃ¼ngere IDF-Version im `menuconfig` eine "Minimum supported
  ESP32-P4 revision" â‰¥ 3.0 vorgibt, korrigiere sie (siehe unten).

### Projektstruktur

```
â”œâ”€â”€ CMakeLists.txt          # Projekt-Werkzeug
â”œâ”€â”€ partitions.csv          # 16 MB Flash, 6 MB App-Partition
â”œâ”€â”€ sdkconfig.defaults      # Grund-Konfig (Ziel, Flash, MQTT-Defaults)
â””â”€â”€ main/
    â”œâ”€â”€ CMakeLists.txt
    â”œâ”€â”€ Kconfig.projbuild   # *** alle Buildzeit-Optionen (MQTT, Pins, Polling) ***
    â”œâ”€â”€ ca_cert.pem         # Broker-CA (nur fÃ¼r mqtts:// relevant)
    â”œâ”€â”€ main.c              # RS485-UART (handverkoppeltes DE/RE), Framing, Polling-Task, VerknÃ¼pfung ETH/MQTT
    â”œâ”€â”€ jkbms.h / jkbms.c   # JK-Modbus RTU: CRC16, FC-0x03-Frames, Block-Decoder (alle Felder)
    â”œâ”€â”€ app_eth.h/.c        # ESP-IDF EMAC + IP101GRI, DHCP-Client, Ereignisse
    â””â”€â”€ app_mqtt.h/.c       # esp_mqtt: Broker, JSON-Status, HA-Autodiscovery
```

## Build & Flash

### Voraussetzung

ESP-IDF 5.4.2 installieren (Windows: ESP-IDF-Installer, Linux: Standard-Installation).
Danach die "ESP-IDF 5.4 PowerShell" (bzw. `export.sh`/`activate.sh`) Ã¶ffnen.

```
cd jk-bms-poe-p4
idf.py set-target esp32p4
idf.py menuconfig     # (optional) â€” MQTT-Settings siehe unten
idf.py build
idf.py -p COM6 flash monitor      # Windows; unter Linux: /dev/ttyACM0 o.Ã¤.
```

- COM-Port im GerÃ¤temanager (USB Serial/JTAG) ermitteln.
- Falls `idf.py monitor` neuentdeckte Bytes verliert: `idf.py -p <PORT> monitor` neu starten reicht.
- Download-Modus vorher aktivieren (Knopf 3 s â€“ grÃ¼ne LED).
- Nach dem (Re)Flashen: normal starten (RST-Knopf oder Stecker). Logs:

```
I (â€¦ ) jkbms: JK-BMS to MQTT bridge started
I (â€¦) app_eth: IPv4 â€¦, netmask â€¦, gateway â€¦
I (â€¦) app_mqtt: MQTT connected to mqtt://â€¦:1883
I (â€¦) jkbms: JK-BMS online (16 cells, â€¦)
```

## Buildzeit-Konfiguration (menuconfig / sdkconfig.defaults)

Alle Optionen unter **"Component config â†’ JK-BMS to MQTT Bridge (M5Stack Unit PoE-P4)"**:

| Option | Default | Bedeutung |
|---|---|---|
| `JKBMS_MQTT_URI` | `mqtt://homeassistant.local:1883` | Broker-URL: Scheme `mqtt://` oder `mqtts://` (mit TLS) + Name/IP + Port. **Der Name wird bei jedem Verbindungsversuch via DNS aufgelÃ¶st** (auch nach Netzwerk-/Broker-Ausfall) |
| `JKBMS_MQTT_USERNAME` | (leer) | MQTT-Benutzer (leer = anonym) |
| `JKBMS_MQTT_PASSWORD` | leer | MQTT-Passwort |
| `JKBMS_TOPIC_PREFIX` | `jkbms` | Topics `<prefix>/state` (JSON, retained) + `<prefix>/available` (LWT) |
| `JKBMS_HA_DISCOVERY` | y | Home-Assistant-Autodiscovery publishen (retained) |
| `JKBMS_MQTT_TLS` | n | Bei `mqtts://â€¦` das eingebettete `ca_cert.pem` als Broker-CA validieren |
| `JKBMS_FALLBACK_DNS` | leer | Fallback-DNS, z. B. `192.168.1.1` (wenn DHCP keinen (brauchbaren) DNS liefert, fÃ¼r Broker-Namen) |
| `JKBMS_POLL_INTERVAL_MS` | 5000 | Lesezyklus "Read all registers" |
| `JKBMS_NO_RESPONSE_TIMEOUT_MS` | 30000 | Timeout bis "BMS offline" meldet |
| RS485: `JKBMS_RS485_UART_NUM` / `RX` / `TX` / `DE` | 1 / 19 / 20 / 21 | UART + Pins (Pinmap wie oben) |
| Ethernet: `JKBMS_ETH_MDC/MDIO/PHY_RST/PHY_ADDR` | 31 / 52 / 51 / 1 | Pinmap des U213-Boards |

Am schnellsten: Werte direkt in **`sdkconfig.defaults`** editieren und danach
`idf.py fullclean && idf.py set-target esp32p4 && idf.py build` ausfÃ¼hren
(oder `idf.py menuconfig` benutzen â€” die MQTT-Passwort-Eingabe wird dort maskiert angezeigt).

## MQTT-Datenformat

`<prefix>/state` â€” JSON mit (u. a.):

```
{"link_up":true,"uptime_s":42,"cell_count":16,
 "cell_voltage_1":3.412, â€¦ "cell_voltage_16":3.405,
 "min_cell_voltage":3.405,"min_cell_voltage_cell":14,
 "max_cell_voltage":3.421,"max_cell_voltage_cell":2,
 "delta_cell_voltage":0.016,"average_cell_voltage":3.412,
 "total_voltage":54.59,"current":-12.34,"total_power":-673.6,
 "charging_power":0,"discharging_power":673.6,
 "mos_temperature":29,"temperature_1":30,"temperature_2":28,
 "temperature_sensor_count":4,
 "state_of_charge":86,"capacity_remaining":120.1,"full_charge_capacity":140,
 "charging_cycles":42,"total_charging_cycle_capacity":9100,"battery_strings":16,
 "errors_bitmask":0,"errors":"","operation_mode_bitmask":7,"operation_mode":"â€¦",
 "balancing_switch":true,"charging_switch":true,"discharging_switch":true,
 "zusÃ¤tzlich alle Schutz-/Balance-Einstellungen, battery_type, device_type,
 software_version, manufacturer, protocol_version, total_runtime_h, â€¦"}
```

- `<prefix>/available`: `online`/`offline` â€” Last Will & Testament: der Broker markiert offline,
  wenn das GerÃ¤t die Verbindung verliert.
- `link_up:false` wird gesetzt, wenn wiederholt keine gÃ¼ltigen Frames ankommen
  (Timeout: `JKBMS_NO_RESPONSE_TIMEOUT_MS`).

### Home Assistant (falls `JKBMS_HA_DISCOVERY=y`)

Es werden automatisch `homeassistant/sensor|binary_sensor/<prefix>-<Device-ID>/<Feld>/config`-
Discovery-Meldungen (retained) verÃ¶ffentlicht: sÃ¤mtliche Zellspannungen (alle Zellen), Spannungen,
StrÃ¶me, Leistung, Temperaturen, SOC, KapazitÃ¤ten, Zyklen, alle Schutzparameter, Fehler- und
Betriebsbits. Steuer-Switches gibt es bewusst nicht (Read-only-Monitor: das Schreiben von
Registern ist nicht implementiert).

## Wichtige Hinweise / Troubleshooting

- **"alte Revision"**: legacy U213 (ESP32-P4 Rev < 3.0) + ESP-IDF 5.4.2. Revisionen â‰¥ 3.0 (PoE-P4X)
  sind mit dieser Version **nicht** kompatibel. Falls `idf.py menuconfig` eine
  "Minimum supported ESP32-P4 revision" von â‰¥ 3.0 anzeigt (jÃ¼ngere IDF-Version), setze sie
  im MenÃ¼ *Component config â†’ Hardware Settings â†’ Chip revision* auf 1.0/2.0 zurÃ¼ck oder
  bleibe bei ESP-IDF 5.4.2.
- Der RS485-Port hÃ¤lt den BMS wach; ein Sleep-Modus (Deep Sleep) des BMS blockiert die
  Kommunikation â€“ im BMS ggf. die Sleep-Wartezeit entsprechend konfigurieren
  (Feld `sleep_wait_time` in der MQTT-Ausgabe).
- Die 4 Temperatursensoren: Der Frame dieses Protokolls Ã¼bertrÃ¤gt MOS-Temperatur + 2
  Batterie-Temperatursensoren (T1/T2) â€” genau wie syssi/esphome-jk-bms es dekodiert. Das Feld
  `temperature_sensor_count` zeigt, wie viele Sensoren der BMS meldet (z. B. 4). Falls deine
  v15-Firmware T3/T4 als eigene Felder schickt, wÃ¤ren die Offsets um diese Bytes zu erweitern â€”
  im Zweifel mit `esp_log_level` DEBUG prÃ¼fen.
- CRC-Fehler im Log â†’ meist A/B vertauscht, fehlende gemeinsame GND-Referenz oder falsche
  Baudrate (must: 115200 8N1).
- `MQTT_ERROR` (DNS): Der Broker-Host muss via DHCP/DNS auflÃ¶sbar sein. `.local`-mDNS-Namen
  funktionieren nur, wenn eine mDNS-Basis vorhanden ist â€” im Zweifel einen echten DNS-Namen,
  die IP des Brokers oder `JKBMS_FALLBACK_DNS` verwenden.
