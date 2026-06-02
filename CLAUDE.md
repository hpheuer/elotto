# elotto – ESP32-P4 Projekt

## Umgebung
- Windows, Laufwerk D:\E-Lotto\elotto
- ESP-IDF unter C:\esp\esp-idf
- VS Code mit Espressif ESP-IDF Extension
- Target: **esp32p4**
- Build-System: idf.py via ESP-IDF Extension

## Projektstruktur
- main/elotto.c   – app_main
- main/sensor.c   – TRNG, GCP-Analyse, Eurojackpot-Extraktion
- main/sensor.h   – Typen und Deklarationen

## Konzept
50 Läufe à 100.000 TRNG-Werte (ESP32-P4 Hardware-TRNG, Register 0x501101A4).
Jeder Lauf wird nach GCP-Methodik ausgewertet (16.000 Segmente à 200 Bit, Z-Score).
Die 5 positivsten Läufe werden mit Eurojackpot-Zahlen (5 aus 50 + 2 aus 12) angezeigt.
Ausgabe ausschließlich über Serial Monitor (kein Webserver, kein Ethernet).

## Build, Flash, Monitor
| Aktion              | VS Code Shortcut |
|---------------------|------------------|
| Build + Flash + Monitor | F3           |
| Nur Build           | Ctrl+Shift+B     |
| Menuconfig          | Ctrl+E G         |

## Regeln
- sdkconfig NIEMALS manuell bearbeiten
- Target ist immer esp32p4
