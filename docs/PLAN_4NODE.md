# PLAN: 4-Node GCP Array with OV5647 Camera Entropy

Status: **rescoped for actual hardware**. Only 2 ESP32-P4 boards exist today — master
(COM4) and slave (COM6), the same pair already running the TRNG-based system. An OV5647
camera is wired to the master's CSI connector and ready to bring up. The 2 additional
ESP32-P4-ETH boards + 3 more OV5647 cameras needed for the 4-node array have **not**
arrived. Phases 0–3 below target the 2-node system end-to-end (camera entropy on both
master and slave); the original 4-node scale-out is preserved as **Phase 4 (future,
blocked on hardware)** and stays unimplemented until those boards show up. Implementation
is phased; each phase is one focused coding session with clear acceptance criteria. Do not
start a phase before the previous one's gate passes.

## Goal

Replace the on-chip TRNG (~0.5 Mbit/s effective entropy, whitened, opaque) with **OV5647
dark-frame noise** (photon shot + read noise ≈ quantum-origin, raw, ~2–5 Mbit/s clean per
node) on both existing nodes, keeping the proven master/slave architecture at 2 nodes
(SNR ×√2, unchanged from today). All existing statistics machinery (studentization,
permuted order, Stouffer accumulation, coverage, independence checks) stays unchanged —
only the bit source changes. The combine/PairStats math is written generally (√n over n
healthy nodes) so it extends to 4 nodes later without rework, but n=2 is the only
configuration built and tested now.

## Architecture decisions (made, do not re-litigate)

- **Topology: UART point-to-point, unchanged.** Master (COM4) + slave (COM6) on UART1
  (GPIO14/15, 460800 baud) — the wiring already in place. The star topology for a 3rd/4th
  slave (UART2/3) is deferred to Phase 4; no GPIO-matrix routing work happens now.
- **One entropy source per node** (its own camera). Never share a noise source between
  nodes — it would break independence by construction.
- **Camera replaces TRNG behind the same interface.** `gcp_zscore_raw()` keeps its
  segment math; only the word source changes. TRNG register remains available as fallback
  and for A/B comparison in /diag.
- **Combine:** `z = Σ z_i / √n` over master + all healthy slaves (generalizes the current
  ÷√2; with 2 nodes today this is exactly ÷√2). Per-node health degradation exactly as
  today (drop node, adjust √n).
- **Run length becomes a config knob.** With honest (non-oversampled) bits, a run no longer
  needs 6.4 Mbit. Target ~0.5 s/run initially (≈1–2 Mbit/run, i.e. NUM_SEGMENTS becomes
  source-dependent) so Eurojackpot loops stay ~1.5 h. Statistical power per second is
  rate-limited either way; run length only sets granularity.

## Phase 0 — Camera bring-up + validation (master, COM4)

- Component: `espressif/esp_video` (ESP-IDF v6, P4 MIPI-CSI) + OV5647 sensor driver
  (`esp_cam_sensor`). Camera is already wired to the master's CSI connector.
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

**Status: PASSED** (2026-07-25, master/COM4, RAW8 800x800 @ 50fps, exposure 16, gain 1023,
XCLK unwired — the RPi-style OV5647 module clocks itself; SCCB on GPIO8/7):

| bias | σ | Mbit/s | lag-1..4 r | stuck | mean px |
|------|---|--------|------------|-------|---------|
| 0.499844 | 0.9975 | 3.419 | ≤ 0.0002 | 0 | 3.56/255 |

Three findings worth carrying forward:
- The raw (unfolded) LSB stream is biased 0.4849 — a ~3% excess of even diffs that a
  symmetric noise distribution cannot produce (only 12.6% of diffs are 0, so the noise
  spans ~2.7 ADU and quantization starvation is ruled out). Most likely ISP digital
  processing making the raw LSB non-uniform. XOR-folding masks it
  (`CONFIG_ELOTTO_CAM_XOR_FOLD`, default y); it does not remove the cause. Attacking the
  source (RAW10, or bypassing ISP digital gain) is open work — turn the fold off to A/B it.
- Even folded, the residual bias is ~3σ from 0.5 given 105.9 Mbit (SE ≈ 4.9e-5). Inside the
  gate by 6×, but not a clean 0.5.
- Throughput is dominated by PSRAM bandwidth, not the sensor: holding both frames of a pair
  dequeued instead of memcpy'ing one aside took the rate from 1.8 to 5.7 Mbit/s (3.2×).

## Phase 1 — Entropy abstraction (master)

- `noise_word()` interface behind `fast_rng()`'s call sites; runtime source select
  (camera / TRNG) + automatic fallback to TRNG with a status flag if the camera stalls.
- Blocking semantics: if the ring buffer underruns, the GCP task waits (vTaskDelay) — never
  reuse or fabricate bits.
- Make segments-per-run a per-source constant (TRNG: 32000 as today; camera: sized for
  ~0.5 s/run at measured rate).
- **Gate:** full 6/49 quick session (Loops=3, Runs=200, camera source) with per-run
  σ ≈ 1, clean pair_r vs the TRNG-based slave, no underruns at sustained rate.

**Status: PASSED** (2026-07-25, 6/49, Loops=3, Runs=200, `?src=1`, ~22 min, 5.34 Gbit):

| loop_sigma (3 loops) | pair_r (n=600) | sigma_m / sigma_s | stalls | fallback |
|----------------------|----------------|-------------------|--------|----------|
| 0.9982 / 0.9983 / 0.9917 | −0.0191 (\|r\|·√n = 0.47) | 1.0178 / 0.9935 | 0 | no |

- `?src=1` on /start selects camera, `?src=0` TRNG; /status reports `src` and
  `src_fallback`, /diag adds ring `drops`/`waits`/`stalls`.
- High `drops` (~6.6M words) are normal and not a defect: production (3.43 Mbit/s) and
  consumption (~3.2 Mbit/s in-run) are near-balanced, so the ring fills during inter-run
  gaps and surplus bits are discarded. Unread words are never clobbered, never reused.
  `waits` is likewise normal backpressure. Only `stalls` indicates a real underrun.
- The Fisher–Yates shuffle deliberately stays on the TRNG: measurement *order* is
  administrative randomness, not measured data, and must not consume rate-limited
  camera entropy.
- **Residual bias propagates to a per-run z offset of ≈ −0.33** (1.29e-4 bias × 200
  bits/segment × √8000 segments). It is a 19σ deviation at this sample size, not noise.
  Harmless here only because `studentize()` removes a constant per-loop offset exactly
  and the per-loop permutation prevents coherent accumulation. **Re-verify in Phase 3**:
  a 20 h session gives drift far more room than three loops, and drift is the one form
  this correction does not fully absorb.

## Phase 2 — Camera on slave (2-node parity)

- Slave firmware (repo `elotto_slave`): integrate the same camera pipeline (component is
  shared from the master repo via `EXTRA_COMPONENT_DIRS=../elotto/components` — both repos
  are siblings on disk; note this in both READMEs). Requires a 2nd OV5647 wired to the
  slave's (COM6) CSI connector.
- Both nodes now run camera-sourced entropy in parallel over the existing UART1 link — no
  new UART wiring, no `slaves[]` array yet (still exactly one slave).
- PairStats stays the single (master, slave) tracker it already is; verify pair_r ≈ 0 and
  sigma_m/sigma_s ≈ 1 with both sides on camera source.
- UI: stats line reflects camera source per node (reuse the existing master/slave σ row;
  no "N-node" badge needed until Phase 4).
- **Gate:** 2-node quick session, both nodes on camera; pair_r ≈ 0; combined σ ≈ 1; either
  side falling back to TRNG (camera stall) degrades gracefully with a UI flag, not a crash.

## Phase 3 — Long-run validation + docs

- 20 h Eurojackpot cumulative session on the 2-node camera system; verify significance line
  behaves (corrected p honest), σ stable across loops, no camera stalls.
- README: new "Camera entropy" section (physics, extraction, gates), updated wiring
  diagram/screenshots; CLAUDE.md concept sync; version bump.

## Phase 4 — 4-node scale-out (future, blocked on hardware)

Not started. Requires 2 more ESP32-P4-ETH boards and 3 more OV5647 cameras (see hardware
checklist below). Preserved here as the original design so it can be picked up without
re-deriving the architecture.

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

## Hardware checklist

Done today: 2× ESP32-P4-ETH (master COM4, slave COM6), 1× OV5647 wired to master.

Before Phase 2: 2nd OV5647 wired to the slave's CSI connector, light-tight capping for
both cameras (cap + tape + opaque box).

Before Phase 4 (not yet acquired):
1. 2 more ESP32-P4-ETH boards + 2 more OV5647 modules.
2. **CSI connector/cable match**: OV5647 modules usually ship RPi-style 15-pin 1.0 mm FFC;
   many P4 boards use 22-pin 0.5 mm — verify, order 15↔22 adapter cables if needed.
3. Confirm the ESP32-P4-ETH board exposes MIPI-CSI at all (else cameras go on whichever
   boards do, roles reassigned — slaves don't need Ethernet).
4. UART wiring: 2 more crossovers (masterTX→slaveRX, masterRX←slaveTX) + common GND star.
5. Power for 4 boards + cameras (USB per board is fine; avoid one weak hub).

## Workflow

Planning/architecture: Fable/Opus (this document is the contract). Implementation: Sonnet,
one phase per session, prompt: *"Implement Phase N of docs/PLAN_4NODE.md — everything you
need is in that file and CLAUDE.md."* Escalate back to Fable only if a phase gate fails
twice or an architectural decision is missing here. Commit at every green gate; master and
slave repos must be committed together when the shared component changes.
