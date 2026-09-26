# BITSCAN — Verwertbarkeit der höheren Differenz-Bits (Dunkel-Modus)

Messprotokoll: trägt Bit k (k=0..9) der 10-Bit-Framepaar-Differenz dieselben
GCP-relevanten Eigenschaften wie das LSB (Bit 0)? **Keine Design-Entscheidung, nur
Messung.** Die Größen sind dieselben, die der Sweep am LSB ohnehin auswertet
(`raw_bias`, `raw_sigma`, `raw_runs_z`, `autocorr_lag`), damit das Ergebnis direkt
gegen die bestehenden Gates und das LSB als Baseline vergleichbar ist.

## 1. Was „verwertbar für GCP“ heißt

`gcp_zscore_raw()` bildet pro Segment (224 Bit) den Binomial-z unter **iid
Bernoulli(p=0,5)**. Ein Bit k ist GCP-tauglich, wenn sein Strom in diesen Größen
wie das LSB (oder besser) abschneidet:

| Größe | GCP-Test | Messgröße |
|---|---|---|
| Bias `p̂ = ones/bits` | Fairness; p̂≉0,5 ⇒ Segment-z trägt Bias-Term, nur per Centring entfernbar | `raw_bias` |
| Dispersion `raw_sigma` | Mini-Run-z soll Einheits-Varianz; Überdispersion = Klumpung ⇒ falsche „Signale“ | `raw_sigma` (Selektions-Schlüssel) |
| Autokorrelation lag 1..4 | Unabhängigkeit (Binomial-null setzt sie voraus) | `autocorr_lag[1..4]` |
| Runs-Statistik | Unabhängigkeit, die der Monobit nicht sieht | `raw_runs_z` |

Eingefroren/ungenutzt: `p̂=0` oder `1` (bzw. `trans==0`).

Entscheidungsgröße: `raw_sigma` — **niedriger = besser** (`cal_key()`). „Verwertbar“
= Bit k besteht dieselben Gates wie das LSB und hat `raw_sigma` nicht schlechter als
das LSB **auf demselben Node, im selben Fenster**.

## 2. Messbedingungen (vorher verifizieren)

1. Dunkel-Modus bestätigt: `/diagjson?all=1` → `cam_sensor` = `IMX219`,
   `exposure` = 1600 (`IMX_DARK_EXPOSURE`), `gain` = 232 (`IMX219_GAIN_MAX`).
   Enclosure lichtdicht, LEDs aus.
2. `CAM_IMX_DARK` = 1 (`camera.c`). Sonst läuft die lit-Leiter und das Experiment
   misst Photonen statt Readout-Noise.
3. **Nicht während einer Session** (Diagnose-Modus ist dann ohnehin 409). Auf einem
   **Slave**; idealerweise auf allen Nodes, weil Bias/`raw_sigma` per Node
   nicht-stationär sind.
4. Settle: nach Boot/Exposure-Wechsel ~1 min warten (Bias wandert ~0,002). Der
   Dunkel-Rung bewegt sich nicht — trotzdem Fenster lang genug für Drift-Mittelung.

## 3. Bit-Abbildung (RAW10, IMX219) — exakt

Pixel ist 10 Bit: `value(p) = (byte[p] << 2) | ((byte4 >> (2p)) & 0x3)`
(p = 0..3; Bytes 0..3 = Bits 9..2, Byte 4 = Bits 1..0).

FPN-löschende Größe ist die **Differenz** zweier Frames:

```
d(p)  = (value_b(p) - value_a(p)) & 1023      // 10 Bit, mod 2^10
bit_k = (d(p) >> k) & 1                       // k = 0..9
```

⚠ Die Fast-Path-Identität `LSB(b−a) == LSB(a)^LSB(b)` gilt **nur für k=0**. Für k≥1
ist Bit k von `b−a` ≠ Bit k von `a XOR b` (Borrows). Der Monitor muss die **numerische
Differenz** rekonstruieren, nicht das XOR verallgemeinern. (Nur der Zero-Detect darf
weiter XOR verwenden: `d==0 ⟺ a==b`.)

Lit-OV5647-Kontrollknoten (RAW8): `d = (b−a) & 255`, k = 0..7.

## 4. Implementierung: bewaffneter Per-Bit-Monitor

Additiv, ändert den **emittierten Wortstrom nicht** → der bit-identische Self-Test
(`r10_equal` in `/camtest`) bleibt unberührt. Nur **bewaffnet** (wie `want_runs`),
damit er die Messrate im compute-gebundenen Loop nicht kostet.

Akkumulator (pro Bit k, integer-only, in Locals):
- `ones_k`, `bits` (gemeinsam) → `p̂_k`
- `trans_k`, `prev_k`, `have_prev_k` → Runs → `raw_runs_z_k`
- `run_ones_k`, `run_bits_k`, `mr_n_k`, `mr_sum_k`, `mr_sumsq_k` → `raw_sigma_k`
  (Mini-Run 3200 Bit, gleiche Reduktion wie `publish_stats()`)
- `ac_both1_k[4]`, `ac_pairs_k[4]` → `autocorr_lag_k[1..4]` (gleicher
  Pearson-Estimator wie `publish_stats()`, am Bitstrom)
- `frozen_k`

Einhängepunkt: `cam_bitscan_pair(a, b, n)` analog `diff_and_extract()`, rekonstruiert
pro Pixel die 10-Bit-Differenz und akkumuliert. Aufruf nur wenn `s_bitscan_on` gesetzt
(wie `s_raw_runs_on`), sonst ein Branch und raus.

Endpoint: **`GET /camtest?bitscan=<s>`** (s = 1..120, jeder Node, 409 in einer Session)
→ armiert für `s` Sekunden, sammelt, dearmiert, liefert JSON. Implementiert:
`cam_bitscan_pair()` in `extract.c`, Reduktion in `cam_bitscan_send()` in `camera.c`.
Pro Bit: `bias`, `sigma` (= raw_sigma), `runs_z`, `ac[4]`/`ac_z[4]` (r und r·√Paare),
`r0`/`z0` (Korrelation mit Bit 0 am selben Pixel — misst, ob Bit k neue Information
trägt), `frozen`. Kopf: `d_mean`/`d_sd` (vorzeichenbehaftete Differenz in DN),
`zero_diff`, `pairs`. Geplant war:

```
pro Bit k:  {bias, sigma, runs_z, ac1..4, frozen, bits}
Baseline:   raw_bias/raw_sigma/raw_runs_z/autocorr des LSB-Stroms (s_raw) im selben Fenster
Zustand:    sensor, gain, exposure, mean_px (leak), zero_diff_frac
```

⚠ **Bit 0 läuft im selben Akkumulator mit**, damit der Vergleich Bit-k-gegen-LSB aus
ein und demselben Code/Fenster stammt.

Self-Test: `/camtest` vorher/nachher — `r10_equal` muss `true` bleiben.

## 5. Stichprobe und Dauer

- 1 Frame-Paar IMX219 = 1640×1232 = 2,02 Mpx → 2,02 Mbit **pro Bit-Index** (alle
  10 Bits fallen aus denselben Pixeln an; der Monitor kostet keine zusätzliche Rate).
- Empfohlen **≥ `CAL_TARGET_BITS` = 8 Mbit pro Bit** → ~4 Frame-Paare. Für
  Drift-Mittelung das Fenster auf **2–10 s** strecken (Default-Sweep-Budget 10 s).
- Auflösung bei 8 Mbit: SE(bias)=1,77e-4 · SE(autocorr lag)≈3,5e-4 ·
  SE(raw_sigma)≈1,4 % (m≈2500 Mini-Runs) → eine 5 %-Differenz in `raw_sigma` ist
  auflösbar (dieselbe „3 SE ≈ 5 %“-Auflösung, die der Sweep selbst nennt).

## 6. Auswertung / Akzeptanzkriterien

Für jedes Bit k>0 gegen die **LSB-Baseline derselben Messung**:

| Kriterium | Schwelle | Quelle |
|---|---|---|
| `p̂_k` | `\|p̂−0,5\|` ≈ LSB (1e-3..7e-3). Flag bei `> 1e-2` | `sensor.c` LSB-Bias-Skala |
| `raw_sigma_k` | ≤ `raw_sigma_0`; relativ-Gate ≤ `CAL_RAW_SIGMA_K`(1,35)× Bestes | `cal_key()` / RSIG-Gate |
| `autocorr_k[1..4]` | `\|r\| < CAL_AUTOC_TOL` 0,03 | AUTOC-Gate |
| `raw_runs_z_k` | berichten; Flag bei `\|z\| > 5` | nicht gegatet, Indiz |
| `frozen_k` | muss 0 sein | — |

Entscheidungsschema:
- **Alle k>0** mit `\|p̂−0,5\| ≫` LSB oder frozen → höhere Bits tragen keine nutzbare
  Entropie; „LSB as measured“ ist für den Dunkel-Modus messbar bestätigt.
- **Mindestens ein k>0** mit p̂ ≈ 0,5 **und** `raw_sigma ≤ LSB` **und** sauberen
  autocorr/runs → GCP-tauglicher Kandidat. Dann (und erst dann) Folgefrage, ob er
  *besser* ist (niedrigeres `raw_sigma`) und ob der Wechsel lohnt — Design, keine Messung.
- Abschluss: bestes Bit k durch `gcp_zscore_raw()` schicken und prüfen, dass die
  z-Serie σ≈1 und lag-AC≈0 hat (die GCP-Null selbst).

## 7. Fallstricke

- ⚠ Keine Einzelmessung glauben. `raw_sigma` ist per Node auf Minuten-Skala
  nicht-stationär. Bits **back-to-back im selben Fenster auf demselben Node**
  vergleichen; einen „Gewinner“ in einem **zweiten Lauf reproduzieren**.
- ⚠ Nicht XOR für k≥1 (siehe §3).
- ⚠ Monitor nur bewaffnet, nie immer-on (Rekonstruktion der vollen 10-Bit-Differenz
  zieht sonst `ms_extract`).
- ⚠ Nie über Nodes oder Zeiten hinweg vergleichen — nur interner Bit-0-vs-Bit-k-
  Vergleich im selben Lauf ist sauber.
- ⚠ `r10_equal` nach jeder Änderung — Beweis, dass der Messpfad selbst unberührt ist.
