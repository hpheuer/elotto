# E-Lotto — GCP-Analyse auf ESP32-P4

ESP32-P4 Projekt das Eurojackpot- und 6-aus-49-Lottozahlen mittels Hardware-TRNG und
GCP-Methodik (Global Consciousness Project) generiert.

## Hardware

- **Board:** Waveshare ESP32-P4-ETH
- **PHY:** IP101GRI via RMII (Ethernet RJ45, DHCP)
- **CPU:** ESP32-P4 @ 360 MHz, 768 KB SRAM
- **Chip Revision:** v1.3 (sdkconfig angepasst: `CONFIG_ESP32P4_REV_MIN_0=y`)

## Konzept

Pro Lauf werden 200.000 TRNG-Werte (32 Bit je) gelesen und nach GCP-Methodik ausgewertet:
- **32.000 Segmente** à 200 Bit
- Z-Score je Segment: `(Einsen − 100) / √50`
- Lauf-Z-Score: Summe aller Segment-Z-Scores / √32.000
- Die **Top-10 Läufe** mit dem höchsten positiven Z-Score liefern die Lottozahlen

## Web-Interface

Nach dem Start per Ethernet im Browser erreichbar (IP via Serial Monitor ablesen).

| Element | Beschreibung |
|---|---|
| **Läufe** | Eingabefeld, vorbelegt 1000, max. 8000 |
| **Euro-Lotto** | 5 Zahlen (1–50) + 2 Eurozahlen (1–12) |
| **6 aus 49** | 6 Zahlen (1–49) |
| **Fortschrittsbalken** | Live-Fortschritt, Laufzeit, ETA |
| **Abbrechen** | Stoppt nach aktuellem Lauf, zeigt Top-10 der bisherigen Läufe |
| **Browser-Reload** | ESP32 läuft im Hintergrund weiter; Seite reconnectet automatisch |

## Erkenntnisse aus der Entwicklung

### TRNG-Register-Adresse war fehlerhaft
Der direkte Zugriff auf Register `0x501101A4` lieferte **ausschließlich positive Z-Scores**
(alle 50 Läufe > 0, Z-Werte bis +5.5). Das deutet auf systematische Bias hin —
vermutlich war das Register nicht korrekt initialisiert oder die Adresse falsch.

**Fix:** Wechsel auf `esp_random()` aus dem ESP-IDF → sofort normale Verteilung
mit positiven und negativen Z-Scores wie erwartet.

### TRNG-Durchsatz ist der Flaschenhals
- Hardware-TRNG erzeugt ~57 KB/s unabhängig von der Leserate
- `__builtin_popcount` statt 200-Bit-Schleife: CPU-Overhead massiv reduziert
- `esp_fill_random()` statt 200.000× `esp_random()`: weniger Call-Overhead
- **Ergebnis:** Doppelte Werte = doppelte Zeit (linear, TRNG-limitiert)

### Timing-Richtwerte (200.000 Werte/Lauf, ESP32-P4 @ 360 MHz)

| Läufe | Segmente/Lauf | Dauer ca. |
|---|---|---|
| 500 | 32.000 | ~2 Std |
| 1000 | 32.000 | ~4 Std |
| 100K Werte × 1000 | 16.000 | ~2 Std |

Empfehlung: **500 Läufe × 200K Werte** = gleiche Zeit wie früher 1000 × 100K,
aber jeder Z-Score statistisch doppelt so belastbar.

### RAM-Limit
`RunResult` belegt ~40 Bytes. Bei 8000 Läufen = ~320 KB Resultate-Array.
ESP32-P4 hat 768 KB SRAM; nach IDF-Overhead bleiben ~400 KB für globale Variablen.
**Maximum: ~8000 Läufe** (im UI erzwungen).

### Chip Revision v1.3
Bootloader-Fehler beim ersten Flash: `requires chip revision [v3.1 - v3.99]`.
Fix: `idf.py menuconfig` → Component config → ESP32P4-Specific →
Minimum Supported ESP32-P4 Revision → v0.0

## Build & Flash

```powershell
# IDF-Terminal (Desktop-Shortcut "IDF_v6.0.1_Powershell")
cd D:\E-Lotto\elotto
idf.py build
idf.py flash -p COM4
idf.py monitor -p COM4
```

Oder mit `build.ps1` aus normalem PowerShell:

```powershell
.\build.ps1 build
.\build.ps1 flash -p COM4
.\build.ps1 monitor -p COM4
```

## Umgebung

- ESP-IDF v6.0.1 (`C:\esp\v6.0.1\esp-idf`)
- Tools: `C:\Espressif` (EIM-Standard auf diesem System)
- Python-Venv: `C:\Espressif\tools\python_env\idf6.0_py3.11_env`
- Target: `esp32p4`

## Projektstruktur

```
main/
  elotto.c    — app_main, Ethernet-Init, Webserver, HTML/JS
  sensor.c    — GCP-Analyse, TRNG-Batch, popcount, Lottozahl-Extraktion
  sensor.h    — Typen, Konstanten, ElottoStatus
build.ps1     — Build-Hilfsskript für normales PowerShell
sdkconfig     — ESP-IDF Konfiguration (Chip-Rev, Partitionen etc.)
```

## Versionshistorie

| Version | Beschreibung |
|---|---|
| v1.0 | GCP-Webserver, Eurojackpot + 6-aus-49, Live-Progress, Abort, Top-10 |
| v1.1 | Browser-Reconnect: Seite stellt State nach Reload wieder her |
| v1.2 | 200K TRNG-Werte/Lauf, popcount-Optimierung, konfigurierbarte Läufe (max 8000) |
