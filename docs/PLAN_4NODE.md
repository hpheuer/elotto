# PLAN: 4-Node GCP Array with OV5647 Camera Entropy

Status: **planning approved, hardware arriving** (4× OV5647, 2× additional ESP32-P4-ETH →
4 identical units). Implementation is phased; each phase is one focused coding session with
clear acceptance criteria. Do not start a phase before the previous one's gate passes.

## Goal

Replace the on-chip TRNG (~0.5 Mbit/s effective entropy, whitened, opaque) with **OV5647
dark-frame noise** (photon shot + read noise ≈ quantum-origin, raw, ~2–5 Mbit/s clean per
node) on all 4 nodes, and scale the proven master/slave architecture from 2 to 4 nodes
(SNR ×2 from √4). All existing statistics machinery (studentization, permuted order,
Stouffer accumulation, coverage, independence checks) stays unchanged — only the bit source
and the node count change.

## Architecture decisions (made, do not re-litigate)

- **Topology: UART star.** Master + 3 slaves on UART1/2/3 (GPIO-matrix routable). The
  Ethernet-mesh alternative (all nodes have RJ45) is deferred — UART is proven,
  deterministic, needs no discovery. Existing ASCII protocol `P/B<n>/M/A` unchanged, one
  instance per slave UART.
- **One entropy source per node** (its own camera). Never share a noise source between
  nodes — it would break independence by construction.
- **Camera replaces TRNG behind the same interface.** `gcp_zscore_raw()` keeps its
  segment math; only the word source changes. TRNG register remains available as fallback
  and for A/B comparison in /diag.
- **Combine:** `z = Σ z_i / √n` over master + all healthy slaves (generalizes the current
  ÷√2). Per-node health degradation exactly as today (drop node, adjust √n).
- **Run length becomes a config knob.** With honest (non-oversampled) bits, a run no longer
  needs 6.4 Mbit. Target ~0.5 s/run initially (≈1–2 Mbit/run, i.e. NUM_SEGMENTS becomes
  source-dependent) so Eurojackpot loops stay ~1.5 h. Statistical power per second is
  rate-limited either way; run length only sets granularity.

## Phase 0 — Camera bring-up + validation (one board, master)

- Component: `espressif/esp_video` (ESP-IDF v6, P4 MIPI-CSI) + OV5647 sensor driver
  (`esp_cam_sensor`). Check the Waveshare ESP32-P4-ETH CSI connector pinout/FFC.
- Sensor config: RAW8 (or RAW10→8), 640×480 or 800×640 @ max stable fps, **AE/AGC/AWB
  off**, fixed max analog gain, short fixed exposure. Lens capped + taped, opaque housing.
- Extraction pipeline (camera task → ring buffer):
  - Non-overlapping frame pairs: diff = f[2k+1] − f[2k] per pixel (cancels FPN exactly).
  - Take LSB of each diff, pack 32 bits → uint32 words into a ring buffer (≥ 64 KB).
  - Optional XOR-fold (bit ⊕ next-bit, halves rate) — only if autocorrelation gate fails.
- Extend `/diag` with per-source stats (TRNG vs camera): bit bias, per-run σ over ≥200
  mini-runs, lag-1..4 word autocorrelation, sustained Mbit/s, stuck-frame counter, and a
  **light-leak check** (mean raw pixel level must sit at the black floor; warn above
  threshold).
- **Gate:** |bias−0.5| < 1e−3, |lag-1 autocorr| < 0.01, σ within 1±0.05, no stuck frames,
  light-leak pass, sustained ≥ 2 Mbit/s.

## Phase 1 — Entropy abstraction (master)

- `noise_word()` interface behind `fast_rng()`'s call sites; runtime source select
  (camera / TRNG) + automatic fallback to TRNG with a status flag if the camera stalls.
- Blocking semantics: if the ring buffer underruns, the GCP task waits (vTaskDelay) — never
  reuse or fabricate bits.
- Make segments-per-run a per-source constant (TRNG: 32000 as today; camera: sized for
  ~0.5 s/run at measured rate).
- **Gate:** full 6/49 quick session (Loops=3, Runs=200, camera source) with per-run
  σ ≈ 1, clean pair_r vs the TRNG-based slave, no underruns at sustained rate.

## Phase 2 — 4-node scale-out

- Slave firmware (repo `elotto_slave`): integrate the same camera pipeline (component is
  shared from the master repo via `EXTRA_COMPONENT_DIRS=../elotto/components` — both repos
  are siblings on disk; note this in both READMEs).
- Master: `slaves[]` array {uart_num, tx, rx, ok}; UART2/3 pins chosen from free header
  GPIOs (verify against Waveshare pinout; avoid 14/15 UART1, 31/51/52 ETH MDC/MDIO/RST,
  RMII-fixed pins, 37/38 console). Broadcast `M` to all healthy slaves **before** the local
  measurement, collect replies after, per-slave timeouts as today.
- Generalize PairStats: one (master, slave_i) pair-tracker per slave; publish per-node σ
  and max |r| (flag ⚠ if any |r|·√n > 3).
- UI: badge "N-node • SNR ×√N", per-node health/σ row in the stats line.
- Baseline `B` broadcast + wait-all; abort `A` broadcast (already per-UART).
- **Gate:** 4-node quick session; all pairwise r ≈ 0; combined σ ≈ 1; a node unplugged
  mid-run degrades gracefully to √3 with a UI flag.

## Phase 3 — Long-run validation + docs

- 20 h Eurojackpot cumulative session on 4 nodes; verify significance line behaves
  (corrected p honest), σ stable across loops, no camera stalls.
- README: new "Camera entropy" section (physics, extraction, gates), 4-node wiring diagram,
  updated screenshots; CLAUDE.md concept sync; version bump.

## Hardware checklist (day 1, before any code)

1. **CSI connector/cable match**: OV5647 modules usually ship RPi-style 15-pin 1.0 mm FFC;
   many P4 boards use 22-pin 0.5 mm — verify, order 15↔22 adapter cables if needed.
2. Confirm the ESP32-P4-ETH board exposes MIPI-CSI at all (else cameras go on whichever
   boards do, roles reassigned — slaves don't need Ethernet).
3. Light-tight capping for all 4 cameras (cap + tape + opaque box).
4. UART wiring: 3 crossovers (masterTX→slaveRX, masterRX←slaveTX) + common GND star.
5. Power for 4 boards + cameras (USB per board is fine; avoid one weak hub).

## Workflow

Planning/architecture: Fable/Opus (this document is the contract). Implementation: Sonnet,
one phase per session, prompt: *"Implement Phase N of docs/PLAN_4NODE.md — everything you
need is in that file and CLAUDE.md."* Escalate back to Fable only if a phase gate fails
twice or an architectural decision is missing here. Commit at every green gate; master and
slave repos must be committed together when the shared component changes.
