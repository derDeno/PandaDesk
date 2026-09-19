# PandaDesk – Projektzusammenfassung und Firmware-Anforderungen

Stand: 19.09.2026 · Hardware V1 · Prototyp vor Fertigungsfreigabe.

Diese Zusammenfassung ersetzt die bisherige universelle DESK1–DESK8-Architektur. Sie beschreibt den zuletzt geprüften Projekt-/Fertigungsstand. Empfehlungen und offene Punkte sind keine bereits umgesetzten Änderungen. Die Firmwareabschnitte sind Implementierungsanforderungen, kein Nachweis vorhandener Software.

## 1. Ziel und Umfang

PandaDesk ist ein ESP32-C6-Adapter für höhenverstellbare Tische. Ziel ist die Unterstützung mehrerer verbreiteter Schnittstellen, nicht aller Tischmodelle oder beliebiger unbekannter Pinbelegungen.

- Drei dedizierte Schnittstellen: Jiecang RJ12, Jiecang/Jarvis RJ45 und Loctek/FlexiSpot RJ45; jeweils IN und OUT.
- Genau ein Tisch und eine Schnittstellenfamilie gleichzeitig. Mehrere Portpaare dürfen nicht parallel benutzt werden; wichtige Signalnetze werden gemeinsam verwendet.
- Keine automatische Tischerkennung. Nutzer wählen ein freigegebenes Modellprofil und den passenden Anschluss.
- Maximal 5 V an den Tischanschlüssen. Höhere Spannungen werden bewusst weder unterstützt noch als Fehlanschluss abgesichert.
- Passender Stecker oder Markenname allein bedeutet keine Kompatibilität.
- Keine Lötarbeiten, Jumper-Umbauten oder Widerstandsnachbestückung durch Endkunden.
- Versorgung über USB-C oder ausreichend belastbare 5-V-Tischversorgung; USB hat Vorrang.
- Optionales Originalhandset; dessen Senderichtung wird im Inline-Betrieb aktiv durch die Firmware vermittelt.

Maidesite, IKEA, Kaidi und weitere Varianten sind derzeit nicht als getestet freigegeben. Auch innerhalb der drei gewählten Schnittstellenfamilien sind konkrete Controller-/Handset-Kombinationen einzeln zu qualifizieren.

## 2. Referenzdateien und letzte Prüfung

Dateien im Projektverzeichnis:

| Datei | Bedeutung |
|---|---|
| ProPrj.epro2 | EasyEDA-Pro-Projekt mit Schaltplan, PCB und Bibliotheksdaten |
| SCH.pdf | Schaltplanexport als zusätzliche Lesereferenz |
| Gerber.zip | Vier Kupferlagen, Kontur, Masken, Paste, Bohrdaten, Flying-Probe-Pinnetzliste |
| BOM.xlsx | Stückliste |
| PickAndPlace.xlsx | Bestückungspositionen |

Prüfung der am 19.09.2026 bereitgestellten Projekt-, Gerber-, BOM- und Placement-Dateien:

- 53 Bauteilreferenzen stimmen nach Entfernen umgebender Leerzeichen zwischen BOM, Placement und Gerber-Komponentenliste überein.
- Koordinaten, Drehungen und Bestückungsseiten stimmen zwischen Placement und Gerber-Export überein. Maschinenabhängige Rotation und reale Bauteilorientierung bleiben im Bestücker-Preview zu prüfen.
- BOM-Bestellnummern und Footprintnamen stimmen mit dem Projekt überein; das leere U1-Supplier-Part-Feld ist auch im Projekt vorhanden.
- Eine rasterbasierte Vierlagen-Kupferprüfung gegen die exportierte Pinnetzliste zeigte keine offensichtlichen Unterbrechungen oder Kurzschlüsse.
- Diese Prüfung ersetzt keine native ERC/DRC, Fertigerfreigabe, Impedanzprüfung oder Funktionsprüfung.

Offene Freigabepunkte stehen in Abschnitt 10. Nach Änderungen müssen Projekt und alle Fertigungsdateien gemeinsam neu exportiert werden.

## 3. Aktuelle Architektur

Entfallen sind LSF0108, zwei 74HCT4066, TCA9535 und AMS1117. Ebenso entfallen die universellen DESK1–DESK8-Leitungen, dynamische Pull-ups pro Leitung, der 8-polige Backup-Terminal und der separate externe 5-V-Terminal. Diese Funktionen existieren nicht zusätzlich zur neuen Architektur.

### IN und OUT

- **IN:** originales Handbedienteil im Inline-Betrieb.
- **OUT:** Tischsteuerung bzw. Kommunikationsendpunkt, an den PandaDesk Befehle sendet.
- Bei einem freien Zubehörport wird nur OUT angeschlossen; IN bleibt frei.
- Sitzt der zu steuernde Kommunikationsendpunkt physisch im Handset, wird dieser an OUT angeschlossen. Das bedeutet nicht, dass jedes RJ12-Handset selbst die Motorsteuerung enthält.

### UART-Fluss

    Handset-IN TX -> HANDSET_TX -> U3 -> E_RX_HANDSET -> ESP GPIO5
    ESP GPIO19 E_TX -> U3 -> D_RX -> Controller-OUT RX
    Controller-OUT TX -> D_TX -> U3 -> E_RX -> ESP GPIO18
                             -> direkt zum Handset-IN RX

HANDSET_TX und D_RX sind elektrisch getrennt. Der ESP muss Handset-Telegramme empfangen und erneut zum Controller senden. Controller-Antworten erreichen das Handset direkt und werden gleichzeitig vom ESP mitgelesen.

Es gibt keinen zusätzlichen frei steuerbaren ESP-Sendepfad zum Handset-IN. Eigene Befehle und weitergeleitete Handset-Telegramme teilen sich den Controller-Sendepfad. Controller-Antworten dürfen nicht zum Controller zurückgespiegelt werden.

Bei Reset, ausgeschalteter Platine, Firmwarefehler oder deaktiviertem U3 ist die Handset-Senderichtung unterbrochen. Es gibt keinen passiven UART-Bypass. „Immer versorgt“ bedeutet nicht „Handset funktioniert unabhängig von Firmware“.

## 4. Feste Portbelegungen

Nummern entsprechen Schaltplan-/Footprint-Pinnummern, nicht ungeprüften Steckeransichten oder Kabelfarben. Kabeldurchgang und Orientierung vor Erstanschluss messen.

### Jiecang RJ12 / 6P6C

| Pin | IN – Handset | OUT – Controller/Zubehörport |
|---|---|---|
| 1 | D_PASS1 | D_PASS1 |
| 2 | GND | GND |
| 3 | D_TX zum Handset-RX | D_TX vom Controller-TX |
| 4 | +5V_D1 | +5V_D1 |
| 5 | HANDSET_TX vom Handset | D_RX zum Controller |
| 6 | D_PASS2 | D_PASS2 |

Pins 1, 2, 3, 4 und 6 sind paarweise direkt verbunden. RJ12 besitzt keinen dedizierten firmwaregesteuerten Wake-Ausgang. Modellabhängige UART-Initialisierung gehört ins Profil; zusätzliche elektrische Wake-Anforderungen sind nicht automatisch unterstützt.

### Jiecang/Jarvis RJ45 / 8P8C

| Pin | IN – Handset | OUT – Controller |
|---|---|---|
| 1 | D_HS3 | D_HS3 |
| 2 | D_TX zum Handset-RX | D_TX vom Controller-TX |
| 3 | GND | GND |
| 4 | HANDSET_TX vom Handset | D_RX zum Controller |
| 5 | +5V_D2 | +5V_D2 |
| 6 | D_HS2 | D_HS2 |
| 7 | D_HS1 | D_HS1 |
| 8 | D_HS0 | D_HS0 |
| 9/10 | Schirm an GND | Schirm an GND |

HS-Leitungen sind zwischen IN und OUT durchverbunden und zusätzlich über U4 LOW steuerbar. HS steht hier für Handset-/Steuerleitungen, nicht „High Speed“. Auf/Ab/Wake-Zuordnungen sind modellabhängig; keine Funktion allein aus dem Namen HS0–HS3 ableiten.

### Loctek/FlexiSpot RJ45 / 8P8C

| Pin | IN – Handset | OUT – Controller |
|---|---|---|
| 1 | D_RESET | D_RESET |
| 2 | D_SWIM | D_SWIM |
| 3 | D_PASS3 | D_PASS3 |
| 4 | nicht verbunden | D_WAKE von U5 |
| 5 | D_TX zum Handset-RX | D_TX vom Controller-TX |
| 6 | HANDSET_TX vom Handset | D_RX zum Controller |
| 7 | GND | GND |
| 8 | +5V_D3 | +5V_D3 |
| 9/10 | Schirm an GND | Schirm an GND |

D_RESET, D_SWIM und D_PASS3 sind reine Durchleitungen, keine ESP-Ausgänge. D_RESET ist nicht der ESP-Reset. IN-Pin 4 wird weder weitergeleitet noch eingelesen. Die daraus folgende Wake-Anforderung ist in Abschnitt 8 beschrieben.

## 5. Hauptbauteile und Signalstufen

| Referenz | Bauteil/Funktion | LCSC |
|---|---|---|
| U1 | ESP32-C6-MINI-1-N4 | C5736265; aktuelles BOM-Supplier-Feld leer |
| U2 | TPS62162DSGR, fester 3,3-V-Buck | C40256 |
| U3 | TXU0204PWR, zwei Kanäle je Richtung, drei genutzt | C4363888 |
| U4 | SN74LVC07APWR, nichtinvertierende Open-Drain-Puffer | C7809 |
| U5 | TXU0101DCKR, Loctek-Wake | C5217868 |
| U6 | TPS2116DRLR, Power-Mux | C3235557 |
| L1 | SWPA4030S3R3MT, 3,3 µH | C15269 |
| C1 | GRM32ER71E226KE15L, 22 µF, 25 V, X7R, 1210 | C21397 |
| C4/C12 | CL10A106MP8NNNC, 10 µF, 10 V, 0603 | C85713 |
| C2/C3/C10 | CL10B105KA8NNNC, 1 µF, 25 V, 0603 | C29936 |
| C5/C6/C7/C8/C9/C11 | 100 nF, 0603 | C282519 |
| D1–D3 | ESD5451N, USB-ESD-Schutz | C2936977 |
| D4–D6 | SS34-MS, Tischversorgungs-Entkopplung | C2836396 |
| RJ12, zweimal | DS1133-S60BPX | C77859 |
| RJ45, viermal | R-RJ45R10P-B000 | C386758 |
| USB | TYPE-C 16PIN 2MD(073) | C2765186 |
| BOOT/RESET | GT-TC048D-H025-L1 | C843674 |
| I2C | PM2.54-1*4 | C5116530 |
| UART | PM254V-11-06-H85 | C2832269 |
| PWR | TJ-S3806SW1TGLCCW-A5, seitliche LED | C601692 |

Vollständige Mengen/Widerstands-Bestellnummern: BOM.xlsx. L1 ist bis 3 mm hoch; der Footprintname SMNR4012 bedeutet nicht, dass das bestellte Bauteil nur 1,2 mm hoch ist.

### U3 – UART-Pegelwandlung

VCCA/Pin 1 = 3,3 V, VCCB/Pin 14 = ausgewählte +5V, GND/Pin 7; C5/C8 je 100 nF.

| Eingang | Ausgang |
|---|---|
| Pin 2 E_TX | Pin 13 D_RX |
| Pin 11 D_TX | Pin 4 E_RX |
| Pin 10 HANDSET_TX | Pin 5 E_RX_HANDSET |

A2/Pin 3 liegt an GND, B2Y/Pin 12 bleibt frei; Pins 6/9 sind NC. OE/Pin 8 = EN_TX mit R3 = 10 kΩ nach GND.

**EN_TX LOW deaktiviert alle U3-Ausgänge, einschließlich beider Empfangspfade zum ESP. HIGH aktiviert alle genutzten Kanäle.** Es ist kein reines TX-Enable und ermöglicht keinen getrennten Receive-only-Modus mit hochohmigem Desk-TX.

### U4 – HS-LOW-Steuerung

Versorgung 3,3 V, C6 = 100 nF. E_HS0/1/2/3 an Pins 1/3/5/9, D_HS0/1/2/3 an Pins 2/4/6/8.

- Eingang LOW: Ausgang zieht Tischleitung LOW.
- Eingang HIGH: Ausgang freigegeben/hochohmig, nicht aktiv HIGH.
- R5/R6/R7/R10 = 10 kΩ nach 3,3 V: bei hochohmigem ESP freigegeben.
- Ungenutzte Eingänge 11/13 HIGH; Ausgänge 10/12 frei.
- Tischseitige Pull-ups bzw. passende Bias-Beschaltung werden für freizugebende HS-Profile vorausgesetzt.

### U5 – Loctek-Wake

VCCA/Pin 1 = 3,3 V; VCCB/Pin 6 = +5V; GND/Pin 2; C7/C11 je 100 nF. E_WAKE verbindet A/Pin 3 und OE/Pin 5. BY/Pin 4 = D_WAKE. R4 = 10 kΩ E_WAKE nach GND, R16 = 10 kΩ D_WAKE nach GND.

- E_WAKE HIGH: U5 aktiviert, D_WAKE aktiv HIGH ungefähr auf VCCB.
- E_WAKE LOW: U5 hochohmig; R16 zieht D_WAKE LOW. Kein stark aktiv getriebenes LOW.

### Pull-ups und Spannungsgrenzen

Keine schaltbaren Pull-ups pro Tischpin mehr. Für V1 werden passend getriebene UART-Leitungen und bei HS vorhandene tischseitige Pull-ups vorausgesetzt. Das ist keine Behauptung, dass alle Desk-Controller interne Pull-ups besitzen. Modelle mit zusätzlichen Anforderungen werden nicht ohne Prüfung freigegeben.

Die ESP-Seite arbeitet mit 3,3 V. Die Tischseite ist nicht universell zwischen 3,3 V und 5 V umschaltbar; 3,3-V-only-Eingänge sind nicht automatisch kompatibel. Der Netzname +5V bedeutet keine exakt geregelte 5,000-V-Ausgangsspannung.

## 6. Versorgung, USB, Reset und Header

### Versorgung

    USB VBUS -> +5V_USB -> U6 VIN1
    RJ12 +5V_D1 -> D6 -> +5V_COMBI -> U6 VIN2
    Jiecang RJ45 +5V_D2 -> D4 -> +5V_COMBI -> U6 VIN2
    Loctek RJ45 +5V_D3 -> D5 -> +5V_COMBI -> U6 VIN2
    U6 VOUT -> +5V -> U2 -> L1 -> +3V3

Je Portpaar sind IN-/OUT-Versorgung vor der Diode direkt verbunden. D4–D6: Pad 2/Anode an roher Tischversorgung, Pad 1/Kathode an +5V_COMBI. Versorgung wird gegen Rückspeisung aus COMBI entkoppelt; keine galvanische Trennung und keine Garantie gegen sämtliche Ströme über Signalleitungen.

U6: Pin 1 GND, Pins 2/7 VOUT, Pin 3 VIN1 USB, Pin 6 VIN2 COMBI. MODE/Pin 5 an USB. PR1/Pin 4: R14 = 330 kΩ von USB, R15 = 100 kΩ nach GND, beide 1 %. C2/C3 = 1 µF an Eingängen. ST/Pin 8 unbenutzt: keine Quellenstatusmeldung an Firmware. USB-Priorität erfolgt in Hardware.

U2: VIN/Pin 2 und EN/Pin 3 an +5V; PGND/1, AGND/4, FB/5, PG/8 und EP/9 an GND. SW/7 -> L1 -> +3V3; VOS/6 an +3V3 nach L1. C4 = 10 µF am Eingang, C1 = 22 µF am Ausgang; C12 = 10 µF und C9 = 100 nF lokal an U1.

5-V-Tischpins sind nicht automatisch ausreichend belastbar. WLAN-Stromspitzen, Handset-Verbrauch, Leitungs-/Diodenabfall und USB-Umschaltung testen; bei unzureichender Reserve USB verlangen. Die Desk-Schottkydiode reduziert auch die deskseitige Treiberversorgung.

### Nebenfunktionen

- USB-C: R1/R2 je 5,1 kΩ für getrennte CC-Pins; D1 schützt VBUS, D2/D3 D−/D+. Keine zusätzliche USB-Schottkydiode wie im alten Entwurf.
- D− an GPIO12, D+ an GPIO13. Keine separaten 22–33-Ω-Datenserienwiderstände im geprüften Layout bestückt.
- EN: R12 = 10 kΩ nach 3,3 V, C10 = 1 µF nach GND, RESET nach GND.
- BOOT: GPIO9, R11 = 10 kΩ nach 3,3 V, BOOT-Taster nach GND.
- PWR: GPIO7 über R13 = 1 kΩ, Kathode an GND. Firmware-Statusanzeige, kein unabhängiger Versorgungsnachweis.
- I2C-Header: **1 GND, 2 +3V3, 3 SCL, 4 SDA**. R8/R9 = 4,7 kΩ nach 3,3 V. Kein interner GPIO-Expander.
- UART-Header für internes Massenflashen: **1 GND, 2 TXD0, 3 RXD0, 4 +3V3, 5 GPIO9, 6 EN**. Kein Desk-Anschluss. 3,3-V-Logik; externe Programmiererversorgung nicht unkontrolliert gegen die Boardversorgung schalten.

## 7. Verbindliche Firmware-Pinzuordnung

GPIO-Nummern und Modulpinnummern nicht verwechseln.

| Funktion | GPIO | U1-Modulpin |
|---|---|---|
| E_TX zum Controller | 19 | 25 |
| E_RX vom Controller | 18 | 24 |
| E_RX_HANDSET | 5 | 10 |
| EN_TX / U3-OE | 15 | 20 |
| E_WAKE | 14 | 19 |
| E_HS0 | 23 | 29 |
| E_HS1 | 22 | 28 |
| E_HS2 | 21 | 27 |
| E_HS3 | 20 | 26 |
| SDA | 0 | 12 |
| SCL | 1 | 13 |
| LED | 7 | 16 |
| USB D− / D+ | 12 / 13 | 17 / 18 |
| BOOT | 9 | 23 |
| RXD0 / TXD0 | UART0-Standardpins | 30 / 31 |

E_RX_HANDSET heißt im Flying-Probe-Export NET_7: dieselbe Verbindung U3-Pin 5 zu U1-Pin 10, kein weiterer Kanal. Die früher vertauschten Symbolnamen RXD0/TXD0 und GPIO22/GPIO23 sind im geprüften Projekt korrigiert.

## 8. Erforderliches Firmwaredesign

### 8.1 Tischprofile

Jedes freigegebene Profil definiert Modell/Controller/Handset, Portfamilie, Inline- oder Zubehörbetrieb, Baudrate, Datenbits/Parität/Stopbits, Telegrammgrenzen, Prüfsummen, Antwortzuordnung, Polling, Wake-Methode/-Timing, HS-Funktionen, Bewegungs-/Stoppverhalten und Fehler-/Timeout-Regeln.

Portpins sind fest verdrahtet und nicht per Profil umbelegbar. Konkrete Baudraten, Frames und Wake-Zeiten sind hier nicht als für alle Familien verifiziert festgelegt. Referenzprojekte liefern Ausgangspunkte, keine allgemeine Produktfreigabe.

### 8.2 UART-Ressourcen und Aufgaben

Zwei gleichzeitige UART-Empfänger und ein Sender: Controller-RX/TX GPIO18/19 plus Handset-RX GPIO5. Handset-UART-TX bleibt ungeroutet/deaktiviert. Konkrete UART-Instanzen/GPIO-Matrix mit dem gewählten ESP-IDF-/Framework-Stand validieren; keine ungetestete Software-UART als Produktionsgrundlage.

UART0 dient dem ROM-Flashing. Falls es später für Desk-Kommunikation umkonfiguriert wird, dürfen Boot-/Debuglogs niemals auf Desk-TX gelangen. Laufzeitdiagnose bevorzugt über natives USB; U3 während Boot/Umkonfiguration deaktivieren.

Empfohlene Aufgaben: gepufferte RX-Verarbeitung, Parser je Richtung, eine zentrale Controller-TX-Queue mit genau einem Sender, Wake-/Protokollautomat, getrennte Netzwerk-/UI-Aufgaben. Weiterleitung darf nicht von WLAN/Cloud oder langen blockierenden Wartezeiten abhängen.

### 8.3 Sichere Startreihenfolge

1. EN_TX LOW, E_WAKE LOW, HS freigeben (E_HS0–3 HIGH). Ausgangslatches vor Umschalten auf Output sicher vorbelegen, damit keine Aktivierungsimpulse entstehen.
2. Gültiges Profil laden. Ohne Profil keine aktiven Befehle senden; im Inline-Betrieb funktioniert dann auch die Handset-UART-Brücke nicht.
3. Beide UART-RX-Pfade, Puffer und Ereignisverarbeitung initialisieren. E_TX bereits auf UART-Idle-HIGH setzen; keine Logausgabe auf diesem Signal.
4. EN_TX HIGH: aktiviert Sender und beide RX-Ausgänge. Erst jetzt ist Handset-/Controller-UART am ESP sichtbar.
5. Profilabhängige Initialisierung und Wake durchführen; danach reguläre Befehle zulassen.

**EN_TX muss im normalen Bereitschafts-/Desk-Sleep-Betrieb HIGH bleiben, wenn Handset-UART als Wake-Auslöser dienen soll.** LOW schaltet auch die Empfangspfade ab. Ein getrenntes RX-only-Enable existiert nicht.

### 8.4 Handset-Weiterleitung und eigene Befehle

- Handset-Bedienung, insbesondere Stoppen/Richtungswechsel, hat Vorrang vor Automationen und Hintergrundabfragen.
- Handset-Frames unverändert weiterleiten, sofern das Profil keine ausdrückliche Anpassung verlangt.
- Eigene Frames ausschließlich an protokollkonformen Frame-/Transaktionsgrenzen einschieben. Keine byteweise Vermischung und keine konkurrierenden UART-Schreiber.
- Pufferung und zulässige Weiterleitungsverzögerung pro Profil testen. Nicht unbegrenzt auf einen vollständigen Frame warten.
- Controller-Antworten auswerten, nicht zurückechoen. Das Handset erhält sie bereits direkt.
- Das Handset sieht auch Antworten auf eigene PandaDesk-Befehle. Antwortkompatibilität und konkurrierendes Polling sind zusätzlich zur elektrischen Kollisionsfreiheit zu prüfen.
- Bei Overflow, Framing-/Parserfehlern keine beschädigten oder veralteten Fahrbefehle nachträglich senden; Fehler erfassen und profilabhängig resynchronisieren.
- Bei Zubehörbetrieb ohne Handset den unbenutzten Handset-RX-Kanal nicht als Bedienaktivität interpretieren; Break-/Fehlerereignisse berücksichtigen.

### 8.5 Loctek-Wake

GPIO14 HIGH treibt OUT-Pin 4 über U5 HIGH; GPIO14 LOW deaktiviert U5 und R16 zieht OUT-Pin 4 LOW. HIGH folgt der +5V-Schiene. Aktiver HIGH-Pegel, Puls-/Haltezeit und Bereitschaft müssen am konkreten Modell bestätigt werden.

IN-Pin 4 ist absichtlich nicht angeschlossen. V1 kann das originale Handset-Wake-Signal nicht auswerten. Geplantes Verhalten:

1. Handset-UART-Empfang auch bei schlafendem Controller aktiv halten.
2. Profilkonforme Handset-Aktivität oder eigener Befehl startet Wake.
3. Erstes Handset-Telegramm puffern; E_WAKE HIGH.
4. Bereitschaft bzw. profilierte Wartezeit abwarten, während Empfang weiterläuft.
5. Gepufferte Daten geordnet senden; Wake entsprechend Profil zurücknehmen oder für die benötigte Aktivphase halten.

Das funktioniert nur, wenn das Handset UART sendet, bevor es eine Antwort oder separat durchgeschaltetes Wake benötigt. Ein Handset, das ausschließlich IN-Pin 4 schaltet und dann wartet, wird so nicht erkannt. Das ist eine bewusst zu testende Kompatibilitätsgrenze. Erster Tastendruck, langes Halten, Wiederholungen und erneuter Sleep/Wake müssen getestet werden; keine universelle Wake-Dauer hart einkodieren.

### 8.6 Jiecang-HS und Fehlerverhalten

- U4 ist nichtinvertierend: ESP LOW = Leitung LOW; ESP HIGH = freigegeben.
- Handset und PandaDesk können dieselbe HS-Leitung LOW ziehen. PandaDesk kann ein vom Handset gehaltenes LOW nicht auf HIGH zwingen.
- HS-Funktionen nur nach freigegebenem Profil; keine ungetesteten Erkennungspulse.
- Einzelne UART-Fehler nicht pauschal mit EN_TX LOW behandeln, sonst fallen beide RX-Pfade und die Brücke aus. Zunächst eigene Befehle sperren und, soweit profilabhängig sicher, Handset-Weiterleitung erhalten.
- Bei schwerem Fehler/Reset HS freigeben, Wake zurücknehmen, eigene Befehle stoppen und gegebenenfalls U3 deaktivieren. **OE LOW sendet keinen Tisch-Stoppbefehl.**
- Jedes Profil muss festlegen, ob Bewegungsstopp durch Loslassen, Leitungsfreigabe oder Stopptelegramm erfolgt. ESP-Watchdog allein garantiert kein Anhalten.
- Nach Brownout/Reset keine alten Bewegungsbefehle wiederholen. OTA nur im Stillstand; während Update/Neustart kann das Originalhandset eingeschränkt sein.

## 9. PCB-Stand

- Vier Lagen, ungefähr 88 × 72,39 mm, gerundete Kontur, vier Montagebohrungen, Antennenaussparung links.
- TOP: Bauteile/Signale/GND; INNER_1: GND; INNER_2: **+3V3**, nicht +5V; BOTTOM: Signale/GND.
- Jiecang-RJ45 oben, Loctek-RJ45 rechts, RJ12 unten; USB/LED unten; U1 an linker Aussparung.
- GND-Stitching vorhanden; an U2 zusätzliche Vias ergänzt.
- Antenne über der Aussparung ohne darunterliegende Leiterplatte. Gehäuse, Metallteile, Kabel und Funkleistung separat prüfen.
- USB-Impedanz hängt vom tatsächlich bestellten Lagenaufbau ab; der Export bestätigt keine 90-Ω-Auslegung.
- Alte Angaben zu 108 mm Breite, acht offenen DESK-/PU-Netzen und vollständig fehlendem Stitching sind überholt.

## 10. Offene Fertigungs-/Bestückpunkte

1. **U1-BOM:** leeres Supplier-Part-Feld durch C5736265 ergänzen oder exakten ESP32-C6-MINI-1-N4 im Bestückauftrag auswählen.
2. **U2-Via-in-Pad:** zwei 0,305-mm-Bohrungen liegen im 0,9 × 1,6-mm-Exposed-Pad unter der Paste. Für einfache Fertigung vorzugsweise knapp außerhalb platzieren und kurz/breit anbinden; alternativ ausdrücklich vom Bestücker akzeptiertes Füllen/Überplattieren vorsehen. Unterseitiges Tenting ersetzt das nicht.
3. **Bohrdaten:** 129 Via-Positionen sind sowohl in Drill_PTH_Through.DRL als auch Drill_PTH_Through_Via.DRL enthalten. CAM muss sie einmal fertigen. Separate Via-Datei nicht ungeprüft löschen, falls sie zur Prozesszuordnung benötigt wird.
4. **Bestückumfang:** alle sechs RJ-Buchsen, Header und USB bestätigen. USB ist im Placement SMD = No, besitzt aber SMT-Kontakte und Durchsteck-Befestigung. BOOT/RESET/I2C enthalten teils Leerzeichen in Designatoren; Importzuordnung prüfen.
5. **Reglerlayout, empfohlen:** C1-3V3-Pad liegt gegenüber L1-Ausgang; elektrisch verbunden, aber über innere Versorgungslage. C1 drehen/umplatzieren, kurze breite TOP-Verbindung L1–C1, C1-GND nahe U2 und VOS-Abgriff am Ausgangskondensator vorsehen. Noch nicht umgesetzt.
6. **Native Prüfungen:** ERC/DRC, ungeroutete Netze, Pours, reale Footprints/Polaritäten, Steckermechanik, Lagenaufbau und Bestücker-Preview prüfen.

Nach Änderungen alle Exporte synchron erneuern. Erst kleine Prototypenserie, keine Serienfreigabe allein anhand der Dateiprüfung.

## 11. Prototypentests

1. Unbestromt Kurzschlüsse, Durchgang, Steckerorientierung, Dioden und mechanischen Sitz prüfen.
2. Strombegrenzte 5-V-Inbetriebnahme; +5V/+3V3, Stromaufnahme, Regler-/Induktortemperatur messen.
3. Produktions-Flashing, BOOT/RESET, natives USB, LED und unerwünschte Bootausgaben an Desk-TX prüfen.
4. USB-only, Desk-only und USB an/ab unter WLAN-/UART-Last: Spannungseinbrüche, Resets und Rückspeisung messen.
5. UART-Richtungen, Idle-/OE-/Reset-Pegel, unveränderte Frames und Weiterleitungslatenz mit Logikanalysator prüfen.
6. Gleichzeitiges Handset und eigene Befehle: keine gemischten Frames, Priorität manueller Bedienung, keine alten Fahrbefehle.
7. Loctek aus echtem Sleep durch Handset und PandaDesk wecken; erster Tastendruck, Timing und Antworten bei offenem IN-Pin 4.
8. Jiecang-HS-Funktionen und LOW/Freigabe messen, statt Tasterzuordnungen zu vermuten.
9. Reset, Brownout, Kabelabzug, UART-Overflow, WLAN-Ausfall und OTA zunächst im Stillstand prüfen; Bewegungstests kontrolliert mit Stoppmöglichkeit.
10. Pro Modell Controller/Handset, Pegel, Baudrate, Frameformat, Wake-Timing, Versorgungsreserve und bestandene Tests dokumentieren; erst dann freigeben.

## 12. Technische Quellen

Referenzen unterstützen Umsetzung und erneute Prüfung; lokale Projekt-/Fertigungsdateien bestimmen die tatsächliche Verdrahtung.

- [Espressif ESP32-C6-MINI-1/-1U](https://documentation.espressif.com/esp32-c6-mini-1_mini-1u_datasheet_en.html): Modulpins und Grenzen.
- [TI TXU0204](https://www.ti.com/lit/ds/symlink/txu0204.pdf): Kanalrichtungen und gemeinsames OE.
- [TI SN74LVC07A](https://www.ti.com/lit/ds/symlink/sn74lvc07a.pdf): nichtinvertierende Open-Drain-Ausgänge.
- [TI TXU0101](https://www.ti.com/lit/ds/symlink/txu0101.pdf): Wake-Pegelwandler.
- [TI TPS2116](https://www.ti.com/lit/ds/symlink/tps2116.pdf): Quellenpriorität.
- [TI TPS62162](https://www.ti.com/lit/ds/symlink/tps62162.pdf): Regler und Layout.
- [LCSC C5736265](https://www.lcsc.com/product-detail/C5736265.html): U1-Bestellzuordnung.
- [JLCPCB Via Covering](https://jlcpcb.com/help/article/pcb-via-covering): Via-in-Pad-Verfahren.
- [iMicknl/LoctekMotion_IoT](https://github.com/iMicknl/LoctekMotion_IoT): Loctek-Referenzprojekt, keine universelle Freigabe.
- [dimitri-vs/flexispot-esphome](https://github.com/dimitri-vs/flexispot-esphome): modellbezogene Wake-/Protokollreferenz.

## 13. Kurzfassung

V1 nutzt drei feste Portpaare, U3 für die UART-Brücke, U4 für LOW-aktive Jiecang-Steuerleitungen, U5 für HIGH-aktives Loctek-Wake und U6/U2 für USB-priorisierte Versorgung. Keine universelle Pinmatrix und keine dynamischen Desk-Pull-ups.

Kern der Firmware ist zuverlässige Handset-Weiterleitung mit geordneter Einspeisung eigener Befehle. Loctek-Wake wird aus UART-Aktivität bzw. eigenen Befehlen erzeugt, weil IN-Pin 4 fehlt. Diese Annahme und jedes konkrete Tischprofil müssen am Prototyp bestätigt werden. Vor Bestellung Abschnitt 10 abarbeiten.
