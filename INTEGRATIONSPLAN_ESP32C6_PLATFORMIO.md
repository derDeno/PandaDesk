# PandaDesk – Integrationsplan für ESP32-C6 und PlatformIO

Stand: 20.09.2026 · Zielbranch: `dev` · Grundlage: `PandaDesk_Projektzusammenfassung.md` (Hardware V1).

## 1. Zielbild und Abgrenzung

Dieses Vorhaben erzeugt eine wartbare PandaDesk-Firmware für die vorhandene V1-Platine. Sie vermittelt im Inline-Betrieb die Handset-zu-Controller-UART-Richtung, beobachtet die Controller-Antworten und fügt eigene, profilierte Befehle kollisionsfrei ein. In einem Zubehörport-Betrieb sendet sie nur über OUT. Tischprofile sind Daten plus Protokollimplementierung für **konkret qualifizierte** Controller-/Handset-Kombinationen; ein Markenname oder ein RJ-Stecker ist kein Profil.

V1 unterstützt genau ein angeschlossenes Portpaar und niemals automatische Tischerkennung. Die Firmware muss weder unbekannte Pinouts noch >5-V-Schnittstellen absichern. Ein ausgeschaltetes, zurückgesetztes oder fehlerhaftes PandaDesk unterbricht die Handset-Senderichtung; das lässt sich rein in Software nicht zu einem Bypass machen.

**Abnahmekriterium des ersten Releases:** Ein freigegebenes, real vermessenes Referenzmodell kann sicher starten, Handset-Befehle unverändert und mit gemessener Latenz weiterleiten, eigene Befehle nur an gültigen Transaktionsgrenzen senden, zuverlässig stoppen und nach Reset/Kommunikationsfehler keine alte Bewegung fortsetzen. Weitere Modelle werden erst nach eigener Qualifizierung aktiviert.

## 2. Verbindliche Hardware-Schnittstelle

Die Firmware nutzt ausschließlich diese Belegung. Die GPIO-Nummern sind keine U1-Modulpins.

| Funktion | GPIO | Sichere Grundstellung |
|---|---:|---|
| Controller-TX `E_TX` | 19 | UART-idle HIGH, keine Boot-/Logausgabe |
| Controller-RX `E_RX` | 18 | Eingang |
| Handset-RX `E_RX_HANDSET` | 5 | Eingang |
| U3-OE `EN_TX` | 15 | LOW vor Profil-/UART-Initialisierung |
| Loctek Wake `E_WAKE` | 14 | LOW |
| Jiecang HS0…HS3 | 23, 22, 21, 20 | HIGH = U4 hochohmig/freigegeben |
| Status-LED | 7 | aus bzw. definierter Startzustand |
| I²C SDA/SCL | 0 / 1 | reserviert, V1-Produktpfad optional |
| native USB D−/D+ | 12 / 13 | USB-CDC für Diagnose/Flash, nicht Desk-UART |

`EN_TX` ist ein gemeinsames Enable des TXU0204: LOW trennt nicht nur den Desk-TX, sondern auch beide Empfänger vom ESP. Deshalb bleibt es im Bereitschaftsmodus HIGH, sobald die UARTs und der gültige Profilzustand bereit sind. Ein Fehler darf nicht reflexartig mit `EN_TX=LOW` behandelt werden, wenn der Handset-Pfad noch benötigt wird.

## 3. Technische Basis und Repository-Aufbau

### 3.1 Rahmenentscheidung

Als Primärframework wird **ESP-IDF unter PlatformIO** verwendet, nicht Arduino. Gründe sind die benötigten zwei unabhängigen Hardware-UART-Empfänger, GPIO-Matrix-Konfiguration, FreeRTOS-Aufgaben, Event-/Ringpuffer, native USB-CDC, Watchdogs und testbare Komponenten. Arduino kann später als UI-/Integrationsschicht ergänzt werden, darf aber die Desk-UART- und Sicherheitslogik nicht besitzen.

Die Toolchain wird reproduzierbar gepinnt. `platformio.ini` verwendet `platform = espressif32@<geprüfte Version>`, `framework = espidf` und zunächst ein lokales Board-Manifest `boards/pandadesk-c6.json` mit `esp32c6`, 4 MB Flash und den wirklichen Upload-/Debug-Eigenschaften der V1. Das verbreitete DevKit-Board wird nur für frühe Bring-up-Tests verwendet; seine Flash-Größe und Beschaltung dürfen nicht stillschweigend für PandaDesk gelten. Vor dem ersten Commit mit Firmware: `pio pkg show`, ESP-IDF-Version und `sdkconfig.defaults` im Build-Protokoll festhalten.

Vorgeschlagene Struktur:

```text
platformio.ini                  # CI- und Hardware-Umgebungen, Versionen
boards/pandadesk-c6.json        # Produktionsboard, 4 MB
sdkconfig.defaults              # USB-CDC, Flash, Log- und Watchdog-Defaults
src/app_main.cpp                # Komposition, kein Protokollcode
include/pandadesk/pins.hpp      # einzige Quelle der GPIO-Zuordnung
components/
  hal/                           # sichere Pins, U3/U4/U5, LED, USB-Diagnose
  transport/                     # UART-Empfang, Sender und TX-Arbiter
  protocol/                      # Frame-Parser, Checksums, Profile-Interfaces
  desk/                          # Zustandsautomat, Wake, Bewegungssicherheit
  profiles/                      # qualifizierte Modellprofile
  storage/                       # NVS-Konfiguration und Profilmigration
  api/                           # lokale HTTP-/WebSocket-API, authentifiziert Befehle
  webui/                         # eingebettete, versionierte statische Web-Oberfläche
  mqtt/                          # MQTT-Client, HA Discovery und Zustands-Publisher
  provisioning/                 # AP/Captive Portal, WLAN-Test und Neustart
  updater/                      # signierte App-/Dateisystem-OTA und Rollback
  diagnostics/                   # strukturierte USB-Logs, Zähler, Testtracing
test/                            # Native/Unity-Komponententests
tools/                           # Upstream-Vektorimport, Replay- und Profilvalidierung
docs/compatibility/              # Messprotokolle je Modell
```

Keine Desk-Framing-Bytes, Baudraten oder Wake-Zeiten gehören als globale Konstanten in `main`. Sie liegen ausschließlich in einem qualifizierten Profil mit Versionskennung.

### 3.2 Konfiguration und Secrets

NVS enthält nur Nutzerkonfiguration (aktives Desk-Profil, Betriebsmodus, zulässige Automationen, Web-UI-Sprache, IANA-Zeitzone, Netzwerkzugang, API-Token-Hash und MQTT-Zugangsdaten). Secrets und Testdaten kommen nicht ins Repository; `secrets.example.h` bzw. PlatformIO-Environment-Variablen dokumentieren die benötigten Werte. Beim Start wird die gespeicherte Konfiguration gegen Schema- und Profilversion validiert. Fehlt oder scheitert sie, startet PandaDesk im **Safe/Unconfigured**-Zustand ohne aktive Desk-Befehle und ohne Inline-Forwarding.

### 3.3 Erstinbetriebnahme und WLAN-Provisionierung

Ohne gültige WLAN-Zugangsdaten startet PandaDesk als geschützter Access Point mit einer eindeutigen SSID wie `PandaDesk-<6 Hexzeichen>`, lokalem DNS-Captive-Portal und ausschließlich der Provisionierungsoberfläche. Das initiale AP-Passwort bzw. der Pairing-Code ist geräteindividuell und wird am Gerät/Beileger bereitgestellt; es gibt kein gemeinsames Standardpasswort. Während der Provisionierung bleiben `EN_TX` LOW, Wake LOW und alle HS-Ausgänge freigegeben, sodass keine Tischbefehle ausgelöst werden.

Der Assistent scannt WLANs, übernimmt SSID und Passwort, testet die Verbindung mit Zeitlimit und speichert die Zugangsdaten erst nach erfolgreicher Station-Verbindung. Danach beendet er den AP und startet kontrolliert im Station-Modus neu. Bei ungültigen Zugangsdaten oder wiederholtem Verbindungsfehler fällt das Gerät nach einer begrenzten Anzahl Versuche in den Provisionierungs-AP zurück; ein dokumentiertes langes Drücken von BOOT beim Start erzwingt ebenfalls den AP-Modus, ohne Desk-Profil oder Diagnosehistorie automatisch zu löschen. Das Löschen aller Secrets erfordert eine separate, bestätigte Factory-Reset-Aktion.

Im normalen Betrieb läuft kein offener Konfigurations-AP parallel. Die Web UI zeigt Verbindungsversuche und Fehler verständlich an, offenbart aber niemals gespeicherte WLAN- oder MQTT-Passwörter. Hostname/mDNS werden aus einem validierten Gerätenamen abgeleitet.

### 3.4 Lokale Web UI und Steuer-API

Die Web UI ist ein schlanker Single-Page-Client aus einem versionierten Dateisystem-Bundle. Sie wird ausschließlich über dieselbe versionierte lokale HTTP-API versorgt und enthält keine zweite Desk-Logik. Die Bedienseite zeigt Profil, Online-/Fehlerzustand, aktuelle Höhe inklusive Datenqualität, Bewegung, Wake-Zustand, Kindersperren-Status, Netzwerk-/MQTT-Status und Diagnosezähler. Bedienung bietet Halten für Auf/Ab, Stop, Zielhöhe, gespeicherte Positionen sowie Kindersperre **nur dann**, wenn das aktive Profil diese Fähigkeiten ausweist. Unverfügbare Fähigkeiten werden nicht als wirkungslose Steuerung angezeigt.

Zusätzliche Seiten sind verbindlich:

- **Info:** Geräte-ID, Hardware-/Boardrevision, Firmwareversion und Build-Commit, Web-UI-/Dateisystemversion, aktives/kompiliertes Profil, Uptime, Resetgrund, WLAN-SSID ohne Passwort, IP/MAC, RSSI in dBm mit Qualitätsstufe, MQTT-Verbindung, aktuelle freie Heap-Größe, niedrigster freier Heap seit Boot, größte freie Allokation und Flash-/Dateisystembelegung.
- **Log:** paginierte, filterbare strukturierte Ereignisse mit lokaler Zeit und monotonem Zeitstempel, Level, Komponente, Quelle (`handset`, `webui`, `api`, `mqtt`, `system`), Request-ID, Aktion und Ergebnis. Dazu gehören angenommene/abgelehnte Desk-Befehle, Stop/Prioritätsentscheidung, Wake/Timeout, WLAN/MQTT/OTA und Konfigurationsänderungen. Keine Zugangsdaten, Tokens oder vollständigen sensitiven Payloads.
- **Settings:** Desk-Profil/Protokoll und Betriebsmodus, Web-UI-Sprache, IANA-Zeitzone, Gerätename, WLAN-Neukonfiguration, MQTT-Broker/TLS/Zugangsdaten sowie Diagnoselevel. Profilwechsel ist nur im bestätigten Stillstand möglich, validiert Fähigkeiten und wird erst nach bestätigtem Neustart aktiv.
- **Update:** Firmware- und Web-UI-/Dateisystem-Image auswählen, Manifest/Signatur/Kompatibilität prüfen, Fortschritt anzeigen, Neustart auslösen und den erfolgreichen/fehlgeschlagenen Slot anzeigen.

Die UI-Sprachen werden als versionierte Kataloge gebaut; Englisch ist Fallback bei fehlendem Schlüssel. Die Zeitzone verwendet einen IANA-Namen (beispielsweise `Europe/Berlin`), SNTP liefert UTC und Logs speichern UTC plus monotonic timestamp. Ist noch keine Netzzeit verfügbar, zeigt die UI den Zeitstatus ausdrücklich als unsynchronisiert.

Alle Bewegungsbefehle enden am `desk_state_task` und anschließend am exklusiven TX-Arbiter; HTTP, WebSocket und MQTT sind gleichberechtigte, aber nicht privilegierte Intent-Quellen. Die UI erhält Zustandsänderungen über einen lokalen WebSocket (`/api/v1/events`) und fällt bei Verbindungsabbruch auf lesende Abfragen zurück. Ein UI-Klick gilt erst nach API-Annahme als angenommen, nicht als bestätigte Tischbewegung.

Der API-Vertrag liegt als versionierte OpenAPI-Datei unter `docs/api/openapi.yaml`. Minimaler V1-Vertrag:

| Methode / Pfad | Zweck | Ergebnis |
|---|---|---|
| `GET /api/v1/state` | vollständiger, atomarer Desk-/Geräte-Status | Profil, Fähigkeiten, Höhe/Einheit/Qualität, Bewegung, Sperre, Fehler, Sequenznummer |
| `GET /api/v1/capabilities` | vom aktiven Profil erlaubte Funktionen | verhindert unzulässige UI-/Client-Befehle |
| `GET /api/v1/info` | Versionen, Netzwerk, RSSI, Uptime, Resetgrund, Heap-/Flashwerte | Daten für die Info-Seite, keine Secrets |
| `GET /api/v1/logs` | cursorbasierte strukturierte Events mit Filter/Limit | Daten für Log-Seite ohne ungebremste Heap-Nutzung |
| `GET/PUT /api/v1/settings` | lesbare Einstellungen bzw. validierte Änderungen | Secrets nur schreibbar/maskiert; Neustartpflicht kenntlich |
| `POST /api/v1/commands/move` | `up`, `down`, `stop`; Startbefehle enthalten eine Ablaufzeit | `202` mit Befehls-ID, niemals stilles Fahren |
| `POST /api/v1/commands/height` | Zielhöhe in profilierter Einheit | `409/422`, wenn Höhe/Positionierung nicht unterstützt oder Zustand unsicher |
| `POST /api/v1/commands/preset/{id}` | profilierte gespeicherte Position | `202` mit Befehls-ID |
| `PUT /api/v1/child-lock` | Sperre setzen/lösen, nur bei Profilfähigkeit | bestätigter bzw. abgelehnter Status |
| `GET /api/v1/commands/{id}` | Annahme, Fortschritt, Controller-Bestätigung/Timeout | nachverfolgbarer Ausgang |
| `POST /api/v1/update/firmware` | Firmware streamen, prüfen und in inaktiven OTA-Slot schreiben | Fortschritt, Validierung und geplanter Neustart |
| `POST /api/v1/update/filesystem` | Web-UI-Dateisystem streamen und in inaktiven Slot schreiben | Bundle-/API-Kompatibilität vor Aktivierung |

`POST`-Nutzlasten enthalten `request_id` und optional eine erwartete Zustandssequenz; identische IDs sind idempotent, veraltete Zustände werden abgelehnt. Eingaben werden begrenzt und profiliert validiert. „go to height“ ist kein zeitbasierter Blindlauf: Es ist nur verfügbar, wenn das Upstream-Profil eine zuverlässige Höhenrückmeldung und Zielhöhen-Logik bietet. Die API gibt die letzte bekannte Höhe zusammen mit `known`, `stale` oder `unknown` aus.

Die Erstkonfiguration erzwingt ein lokales Gerätepasswort bzw. einen Einmal-Pairing-Code und erstellt danach einen separat widerrufbaren API-Token. Browserzugriffe nutzen sichere Session-Cookies, CSRF-Schutz und Same-Origin-Prüfung; Automationen verwenden Bearer-Token. Updates und sicherheitsrelevante Settings verlangen eine frische Re-Authentifizierung. Die Firmware akzeptiert keine Steuerung ohne Authentifizierung, erlaubt keine CORS-Freigabe per Standard und protokolliert niemals Tokens oder MQTT-Passwörter. Für externen Zugriff wird ein vorhandener VPN/Reverse Proxy empfohlen; unverschlüsseltes Geräte-HTTP wird nicht direkt ins Internet exponiert.

### 3.5 Home Assistant über MQTT

PandaDesk integriert sich ohne eigene Home-Assistant-Custom-Integration über MQTT Device Discovery. Die Konfiguration veröffentlicht ein Gerät mit stabiler `device.identifier` aus der ESP-MAC/Serienkennung, Hersteller `PandaDesk`, Modell/Hardwareversion, **laufender Firmwareversion** (`sw_version`), Konfigurations-URL zur Web UI und stabilen Entity-`unique_id`s. Nach erfolgreichem OTA werden Discovery und Zustand mit der neuen Firmwareversion erneut veröffentlicht. Discovery-Konfigurationen werden retained veröffentlicht und bei HA-Birth auf `homeassistant/status` erneut gesendet; Verfügbarkeit nutzt MQTT Last-Will/Birth auf `pandadesk/<device-id>/availability`. Bewegungsbefehle werden immer mit `retain=false` publiziert bzw. verarbeitet, damit kein alter Auf-/Ab-Befehl nach Broker-Neustart fährt.

| HA-Entität | MQTT-Rolle | Nur wenn Profil dies kann |
|---|---|---|
| `cover.pandadesk` | `OPEN` = auf, `CLOSE` = ab, `STOP`; Bewegungs-/Positionsstatus | Auf/Ab/Stop |
| `sensor.pandadesk_height` | aktuelle Höhe mit Einheit und Datenqualität | Höhenrückmeldung |
| `number.pandadesk_target_height` | Zielhöhe setzen, bestätigt erst über Status | zuverlässige Zielhöhensteuerung |
| `switch.pandadesk_child_lock` | Kindersperre mit tatsächlichem State-Topic | Kindersperre |
| `button.pandadesk_preset_1…4` | gespeicherte Position auslösen | jeweilige Presets |
| `sensor.pandadesk_fault` / `binary_sensor.pandadesk_ready` | Fehler und sichere Betriebsbereitschaft | immer |

Der Namensraum ist `pandadesk/<device-id>/…`, beispielsweise `state`, `availability`, `command/cover`, `command/target-height`, `command/child-lock` und `event`. Befehle tragen eine `request_id`; die Verarbeitung übersetzt sie in den gemeinsamen Intent-Typ und veröffentlicht Ergebnis, Fehlercode und Zustandssequenz auf `event`/`state`. Der MQTT-Client darf keine Bewegung bestätigen, bevor die profilierte Controller-Antwort bzw. der sichere Timeout vorliegt. Zustände dürfen retained sein, erhalten aber Zeitstempel und Qualitätsstatus; ein nach Neustart wiederhergestellter retained Zustand wird nicht als aktuelle Tischwahrheit verwendet, bis das Profil ihn aktualisiert hat.

Brokeradresse, Port, TLS-Modus, CA-Zertifikat/Fingerprint, Benutzer und Passwort sind konfigurierbar und secrets-geschützt in NVS abgelegt. V1 unterstützt einen Broker und authentifiziert ihn; bei fehlendem Broker, WLAN-Ausfall oder HA-Neustart läuft der lokale Desk-/Handset-Pfad weiter. MQTT-Reconnect und Discovery dürfen weder die UART-Aufgaben noch den Sender blockieren.

### 3.6 OTA- und Dateisystemstrategie

Firmware-OTA verwendet die ESP-IDF-A/B-App-Slots (`ota_0`, `ota_1`) plus `otadata`, Imagevalidierung und App-Rollback. Ein neues Image wird nur im bestätigten Stillstand angenommen, signatur- und boardgeprüft, in den inaktiven Slot geschrieben und erst nach Hashprüfung gebootet. Der erste Boot führt einen kurzen Self-Test für NVS-Schema, Dateisystem/API-Kompatibilität, sichere GPIO-Zustände, Heap und Netzwerkgrundfunktion aus; erst danach wird das Image gültig markiert. Andernfalls erfolgt automatischer Rollback.

Das Web-UI-/Dateisystem besitzt eine eigene semantische Version und eine deklarierte API-Kompatibilitätsspanne. Bevorzugt werden zwei Dateisystem-Slots mit kleinem atomarem Aktiv-Marker, sodass auch ein Stromausfall beim Upload das aktive Bundle erhält. Firmware- und Dateisystemimage können getrennt hochgeladen werden, werden aber nur aktiviert, wenn Manifest, SHA-256, Signatur, Board-ID, Größenlimit und gegenseitige Versionsanforderungen passen. Ein inkompatibles oder beschädigtes UI-Bundle führt auf eine minimale, in der Firmware enthaltene Recovery-/Update-Seite zurück.

Da U1 nur 4 MB Flash besitzt, wird in Phase 1 eine feste `partitions.csv` samt CI-Größenbudgets erstellt. Sie muss Bootloader/Partitionstabelle, NVS, PHY, `otadata`, zwei ausreichend große App-Slots und zwei Web-Dateisystem-Slots aufnehmen. Passt ESP-IDF mit Wi-Fi, TLS, MQTT und Webserver nicht mit Sicherheitsreserve in beide App-Slots, wird das Web-Bundle komprimiert in die App-Slots integriert und zusammen mit der Firmware atomar aktualisiert; die UI zeigt weiterhin getrennte Firmware- und Web-Bundle-Versionen. Ein unsicheres In-place-Update einer einzelnen Datenpartition ist keine Freigabeoption.

OTA pausiert keine manuelle Handset-Weiterleitung, sofern der sichere Profilpfad dies erlaubt; neue PandaDesk-Bewegungsbefehle sind während Download, Schreiben, Validierung und Neustart gesperrt. Uploads sind größenbegrenzt, streaming-basiert und dürfen nicht vollständig im Heap landen.

## 4. Laufzeitarchitektur

### 4.1 Aufgaben, Besitz und Datenfluss

| Einheit | Verantwortung | Darf zum Controller senden? |
|---|---|---|
| `controller_rx_task` | Bytes von GPIO18, Zeitstempel, RX-Fehler, Controller-Parser | Nein |
| `handset_rx_task` | Bytes von GPIO5, Zeitstempel, Handset-Parser | Nein |
| `desk_state_task` | Profilzustand, Wake, Antworten, Freigaben und Fehlerreaktion | Nur durch Anforderung an Arbiter |
| `tx_arbiter_task` | eine FIFO, vollständige Frames, Priorität und UART19 | **Ja, exklusiv** |
| `api_task` | HTTP/WebSocket/UI-Befehle authentifizieren und in geprüfte Intents wandeln | Nein |
| `mqtt_task` | HA Discovery, MQTT-Status und MQTT-Intents | Nein |
| `diagnostics_task` | strukturierte Eventqueue, USB-CDC, Web-Log, Metriken und Testtracing | Nein |

Beide RX-Aufgaben lesen ohne Netzwerkzugriffe aus getrennten ESP-IDF-UART-Instanzen bzw. validierter GPIO-Matrix-Zuordnung. `E_TX` wird nur vom exklusiven Sender angehängt. Vor der ersten Implementierung ist auf echter C6-Hardware nachzuweisen, welche UART-Controller/Matrix-Routen gleichzeitig GPIO18/19 und GPIO5 mit dem gewählten ESP-IDF bereitstellen; UART0 bleibt im Flash-/ROM- und ggf. Diagnosepfad. Eine Software-UART ist kein Produktionsfallback.

RX-Bytes erhalten einen monotonic timestamp und werden in begrenzten Ringpuffern abgelegt. Parser geben `Frame`, `PartialFrame`, `Timeout` oder `ParseError` aus. Ein ungeklärtes Teilframe blockiert den Sender nur bis zum profildefinierten Interbyte-/Frametimeout; danach wird es verworfen und resynchronisiert. Pufferüberlauf verwirft niemals später noch eine alte Bewegungsanweisung als gültigen Befehl.

Alle Komponenten schreiben über eine nicht blockierende, größenbegrenzte Eventqueue in ein gemeinsames strukturiertes Logschema. Ein Event enthält Sequenznummer, UTC sofern verfügbar, monotonic timestamp, Level, Komponente, Eingangsquelle, Request-ID, Ereigniscode und redigierte Parameter. Das Diagnose-Task fächert Events auf USB-CDC, WebSocket und einen begrenzten RAM-Ringpuffer auf. Eine optionale persistente Historie nutzt rotierende, größenbegrenzte Dateien und Schreib-Batching zur Begrenzung des Flashverschleißes; Debug-Frames werden standardmäßig nicht persistent gespeichert. Queue-Überlauf wird gezählt und als zusammengefasstes Warnereignis sichtbar, blockiert aber niemals UART oder Stopverarbeitung.

### 4.2 Sender-Arbitrierung und Priorität

Der Arbitrierer akzeptiert nur vollständig validierte Frames und besitzt diese Regeln:

1. Handset-Frame weiterleiten, sobald das Profil ihn akzeptiert; keine byteweise Interleaving.
2. Handset-Stop und Richtungswechsel verdrängen wartende Automation/Abfragen.
3. PandaDesk-Frames dürfen nur in profildefinierten Leerlücken bzw. nach passender Antwort eingefügt werden.
4. Polling wird bei Handset-Aktivität, Bewegung, Fehler oder noch offenen eigenen Transaktionen ausgesetzt.
5. Jedes Frame hat Quelle, Priorität, Ablaufzeit, Profilversion und Bewegungswirkung. Abgelaufene Frames werden verworfen.
6. Controller-Antworten werden nie zurück zum Controller gespiegelt; das Handset erhält sie elektrisch direkt.

Die `desk_state_task` verwaltet einen einzelnen ausstehenden eigenen Vorgang. Sie koppelt Antworten über profilierte Korrelationslogik zu, erzwingt Timeouts und sperrt bei unklarer Lage neue Bewegungen. UART-Schreibfehler führen nicht zu stillen Retries einer Bewegung; ein Retry ist nur erlaubt, wenn das konkrete Profil ihn ausdrücklich als idempotent definiert.

### 4.3 Sicherheits-Zustandsautomat

```text
BOOT_SAFE -> CONFIG_VALIDATE -> UART_READY -> PROFILE_INIT -> READY
    |             |                 |              |          |
    +-> FAULT <---+-----------------+--------------+----------+
READY <-> WAKING <-> ACTIVE <-> WAITING_RESPONSE
FAULT -> SAFE_RECOVERY -> (PROFILE_INIT | BOOT_SAFE)
```

- **BOOT_SAFE:** U3 LOW, Wake LOW, HS HIGH/freigegeben; Latches vor Output-Konfiguration vorbelegen.
- **CONFIG_VALIDATE:** Konfiguration und Profil vollständig prüfen. Ungültig bedeutet Safe/Unconfigured.
- **UART_READY:** GPIO19 auf idle HIGH, beide RX-Pfade, Puffer und Sender bereit; kein Desk-Log.
- **PROFILE_INIT:** U3 HIGH, anschließend ausschließlich profilierte Initialisierung.
- **READY:** Wake-Erkennung und gültige Handset-Weiterleitung aktiv; keine ungeprüften Aktionen.
- **WAKING/ACTIVE:** Wake und Bewegung nur mit Fristen; neue Handset-Eingabe hat Vorrang.
- **FAULT:** eigene Befehle und Polling stoppen, Wake zurücknehmen, HS freigeben, Diagnose sichern. U3 nur deaktivieren, wenn das Profil bzw. der sichere Fehlerzustand dies fordert. Ein Hardware-OE ist kein Stopptelegramm.

Der Task-Watchdog überwacht RX-, Sender- und Zustandsaufgabe. Vor kontrolliertem Reset werden keine Frames wiederholt. Beim Reboot existiert keine persistierte „fortsetzbare Bewegung“. Für jedes Profil ist verbindlich festzulegen, ob ein Stop durch Loslassen, HS-Freigabe oder Stopframe erfolgt und wann der Bewegungszustand als unbekannt gilt.

## 5. Protokollbasis, Profilmodell und Qualifizierung

### 5.1 Protokolle aus bestehenden ESP-Projekten übernehmen

Die Protokolldefinitionen werden nicht durch neue Mitschnitte hergeleitet. Stattdessen wird für jede PandaDesk-Familie ein nachvollziehbarer, versionierter Upstream als fachliche Quelle ausgewählt, dessen Implementierung in eine framework-unabhängige PandaDesk-Komponente übertragen wird. Übernommen werden Frameaufbau, Prüfsumme, Befehlswerte, Poll-/Antwortregeln, Wake-Ablauf, Timing und bekannte Modellgrenzen; PandaDesk ergänzt dazu ausschließlich seine Inline-Weiterleitung und seinen Sicherheits-Arbiter.

| PandaDesk-Familie | Primäre Quellimplementierung | Zu übernehmender Stand |
|---|---|---|
| Jiecang RJ12 und Jiecang/Jarvis RJ45 | [Rocka84/esphome_components – `jiecang_desk_controller`](https://github.com/Rocka84/esphome_components/tree/master/components/jiecang_desk_controller), ergänzt durch [phord/Jarvis](https://github.com/phord/Jarvis) | UART 9600/8N1, Jiecang-Kommandos/Antworten und die für konkret benannte Jarvis-Controller dokumentierte Frameform sowie HS-Matrix |
| Loctek/FlexiSpot RJ45, Zubehörport | [dimitri-vs/flexispot-esphome](https://github.com/dimitri-vs/flexispot-esphome) | 9600/8N1, `9B … 9D`-Frames mit CRC16, Controller-Poll `0x11`, Buttonantwort `0x02`, Wake und Poll-synchrones Senden |
| Loctek/FlexiSpot RJ45, Inline | [iMicknl/LoctekMotion_IoT](https://github.com/iMicknl/LoctekMotion_IoT), insbesondere dessen ESPHome-Pass-through-Paket | Pass-through-Topologie und bekannte Loctek-Portfunktion; Protokollkern wird mit dem obigen FlexiSpot-Treiber vereinheitlicht |

Phase 1 legt die verwendeten Upstream-Commit-IDs in `docs/upstream-protocols.md` fest und archiviert weder fremden Quellcode noch unveränderte ganze Dateien im Repository. Jede Übernahme enthält eine Quellen-URL, Commit-ID, Lizenzprüfung, übernommene Symbole/Verhaltensregeln und eine PandaDesk-spezifische Abweichungsbegründung. Bei inkompatibler Lizenz wird die veröffentlichte Protokollbeschreibung als Spezifikation neu implementiert; fremder Code wird nicht kopiert.

Die Upstreams sind modellgebundene technische Ausgangspunkte, keine Aussage, dass jedes gleich benannte Desk kompatibel ist. Das ist insbesondere bei Jiecang wichtig: phord dokumentiert verschiedene Controller mit unterschiedlichem seriellen Protokoll. Die Profile werden deshalb nach der in der Quelle genannten Controller-/Handset-ID benannt, nicht pauschal nach Marke oder Buchsentyp.

Ein Profil ist versioniert und enthält mindestens:

- eindeutige IDs für Hersteller, Tisch, Controller, Handset und getestete Firmware-/Hardwarestände;
- Familie (`jiecang_rj12`, `jiecang_rj45`, `loctek_rj45`) und Betriebsart (inline/accessory);
- UART-Parameter, Idle-Polarität, maximale Framegröße, Interbyte-/Frametimeout und Parser/Checksumme;
- erlaubte Befehle, Antwortkorrelation, Retries, Polling, Bewegungs- und Stopsemantik;
- Start-/Sleep-/Wake-Automat inklusive gemessener Zeiten und Ready-Nachweis;
- erlaubte HS0…HS3-Funktionen für Jiecang oder Loctek-Wake-Puls/Haltezeit;
- Upstream-Repository, festgeschriebene Commit-ID, Lizenzstatus, übernommene Semantik sowie Latenz-, Versorgungs- und Testgrenzen.

Profile werden zunächst als kompilierte C++-Objekte implementiert, damit Parser- und Sicherheitslogik typgesichert reviewbar bleiben. Ein externes JSON/YAML-Format wird erst ergänzt, wenn zwei valide Profile tatsächlich gemeinsame, deklarative Felder zeigen; heruntergeladene oder benutzereditierte Protokollprofile sind in V1 nicht zulässig.

Die drei Familien teilen eine Schnittstelle, aber nicht Protokollannahmen:

- **Jiecang RJ12:** kein firmwaregesteuerter Wake-Pin; nur verifizierte UART-Initialisierung.
- **Jiecang/Jarvis RJ45:** HS0…HS3 sind U4-open-drain und LOW-aktiv; HIGH bedeutet Freigabe, niemals aktiv HIGH. Keine Erkennungsimpulse.
- **Loctek/FlexiSpot RJ45:** Wake ist OUT Pin 4 über U5 HIGH; IN Pin 4 wird nicht gelesen. Das erste gültige Handset-Frame puffern, Wake setzen, profilierte Ready-Bedingung abwarten und erst dann geordnet senden. Modelle, die ausschließlich ein Handset-Wake an IN Pin 4 erwarten, bleiben inkompatibel.

Ein Profil wechselt von `experimental` nach `supported` nur mit einem festgeschriebenen Upstream, nachvollziehbarer Portierung, gemessenen Pegeln/Versorgung, bestandener Testmatrix und wiederholbarem Stop-/Fehlerverhalten. Die Kompatibilitätsdokumentation nennt immer Controller **und** Handset.

## 6. Implementierungsphasen und Gates

| Phase | Arbeitspaket | Ergebnis / Gate |
|---|---|---|
| 0 | Hardware-/Lab-Readiness | Bestückte V1, Strombegrenzung, USB, UART-Adapter/Logic Analyzer, kontrollierte Stopmöglichkeit; Abschnitt-10-Hardwarepunkte bewertet |
| 1 | PlatformIO-Skelett | gepinnter ESP-IDF-Build, Board-Manifest, A/B-App-/UI-Partitionstabelle mit Größenbudgets, Format/Lint/Unit-Test-Targets, USB-CDC und Produktionsflash auf V1 |
| 2 | Sichere HAL | messbar korrekte BOOT_SAFE-Pegel, LED-Codes, keine Desk-Ausgabe beim Boot; Reset/Brownout-Test bestanden |
| 3 | UART-Transport | zwei gleichzeitige RX-Pfade und exklusiver TX, Ringpuffer/Fehlerzähler, Loopback-/Logic-Analyzer-Test; GPIO-Matrix validiert |
| 4 | Frame-/Arbiterkern | Replay-Tests für Fragmentierung, Timeout, Overflow, Priorität und keine Bytevermischung; Controller-Antworten werden nicht gesendet |
| 5 | Erstes Referenzprofil | Upstream-basierte Portierung von Parser-/Wake-/Stop-Modell, zunächst ohne WLAN/API; echte Inline-Handset-Weiterleitung bestanden |
| 6 | Provisioning, API, Web UI und Persistenz | AP/Captive Portal, OpenAPI-Vertrag, Authentifizierung, Info-/Log-/Settings-Seiten, WebSocket-Status; NVS-Migration, Rate-Limits und Freigabe für Bewegungsbefehle |
| 7 | MQTT/HA, Netzwerk und OTA | MQTT Device Discovery einschließlich Firmwareversion; signierte Firmware-/UI-OTA mit Rollback; Wi-Fi getrennt vom Transport und OTA ausschließlich im Stillstand |
| 8 | Familienerweiterung | je neues Profil vollständige Qualifizierung, erst dann Aktivierung in Release |
| 9 | Release-Härtung | Langzeittest, Fehlereinbringung, Strom-/Thermik-/USB-Tests, Signierung/Versionierung, Recovery-Anleitung |

Die Phasen 5–9 werden nicht mit geratenen oder simulierten Protokollwerten abgeschlossen. Baudraten, Checksummen, Wake-Zeiten und HS-Zuordnungen stammen aus der festgeschriebenen ESP-Upstream-Implementierung; falls diese für einen Controller nicht vorhanden oder widersprüchlich sind, wird dafür kein Profil ausgeliefert.

## 7. Teststrategie und Messprotokoll

### 7.1 Automatisiert in CI

- Host/Unity-Tests für Framegrenzen, Checksummen, Parser-Resync, Timeout und TX-Priorität.
- Synthetische Replays aus den festgeschriebenen Upstream-Framevektoren mit Fragmentierung an jeder Byteposition, beschädigten Frames und langen Pausen.
- Tests, dass ein Controller-Frame nie eine TX-Anforderung erzeugt und dass abgelaufene Bewegungsframes verworfen werden.
- API-Vertragstests für Authentifizierung, CSRF, Idempotenz, Sequenzkonflikte, Eingabegrenzen, Fähigkeits-Gates und Antwortcodes.
- Provisionierungstests für leeres/defektes NVS, falsches WLAN-Passwort, Timeout, erfolgreichen AP→STA-Neustart, erzwungenen AP-Modus und Secret-Redaction.
- UI-End-to-End-Tests: keine Steuerfunktion ohne Fähigkeit/Anmeldung; Info-Werte sind plausibel, Logfilter/-pagination begrenzt, Settings validieren Profil/Sprache/Zeitzone, WebSocket-Status und Reconnect stimmen mit `GET /state` überein.
- MQTT-Tests mit lokalem Broker: Discovery/Unique-IDs und `sw_version`, HA-Birth-Neuveröffentlichung, LWT-Verfügbarkeit, nicht-retained Bewegungsbefehle, JSON-Validierung und identische API-/MQTT-Arbitrierung.
- OTA-Matrix für gültige/falsche Signatur, falsches Board, zu großes Image, App-/UI-Inkompatibilität, Stromverlust in jeder Phase, First-Boot-Self-Test, Rollback und Recovery-Seite.
- Build-Matrix mindestens `pandadesk-c6` und C6-Entwicklungsboard; `pio run`, Unit Tests, Format-/statische Analyse sowie Größenbudget.
- Profil-Validator: eindeutige IDs, gültige UART-Parameter, Limits, sichere Stopdefinition und vorhandene Testdokumentation.

### 7.2 Hardware in definierter Reihenfolge

1. Ohne Tisch: Durchgang/Orientierung, USB-only und Desk-only, +5 V/+3,3 V, Boot/Reset, native USB, LED und Desk-TX mit Logic Analyzer prüfen.
2. USB an/ab unter Wi-Fi- und UART-Last: U6-Umschaltung, Einbrüche, Reset und Rückspeisung messen.
3. Mit Logic Analyzer: Idle/OE-/Wake-/HS-Pegel, beide UART-Richtungen, Frame-Integrität und Weiterleitungslatenz messen.
4. Mit echtem Referenzmodell: Start, Schlaf/Wake, erster Tastendruck, langes Halten, Richtungswechsel und Stop.
5. Gleichzeitige Handset- und PandaDesk-Befehle: Vorrang, keine vermischten Frames, keine unzulässigen Controllerantworten am Handset.
6. Provisionierung/API/Web UI/MQTT: AP-Erststart, Authentifizierung, Settings, Logs, abgelehnte nicht unterstützte Befehle, parallele Quellen, Broker-/WLAN-Ausfall und Wiederverbindung; anschließend keine alte Bewegung.
7. Fehler: Kabelabzug, USB-Wechsel, Brownout, Reset, RX-Overflow, Framingfehler und Watchdog; anschließend keine alte Bewegung.
8. Firmware- und UI-/Dateisystem-OTA nur im Stillstand sowie Stromausfall-/Rollback-Tests; danach Handset-/Profilzustand, Safe-Start, Versionen und Recovery bestätigen.

Für jeden Testlauf liegen in `docs/compatibility/<profile-id>/` mindestens Identifikationen, Platinenversion, Firmware-Commit, verwendeter Upstream-Commit, Versorgung/Last, Messmittel, erwartetes Ergebnis, Ist-Ergebnis und Freigabeentscheidung vor. Personen-/Sicherheitsrisiken beim Bewegungstest werden durch freie Umgebung und eine unabhängige Stopmöglichkeit kontrolliert.

## 8. Release-, Diagnose- und Betriebsregeln

Native USB-CDC ist Standard für strukturierte Diagnose. Serial-Log-Ausgaben auf UART0 oder GPIO19 werden vor Aktivierung am Tisch geprüft und dürfen nie auf `E_TX` routbar sein. Web-Log und USB-Diagnose verwenden dieselben redigierten Eventcodes. Diagnose zeigt Zustandsautomat, Profil-ID/-Version, RX/TX-Zähler, Overflow/Framingfehler, Wake-Versuche, abgelaufene Befehle sowie API-/MQTT-/OTA-Verbindungsstatus, aber keine Zugangsdaten oder kompletten sensitiven Netzwerkinhalte.

Ein Release enthält Firmwareversion, Plattform-/ESP-IDF-Version, Profilmanifest, Kompatibilitätsliste, bekannte Grenzen und Rollback-/Recovery-Anleitung. OTA ist nur erlaubt, wenn das Profil einen Stillstand bestätigt, keine Bewegung angefordert ist und der Sender leer ist. Nach Update oder jeder Konfigurationsmigration beginnt die Firmware wieder in BOOT_SAFE.

## 9. Risiken, Entscheidungen und offene Inputs

| Risiko / offene Frage | Konsequenz | Umgang |
|---|---|---|
| C6-UART-Matrix liefert nicht beide RX-Wege wie geplant | Kernarchitektur blockiert | Phase 3 auf V1 messen; bei Scheitern Hardwareänderung oder explizit validierte Alternative vor Profilentwicklung entscheiden |
| Deskversorgung trägt Wi-Fi-Spitzen nicht | Resets/Protokollfehler | USB als Referenzversorgung; je Profil Strom-/Spannungsreserve qualifizieren |
| U3-OE trennt RX und TX gemeinsam | Handset-Wake kann ausfallen | OE im READY/Sleep HIGH; gezielte, profilierte Fehlerreaktionen |
| Loctek-Handset sendet erst nach IN-Pin-4-Wake | V1 erkennt ersten Tastendruck nicht | als inkompatibel dokumentieren; keine Firmware-Behauptung oder Blind-Pulse |
| Fehlendes passives Bypass | Reset unterbricht Bedienung | Watchdog, kurze Safe-Startzeit, UX-/Installationshinweis; Hardware V2 separat bewerten |
| Kein passender, belastbarer ESP-Upstream für einen Controller | falsche Bewegung/Stop | kein Profil und keine heuristische Erkennung; erst nach einer separat belegten Quellimplementierung wieder bewerten |
| OTA während Bewegung | Bedien-/Sicherheitslücke | Stillstands-Gate, Sender leer, danach BOOT_SAFE |
| Stromausfall beim UI-Dateisystem-Update | Web UI unbrauchbar | inaktiver A/B-Slot, atomarer Aktiv-Marker und firmwareinterne Recovery-Seite |
| 4-MB-Flash reicht nicht für sichere App- und UI-A/B-Slots | kein sicherer OTA-Aufbau | CI-Größenbudget; Web-Bundle in App-Image integrieren oder vor Implementierung Hardware mit größerem Flash festlegen |
| Provisionierungs-AP bleibt ungeschützt/aktiv | lokaler Zugriff auf Secrets/Settings | geräteindividueller Code, nur bei Bedarf aktiv, Desk-Ausgänge safe und nach Erfolg AP abschalten |
| Persistentes Logging verschleißt Flash oder leakt Secrets | vorzeitiger Ausfall/Informationsabfluss | Redaction, feste Rotation, Batching, Level-/Größenlimit und Debug nur flüchtig |
| Web/API/MQTT umgeht Transportkern | konkurrierende oder unsichere Bewegung | alle Eingänge erzeugen denselben validierten Intent und warten auf dieselbe Statusrückmeldung |
| Retained MQTT-Bewegungsbefehl | Fahrt nach Reconnect | Bewegungs- und Preset-Topics nie retained; Ablaufzeit und Request-ID erzwingen |
| Unzuverlässige Höhe | falsche Zielposition | Höhe mit Qualitätsstatus; Zielhöhe nur als Profilfähigkeit und nie aus altem retained Zustand |

Vor Beginn von Phase 5 werden drei Projektentscheidungen festgehalten: das erste reale Referenzmodell, die konkrete Web-UI-/API-Ausgestaltung innerhalb dieses Vertrags und der Sicherheits-/Authentifizierungsumfang. Web UI, REST/WebSocket und Home-Assistant-MQTT sind bereits Projektumfang; BLE-Steuerung bleibt eine optionale spätere Erweiterung. Keine dieser Entscheidungen darf den Transportkern umgehen oder dessen Prioritätsregeln verändern.

## 10. Konkrete erste Arbeitsliste

1. V1-Prototyp und Messplatz vorbereiten; die noch offenen Fertigungs- und Reglerlayoutpunkte aus der Projektzusammenfassung als Hardware-Risiko akzeptieren oder vorab korrigieren.
2. PlatformIO-Repository-Skelett mit ESP-IDF, lokalem PandaDesk-C6-Boardmanifest, `sdkconfig.defaults`, sicherer A/B-Partitionstabelle, CI-Größenbudgets und USB-CDC anlegen; Toolversionen pinnen.
3. `pins.hpp` und sichere HAL implementieren; BOOT_SAFE mit Logic Analyzer vor jedem weiteren Schritt nachweisen.
4. Gleichzeitig laufende RX-Pfade GPIO18/GPIO5 plus TX GPIO19 auf der tatsächlichen C6-Platine verifizieren und dokumentieren.
5. Die oben genannten GitHub-Quellen auf konkrete, lizenzrechtlich verwendbare Commit-IDs festschreiben und daraus Parser-/Wake-/Befehlsvektoren als Tests ableiten.
6. TX-Arbiter, Parser-Schnittstelle und Replay-Testharness implementieren, bevor irgendein Fahrbefehl implementiert wird.
7. Provisionierungsablauf, OpenAPI- und MQTT-Discovery-Contract sowie Bedien-/Info-/Log-/Settings-/Update-Seiten entwerfen; jede Steuerquelle gegen dieselben Intent-/Fähigkeitsprüfungen testen.
8. Einen zur Quellimplementierung passenden Tisch/Controller/Handset-Satz auswählen und das portierte Profil zunächst im überwachten Laborbetrieb gegen die Testmatrix qualifizieren; erst danach Netzwerksteuerung und weitere Profile ergänzen.

## Quellenbasis

Die Verdrahtung und funktionalen Grenzen dieses Plans stammen aus `PandaDesk_Projektzusammenfassung.md` und dem Schaltplanexport im Repository. Die Protokoll-/Wake-Grundlage sind [Rocka84/esphome_components](https://github.com/Rocka84/esphome_components/tree/master/components/jiecang_desk_controller), [phord/Jarvis](https://github.com/phord/Jarvis), [dimitri-vs/flexispot-esphome](https://github.com/dimitri-vs/flexispot-esphome) und [iMicknl/LoctekMotion_IoT](https://github.com/iMicknl/LoctekMotion_IoT); vor Implementierung werden deren exakte Commits und Lizenzen festgeschrieben. Die Home-Assistant-Anbindung richtet sich nach der offiziellen [MQTT Discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery)-Dokumentation und der [MQTT-Cover-Spezifikation](https://www.home-assistant.io/integrations/cover.mqtt/). Provisionierung und Update folgen den zur gepinnten IDF-Version passenden offiziellen ESP32-C6-Dokumentationen für [Provisioning](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-reference/provisioning/index.html) und [OTA/Rollback](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-reference/system/ota.html). Für die Umsetzung sind zusätzlich die offiziellen Dokumentationen von [PlatformIO Espressif32](https://docs.platformio.org/en/latest/platforms/espressif32.html), [PlatformIO ESP-IDF](https://docs.platformio.org/en/latest/frameworks/espidf.html) und [ESP-IDF für ESP32-C6](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/) heranzuziehen. Bei einem Konflikt zwischen externem Quellprojekt und der festen V1-Verdrahtung hat PandaDesk-Hardware Vorrang; das betroffene Profil bleibt bis zur geklärten Portierung gesperrt.
