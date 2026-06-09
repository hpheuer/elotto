# elotto – ESP32-P4 Project

## Environment
- Windows, drive D:\E-Lotto\elotto
- ESP-IDF at C:\esp\v6.0.1\esp-idf  (Tools: C:\Espressif)
- VS Code with Espressif ESP-IDF Extension
- Target: **esp32p4**
- Build system: idf.py via ESP-IDF Extension

## Project structure
- main/elotto.c   – app_main, Ethernet, webserver, HTML/JS UI, /diag endpoint
- main/sensor.c   – TRNG, GCP analysis, baseline calibration, slave UART, lottery extraction
- main/sensor.h   – types and declarations

## Concept
Dual-ESP32-P4 system. Master (COM4) scores lottery numbers via GCP methodology using the
hardware TRNG (register 0x501101A4). An optional slave ESP32-P4 (COM6) measures in parallel
via UART1 (GPIO14/15, 460800 baud); combined z-score = (z_master + z_slave) / sqrt(2) (SNR x sqrt(2)).

Phase 1: baseline calibration (master + slave in parallel).
Phase 0: score individual numbers 1..N with GCP runs to build candidate pool.
Phase 2: measure all pool combinations (lexicographic enumeration), sort by z-score.
Results shown in browser UI (Ethernet, DHCP). Top-10 runs + most-frequent from Z>2 runs.

Modes: Eurojackpot (5 of 50 + 2 of 12, 7920 combinations) and 6 of 49 (5005 combinations).

## Build, Flash, Monitor
| Action                  | VS Code shortcut |
|-------------------------|------------------|
| Build + Flash + Monitor | F3               |
| Build only              | Ctrl+Shift+B     |
| Menuconfig              | Ctrl+E G         |

## Rules
- Never edit sdkconfig manually
- Target is always esp32p4
