# Where things stand

Moved out of CLAUDE.md on 2026-08-27: this is project HISTORY, not a rule.
CLAUDE.md says it itself — rules live there, evidence in [DECISIONS.md](DECISIONS.md).
Keeping a dated status snapshot in the always-loaded file cost every session ~1.200
tokens to carry a paragraph that is stale within the week.

## 2026-08-20

**The instrument is sound and every result so far is null**, which is what a working null instrument
produces.

**Hardware:** all four nodes lit and healthy at idle — bias within 1,4e-4 of 0,5, σ 0,996–1,003,
autocorr ≈ 0, 5,71 Mbit/s each. Settled light at exp 128: master 34,5 · slave0 27,9 · slave1 27,0 ·
slave2 19,5, stable to ±0,6 % over 18 min.

**Flashed 2026-08-20** — master `703e98b24897e10f`, all three slaves `3c53a5e1ff7adff0` (unchanged;
the slave sources did not move). The 08-19 checks still hold: all four certify a rung and land on
**exp 128/64/64/64**, none on 4 or 8; `clear_sig`, `quar`, per-node `soft`/`trip`/`mflag` in `/loops`;
`# fw_nodes=` in a fresh CSV.
⚠ The 2026-08-20 history rewrite renumbered every commit from `18074b5` on. **Match a pre-08-20 build
by ELF SHA, not by the version string.**
⚠ **Not exercised on hardware**: the `round_base` fix — compaction itself has now run, see below.
(`mflag` firing is no longer on this list: it fired in 3 of 35 blocks on 2026-08-26, on the master,
on a certified rung `[D11]`.)
⚠ Flash master and all three slaves **together**: `K` carries a field and `D` another. Both
mismatches degrade safely (legacy bias bar / no sha in the header), but a half-flashed array is not
one instrument.

**The 2026-08-20 session, and what it cost.** 8019 items, 18 rounds, 4 h 09 min, aborted at ~04:03
when all three slaves missed `NODE_MISS_LIMIT` inside the same ~50 s. Every node was healthy
afterwards; the FritzBox logged a DNS fault at 04:09 that this array cannot even use, since it
resolves no names. The master now records which side went quiet — `eth_up`, `eth_downs`,
`eth_lost_ips`, `drop_*` and `uptime_ms` in `/status` — so the next one is answerable without
reconstructing the abort time from slave camera counters.
⚠ **The session's own statistics are wrong** and it is not archived. Compaction fired for the first
time and `round_base` was taken from `items_done` where it had to come from `runs_completed`: the
round after the compaction wrote past the compacted array and the dropped rows were counted twice.
`pass_n_valid` 15806 for 8019 items. Fixed the same day `[D42]`.

**Spectral entropy went in 2026-08-25** (see the section above), flashed to all four and verified on
hardware the same day. Master `dcf44ad31c6e8e2d`, all three slaves `a0a5d77e03fcb942`.

What was checked: `/spectest` `worst_rel` 2,8e-06 and the null moments identical to the Python
reference at both window counts · `/camtest` still `equal:true`, so the z path is untouched · the key
reproduces by hand from the CSV columns (`(0,5·0,2771 − 0,5·(−0,8199))/√0,5 = 0,7757`, the published
value) · Top-5 ordered by the key with negative `zh_ctr` throughout · the optional wire field
degrading correctly, with one node deliberately left on the pre-FFT image writing an empty `h3` and a
`k_h` of 3 while still contributing its z · and the bit-rate control above.

**Still not exercised**: a long run with real 15-minute blocks, so the entropy channel has never been
BLOCK-CENTRED on more than a couple of blocks — every check so far ran at `?calint=0` on provisional
values. ⚠ Watch the per-node spread: in the first run the master sat at z_h ≈ −2,9 while the slaves
were near +1,7, which is the offset centring is there to remove and the reason it must be confirmed
over many blocks before any ranking is believed.

**Open, in the order I would pick them up:**
1. **Does calibration reduce the RATE of bad blocks?** The control pair only asked "does it add
   variance?" (no). The tail question needs a count of excursions over many blocks, not a mean.

**Closed 2026-08-26**, both on the 7354-item / 35-block session in
`docs/data/2026-08-26_aborted7354_rescue/`:
- **An offset DOES survive on a gated rung.** The master reported `cam_cal=1` in every block and
  still averaged **−0,4648** (min −4,141), with `mflag` in 3 blocks — at σ 0,9951, no trip, no
  soft-down. A pure location effect, which is the split `[D11]` rests on.
- **Centring verified over 35 blocks**, not just a short run: Σ`z_ctr`/√n = **−0,0000** for the whole
  session and max |Stouffer| 5,5e-5 per block. ⚠ On `z_raw` the same statistic is **−19,01**, of which
  the master alone is −39,8 — the raw cumulative Z measures the exposure rung, not an effect.

**Recently closed:** the master's block offsets `[D11]` · slave1's σ excess follows the board, not the
camera `[D15]` · why the last two extraction changes bought nothing `[D24]` · which side goes quiet
when nodes drop (`drop_*` in `/status`, 2026-08-20) · the `round_base` fix, verified on hardware in
the 08-20 Eurojackpot session at 48 rounds and `compacted` 15828: `pass_n_valid` == `completed`.
**Dropped and deferred:** see the last section of [docs/DECISIONS.md](docs/DECISIONS.md) — check it
before proposing anything that sounds obvious.

