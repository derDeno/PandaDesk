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
  api/                           # lokale/Netzwerk-Schnittstelle, autorisiert Befehle
  diagnostics/                   # strukturierte USB-Logs, Zähler, Testtracing
test/                            # Native/Unity-Komponententests
tools/                           # Upstream-Vektorimport, Replay- und Profilvalidierung
docs/compatibility/              # Messprotokolle je Modell
```

Keine Desk-Framing-Bytes, Baudraten oder Wake-Zeiten gehören als globale Konstanten in `main`. Sie liegen ausschließlich in einem qualifizierten Profil mit Versionskennung.

### 3.2 Konfiguration und Secrets

NVS enthält nur Nutzerkonfiguration (aktives Profil, Betriebsmodus, zulässige Automationen, Netzwerkzugang). Secrets und Testdaten kommen nicht ins Repository; `secrets.example.h` bzw. PlatformIO-Environment-Variablen dokumentieren die benötigten Werte. Beim Start wird die gespeicherte Konfiguration gegen Schema- und Profilversion validiert. Fehlt oder scheitert sie, startet PandaDesk im **Safe/Unconfigured**-Zustand ohne aktive Desk-Befehle und ohne Inline-Forwarding.

## 4. Laufzeitarchitektur

### 4.1 Aufgaben, Besitz und Datenfluss

| Einheit | Verantwortung | Darf zum Controller senden? |
|---|---|---|
| `controller_rx_task` | Bytes von GPIO18, Zeitstempel, RX-Fehler, Controller-Parser | Nein |
| `handset_rx_task` | Bytes von GPIO5, Zeitstempel, Handset-Parser | Nein |
| `desk_state_task` | Profilzustand, Wake, Antworten, Freigaben und Fehlerreaktion | Nur durch Anforderung an Arbiter |
| `tx_arbiter_task` | eine FIFO, vollständige Frames, Priorität und UART19 | **Ja, exklusiv** |
| `api_task` | Nutzer-/Netzwerkbefehle in geprüfte Intent-Objekte wandeln | Nein |
| `diagnostics_task` | USB-CDC-Logs, Metriken und Testtracing | Nein |

Beide RX-Aufgaben lesen ohne Netzwerkzugriffe aus getrennten ESP-IDF-UART-Instanzen bzw. validierter GPIO-Matrix-Zuordnung. `E_TX` wird nur vom exklusiven Sender angehängt. Vor der ersten Implementierung ist auf echter C6-Hardware nachzuweisen, welche UART-Controller/Matrix-Routen gleichzeitig GPIO18/19 und GPIO5 mit dem gewählten ESP-IDF bereitstellen; UART0 bleibt im Flash-/ROM- und ggf. Diagnosepfad. Eine Software-UART ist kein Produktionsfallback.

RX-Bytes erhalten einen monotonic timestamp und werden in begrenzten Ringpuffern abgelegt. Parser geben `Frame`, `PartialFrame`, `Timeout` oder `ParseError` aus. Ein ungeklärtes Teilframe blockiert den Sender nur bis zum profildefinierten Interbyte-/Frametimeout; danach wird es verworfen und resynchronisiert. Pufferüberlauf verwirft niemals später noch eine alte Bewegungsanweisung als gültigen Befehl.

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
| 1 | PlatformIO-Skelett | gepinnter ESP-IDF-Build, Board-Manifest, Format/Lint/Unit-Test-Targets, USB-CDC und Produktionsflash auf V1 |
| 2 | Sichere HAL | messbar korrekte BOOT_SAFE-Pegel, LED-Codes, keine Desk-Ausgabe beim Boot; Reset/Brownout-Test bestanden |
| 3 | UART-Transport | zwei gleichzeitige RX-Pfade und exklusiver TX, Ringpuffer/Fehlerzähler, Loopback-/Logic-Analyzer-Test; GPIO-Matrix validiert |
| 4 | Frame-/Arbiterkern | Replay-Tests für Fragmentierung, Timeout, Overflow, Priorität und keine Bytevermischung; Controller-Antworten werden nicht gesendet |
| 5 | Erstes Referenzprofil | Upstream-basierte Portierung von Parser-/Wake-/Stop-Modell, zunächst ohne WLAN/API; echte Inline-Handset-Weiterleitung bestanden |
| 6 | Steuer-API und Persistenz | authentifizierte, minimale lokale Steuerung; NVS-Migration, Rate-Limits und Freigabe für Bewegungsbefehle |
| 7 | Netzwerk und OTA | Wi-Fi/BLE nur getrennt vom Transport; OTA ausschließlich in verifiziertem Stillstand, danach Safe-Start und Konfigurationsprüfung |
| 8 | Familienerweiterung | je neues Profil vollständige Qualifizierung, erst dann Aktivierung in Release |
| 9 | Release-Härtung | Langzeittest, Fehlereinbringung, Strom-/Thermik-/USB-Tests, Signierung/Versionierung, Recovery-Anleitung |

Die Phasen 5–9 werden nicht mit geratenen oder simulierten Protokollwerten abgeschlossen. Baudraten, Checksummen, Wake-Zeiten und HS-Zuordnungen stammen aus der festgeschriebenen ESP-Upstream-Implementierung; falls diese für einen Controller nicht vorhanden oder widersprüchlich sind, wird dafür kein Profil ausgeliefert.

## 7. Teststrategie und Messprotokoll

### 7.1 Automatisiert in CI

- Host/Unity-Tests für Framegrenzen, Checksummen, Parser-Resync, Timeout und TX-Priorität.
- Synthetische Replays aus den festgeschriebenen Upstream-Framevektoren mit Fragmentierung an jeder Byteposition, beschädigten Frames und langen Pausen.
- Tests, dass ein Controller-Frame nie eine TX-Anforderung erzeugt und dass abgelaufene Bewegungsframes verworfen werden.
- Build-Matrix mindestens `pandadesk-c6` und C6-Entwicklungsboard; `pio run`, Unit Tests, Format-/statische Analyse sowie Größenbudget.
- Profil-Validator: eindeutige IDs, gültige UART-Parameter, Limits, sichere Stopdefinition und vorhandene Testdokumentation.

### 7.2 Hardware in definierter Reihenfolge

1. Ohne Tisch: Durchgang/Orientierung, USB-only und Desk-only, +5 V/+3,3 V, Boot/Reset, native USB, LED und Desk-TX mit Logic Analyzer prüfen.
2. USB an/ab unter Wi-Fi- und UART-Last: U6-Umschaltung, Einbrüche, Reset und Rückspeisung messen.
3. Mit Logic Analyzer: Idle/OE-/Wake-/HS-Pegel, beide UART-Richtungen, Frame-Integrität und Weiterleitungslatenz messen.
4. Mit echtem Referenzmodell: Start, Schlaf/Wake, erster Tastendruck, langes Halten, Richtungswechsel und Stop.
5. Gleichzeitige Handset- und PandaDesk-Befehle: Vorrang, keine vermischten Frames, keine unzulässigen Controllerantworten am Handset.
6. Fehler: Kabelabzug, USB-Wechsel, Brownout, Reset, RX-Overflow, Framingfehler, WLAN-Ausfall und Watchdog; anschließend keine alte Bewegung.
7. OTA nur im Stillstand; danach Handset-/Profilzustand, Safe-Start und Recovery bestätigen.

Für jeden Testlauf liegen in `docs/compatibility/<profile-id>/` mindestens Identifikationen, Platinenversion, Firmware-Commit, verwendeter Upstream-Commit, Versorgung/Last, Messmittel, erwartetes Ergebnis, Ist-Ergebnis und Freigabeentscheidung vor. Personen-/Sicherheitsrisiken beim Bewegungstest werden durch freie Umgebung und eine unabhängige Stopmöglichkeit kontrolliert.

## 8. Release-, Diagnose- und Betriebsregeln

Native USB-CDC ist Standard für strukturierte Diagnose. Serial-Log-Ausgaben auf UART0 oder GPIO19 werden vor Aktivierung am Tisch geprüft und dürfen nie auf `E_TX` routbar sein. Diagnose zeigt Zustandsautomat, Profil-ID/-Version, RX/TX-Zähler, Overflow/Framingfehler, Wake-Versuche und abgelaufene Befehle, aber keine Zugangsdaten oder kompletten sensitiven Netzwerkinhalt.

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

Vor Beginn von Phase 5 werden drei Projektentscheidungen festgehalten: das erste reale Referenzmodell, die gewünschte lokale Steueroberfläche (z. B. REST/MQTT/BLE) und der Sicherheits-/Authentifizierungsumfang. Diese Entscheidungen dürfen den Transportkern nicht umgehen oder dessen Prioritätsregeln verändern.

## 10. Konkrete erste Arbeitsliste

1. V1-Prototyp und Messplatz vorbereiten; die noch offenen Fertigungs- und Reglerlayoutpunkte aus der Projektzusammenfassung als Hardware-Risiko akzeptieren oder vorab korrigieren.
2. PlatformIO-Repository-Skelett mit ESP-IDF, lokalem PandaDesk-C6-Boardmanifest, `sdkconfig.defaults`, CI und USB-CDC anlegen; Toolversionen pinnen.
3. `pins.hpp` und sichere HAL implementieren; BOOT_SAFE mit Logic Analyzer vor jedem weiteren Schritt nachweisen.
4. Gleichzeitig laufende RX-Pfade GPIO18/GPIO5 plus TX GPIO19 auf der tatsächlichen C6-Platine verifizieren und dokumentieren.
5. Die oben genannten GitHub-Quellen auf konkrete, lizenzrechtlich verwendbare Commit-IDs festschreiben und daraus Parser-/Wake-/Befehlsvektoren als Tests ableiten.
6. TX-Arbiter, Parser-Schnittstelle und Replay-Testharness implementieren, bevor irgendein Fahrbefehl implementiert wird.
7. Einen zur Quellimplementierung passenden Tisch/Controller/Handset-Satz auswählen und das portierte Profil zunächst im überwachten Laborbetrieb gegen die Testmatrix qualifizieren; erst danach Netzwerksteuerung und weitere Profile ergänzen.

## Quellenbasis

Die Verdrahtung und funktionalen Grenzen dieses Plans stammen aus `PandaDesk_Projektzusammenfassung.md` und dem Schaltplanexport im Repository. Die Protokoll-/Wake-Grundlage sind [Rocka84/esphome_components](https://github.com/Rocka84/esphome_components/tree/master/components/jiecang_desk_controller), [phord/Jarvis](https://github.com/phord/Jarvis), [dimitri-vs/flexispot-esphome](https://github.com/dimitri-vs/flexispot-esphome) und [iMicknl/LoctekMotion_IoT](https://github.com/iMicknl/LoctekMotion_IoT); vor Implementierung werden deren exakte Commits und Lizenzen festgeschrieben. Für die Umsetzung sind zusätzlich die jeweils zur gepinnten Toolchain passenden offiziellen Dokumentationen von [PlatformIO Espressif32](https://docs.platformio.org/en/latest/platforms/espressif32.html), [PlatformIO ESP-IDF](https://docs.platformio.org/en/latest/frameworks/espidf.html) und [ESP-IDF für ESP32-C6](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/) heranzuziehen. Bei einem Konflikt zwischen externem Quellprojekt und der festen V1-Verdrahtung hat PandaDesk-Hardware Vorrang; das betroffene Profil bleibt bis zur geklärten Portierung gesperrt.
