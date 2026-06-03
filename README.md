# E-Lotto — GCP-Analyse auf ESP32-P4

ESP32-P4 Projekt das Eurojackpot- und 6-aus-49-Lottozahlen mittels Hardware-TRNG und
[GCP-Methodik (Global Consciousness Project)](https://grokipedia.com/page/Global_Consciousness_Project)
generiert.

## Hardware

- **Board:** Waveshare ESP32-P4-ETH
- **PHY:** IP101GRI via RMII (Ethernet RJ45, DHCP)
- **CPU:** ESP32-P4 @ 360 MHz, 768 KB SRAM
- **Chip Revision:** v1.3 (sdkconfig angepasst: `CONFIG_ESP32P4_REV_MIN_0=y`)

## Konzept

Pro Lauf werden 200.000 TRNG-Werte direkt vom Hardware-Register gelesen und nach
GCP-Methodik ausgewertet:
- **32.000 Segmente** à 200 Bit
- Z-Score je Segment: `(Einsen − 100) / √50`
- Lauf-Z-Score: `Σ(Z_segment) / √32.000`, **korrigiert um Baseline-Mittelwert**
- Die **Top-10 Läufe** mit dem höchsten korrigierten Z-Score liefern die Lottozahlen

## Web-Interface

Nach dem Start per Ethernet im Browser erreichbar (IP via Serial Monitor ablesen).

| Element | Beschreibung |
|---|---|
| **Läufe** | Eingabefeld, Standard 1000, max. 8000 |
| **Baseline** | Kalibrierungsläufe, Standard 100, max. 5000 |
| **Euro-Lotto** | 5 Zahlen (1–50) + 2 Eurozahlen (1–12) |
| **6 aus 49** | 6 Zahlen (1–49) |
| **Kalibrierungsphase** | Goldene Fortschrittsleiste mit ✅ wenn fertig |
| **Messphase** | Grüne Fortschrittsleiste mit Laufzeit, ETA und ✅ wenn fertig |
| **Abbrechen** | Stoppt nach aktuellem Lauf, zeigt Top-10 der bisherigen Läufe |
| **Browser-Reload** | ESP32 läuft im Hintergrund weiter; Seite reconnectet automatisch |
| **Diagnose** | `http://<IP>/diag` — vergleicht Register vs esp_random() |

## Erkenntnisse aus der Entwicklung

### TRNG-Register ist 75× schneller als esp_random()

Die Diagnose (`/diag`) ergab:

```json
{"reg_ms":3, "reg_bias":0.499220, "reg_stuck":0, "reg_z_mean":-0.0221,
 "esp_ms":225, "esp_bias":0.499310, "esp_stuck":0, "esp_z_mean":-0.0195,
 "speedup":75.0}
```

- Kein einziger Stuck-Wert (reg_stuck: 0) — keine Korrelationen
- Bit-Bias: 0.499220 statt ideal 0.500000 — winzige aber messbare Abweichung
- **Kritisch:** Ohne Baseline-Korrektur ergibt der Bias systematisch Z ≈ −3.95 pro Lauf

### Baseline-Korrektur ist zwingend erforderlich

Der systematische Hardware-Bias akkumuliert sich über 32.000 Segmente:

```
E[z_run] = -0.0221 × √32.000 ≈ -3.95 pro Lauf
```

Lösung analog zum eTensor-Projekt (Princeton PEAR-Labor-Methodik):
1. **Phase 1:** N Kalibrierungsläufe → `baseline_mean` ermitteln
2. **Phase 2:** Messläufe, jeder korrigiert: `z_korrigiert = z_raw - baseline_mean`

Damit hat jede Messung einen Erwartungswert von 0 — statistisch korrekt.

### TRNG-Register-Adresse war anfangs biased

Der direkte Zugriff auf Register `0x501101A4` lieferte in einem frühen Test
**ausschließlich positive Z-Scores** (alle 50 Läufe > 0). Vermutliche Ursache:
TRNG-Initialisierungszustand beim allerersten Start. Nach vollständigem IDF-Boot
und mit Baseline-Korrektur arbeitet das Register korrekt.

Zwischenzeitlich wurde `esp_random()` verwendet — korrekte Ergebnisse, aber 75× langsamer.

### Timing-Richtwerte (200.000 Werte/Lauf, ESP32-P4 @ 360 MHz, direktes Register)

| Config | Kalibrierung | Messung | Gesamt |
|---|---|---|---|
| 100 Baseline + 1000 Läufe | ~20 Sek | ~3 Min | **~3 Min** |
| 100 Baseline + 4000 Läufe | ~20 Sek | ~13 Min | **~14 Min** |
| 100 Baseline + 8000 Läufe | ~20 Sek | ~26 Min | **~27 Min** |

Zum Vergleich mit `esp_random()` (75× langsamer): 1000 Läufe ≈ 4 Stunden.

### Optimierungen

- **`__builtin_popcount`** statt 200-Bit-Schleife: 28× weniger CPU-Arbeit pro Segment
- **Direktes TRNG-Register** statt `esp_random()`: 75× schneller (TRNG-limitiert)
- **Baseline-Korrektur**: eliminiert Hardware-Bias, statistisch korrekte Z-Scores

### RAM-Limit

`RunResult` belegt ~40 Bytes. **Maximum: ~8000 Läufe** (320 KB Resultate-Array).
Im UI erzwungen. ESP32-P4 hat 768 KB SRAM.

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

## Diagnose

```
http://<IP>/diag
```

Vergleicht direktes TRNG-Register mit `esp_random()`: Geschwindigkeit, Bias,
Korrelationen, Z-Score-Verteilung. Ca. 5 Sekunden Laufzeit.

## Umgebung

- ESP-IDF v6.0.1 (`C:\esp\v6.0.1\esp-idf`)
- Tools: `C:\Espressif` (EIM-Standard auf diesem System)
- Target: `esp32p4`, Chip Rev v1.3

## Projektstruktur

```
main/
  elotto.c    — app_main, Ethernet, Webserver, HTML/JS inkl. /diag
  sensor.c    — GCP-Analyse, TRNG-Register, Baseline, Lottozahl-Extraktion
  sensor.h    — Typen, ElottoStatus (inkl. Phase/Baseline-Felder)
build.ps1     — Build-Hilfsskript für normales PowerShell
sdkconfig     — ESP-IDF Konfiguration
```

## Versionshistorie

| Version | Beschreibung |
|---|---|
| v1.0 | GCP-Webserver, Eurojackpot + 6-aus-49, Live-Progress, Abort, Top-10 |
| v1.1 | Browser-Reconnect: Seite stellt State nach Reload wieder her |
| v1.2 | 200K TRNG-Werte/Lauf, popcount-Optimierung, konfigurierbare Läufe (max 8000) |
| v1.3 | Direktes TRNG-Register (75× schneller) + Baseline-Kalibrierung, /diag-Endpunkt |
