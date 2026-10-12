<!-- SPDX-FileCopyrightText: 2026 Thomas Stoll
     SPDX-License-Identifier: Apache-2.0 -->

# EBCTester

**Batterien testen, Messdaten behalten – ohne wochenlang einen PC laufen zu lassen.**

EBCTester verbindet einen ZKE EBC-A20 mit einem kleinen ESP32-S3. Der
Mikrocontroller übernimmt die Bedienung, führt Testpläne aus und zeichnet
Messdaten auf einer SD-Karte auf. Über das Webinterface lassen sich Batterien
verwalten, Messungen verfolgen und Ergebnisse als Diagramme ansehen – auch mit
dem Handy und ohne Verbindung zum Heim-WLAN.

## Warum dieses Projekt?

ZKE-Geräte sind für Hobbyanwender brauchbar und bezahlbar. Die Geräte und ihre
PC-Software sind allerdings technisch schon etwas älter. Aus meiner Sicht wird
die Software nicht mehr wesentlich weiterentwickelt; da sie proprietär ist,
kann ich gewünschte Funktionen auch nicht selbst ergänzen.

Besonders wichtig war mir eine komfortablere Speicherung der Messdaten. Der
größte Nachteil war jedoch der dauerhaft benötigte PC: Batterietests können
Tage oder sogar Wochen dauern. Mein Laptop wäre währenddessen ständig an das
Testgerät gebunden und für andere Aufgaben nur eingeschränkt verfügbar.

Mit EBCTester kann ich Funktionen selbst hinzufügen, statt darauf zu warten,
ob der Hersteller sie irgendwann anbietet. Die Speicherung in einer Datenbank
schafft eine Grundlage für spätere Auswertungen über mehrere Batterien hinweg
und für weitere Exportformate.

Eine optionale Übertragung in MariaDB ermöglicht die Archivierung großer
Datenmengen außerhalb der SD-Karte. Nach einer überprüften Sicherung kann ich
lokale Testläufe gezielt löschen, ohne ihre extern gesicherten Daten zu verlieren.
Die Software löscht lokale Daten dabei **nicht automatisch**.

Begonnen habe ich mit ESPHome. Einige meiner Batterien befinden sich jedoch
außerhalb der Reichweite des Heim-WLANs. Ich brauchte eine eigenständige Lösung,
die auch dort weiter aufzeichnet und über einen eigenen WLAN-Zugangspunkt mit
dem Handy bedienbar bleibt.

— **Thomas Stoll**

## Was funktioniert bereits?

- **Ohne laufenden PC:** Manuelle Bedienung und automatische Lade-/Entladepläne
  laufen auf dem ESP32-S3; die Browserseite darf geschlossen werden.
- **Offline-Aufzeichnung:** SQLite-Datenbank auf einer FAT32-SD-Karte.
  Die Aufzeichnung benötigt kein Heim-WLAN, aber eine verfügbare SD-Datenbank.
- **Bedienung per Browser:** Lokales Bootstrap-Webinterface mit Diagrammen,
  Batterieinformationen und getrennten Bereichen für Tests und Verwaltung.
  CSS, JavaScript und Diagrammbibliothek liegen auf dem Gerät, nicht auf einem CDN.
- **Innenwiderstandsmessung:** Lastpulstest mit getrennten Ergebnissen bei
  standardmäßig 1, 5 und 10 Sekunden, gemessenem Strom und einstellbaren Grenzen.
  Vorbereiteter Ladezustand oder automatische Vorbereitung sind möglich.
- **Optionale MariaDB-Sicherung:** Testläufe und strukturierte
  Innenwiderstandsergebnisse können in eine externe Datenbank übertragen werden.
- **Sprachen:** Deutsch und Englisch; Englisch als Rückfallsprache.
  Zusätzliche Sprachdateien werden von der SD-Karte geladen.
- **Bewusst gestartete Updates:** Eine neue Firmware wird angezeigt, aber
  erst nach einem Klick und einer Bestätigung installiert.

**Aktueller Stand:** Firmware 1.3.35, entwickelt und gebaut mit ESP-IDF 6.0.1
für ESP32-S3. Der EBC-A20 ist das derzeit unterstützte Testgerät; eine
allgemeine Unterstützung aller ZKE-Modelle wird nicht behauptet.

Weitere Exportformate und batterieübergreifende Analysen sind
Erweiterungsmöglichkeiten, noch keine fertigen Funktionen.

## Aufbau und Wiederverwendung

Das Projekt ist bewusst in Funktionseinheiten aufgeteilt. Das Hauptprogramm
bleibt klein und initialisiert die Komponenten; die eigentliche Arbeit liegt
in den jeweiligen Modulen.

Zwei Pakete sind eigenständige Repositories, damit sie in anderen Projekten
wiederverwendet werden können, ohne WLAN, Updates und SD-Verwaltung erneut
schreiben zu müssen:

| Repository | Aufgabe |
| --- | --- |
| **[EBCTester](https://github.com/Gemelon/EBCTester)** | Batteriebedienung, Testpläne, Aufzeichnung, Diagramme, DCIR und MariaDB-Sicherung |
| **[ota-suite](https://github.com/Gemelon/ota-suite)** | WLAN-Einrichtung, manuelle OTA-Updates, gemeinsames Webinterface, Sprachunterstützung und Status-LED |
| **[sd-card-manager](https://github.com/Gemelon/sd-card-manager)** | SDMMC-/SPI-Anbindung, Kartenwechsel, gemeinsame Speicherzugriffssperre und Dateiverwaltung |

EBCTester bindet die beiden Pakete als **Git-Submodule mit festen
Commit-Ständen** in `external/` ein. Die SD-Verwaltung nutzt das Webinterface
der OTA Suite. OTA Suite selbst benötigt weder EBCTester noch eine SD-Karte.

Die projektspezifischen Komponenten liegen in `components/`:

| Komponente | Aufgabe |
| --- | --- |
| `ebc_link` | UARTs, Protokollparser und Testerkommunikation |
| `test_control` / `test_store` | Bedienung, Testplanablauf, Batterien und Aufzeichnung |
| `resistance_test` | DCIR-Vorbereitung, Lastpuls und Auswertung |
| `sqlite3` / `battery_database` | SQLite-Anbindung, SD-VFS und Datenbankschema |
| `battery_dashboard` | Messdaten und lokale Diagramme |
| `mariadb_sync` | Inkrementelle externe Sicherung |
| `ui_localization` | Zusätzliche SD-Sprachen |

API-Endpunkte, Zustandsabläufe und detaillierte Parameter sind in der
[technischen Referenz](docs/TECHNICAL_REFERENCE.md) beschrieben.

## Selbst verwenden und kompilieren

### Voraussetzungen

- ESP32-S3 mit **16 MiB Flash**, beispielsweise N16R8; die Defaults verwenden
  optionales Octal-PSRAM. Andere Boards benötigen passende Einstellungen.
- ZKE EBC-A20 und eine geeignete **3,3-V-/5-V-TTL-Pegelanpassung**.
  ESP32-GPIOs sind nicht 5-V-tolerant.
- FAT32-SD-Karte und passende SD-Anbindung mit Pull-ups.
- VS Code mit ESP-IDF-Erweiterung, ESP-IDF **6.0.1** und Git.
- Internet beim ersten Build für verwaltete Komponenten und den
  hashgeprüften SQLite-Download. Die fertige Oberfläche arbeitet offline.

### Projekt herunterladen

Die Repositories sind derzeit privat; dein GitHub-Konto benötigt Zugriff auf
alle drei. Ein ZIP-Download allein enthält die Submodule nicht.

```powershell
git clone --recurse-submodules https://github.com/Gemelon/EBCTester.git
```

In einem vorhandenen Checkout:

```powershell
git submodule update --init --recursive
```

Für reproduzierbare Builds nicht ungeprüft `git submodule update --remote`
verwenden: Der Hauptstand referenziert die getesteten Paketversionen.

### Hardware konfigurieren und bauen

1. Projekt in VS Code öffnen und die ESP-IDF-Umgebung auswählen.
2. Ziel **esp32s3** einstellen.
3. In menuconfig Board, Flash, PSRAM, SD-Modus und UART-Pins prüfen.
4. Mit **ESP-IDF: Build your project** kompilieren.
5. Für die Erstinstallation Bootloader, Partitionstabelle und Anwendung über
   USB installieren. **Die Partitionstabelle setzt 16 MiB Flash voraus.**

Der optionale Dev Container verwendet ebenfalls ESP-IDF 6.0.1.
`sdkconfig.defaults` enthält portable Vorgaben; eine vorhandene lokale
`sdkconfig` hat Vorrang. `dependencies.lock` hält die verwalteten Abhängigkeiten
fest. Alternative Paketpfade lassen sich über die CMake-Variablen
`OTA_SUITE_DIR` und `SD_CARD_MANAGER_DIR` einstellen.

**Ein Build flasht das Gerät nicht und startet keinen Batterietest.**
Eine geänderte Partitionstabelle kann nicht per OTA installiert werden.
Vor einer Neuaufteilung vorhandene Einstellungen und Daten sichern.

### Voreingestellte Anschlüsse

| Anschluss | TX | RX | Parameter |
| --- | ---: | ---: | --- |
| UART1 zum Tester | 17 | 18 | 9600 Baud, 8O1 |
| UART2 zum PC-Dongle | 8 | 9 | 9600 Baud, 8O1 |

Die Dongle-Brücke ist **noch nicht implementiert**. UART2 wird initialisiert;
es findet noch keine Weiterleitung zur Herstellersoftware statt.

| SD-Signal | SDMMC-GPIO |
| --- | ---: |
| CLK / CMD | 14 / 15 |
| D0 / D1 / D2 / D3 | 2 / 4 / 5 / 6 |
| Kartenerkennung | 7, aktiv HIGH |

SPI ist ebenfalls auswählbar: MISO 2, MOSI 15, CS 6, CLK 14.
Beide Modi starten mit 10 MHz; die Anschlüsse sind konfigurierbar.
Karten werden **nie automatisch formatiert**. exFAT wird nicht unterstützt.
Die optionale WS2812-LED nutzt standardmäßig GPIO 48; manche Boards benötigen 38.

### Erster Zugriff und alltägliche Bedienung

1. Gerät starten und das AP-/Admin-Passwort über die serielle Ausgabe ablesen.
   Ohne fest eingestelltes Passwort wird einmalig ein zufälliges erzeugt.
2. Handy oder Computer mit `BatteryTester-XXXXXX` verbinden.
3. `http://192.168.4.1` öffnen und als `admin` mit dem AP-Passwort anmelden.
4. Optional Heim-WLAN einrichten. Für lokale Bedienung und Aufzeichnung
   ist diese Verbindung nicht erforderlich.
5. FAT32-Karte einlegen, Datenbankbereitschaft prüfen, Batterie anlegen und
   Tester verbinden. Erst danach eine Messung oder einen Plan starten.

Unter **Batterietester** liegen Messdaten, Batterie, Manuell, Automatik und
Innenwiderstand. Unter **Verwaltung** liegen Geräteinformation, SD-Karte,
Updates und Einstellungen.

Testpläne unterstützen D-CC, D-CP, C-CV, Wartezeiten und Wiederholungen.
Ein nach Neustart unterbrochener Plan wird nicht ungeprüft fortgesetzt.
Stop und Abbruch bleiben verfügbar; konkurrierende Startaktionen sind gesperrt.

### Daten, Sicherung und Sprachen

Die lokale Datenbank ist `/sdcard/ebc_a20.db`. Schema und Integrität werden beim
Öffnen geprüft. Fehlende Karten verhindern nicht den Start des Webinterfaces,
aber Messaufzeichnung erfordert eine bereite Datenbank. Ungültige
Datenbankdateien werden erhalten und nicht stillschweigend überschrieben.

MariaDB ist optional und auf neuen Installationen deaktiviert. Host und
Zugangsdaten werden in den Einstellungen hinterlegt; die Übertragung benötigt
eine Netzwerkverbindung. Die Basistabellen aus den bisherigen Migrationen
001–005 müssen bereits existieren. Dieses Repository enthält zusätzlich
[Migration 006 für DCIR-Ergebnisse](sql/006_resistance_results_mariadb.sql),
aber **keine vollständige MariaDB-Ersteinrichtung**. Nach geprüfter Sicherung
können lokale Testläufe manuell gelöscht werden; die externe Kopie bleibt bestehen.

Deutsch und Englisch sind eingebaut. Die
[englische Vorlage](sd_card/languages/en-custom.json) und die
[Filipino-Testdatei](sd_card/languages/fil.json) werden in den Ordner
`/languages/` auf der SD-Karte kopiert. Danach Sprachen neu laden und auswählen.
Fehlende Übersetzungen fallen auf Englisch zurück. Ein Sprachwechsel lädt die
Seite neu und verwirft ungespeicherte Formulare, beendet aber keinen laufenden Test.

## Updates und Veröffentlichung eigener Firmware

Automatische Verfügbarkeitsprüfungen laden nur das Manifest. Installation
erfolgt ausschließlich manuell nach Bestätigung. HTTPS prüft Zertifikate;
HTTP muss ausdrücklich für ein vertrauenswürdiges Testnetz freigegeben werden.
Größe, SHA256 und Firmware-Metadaten werden vor der Boot-Auswahl geprüft.
Rollback schützt eine neue Firmware, wenn sie das Startfenster nicht übersteht.

Die voreingestellte Manifest-Adresse ist nur ein Beispiel. Eigene
HTTPS-Adresse in menuconfig unter **OTA Suite update checks** eintragen.

Das Kopieren auf ein NAS ist in einem frischen Checkout deaktiviert.
Bei Bedarf eine lokale, Git-ignorierte `CMakeUserConfig.cmake` erstellen:

```cmake
set(EBCTESTER_PUBLISH_FIRMWARE ON)
set(EBCTESTER_PUBLISH_DIR "D:/firmware-releases")
set(EBCTESTER_PUBLISH_ORIGIN "https://updates.example.org")
set(EBCTESTER_PUBLISH_URL_PATH "/BatteryTester")
```

Nach Rekonfiguration veröffentlicht jeder erfolgreiche Build die versionierte
Binärdatei, den Kompatibilitätsnamen und zuletzt atomar `firmware.json`.
Das Zielverzeichnis muss bereits existieren. Ein veröffentlichter Versionsname
darf nicht mit anderen Bytes überschrieben werden: **Version vorher erhöhen**.
Kopierfehler lassen den Veröffentlichungsschritt fehlschlagen.

Bei der Weitergabe von Firmware die Lizenzunterlagen mitliefern, nicht nur
die `.bin`: siehe [Drittanbieterhinweise](THIRD_PARTY_NOTICES.md).
Das aktivierte EBCTester-Publishing kopiert sie automatisch vor dem Image in
`licenses/EBCTester-<version>/` im Zielverzeichnis. Fehlende Lizenzunterlagen
lassen den Schritt fehlschlagen. Den vollständigen Ordner mit der Firmware
weitergeben; das OTA-Manifest selbst wird dadurch nicht verändert.

## Grenzen, Sicherheit und Mitarbeit

- Unabhängiges Hobbyprojekt, **kein offizielles ZKE-Produkt** und keine
  Zusicherung der Unterstützung durch den Gerätehersteller.
- Batterieparameter und Schutzgrenzen müssen zur Chemie, Verkabelung und
  Belastbarkeit passen. Software ersetzt keine Hardware-Schutzmaßnahmen.
  Ein Langzeittest ist nicht automatisch ein sicher unbeaufsichtigter Test.
- Der lokale Webserver nutzt HTTP Basic Authentication. Nur in einem
  vertrauenswürdigen LAN oder geschützten AP verwenden, nicht ins Internet stellen.
- MariaDB-Kommunikation ist unverschlüsseltes TCP. NVS/Flash sind standardmäßig
  nicht verschlüsselt; Passwörter und seriellen Zugang schützen.
- SHA256 allein ist keine Signatur. Secure Boot, signierte Firmware und
  Flash-/NVS-Verschlüsselung sind gesonderte Deployment-Entscheidungen.
- UTC-Uhrzeiten benötigen eine gültige Gerätezeit; ohne sie bleibt die
  Aufzeichnung über Laufzeitwerte möglich. Keine erfundenen Zeitstempel.
- SD-Karte, Stromversorgung und Backups regelmäßig prüfen. Keine Garantie für
  Messgenauigkeit, Datenverlustfreiheit oder Eignung für einen bestimmten Zweck.

Fehlerberichte und Verbesserungen sind willkommen. Bitte Board, Firmwarestand,
Reproduktionsschritte und bereinigte Logs nennen; niemals Passwörter, Tokens
oder private Messdaten in Issues oder Commits veröffentlichen.
Änderungen an Protokoll, Datenbankschema und Sicherheitsgrenzen mit Tests begleiten.
Weitergegebene geänderte Dateien nach Apache-2.0 als geändert kennzeichnen und
bestehende Urheber-/Lizenzhinweise erhalten.

## Tests

Hosttests benötigen Node.js, Python und für den Publisher CMake; keine
zusätzlichen Node-Pakete:

```powershell
node --test tests\web_interface.test.js tests\publication.test.js tests\ui_languages_test.js
python tests\resistance_schema_test.py
python tests\license_metadata_test.py
```

Falls nötig `CMAKE_EXE`, `OTA_SUITE_DIR` und `SD_CARD_MANAGER_DIR` setzen.
Native Manifest- und DCIR-Tests sowie Hardwareprüfungen stehen in der
[technischen Referenz](docs/TECHNICAL_REFERENCE.md).
Der lokale Browser-Testserver `python tests\ui_language_fixture.py` verwendet
nur simulierte Daten und steuert keine Hardware.

## Lizenz und Urheber

**Copyright 2026 Thomas Stoll.** Der ursprüngliche Code und die Dokumentation
von EBCTester, OTA Suite und SD Card Manager stehen unter der
**[Apache License 2.0](LICENSE)**. Sie erlaubt auch gewerbliche Nutzung,
Änderungen und Verkauf unter ihren Bedingungen. Es gilt der Haftungs- und
Gewährleistungsausschluss der Lizenz. Sie verleiht keine Rechte an fremden Marken.

[NOTICE](NOTICE) enthält den Projekthinweis. Quelldateien tragen
SPDX-Lizenz-/Urheberhinweise; Formate ohne Kommentare sind in
[.reuse/dep5](.reuse/dep5) zugeordnet. Diese Lizenzzuordnung betrifft
**nicht** den fremden Code der folgenden Abhängigkeiten.

## Drittanbieter und ihre Lizenzen

Dieses Projekt baut auf der Arbeit anderer auf. Ihre Urheberhinweise,
Lizenztexte und Haftungsausschlüsse bleiben erhalten; keine dieser Bibliotheken
wird als eigene Entwicklung ausgegeben oder pauschal unter eine neue Lizenz gestellt.

| Bestandteil | Urheber / Herkunft | Lizenz und mitgelieferte Hinweise |
| --- | --- | --- |
| Bootstrap 5.3.3 | The Bootstrap Authors | MIT – [Originaltext](external/ota-suite/components/web_interface/assets/BOOTSTRAP-LICENSE) |
| Chart.js 4.4.9 | Chart.js Contributors | MIT – [Originaltext](components/battery_dashboard/assets/CHARTJS-LICENSE.md) |
| @kurkle/color 0.3.2, in Chart.js enthalten | Jukka Kurkela | MIT – [Originaltext](components/battery_dashboard/assets/KURKLE-COLOR-LICENSE.md) |
| SQLite 3.53.4 | SQLite-Autoren | [Public Domain](https://sqlite.org/copyright.html); separat heruntergeladene Amalgamation |
| ESP-IDF 6.0.1 | Espressif Systems und weitere Beitragende | Apache-2.0 für Espressifs ursprünglichen Code; [SDK-Lizenz](third_party/licenses/esp-idf/LICENSE), weitere SDK-Bestandteile siehe unten |
| cJSON 1.7.19~2 | Dave Gamble und cJSON contributors | MIT – [Originaltext](third_party/licenses/cjson/LICENSE) |
| led_strip 3.1.0~1 | Espressif Systems | Apache-2.0 – [Originaltext](third_party/licenses/led_strip/LICENSE) |
| FreeRTOS-Kernel | Amazon.com und Beitragende | MIT – [Originaltext](third_party/licenses/esp-idf/FreeRTOS-LICENSE.md) |
| lwIP | Swedish Institute of Computer Science und weitere Autoren | BSD-3-Clause – [Originaltext](third_party/licenses/esp-idf/lwip-COPYING) |
| FatFs | ChaN | Eigene permissive FatFs-Bedingungen – [Originalhinweis](third_party/licenses/esp-idf/FatFs-NOTICE.txt) |
| Mbed TLS / TF-PSA-Crypto | Upstream-Autoren laut Lizenz-/Quelldateien | Apache-2.0 als gewählte Dual-License-Option; [Mbed TLS](third_party/licenses/esp-idf/mbedtls-LICENSE), [TF-PSA-Crypto](third_party/licenses/esp-idf/tf-psa-crypto-LICENSE); einzelne Dateien haben eigene Hinweise |
| wpa_supplicant | Jouni Malinen und Beitragende | BSD-3-Clause – [Lizenz](third_party/licenses/esp-idf/wpa_supplicant-README), [Copyright](third_party/licenses/esp-idf/wpa_supplicant-COPYING) |
| http_parser | Igor Sysoev, Joyent und Node contributors | MIT – [Originaltext](third_party/licenses/esp-idf/http_parser-LICENSE.txt) |
| C-Laufzeit: Newlib / Picolibc | Verschiedene Autoren | Sammlung eigener, überwiegend permissiver Bedingungen – [Newlib](third_party/licenses/esp-idf/COPYING.NEWLIB), [Picolibc](third_party/licenses/esp-idf/COPYING.picolibc) |
| GCC-Laufzeit: libgcc / libstdc++ | Free Software Foundation und Beitragende | GPL-3.0 mit GCC Runtime Library Exception 3.1 – [GPL](third_party/licenses/toolchain/COPYING3), [Ausnahme](third_party/licenses/toolchain/COPYING.RUNTIME) |
| TLSF-Speicherverwaltung | Matthew Conte | BSD-3-Clause – [Originalhinweis](third_party/licenses/esp-idf/TLSF-NOTICE.txt) |
| Argtable3 / linenoise | Stewart Heitmann; Salvatore Sanfilippo und Pieter Noordhuis | BSD-Bedingungen – [Argtable3](third_party/licenses/esp-idf/argtable3-LICENSE), [linenoise](third_party/licenses/esp-idf/linenoise-LICENSE) |
| protobuf-c / SPIFFS, SDK-Komponenten | Dave Benson und protobuf-c authors; Peter Andersson | [BSD-2-Clause](third_party/licenses/esp-idf/protobuf-c-LICENSE) / [MIT](third_party/licenses/esp-idf/spiffs-LICENSE); je nach Build eingebunden |
| Weitere dateispezifische SDK-Hinweise | Urheber laut ursprünglichen Dateiheadern | [Gesammelte Originalhinweise](third_party/licenses/esp-idf/SOURCE-NOTICES.txt), einschließlich BSD, MIT und CC0; [zugehörige Standardtexte](third_party/licenses/standard/) |

Weitere unveränderte SDK-Lizenztexte liegen in
[third_party/licenses/](third_party/licenses/). Versionen verwalteter Komponenten
werden durch [dependencies.lock](dependencies.lock) festgehalten.
Details zur Herkunft, zu dateispezifischen Hinweisen und zur Weitergabe von
Quellcode oder Firmware stehen in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
Bei anderen SDK-Versionen oder zusätzlichen Komponenten ist diese Liste erneut
zu prüfen; sie ersetzt nicht die Lizenztexte der tatsächlich verwendeten Dateien.
