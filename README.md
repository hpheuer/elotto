# E-Lotto — GCP-Analyse auf ESP32-P4

ESP32-P4 Projekt das Eurojackpot- und 6-aus-49-Lottozahlen mittels Hardware-TRNG und
[GCP-Methodik (Global Consciousness Project)](https://grokipedia.com/page/Global_Consciousness_Project)
generiert.

## Screenshots

<table>
<tr>
<td align="center"><b>Messung läuft</b></td>
<td align="center"><b>Ergebnis mit Top-10 + Am häufigsten</b></td>
</tr>
<tr>
<td><img src="docs/screenshot_laufend.png" width="390"></td>
<td><img src="docs/screenshot_ergebnis.png" width="390"></td>
</tr>
</table>

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
- Zusätzlich: häufigste Zahlen aus **allen Läufen mit Z > 2**

## Web-Interface

Nach dem Start per Ethernet im Browser erreichbar (IP via Serial Monitor ablesen).

| Element | Beschreibung |
|---|---|
| **Läufe** | Eingabefeld, Standard 1000, max. 8000 |
| **Baseline** | Kalibrierungsläufe, Standard 100, max. 5000 |
| **Euro-Lotto** | 5 Zahlen (1–50) + 2 Eurozahlen (1–12) |
| **6 aus 49** | 6 Zahlen (1–49) |
| **Kalibrierungsphase** | Goldene Fortschrittsleiste mit ✔ wenn fertig |
| **Messphase** | Grüne Fortschrittsleiste mit Laufzeit, ETA und ✔ wenn fertig |
| **Am häufigsten** | Häufigste Zahlen aus Top-10 + alle weiteren Z>2-Läufe |
| **Abbrechen** | Stoppt nach aktuellem Lauf, zeigt Top-10 der bisherigen Läufe |
| **Browser-Reload** | ESP32 läuft im Hintergrund weiter; Seite reconnectet automatisch |
| **Diagnose** | `http://<IP>/diag` — vergleicht Register vs esp_random() |

## Schlüsselcode

### 1 — Direkter TRNG-Register-Zugriff

Statt `esp_random()` (der intern einen Treiber durchläuft) wird das Hardware-Register
direkt gelesen — **75× schneller**, identische Qualität:

```c
// sensor.c
#define RNG_REG  (*((volatile uint32_t *)0x501101A4UL))
static inline uint32_t fast_rng(void) { return RNG_REG; }
```

### 2 — GCP-Z-Score mit `__builtin_popcount`

Pro 200-Bit-Segment werden 6×32 + 1×8 = 200 Bit mit 7 TRNG-Reads ausgelesen.
`__builtin_popcount` zählt die Einsen in einem Takt statt in einer 32-Bit-Schleife
(**28× weniger CPU-Arbeit** pro Segment):

```c
// sensor.c — gcp_zscore_raw()
for (int seg = 0; seg < 32000; seg++) {
    int ones = __builtin_popcount(fast_rng())   // 32 Bit
             + __builtin_popcount(fast_rng())
             + __builtin_popcount(fast_rng())
             + __builtin_popcount(fast_rng())
             + __builtin_popcount(fast_rng())
             + __builtin_popcount(fast_rng())
             + __builtin_popcount(fast_rng() & 0xFF);  //  8 Bit
    z_sum += (ones - 100.0) / 7.07106781;  // sqrt(50) ≈ 7.071
}
return z_sum / sqrt(32000.0);
```

### 3 — Zwei-Phasen-Messung (Baseline-Korrektur)

Der TRNG hat einen systematischen Bias von ca. −0,022 pro Segment.
Über 32.000 Segmente akkumuliert das zu **Z ≈ −3,95 pro Lauf** ohne Korrektur.
Lösung: Phase 1 misst den Bias, Phase 2 subtrahiert ihn:

```c
// sensor.c — elotto_task()

// Phase 1: Kalibrierung
g_status.phase = PHASE_BASELINE;
double bsum = 0.0;
for (int i = 0; i < baseline_total; i++) {
    bsum += gcp_zscore_raw();
    g_status.baseline_done = i + 1;
}
double baseline_mean = bsum / baseline_total;

// Phase 2: Bias-korrigierte Messung
g_status.phase = PHASE_MEASURING;
for (int i = 0; i < runs_total; i++) {
    double z = gcp_zscore_raw() - baseline_mean;   // ← Korrektur
    g_status.results[i].z_score = z;
}
```

### 4 — Frequenz-Analyse (Am häufigsten)

Nach Abschluss aller Läufe werden die Nummern-Häufigkeiten über **alle Z>2-Läufe**
aggregiert. Für die Top-10 werden die bereits gezeichneten Zahlen direkt verwendet;
für weitere Z>2-Läufe jenseits Rang 10 werden neue Ziehungen vorgenommen:

```c
// sensor.c — nach qsort + extract_numbers()
for (int i = 0; i < done; i++) {
    if (g_status.results[i].z_score <= 2.0) break;  // sortiert absteigend
    z2_count++;
    if (i < TOP_N) {
        // Bereits gezeichnete Top-10-Zahlen direkt zählen
        for (int j = 0; j < nm; j++) freq[results[i].nums[j]]++;
    } else {
        // Neue Ziehung für weitere Z>2-Läufe
        draw_unique_sorted(tmp, nm, max_val, mask);
        for (int j = 0; j < nm; j++) freq[tmp[j]]++;
    }
}
// Top-N häufigste Zahlen extrahieren + aufsteigend sortieren
```

### 5 — Unbiased Rejection Sampling für Lottozahlen

Modulo-Operationen erzeugen einen Bias wenn `max_val` kein Teiler von 2^n ist.
Rejection Sampling verwirft unpassende Werte vollständig:

```c
// sensor.c
static uint8_t draw_unbiased(uint8_t max_val, uint8_t mask) {
    uint8_t v;
    do { v = (uint8_t)((fast_rng() & mask) + 1); } while (v > max_val);
    return v;
    // Eurojackpot: mask=63 → 1..64, reject >50; ~21% Verwurf
    // 6 aus 49:    mask=63 → 1..64, reject >49; ~23% Verwurf
    // Eurozahlen:  mask=15 → 1..16, reject >12; ~25% Verwurf
}
```

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
| 100 Baseline + 7000 Läufe | ~20 Sek | ~26 Min | **~27 Min** |
| 1000 Baseline + 7000 Läufe | ~3 Min | ~26 Min | **~29 Min** |

Zum Vergleich mit `esp_random()` (75× langsamer): 1000 Läufe ≈ 4 Stunden.

### Optimierungen

- **`__builtin_popcount`** statt 200-Bit-Schleife: 28× weniger CPU-Arbeit pro Segment
- **Direktes TRNG-Register** statt `esp_random()`: 75× schneller (TRNG-limitiert)
- **Baseline-Korrektur**: eliminiert Hardware-Bias, statistisch korrekte Z-Scores
- **Rejection Sampling**: bias-freie Lottozahlen-Ziehung ohne Modulo-Bias

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
docs/
  screenshot_laufend.png   — Web-UI während der Messung
  screenshot_ergebnis.png  — Web-UI mit Top-10 + Am-häufigsten-Ergebnis
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
| v1.4 | Buttons-Grid-Layout, Am-häufigsten-Zeile (Z>2), Abbruchtext, Checkmarks |
