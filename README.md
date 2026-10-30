# JK-BMS (v15) -> MQTT Bridge fuer M5Stack Unit PoE-P4 (U213, alte Revision)

Dieser Stack liest einen **JK-BMS (Firmware v15, 16S LiFePO4, 5 Temperatursensoren)** ueber
einen dedizierten **RS485-Port** (SP3485-Wandler an GPIO19/20/21) aus und veroeffentlicht alle
Balancer-/Statusdaten per **MQTT** ueber **Ethernet/PoE**. Konfiguriert wird das Geraet
(Netzwerk, MQTT, Web-Passwort, Debug) zur **Laufzeit ueber eine eingebaute Web-UI** - die
Einstellungen liegen in einer eigenen NVS-Partition und ueberleben Firmware-Updates.
Das ist die native ESP-IDF-Umsetzung des Protokolls von
[phinix-org/Multiple-JK-BMS-by-Modbus-RS485](https://github.com/phinix-org/Multiple-JK-BMS-by-Modbus-RS485)
(ohne ESPHome; `jkbms.h`/`jkbms.c` implementieren das **JK BMS RS485 Modbus V1.0**
(Protokoll 001) selbst: Modbus RTU, Funktion 0x03, CRC16-MODBUS, Zellblock plus
Status-/Config-Bloecke, Identity (0x1400) und Protokollnummer (0x14B2)).

## Hardware

Geraet: M5Stack **Unit PoE-P4** (SKU U213, legacy/alte Revision - ESP32-P4 Rev < 3.0,
16 MB Flash, IP101GRI-PHY am internen RMII-Ethernet-MAC).

### Verdrahtung SP3485 <-> Unit PoE-P4

| SP3485-Board | Unit PoE-P4 |
|---|---|
| `RO` (Data Out) | **GPIO20** (RX) |
| `DI` (Data In)  | **GPIO19** (TX) |
| `RE` + `DE` gebrueckt | **GPIO21** (wird beim Senden auf HIGH = Senden gezogen, danach LOW = Empfang) |
| `VCC` | 3V3 |
| `GND` | GND |

### Verdrahtung SP3485 <-> JK-BMS

Angeschlossen wird die Buchse **RS485-1 (Port 1 / UART1) auf der rechten Seite** des BMS
(diese ist immer Modbus-Slave; NICHT die linken Inverter-CAN/RS485-Ports).

RJ45 am BMS (T568B-Belegung, verifiziert):

| BMS RJ45-Pin | Farbe   | Signal | -> SP3485   |
|---|---|---|---|
| Pin 1        | weiss/orange | **B**  | Pin 7 (B)   |
| Pin 2        | orange       | **A**  | Pin 6 (A)   |
| Pin 3        | weiss/gruen  | **GND**| Pin 5 (GND) |

(Kabel: verdrilltes Paar). Keine Antwort? -> **A/B tauschen** (Pin 1 <-> Pin 2).

Am BMS einstellen:
- **UART1-Protokoll = 001** ("JK BMS RS485 Modbus V1.0") im Display-Menue (sonst antwortet
  der BMS nicht auf Modbus; 013 waere 9600 Baud - nicht verwenden).
- **DIP-Switch = Adresse 1** (Schalter 1 ON, Rest OFF) - muss zu
  `CONFIG_JKBMS_MODBUS_SLAVE_ADDR` (Default 1) passen.

UART-Parameter: 115200 Baud, 8N1 (Fixwert, im Polling-Task in main.c).

### Flashen (Download-Modus)

USB-C-Kabel (Download-Port) verbinden -> **Seitenknopf ca. 3 s halten, bis die gruene LED leuchtet** ->
Geraet wartet im Download-Modus. Power laeuft waehrenddessen ueber PoE oder USB (beides ist ok).
Die Konsole (`idf.py monitor`) laeuft ueber denselben USB-Port (USB-Serial-JTAG).

## Software-Stack

- **ESP-IDF 5.4.x** (so baut M5Stack die alte Revision; 5.5.x funktioniert erfahrungsgemaess auch).
  Die neue Revision *PoE-P4X* (Silicon-Rev 3.x) benoetigt ESP-IDF >= 6.1 und ist **nicht** Zielsystem
  dieses Projekts. Falls eine juengere IDF-Version im `menuconfig` eine "Minimum supported
  ESP32-P4 revision" >= 3.0 vorgibt, korrigiere sie (siehe unten).

### Projektstruktur

```
|- CMakeLists.txt            # Projekt-Werkzeug
|- partitions.csv            # 16 MB Flash: nvs/phy/otadata/cfg + ota_0/ota_1 (je 6 MB)
|- sdkconfig.defaults        # Grund-Konfig (Ziel, Flash, Pins, Polling)
`- main/
    |- CMakeLists.txt        # kompiliert + bettet config_page.html ein
    |- Kconfig.projbuild     # Buildzeit-Optionen (Pins, Polling, OTA-Default-URL, Reset-GPIO)
    |- ca_cert.pem           # CA fuer mqtts://, OTA-https und HTTP-Server (ausgeteilt beim Build)
    |- config_page.html      # Web-UI (eingebettet in die Firmware)
    |- main.c                # RS485-UART, Framing, Polling-Task, Reset-Button, ETH/MQTT/Web-Verkabelung
    |- jkbms.h / jkbms.c     # JK-Modbus RTU: CRC16, FC-0x03-Frames, Block-Decoder (alle Felder)
    |- app_config.h/.c       # Laufzeit-Konfig in eigener NVS-Partition 'cfg' (inkl. manueller Update-URL)
    |- app_web.h/.c          # Web-UI (HTTP-Server): Konfiguration, OTA-Button, Update-Check
    |- app_eth.h/.c          # EMAC + IP101GRI, DHCP oder statische IP, Ereignisse, Netzinfo-Cache
    |- app_mqtt.h/.c         # esp_mqtt: Broker (aus Laufzeit-Konfig), JSON-Status, HA-Autodiscovery, Device-Topic
    `- app_ota.h/.c          # OTA: MQTT-Trigger + Web-UI-Button, Rollback-Bestaetigung, Online-Update-Check
```

## Build & Flash

### Voraussetzung

ESP-IDF 5.4.x installieren (Windows: ESP-IDF-Installer, Linux: Standard-Installation).
Danach die "ESP-IDF 5.4 PowerShell" (bzw. `export.sh`/`activate.sh`) oeffnen.

```
cd jk-bms-poe-p4
idf.py set-target esp32p4
idf.py menuconfig     # (optional) - nur noch Pins/Polling/OTA-URL, siehe unten
idf.py build
idf.py -p COM5 flash monitor      # Windows; unter Linux: /dev/ttyACM0 o.ae.
```

- COM-Port im Geraetemanager (USB Serial/JTAG) ermitteln.
- Falls `idf.py monitor` neuentdeckte Bytes verliert: `idf.py -p <PORT> monitor` neu starten reicht.
- Download-Modus vorher aktivieren (Knopf 3 s - gruene LED).
- Nach dem (Re)Flashen: normal starten (RST-Knopf oder Stecker). Logs:

```text
I (...) jkbms: App ... v0.0.3 running from partition ota_0
I (...) jkbms: Firmware v0.0.3 | IP ... 
I (...) app_cfg: Config loaded: IP DHCP, MQTT (off), debug off
I (...) app_eth: DHCP: IP 192.168.x.x, netmask ..., gateway ...
I (...) app_web: Web UI up on port 80 (no password)
I (...) jkbms: JK-BMS online (16 cells, ...)
```

## Konfiguration

Es gibt zwei Konfig-Ebenen:

1. **Laufzeit (Web-UI)**: MQTT-Broker samt Credentials/TLS, DHCP oder statische IP, optionales
   Web-Passwort, RS485-Debugschalter. Gespeichert in der NVS-Partition **`cfg`** -
   **ueberlebt Firmware-Updates (OTA)**. Wirksam erst nach dem Neustart, den der Speicher-Vorgang ausloest.
2. **Buildzeit (menuconfig)**: nur die technisch festen Werte (Pins, Polling-Intervalle, Modbus-Adresse,
   Topic-Praefix, HA-Discovery, Default-OTA-URL, Reset-Button-GPIO).

### Web-UI (Laufzeit-Konfiguration)

Erster Start: das Geraet ist defaultmaessig auf **DHCP** gestellt, MQTT aus, Web-UI ohne Passwort.
Suche die erhaltene IP im Router (Lease-Tabelle) oder im Log (Zeile `app_eth: DHCP: IP ...`),
im Log steht auch die Web-UI-Zeile. Dann einfach im Browser:

```
http://<device-ip>/
```

Die Seite (eingebettet in die Firmware, kein Internet noetig):

- **Web-Oberflaeche**: optionales Basic-Auth-Passwort. Leer lassen = Passwort unveraendert;
  die Checkbox "entfernen" loescht ein gesetztes Passwort. Ohne Passwort ist die Web-UI
  im Netz frei erreichbar.
- **Netzwerk**: DHCP an/aus. Bei ausgeschaltetem DHCP sind IP/Netzmaske/Gateway/DNS Pflicht
  (Validierung: Format `a.b.c.d`). Im DHCP-Modus dient das DNS-Feld als zusaetzlicher
  Fallback-DNS (falls der DHCP-Server keinen brauchbaren Resolver liefert - relevant,
  wenn der Broker nur per Name erreichbar ist).
- **MQTT**: Broker-URI (`mqtt://host:port`), Benutzer, Passwort. Leere Broker-URI schaltet
  MQTT komplett aus (nur Web-UI bleibt aktiv). Die Checkbox **TLS** wandelt `mqtt://`
  beim Verbinden in `mqtts://` (und umgekehrt) und validiert den Server gegen das
  eingebettete `ca_cert.pem`. Passwort leer lassen = unveraendert, Checkbox = entfernen.
- **Diagnose**: RS485-Debug (Hexdump der Rohdaten im Log) zur Laufzeit an/aus.
  GPIO-Layouts und Polling-Parameter bleiben Buildzeit-Werte.
- **Firmware-Update**: Manuelle Update-URL (leer = die bei Buildzeit hinterlegte
  Default-URL `CONFIG_JKBMS_OTA_DEFAULT_URL` wird verwendet) samt Button
  "Manuelle URL zuruecksetzen" (leert das Feld - nach Speichern gilt wieder die
  Default-URL). Der Button "Update von der aktiven URL installieren" installiert
  von dieser URL; der Status laeuft darunter mit. Der Update-Check (regelmaessig
  automatisch bzw. per "Jetzt pruefen") vergleicht die installierte Version gegen
  eine remote-"version"-Datei und zeigt an, ob eine neuere Version verfuegbar ist.

Nach "Speichern" antwortet der Webserver und das Geraet startet neu; erst dann sind die
neuen Werte wirksam (Netzwerk, MQTT, Debug ...). Die laufende Konfig bleibt waehrend
des Betriebs unveraendert (keine Pointer-Rennen im MQTT-Client).

**Migration von einer aelteren Version**: Broker-URI/-Benutzer/-Passwort einmalig in der
Web-UI eintragen (alle Buildzeit-MQTT-Optionen sind entfallen; dafuer ueberlebt die Konfig
ab jetzt jedes Update).

**Werksreset (Recovery)**: Den BOOT-Knopf des Boards (default **GPIO45**, siehe
`CONFIG_JKBMS_RESET_BUTTON_GPIO`, -1 = aus) mindestens **10 Sekunden** gedrueckt halten.
Das loescht die Partition `cfg` komplett (Web-Passwort, IP-Einstellungen, MQTT, Debug,
manuelle Update-URL) und
startet neu in die Defaults: DHCP, Web-UI ohne Passwort, MQTT aus. Das ist der Weg zurueck,
wenn ein statischer IP-Satz nicht mehr passt oder das Web-Passwort verloren geht - das Geraet
reagiert dann wieder auf die frische DHCP-Adresse aus dem Router.

### Buildzeit-Konfiguration (menuconfig / sdkconfig.defaults)

Alle Optionen unter **"Component config -> JK-BMS to MQTT Bridge (M5Stack Unit PoE-P4)"**:

| Option | Default | Bedeutung |
|---|---|---|
| `APP_PROJECT_VER` | `v0.0.3` | **Software-Version** (Menü "Application manager", per `CONFIG_APP_PROJECT_VER_FROM_CONFIG=y` aus der sdkconfig - siehe Abschnitt "Software-Version / Releases"); landet im App-Deskriptor und wird in Log, Web-UI, MQTT und Update-Check verwendet |
| `JKBMS_TOPIC_PREFIX` | `jkbms` | Topics `<prefix>/state`, `<prefix>/available`, `<prefix>/mqtt_available`, `<prefix>/device`, `<prefix>/ota(/status)` |
| `JKBMS_HA_DISCOVERY` | y | Home-Assistant-Autodiscovery publishen (retained) |
| `JKBMS_POLL_INTERVAL_MS` | 5000 | Lesezyklus "Read all registers" |
| `JKBMS_CONFIG_READ_INTERVAL_MS` | 60000 | Re-Read-Intervall des Config-Blocks (Schutzparameter/Switches) in ms; dazwischen bleiben die letzten Werte stehen |
| `JKBMS_STATE_HEARTBEAT_MS` | 60000 | State-Topic wird nur bei Aenderung gesendet; nach dieser Zeit folgt hoechstens ein Heartbeat-Publish |
| `JKBMS_NO_RESPONSE_TIMEOUT_MS` | 30000 | Timeout bis "BMS offline" meldet |
| `JKBMS_MODBUS_SLAVE_ADDR` | 1 | Modbus-RTU-Adresse des BMS (muss zum DIP-Switch passen) |
| `JKBMS_OTA_DEFAULT_URL` | (leer) | Default-Firmware-URL fuer den Web-UI-Update-Button (wird genutzt, wenn keine manuelle URL gesetzt ist) |
| `JKBMS_VERSION_URL` | (leer) | URL der "version"-Textdatei fuer den Update-Check; leer = aus der aktiven Update-URL ableiten (letztes Pfadsegment durch `version` ersetzen) |
| `JKBMS_VERSION_CHECK_INTERVAL_S` | 21600 | Automatischer Update-Check jede x Sekunden (21600 = 6 h); 0 = nur manuell ueber die Web-UI bzw. nach jedem Neustart |
| `JKBMS_RESET_BUTTON_GPIO` | 45 | GPIO des Konfig-Reset-Knopfs (>= 10 s halten); -1 = aus |
| RS485: `JKBMS_RS485_UART_NUM` / `RX` / `TX` / `DE` | 1 / 20 / 19 / 21 | UART + Pins (Pinmap wie oben) |
| Ethernet: `JKBMS_ETH_MDC/MDIO/PHY_RST/PHY_ADDR` | 31 / 52 / 51 / 1 | Pinmap des U213-Boards |

### Software-Version / Releases

Die Versionsnummer steht **in `sdkconfig.defaults`** (Abschnitt "Software-Version"):

```
CONFIG_APP_PROJECT_VER_FROM_CONFIG=y
CONFIG_APP_PROJECT_VER="v0.0.3"
```

Mit `CONFIG_APP_PROJECT_VER_FROM_CONFIG=y` landet der Wert direkt im App-Deskriptor
(`esp_app_get_description()->version`); git-describe/buildzeitliche Alternativen werden
ignoriert. Fuer ein Release genau diese eine Zeile (`CONFIG_APP_PROJECT_VER`) anpassen -
die Version erscheint dann automatisch im Boot-Log, in der Web-UI, im MQTT-Device-Topic
(`<prefix>/device`) und als Referenz des Online-Update-Checks. Nach dem Aendern der
sdkconfig.defaults einmal `idf.py fullclean && idf.py set-target esp32p4 && idf.py build`
(also Partitions-/Versionswerte neu einlesen; bei einem bestehenden `sdkconfig` wird
der neue Wert sonst nicht uebernommen).

Am schnellsten: `idf.py menuconfig` benutzen oder `sdkconfig.defaults` editieren und danach
`idf.py fullclean && idf.py set-target esp32p4 && idf.py build` ausfuehren.

## MQTT-Datenformat

`<prefix>/state` - JSON mit (u. a.):

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
 "zusaetzlich alle Schutz-/Balance-Einstellungen, battery_type, device_type,
 software_version, manufacturer, protocol_version, total_runtime_h, ..."}
```

Zusätzlich gibt es `<prefix>/device` (retained) - einen Momentaufnahme-Status des
Geraets **selbst**: Software-Version (aus der sdkconfig) sowie die aktuellen
Netzwerkdaten (DHCP-leased oder statisch gesetzt) und das Ergebnis des
Online-Update-Checks. Das Topic wird nach jedem MQTT-(Re)Connect und nach jedem
fertigen Update-Check neu veroeffentlicht:

```
{"device_id":"ABCDEF123456","project":"jkbms_mqtt_poe_p4","sw_version":"v0.0.3",
 "idf_version":"v5.4.4","partition":"ota_0","ip_mode":"dhcp",
 "ip":"192.168.1.50","netmask":"255.255.255.0","gateway":"192.168.1.1",
 "dns":"192.168.1.1","dns_backup":"",
 "sw_update_available":false,"sw_update_version":""}
```

Availability ist bewusst doppelt gefuehrt:

- `<prefix>/mqtt_available`: Verbindung Geraet -> Broker. Retained `online` bei Connect,
  der Broker setzt `offline` (Last Will), wenn die MQTT-Verbindung stirbt.
- `<prefix>/available`: Zustand der RS485-Strecke zum BMS. Retained `online` sobald der
  erste gueltige Poll-Zyklus durchlief, `offline` nach dem Quiet-Timeout
  (`JKBMS_NO_RESPONSE_TIMEOUT_MS`). `link_up:false` im State-JSON folgt derselben Logik.

- Das State-JSON enthaelt kein `uptime_s` mehr: `<prefix>/state` wird nur publiziert,
  wenn sich Werte geaendert haben; nach hoechstens `JKBMS_STATE_HEARTBEAT_MS` folgt ein
  Heartbeat-Publish, damit die retained Message aktuell bleibt. Nach jedem
  MQTT-(Re)Connect wird einmal voll gepublisht.
- Fehlende Antworten einzelner Reads sind unsichtbar: die betroffenen Register behalten
  ihre letzten Werte und werden im naechsten Zyklus neu gelesen; erst nach 60 stillen
  Polls in Folge gibt es eine Warnung im Log.

### Home Assistant (falls `JKBMS_HA_DISCOVERY=y`)

Es werden automatisch `homeassistant/sensor|binary_sensor/<prefix>-<Device-ID>/<Feld>/config`-
Discovery-Meldungen (retained) veroeffentlicht: saemtliche Zellspannungen (alle Zellen), Spannungen,
Stroeme, Leistung, Temperaturen, SOC, Kapazitaeten, Zyklen, alle Schutzparameter, Fehler- und
Betriebsbits. Steuer-Switches gibt es bewusst nicht (Read-only-Monitor: das Schreiben von
Registern ist nicht implementiert).

## OTA-Firmware-Update

Web-UI-Unabhaengig aktualisierbar; drei Ausloeser, gleicher Transport:

1. **MQTT-Trigger**: firmware-artige URL (`http://...`/`https://...`) im Payload von
   `<prefix>/ota` publishen. Ablauf:
   - `idf.py build` -> `cd build` -> `python -m http.server 8000`
   - `mosquitto_pub -h <broker> -u <user> -P <pass> -t jkbms/ota -m "http://<PC-IP>:8000/jkbms_mqtt_poe_p4.bin"`
2. **Web-UI-Button**: laedt von der **aktiven Update-URL** - der in der Web-UI
   eingetragenen manuellen URL, sonst (Feld leer) der Buildzeit-Default-URL
   `CONFIG_JKBMS_OTA_DEFAULT_URL` (`http://` ist erlaubt, siehe unten zum
   TLS-Verhalten). Mit "Manuelle URL zuruecksetzen" + Speichern gilt wieder
   ausschliesslich die Default-URL.
3. **Online-Update-Check** zeigt nur an, wechselt keine Firmware:
   - Beim ersten IP-Up startet ein Hintergrund-Task, der regelmaessig
     (`JKBMS_VERSION_CHECK_INTERVAL_S`, default 6 h) per HTTP-GET eine
     Klartext-"version"-Datei laedt (fuer GitHub-Releases z. B. einfach eine
     `version`-Datei ins Release legen, darin z. B. `v0.0.4`).
   - Die URL ergibt sich aus der aktiven Update-URL (letztes Pfadsegment ->
     `version`) oder aus `JKBMS_VERSION_URL`. `https://github.com/.../releases/
     latest/download/version` folgt auch Redirects automatisch.
   - Ist die remote-Version numerisch neuer als die installierte
     (`CONFIG_APP_PROJECT_VER`), zeigt die Web-UI "**UPDATE VERFUEGBAR**"
     unter "Update-Check" an und `<prefix>/device` enthaelt
     `sw_update_available:true` samt Version. Installieren muss man dann
     selbst (Button oder per Skript automatisiert).

### TLS-Server-Verifizierung aktuell DEAKTIVIERT (temporär, unsicher!)

Das eingebettete `ca_cert.pem` ist bislang ein **Platzhalter** (kein echtes
Zertifikat). Ein https-OTA-Versuch bricht deshalb beim TLS-Verbindungsaufbau
ab (`mbedtls_x509_crt_parse of CA cert returned -0x112C`, also
`PEM_INVALID_DATA + BASE64_INVALID_CHARACTER` - der Dateiinhalt ist gar kein
PEM-Base64). Bis eine echte CA-Kette eingebettet ist, validiert die Firmware
TLS-Serverzertifikate **nicht** (Verschluesselung bleibt aktiv, die
Server-Identitaet wird aber nicht geprueft - MITM moeglich!):

- Erlaubt via `CONFIG_ESP_TLS_INSECURE=y` +
  `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y` (sdkconfig.defaults, beide Dateien
  sind mit "TEMPORARY" markiert).
- OTA (https und http) und MQTTS laufen dadurch ohne Cert-Pruefung; beide
  Codepfade loggen beim Start eine `DISABLED`-Warnung.

**Wenn wieder sicher:** echtes Server-Zertifikat bzw. die Signier-Kette
(die Reihenfolge im `ca_cert.pem`: Serverzertifikat + Intermediate + Root -
oder nur Root/Intermediate, wenn der Server sie mitschickt; Beispiel:
`openssl s_client -showcerts -connect github.com:443 </dev/null 2>/dev/null |
sed -n '/-----BEGIN CERT/,/-----END CERT/p' > main/ca_cert.pem`), dann
sdkconfig.defaults die zwei `ESP_TLS_*`-Zeilen entfernen, `idf.py fullclean`
und beide `DISABLED`-`#if`-Blocks in `main/app_ota.c` / `main/app_mqtt.c`
durch die Attach-Variante ersetzen (stehen dort "TEMPORARY" markiert).

Das Ergebnis landet unter `<prefix>/ota/status` (retained): `"downloading 30%"`,
`"success, rebooting"` oder `"failed: ..."`; derselbe Text steht in der Web-UI.

Sicherheitsnetz: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` - das neue Image bootet
als "pending verify" und bestaetigt sich selbst erst, wenn MQTT laeuft und ein
Poll-Zyklus gueltige BMS-Daten geliefert hat. Ohne Bestaetigung bootet der
Bootloader beim naechsten Neustart automatisch wieder den alten Slot (kein
Brick-Risiko).

Migration auf die OTA-Partitionstabelle (einmalig ueber USB): die Tabelle hat
statt `factory` jetzt `ota_0`/`ota_1` + `otadata`; `idf.py -p COM5 flash`
schreibt das Image nach `ota_0` und bootet daraus (blank `otadata` = Slot 0).
Empfehlung: einmal `idf.py -p COM5 erase-flash` davor. Die Partitionen
`nvs`/`cfg` werden dadurch nur zurueckgesetzt - die Laufzeit-Konfig in `cfg`
muss danach einmal eingegeben werden; spaeter bleibt sie aber ueber jedes
OTA hinweg erhalten (Partition taucht nicht im App-Bereich auf).

## Wichtige Hinweise / Troubleshooting

- **"alte Revision"**: legacy U213 (ESP32-P4 Rev < 3.0) + ESP-IDF 5.4.x. Revisionen >= 3.0 (PoE-P4X)
  sind mit dieser Version **nicht** kompatibel. Falls `idf.py menuconfig` eine
  "Minimum supported ESP32-P4 revision" von >= 3.0 anzeigt (juengere IDF-Version), setze sie
  im Menu *Component config -> Hardware Settings -> Chip revision* auf 1.0/2.0 zurueck oder
  bleibe bei ESP-IDF 5.4.x.
- Der RS485-Port haelt den BMS wach; ein Sleep-Modus (Deep Sleep) des BMS blockiert die
  Kommunikation - im BMS ggf. die Sleep-Wartezeit entsprechend konfigurieren.
- Die 5 Temperatursensoren: Der Frame dieses Protokolls uebertraegt MOS-Temperatur,
  Batterie-Temperaturen T1/T2 und (v15-Firmware) T3/T4/T5; das Feld
  `temperature_sensor_count` zeigt, wie viele Sensoren der BMS meldet.
- CRC-Fehler im Log -> meist A/B vertauscht, fehlende gemeinsame GND-Referenz oder falsche
  Baudrate (must: 115200 8N1).
- `MQTT_ERROR` (DNS): Der Broker-Host muss via DNS aufloesbar sein. `.local`-mDNS-Namen
  funktionieren nur, wenn eine mDNS-Basis vorhanden ist - im Zweifel einen echten DNS-Namen,
  die IP des Brokers oder das DNS-Feld der Web-UI (Fallback im DHCP-Modus) verwenden.
- **Geraet verkonfiguriert?** (statische IP passt nicht mehr / Web-Passwort vergessen):
  BOOT-Knopf 10 Sekunden halten -> Werksreset der Konfig (siehe oben). Danach
  wieder per DHCP-Adresse erreichbar.
