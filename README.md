# E-Lotto — GCP-Analyse auf ESP32-P4

ESP32-P4 Projekt das Eurojackpot- und 6-aus-49-Lottozahlen mittels Hardware-TRNG und
GCP-Methodik (Global Consciousness Project) generiert.

## Hardware

- **Board:** Waveshare ESP32-P4-ETH
- **PHY:** IP101GRI via RMII
- **Anschluss:** RJ45 Ethernet (DHCP)

## Konzept

1000 Läufe à 100.000 TRNG-Werte (Hardware-RNG via `esp_random()`).
Jeder Lauf wird nach GCP-Methodik ausgewertet:
- 16.000 Segmente à 200 Bit
- Z-Score = Σ(Einsen − 100) / √50 pro Segment, normiert auf √16000
- Die 10 Läufe mit dem höchsten positiven Z-Score liefern die Lottozahlen

## Web-Interface

Nach dem Start per Ethernet im Browser erreichbar (IP via Serial Monitor ablesen).

| Element | Beschreibung |
|---|---|
| **Euro-Lotto** | 5 Zahlen (1–50) + 2 Eurozahlen (1–12) |
| **6 aus 49** | 6 Zahlen (1–49) |
| **Fortschrittsbalken** | Live-Fortschritt mit Zeit und ETA |
| **Abbrechen** | Stoppt nach aktuellem Lauf, zeigt bisherige Top-10 |

## Build & Flash

```powershell
# IDF-Terminal (Desktop-Shortcut "IDF_v6.0.1_Powershell")
cd D:\E-Lotto\elotto
idf.py build
idf.py flash -p COM4
idf.py monitor -p COM4
```

Oder mit `build.ps1` direkt aus einem normalen PowerShell-Terminal:

```powershell
.\build.ps1 build
.\build.ps1 flash -p COM4
.\build.ps1 monitor -p COM4
```

## Umgebung

- ESP-IDF v6.0.1 (`C:\esp\v6.0.1\esp-idf`)
- Tools: `C:\Espressif`
- Target: `esp32p4`
- Chip Revision: v1.3 (sdkconfig: `CONFIG_ESP32P4_REV_MIN_0=y`)

## Projektstruktur

```
main/
  elotto.c    — app_main, Ethernet-Init, Webserver, HTML/JS
  sensor.c    — GCP-Analyse, TRNG, Lottozahl-Extraktion
  sensor.h    — Typen, Konstanten, ElottoStatus
build.ps1     — Build-Hilfsskript für normales PowerShell
sdkconfig     — ESP-IDF Konfiguration (Chip-Rev, Partition etc.)
```
