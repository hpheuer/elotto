#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "camera.h"
#include "elotto_link.h"

/* v3.0 (PLAN.md §2): ONE pass, every combination measured exactly ONCE.
 * results[] holds the pass in MEASUREMENT order, so NUM_RUNS is the hard cap
 * on the combination space. 8000 and not more because results[] lives in
 * internal RAM, which is full — a few KB more of .bss fails the LINK, not the
 * run. Both pools below fit under it by construction. */
#define NUM_RUNS      8000
#define TOP_N            5
/* ── Round-boundary compaction (2026-08-19) ───────────────────────────────
 * How many items per published table survive a compaction. The three tables
 * publish TOP_N = 5 each; this keeps more, because 5 is the exact requirement
 * only under assumptions that a real session can break.
 *
 * Top-N and Bottom-N are EXACT at any K >= TOP_N, and not empirically: the
 * maximum of a union is the maximum of the per-part maxima, so keeping each
 * round's best 5 keeps the session's best 5, whatever the rounds contain. The
 * only precondition is that the values are final when the part is closed, which
 * at a round boundary they are — every block of the round has closed and
 * center_block() has replaced every provisional z_ctr.
 *
 * Nearest-zero is the one that is not exact by construction, because its target
 * (the pass mean) keeps moving as later rounds arrive. In practice it barely
 * moves at all -- centring forces each closed block to mean ~0, so the pass mean
 * sat at +-0,00000 after every one of the 18 rounds of the 2026-08-19 session,
 * and the final nearest-5 were ranked 1,1,1,2,1 within their own rounds. Three
 * places of headroom at K = 5. The margin here costs 11 * 3 * 40 B = 1,3 KB per
 * compaction and covers the cases that headroom does not: a round whose last
 * block is still open, a quarantined block removing several survivors at once,
 * and any future change that lets the pass mean drift. */
#define PASS_KEEP_PER_TABLE 16
#define POOL_MAIN_49    15   // C(15,6) = 5005 combinations
#define POOL_MAIN_50    12   // C(12,5) =  792 combinations
#define POOL_EURO_12     5   // C(5,2)  =   10 combinations
// Eurojackpot: C(12,5)·C(5,2) = 7920 — the largest configuration under the
// ~10000 the user set as the ceiling (13+5 would be 12870). 6-of-49: 5005.

/* ── Session parameters: ONE definition each ──────────────────────────────
 *
 * Every value below used to exist two or three times over: once as an HTML
 * input attribute, once as a JavaScript clamp, and once as the C validator in
 * the /start handler — three copies that nothing forced to agree. They had
 * already drifted apart: the baseline default was 50 in the form and 100 in the
 * handler (so a curl session ran a different experiment from a browser one),
 * and the calibration ETA fell back to 30000 ms for a whole build after the
 * budget default became 10000.
 *
 * The page is a C string literal, so the UI copies are made to reference these
 * through EL_STR() rather than being written out again. A limit changed here
 * now changes the form field, the clamp and the validator together.
 *
 * ⚠ Defaults only. Every one is overridable per session on /start, and the
 * matched-control sessions in PLAN.md pass their values explicitly — so
 * changing a default here does NOT retroactively describe an archived run. */
#define BASELINE_DEFAULT        10       // drift-reference runs per block insertion
#define BASELINE_MIN            10
#define BASELINE_MAX          5000
#define BASELINE_STEP           10
#define CAL_BUDGET_DEFAULT_MS 10000      // exposure-sweep CAP, split over 9 rungs
#define CAL_BUDGET_MAX_MS   120000
/* v3: the interval is the BLOCK length. Every cal_interval_ms the pass parks,
 * runs sweep + baseline together, and the boundary closes a block — the unit
 * that inherited everything that used to be per-loop (drift point, pairwise
 * fold, /loops row). 15 min ≈ 205 items/block on the 4.4 s cycle, ~6 %
 * overhead, ~38 blocks over a full Eurojackpot pass — comfortably past
 * DRIFT_MIN_LOOPS. 0 = NO mid-pass insertions (sweep + baseline at session
 * start only): with no loops left, "every loop" has nothing to mean, and the
 * zero control that still matters is "no re-tuning mid-pass". */
#define CAL_INTERVAL_DEFAULT_MS  900000
#define CAL_INTERVAL_MAX_MS     3600000
/* Per-run window (one continuous attention window per focus element) and the
 * intentional blank after it. Both are session parameters on /start (?run= &
 * ?gap=), defaults match the 2026-08-02 live cal. Gap defaults to 40 % of the
 * window (5 s → 2 s, 7 s → 2.8 s ≈ 3 s) so duty stays ~70 % of the intentional
 * cycle; measured focus_gap_ms is larger because it also includes slave collect.
 * Segment count is derived from run_s via the segs↔ms cal — wall time can stretch
 * if the camera rate collapses at long windows (that IS the limit). */
/* ⚠ 1..5 s, FIXED (user, 2026-08-18): the measurement window is never shorter
 * than a second and never longer than five, so nothing outside that should be
 * reachable — not in the form, not on the query string. RUN_S_MAX was 15, which
 * after the 08-18 recalibration could no longer be delivered at all: 15 s asks
 * for 391304 segments against a wire limit of 200000, and the request came back
 * as a silent 7,7 s window. Capping the input removes that failure mode by
 * construction rather than reporting it. */
#define RUN_S_DEFAULT            5
/* Cap on the per-item ring flush (sensor.c onset_settle). A fresh pair costs
 * ~56 ms idle and ~85 ms under load, so the wait is normally well under this;
 * the cap only bounds the damage if the camera has stopped delivering. */
#define ONSET_SETTLE_MS        500

#define RUN_S_MIN                1
#define RUN_S_MAX                5
#define GAP_S_MIN              0.5
#define GAP_S_MAX               10
/* Live 4-node cal. ⚠ THIS PAIR IS A MEASUREMENT, and it must be re-measured
 * after anything that changes the extraction rate — otherwise the window the
 * operator asks for and the window the observer actually gets drift apart
 * silently, which is the one thing the Focus protocol cannot tolerate.
 *
 *   2026-08-02: 66000 segs ↔ 4680 ms  (extraction at ~3,4 Mbit/s)
 *   2026-08-18: 70513 segs ↔ 2703 ms  (word-wise extraction, ~5,7 Mbit/s)
 *
 * The 08-18 pair was measured the same way as the first: a live 4-node session
 * at ?run=5, reading focus_win_ms off /status after a dozen runs. At the new
 * rate a 5 s window buys ~130500 segments instead of ~70500 — 1,85× the bits
 * per item, i.e. √1,85 ≈ 1,36× the sensitivity at the SAME wall time.
 *
 * ⚠ Sessions before and after 2026-08-18 are not the same instrument: same
 * nominal ?run=, different bit count per item. Do not pool them. */
#define RUN_SEGS_REF         70513
#define RUN_MS_REF            2703
/* The wire caps the segment count at EL_SEG_MAX, and a receiver does NOT clamp
 * an out-of-range value — it falls back to its own default, which would put the
 * nodes on different window lengths without anything looking wrong. The master
 * must therefore never ASK for more than the wire allows. With RUN_S_MAX at 5 s
 * that holds with room to spare (130435 of 200000), and the assertion below
 * makes the compiler re-check it after any future recalibration instead of
 * leaving it to whoever edits RUN_MS_REF next. */
_Static_assert(((long long)RUN_S_MAX * 1000 * RUN_SEGS_REF) / RUN_MS_REF <= EL_SEG_MAX,
               "RUN_S_MAX x the segs<->ms calibration exceeds the wire's EL_SEG_MAX: "
               "the longest window the UI offers cannot be delivered. Lower "
               "RUN_S_MAX, or raise EL_SEG_MAX in BOTH firmwares and fix "
               "seg_from_cmd() to reject rather than silently substitute.");

/* ── Wall time of ONE measured item, for the pre-start estimate ────────
 * The UI has to answer "how long will a round take?" BEFORE anything runs, so
 * it needs a model; once a session is live, /status carries the measured pace
 * and the ETA comes from that instead.
 *
 * The model was `cycle_ms ~= 1,36 * run_ms + gap_ms`, fitted to two live 4-node
 * points. It broke on 2026-08-19, measuring 4,43 s at run=2/gap=0,8 where it
 * predicted 3,52 — 21 % short. The fit was not merely stale: a percentage OF
 * THE REQUESTED WINDOW is the wrong shape. What a run costs is the time the
 * SLOWEST node needs to produce its bits, and the bits are set by the segment
 * count while the rate is a property of the hardware — so the two must appear
 * separately or a change to either silently invalidates the constant.
 *
 *   run_bits  = GCP_SEGMENT_BITS * segments      (segments from run_s)
 *   cycle_ms ~= run_bits / rate + CYCLE_FIXED_MS + gap_ms
 *
 * CYCLE_LOAD_MBIT_X100 is the per-node rate UNDER LOAD, which is NOT the idle
 * 5,71: during a session the GCP consumer outranks the extraction task and a
 * frame pair costs ~70 ms instead of 39,5 (see CLAUDE.md). Measured 2026-08-19,
 * all three slaves in agreement at 3,66-3,68; the master is not the constraint
 * (it runs at the idle rate and waits ~1 s per run for the slaves), which is
 * exactly why the model must use the SLOWEST node and not an average.
 *
 * CYCLE_FIXED_MS is what is left over per run and does not scale with the
 * window: the ring flush, the trigger, the reply collect and the publish.
 * Solved from the 08-19 point — 4433 measured, 800 requested gap, 52174
 * segments at 3,66 Mbit/s = 2851 ms of bits, leaving 782.
 *
 * Cross-check against the OTHER instrument generation, which the old constant
 * could not fit at all: at run=5/gap=2 and the ~3,48 Mbit/s the slower nodes
 * ran at before 2026-08-18, this gives 7,50 + 0,78 + 2,00 = 10,3 s against a
 * measured ~10,6-10,8. The form of the model is what carries across; only the
 * rate moved.
 *
 * ⚠ Still an ESTIMATE, and the live UI prefers the measured pace wherever it
 * has one — including the slowest node's own cam_mbit from /status, which makes
 * the constant a cold-start value rather than the answer. */
#define CYCLE_LOAD_MBIT_X100   366   // per-node extraction rate under load, x100
#define CYCLE_FIXED_MS         780   // per-run overhead that does not scale

/* ── Unlimited mode (user, 2026-08-18) ────────────────────────────────────
 * A session that does not end with the combination space. Instead of measuring
 * ONE pool exhaustively, the pass runs in ROUNDS: score every number, keep only
 * as many of the best as fit `runs_cap` measurement runs, measure that whole
 * (smaller) space once, then score again and start the next round. It stops on
 * Abort, or when results[] is full.
 *
 * ⚠ This relaxes the v3 core rule. Inside a round every combination is still
 * measured exactly once; ACROSS rounds a combination can recur, because a later
 * round's scoring may pick overlapping numbers. Each recurrence is a separate
 * measurement and gets its own row — nothing is averaged or overwritten — so
 * `round` is part of a row's identity and `index` (the combination id) is only
 * meaningful WITHIN a round: the pool it enumerates changes every round.
 * ⚠ Never pool unlimited-mode data with a single-pass session without splitting
 * on the `round` column first. */
#define UNLIM_RUNS_DEFAULT     100   // measurement runs per round
#define UNLIM_RUNS_MIN          10
#define UNLIM_RUNS_MAX    NUM_RUNS
#define UNLIM_RUNS_STEP         10
/* ⚠ The pool split is NOT a free choice, and a weighted one was tried and
 * withdrawn (2026-08-18, same day). The probability that a round's pool even
 * CONTAINS the real draw is
 *
 *     P = C(p,5)/C(50,5) · C(q,2)/C(12,2) = [C(p,5)·C(q,2)] / 139838160
 *       = (combinations measured) / (combinations that exist)
 *
 * — the split cancels completely, leaving only the product, and the product IS
 * the run count. So a pool rule is neutral exactly when it SPENDS THE BUDGET,
 * and harmful exactly in proportion to the runs it leaves unspent. The first
 * attempt maximised universe coverage p/50 + q/12 (a bonus number weighted ~4×
 * because there are only 12 of them); it pinned q at 5 and measured 210 of a
 * 500-run budget where 462 were available — a 2.2× loss of coverage, and a
 * shorter round, so proportionally MORE scoring overhead per measured item.
 *
 * The rule is therefore: maximise the combinations measured. The bonus-number
 * preference survives only as the TIE-BREAK, where P is identical and it is
 * free. No weights — see unlimited_pool_sizes(). */

/* Diagnostic thresholds that appear in more than one place. */
#define PAIR_FLAG_T            3.0   // |r|·√n above this = nodes not independent
#define DRIFT_FLAG_T           3.0   // |drift_t| above this = real cross-block drift
/* Pass-level null gates (GCP-first). Ranking is secondary while any of these
 * fire: the published Stouffer z is only N(0,1) under unit variance and no
 * drift. Soft node downweight trips on block σ alone; the |mean| wire was
 * removed on 2026-08-19 once block centring made it redundant and the exposure
 * ladder was found to be generating the offsets it fired on — see
 * NODE_MEAN_REPORT for the whole argument, including why the 2026-08-11 pass
 * (master mean −7, .145 +4.6, σ≈1) is not a counter-example any more. */
#define PASS_SIGMA_LO          0.85  // pass sample σ below this → null broken
#define PASS_SIGMA_HI          1.15  // pass sample σ above this → null broken
#define PASS_NULL_MIN_N       30     // need this many valid items before σ gate
#define NODE_SIGMA_SOFT        1.25  // block per-node σ above this → soft-exclude
/* ── |block mean| REPORTS, it no longer excludes (2026-08-19) ──────────────
 *
 * This was a trip wire at the same value until the 2026-08-19 session, where it
 * put the master soft-down for half the run and the array measured 50 % of its
 * items at k < 4 — the last 3,4 h at k = 2 — with null_flags 0, fault empty and
 * `ok` true throughout. Three findings, in the order they matter:
 *
 * 1. The offsets it fired on are made by the CALIBRATION SWEEP, not by the node.
 *    Per-block offset against the rung chosen for that block: exp 4 → −1,86,
 *    exp 8 → −0,78, exp 16..128 → −0,05…+0,09. exp ≤ 8 averages −1,11 over 10
 *    node-blocks against −0,03 over 106, t = −4,0. Five of the master's six
 *    excursions sat on exposure 4 or 8. CAL_MIN_MEAN_PX / CAL_MAX_ZERO_DIFF
 *    remove those rungs at the source; this bar was punishing the symptom.
 * 2. CENTRING ALREADY REMOVES IT. center_block() subtracts each node's own mean
 *    over the block and everything that is ranked reads z_ctr through rank_z().
 *    A constant offset inside a block is gone from every number a result is
 *    drawn from, so excluding the arm buys nothing and costs √k: keeping a
 *    +0,5-offset arm costs 0 after centring, dropping it costs 13 %.
 *    ⚠ This is precisely why the σ-only rule was WRONG on 2026-08-11 (master
 *    mean −7, .145 +4,6, σ ≈ 1, all kept) and is right now: that pass had no
 *    block centring. Do not read the 08-11 argument as still standing.
 * 3. The bar was not run-length invariant. A fixed 1,50 in z is a bias of
 *    2,3e-4 at run=2 and 1,5e-4 at run=5, so the same camera tripped or passed
 *    depending on ?run= (see gcp_z_per_bias).
 *
 * What survives: the value, as a REPORTING threshold. An arm over it is flagged
 * in the block's /loops row and printed, because a big offset is still worth
 * seeing — it is how the exposure story above was found. It does not exclude,
 * it does not quarantine, and it does not block a clear.
 * ⚠ σ remains a trip wire and must: σ is what the 08-13 pass failed on, it is
 * invariant to the run length by construction, and centring does NOT fix it. */
#define NODE_MEAN_REPORT       1.50  // |block mean z| above this → flagged, not excluded
#define NODE_SOFT_MIN_N       20     // min runs in the block before soft-exclude
/* ── Clearing a soft-down: bars RELATIVE to the peers in the same block ────
 *
 * These were fixed constants until 2026-08-19, and the fixed value was the
 * bug. Measured over a 6,5 h four-node session: the per-block σ of a HEALTHY
 * arm on this rig sits at 1,02–1,05, so `σ ≤ 1,05` was not a health threshold,
 * it was the array's own median. Meeting it is a coin flip per block and
 * meeting it four times running is ~6–15 %, so a node that tripped once was
 * down for the rest of the session whatever its condition: slave1 tripped on a
 * single block (σ 1,290) 40 min in and was still excluded 5,9 h later, having
 * reached 3 of the required 4 at best, with a running z of +0,0025 — i.e. it
 * looked healthy the whole time. 90 % of that session combined over √3.
 *
 * So the bar is now the peers' own median for that block, times a factor. That
 * is real hysteresis (trip and clear stay far apart) AND it makes a common-mode
 * bad block stop punishing the node that is down — every arm is noisy in the
 * same block, so the reference moves with them.
 *
 * The absolute constants survive as FLOORS: the dynamic bar may never be
 * STRICTER than the old fixed one. And it is capped below the TRIP bar, or a
 * block that would trip the node could also count as clean, which is incoherent.
 * ⚠ Peers = ok, produced stats this block, not tripping this block, not
 * soft-down. With no peers left, the floors apply and the behaviour is exactly
 * what it was before. */
#define NODE_SOFT_CLEAR_SIG    1.10  // floor: σ bar is never tighter
#define NODE_SOFT_CLEAR_SIG_K  1.15  // σ bar = K x peer median σ
#define NODE_SOFT_CLEAR_MARGIN 0.95  // cap, as a fraction of the TRIP bar
/* ⚠ There is no |mean| clear bar any more, and removing it was NOT tidying: a
 * criterion that cannot trip a node must not be able to keep it down either.
 * The pair that stood here (floor 0,50, K 2,00) was the second instance of the
 * defect the σ floor already had — the peer median |mean| runs 0,02–0,30, so the
 * floor bound in almost every block, and slave2 broke its streak three times on
 * |mean| 0,52 / 0,65 / 0,74 while its σ never left 0,93–1,06. Replayed over the
 * 29 blocks of 2026-08-19 with σ alone: slave2 clears 4 blocks after tripping
 * instead of never, and the master — which under the old rule was down for 14 of
 * 29 blocks — never trips at all (its σ peaked at 1,109 against the 1,25 bar). */

/* ⚠ The σ FLOOR is 1,10 and NOT the old 1,05. Keeping the old value as a floor
 * was tried first and changed nothing at all — replayed over the same 29 blocks
 * it still left slave1 down for 27 of them, because 1,05 IS the value that was
 * too tight and a floor pins the bar exactly there. Replay before believing a
 * threshold change: at floor 1,10 slave1 clears after 11 blocks, at 1,10 with
 * K 1,15 after 5, and no other node is ever put down by any of them.
 *
 * Why tolerating a slightly hot arm is right: measured per-block σ over those
 * 29 blocks was master 0,999 · slave1 1,040 · slave0 1,010 · slave2 0,996, so
 * slave1 really does run ~4 % hot. Keeping it inflates the COMBINED σ by
 * sqrt(3·1,00² + 1,04²)/2 = 1,010, i.e. 1 %. Dropping it costs √3 against √4,
 * i.e. 13 % of the sensitivity. A 1 % cost against a 13 % one is not a close
 * call, and the trip bar at 1,25 still removes an arm that is genuinely bad. */
#define NODE_SOFT_CLEAR_BLOCKS 4     // consecutive clean blocks required to lift soft-down
/* Never soft-exclude below this many live nodes. ⚠ 1, not 3 (2026-08-13): at
 * four nodes a floor of 3 allowed exactly ONE exclusion, so when two nodes
 * misbehaved at once the second stayed in the combine. The 08-13 full pass is
 * the proof — .145 was excluded, the master (block means down to −6.33) was
 * kept, and blocks 4/14/33 landed in the results at −3.4 with the master's
 * offset intact: (−6.33 + 0.27 + 0.22)/√3 = −3.38, exactly the published value.
 * A bad arm in the combine costs more than a small k does (user, 2026-08-13). */
#define NODE_SOFT_MIN_COMBINE  1     // never soft-exclude below this many live nodes
/* null_flags bits (also published in /status). */
#define NULL_FLAG_SIGMA        0x01  // pass_σ outside [PASS_SIGMA_LO, PASS_SIGMA_HI]
#define NULL_FLAG_DRIFT        0x02  // |drift_t| > DRIFT_FLAG_T
#define NULL_FLAG_CHI2         0x04  // Σz²/n far from 1 (same band as σ, large n)
#define NULL_FLAG_PAIR         0x08  // worst |r|·√n > PAIR_FLAG_T

/* Phase-0 scoring direction (pre-registered). Only affects WHICH numbers enter
 * the pool — never the Phase-2 measurement statistics. Default HIGH matches
 * the historical "largest positive z" rule. */
typedef enum {
    SCORE_DIR_HIGH = 0,   // pick largest raw z
    SCORE_DIR_LOW  = 1,   // pick smallest raw z
    SCORE_DIR_ABS  = 2,   // pick largest |z|
} ScoreDir;

/* Stringify, so the HTML/JS copies of the numbers above are the SAME token the
 * C code compiles. Two levels are required: the inner one would otherwise
 * stringify the macro's name instead of its value. */
#define EL_STR2(x) #x
#define EL_STR(x)  EL_STR2(x)

typedef enum { MODE_EUROJACKPOT = 0, MODE_LOTTO_649 = 1 } ElottoMode;
typedef enum { ELOTTO_IDLE, ELOTTO_RUNNING, ELOTTO_DONE, ELOTTO_ABORTED } ElottoState;
// PHASE_CALIBRATE is appended, not inserted: it runs FIRST in a loop but the
// other three are wired into the UI and the CSV by value.
typedef enum { PHASE_SCORING, PHASE_BASELINE, PHASE_MEASURING,
               PHASE_CALIBRATE, PHASE_POOL_CONFIRM, PHASE_READY } ElottoPhase;

/* PHASE_READY — the observer gate, loop 0 only.
 *
 * Calibration and baseline take ~2 minutes during which nothing is on screen
 * for the operator and nothing is being attended to. Scoring is where the
 * protocol actually begins: it is the first phase whose bits are collected
 * while a target is displayed. Starting it the instant the baseline ends means
 * the first candidate numbers are measured while the observer is still reading
 * the screen and settling — the opposite of the intent.
 *
 * So the session parks here and waits for the operator to press Start. Opt-in
 * on the same `confirm=1` flag as the pool prompt, for the same reason: a
 * device that blocks on a human would hang every scripted or unattended run.
 * A timeout is deliberately NOT applied — unlike the pool proposal there is no
 * sensible default action, and an unattended session never reaches this gate. */
void elotto_ready_go(void);      // the operator pressed Start

/* What the operator did with the proposed pool. Written by the /pool handler,
 * read and cleared by elotto_task — one word, so no lock is needed. */
typedef enum { POOL_WAIT = 0, POOL_ACCEPT, POOL_MORE, POOL_CANCEL } PoolAction;

/* Hand the operator's answer to the waiting session. `n_main`/`n_euro` are the
 * numbers still CHECKED; for POOL_MORE they are the ones to keep and omit from
 * the re-scoring pass. Returns false if no session is waiting. */
bool elotto_pool_reply(PoolAction act,
                       const uint8_t *main_sel, int n_main,
                       const uint8_t *euro_sel, int n_euro);
/* v3.0: NO ranking modes any more (user decision, 2026-08-02, PLAN.md §2).
 * With every combination measured exactly once there is nothing for the four
 * old rules to differ ABOUT — each item's published Z is its own single raw
 * measurement, stored in results[] untouched: no rewrite, no baseline
 * subtraction and no cross-item normalization. */

/* ENTROPY IS PHOTONS, AND ONLY PHOTONS (user decision, 2026-07-26).
 *
 * The on-chip TRNG is gone from this firmware — not deselected, removed. Every
 * measured bit comes from OV5647 dark-frame shot noise. The reason is the GCP
 * methodology itself: the claim under test is about a *physical* random source,
 * and a whitened hardware RNG is an opaque digital post-process whose output
 * would be indistinguishable from the real thing in every statistic this
 * project computes. Keeping it available as an A/B option meant the codebase
 * could always, in principle, produce a result nobody could attribute.
 *
 * There is therefore no fallback. A node whose camera stops delivering has
 * stopped being an instrument: it is reported as a fault and REBOOTED, never
 * quietly switched to another source. */

typedef struct {
    int        index;      // combination id (1-based slot in the enumeration)
    double     z_score;    // RAW combined Stouffer z (Σz_i/√k) — never rewritten
    /* BLOCK-CENTRED combine: Σ(z_i − m_i,block)/√k over the same nodes that
     * entered z_score, where m_i,block is node i's own mean over this block.
     * This is what the ranking, pass mean/σ and Bonferroni run on (2026-08-13).
     *
     * Why: the 08-13 pass showed per-node σ ≈ 1.0 INSIDE every block while the
     * per-block offsets jumped (master −6.33, .145 +24.13 in single blocks).
     * The noise was fine; the zero point moved. Centring removes exactly that
     * and left the pass at σ 0.995 with max|z| 3.92 against the 4.13 expected
     * for 5005 draws — a clean null instead of a table of block artefacts.
     *
     * ⚠ It also removes any real effect that is CONSTANT across a whole block,
     * which is a pre-registration decision, not a detail: what this instrument
     * can still see is an effect that varies BETWEEN items inside a block.
     * z_score stays raw and untouched, so the uncentred view survives in the
     * CSV forever. Provisional (= z_score) until the block closes. */
    float      z_ctr;
    uint16_t   block;      // which block this item was measured in (v3)
    /* Which ROUND measured it (unlimited mode; 1 for an ordinary single-pass
     * session). The pool is re-scored every round, so `index` enumerates a
     * DIFFERENT combination space per round — the pair (round, index) is the
     * identity, and nums[]/euro[] are what a reader should actually key on. */
    uint16_t   round;
    uint8_t    k;          // nodes that entered the combine; 0 = VOID (incomplete)
    uint8_t    have_mask;  // bit i set ⇒ node i contributed (master = bit 0)
    uint8_t    skip_rank;  // 1 = exclude from pass mean/σ/Top-Bottom (trigger block)
    uint8_t    nums[6];
    uint8_t    euro[2];
} RunResult;

// Focus display (docs/PLAN_4NODE.md Phase 5): what is on screen right now, for
// exactly the window its bits are collected in. The observer is meant to be
// present while the noise is sampled — the original GCP/PEAR protocol — so the
// one property that must hold is `active` ⟺ a run is sampling.
typedef enum { FOCUS_NONE = 0, FOCUS_NUMBER = 1, FOCUS_DRAW = 2 } FocusKind;

// Written by elotto_task, read by the /focus handler on the HTTP task. Not
// locked: `seq` is bumped AFTER the numbers are stored and the reader re-reads
// it, so a torn read is detected rather than served (see focus_publish()).
typedef struct {
    volatile uint32_t seq;      // monotonic; +1 per window. A gap seen by the UI
                                // means a window was missed entirely — the one
                                // failure that credits an effect to the wrong
                                // combination, so it is counted, not smoothed
    volatile uint8_t  active;   // 1 = numbers on screen AND bits being collected
    uint8_t  kind;              // FocusKind
    uint8_t  n, ne;             // numbers in nums[] / euro[]
    uint8_t  nums[6];
    uint8_t  euro[2];
} FocusState;

// Nodes in the array, master included as index 0 (docs/PLAN_NETWORK.md Phase D).
// 4 nodes → C(4,2) = 6 pairwise correlations, which is what the gate checks.
#define MAX_NODES   4
#define MAX_SLAVES  (MAX_NODES - 1)
#define MAX_PAIRS   (MAX_NODES * (MAX_NODES - 1) / 2)

// Per-node health, published so a node that quietly degraded is visible rather
// than merely averaged in. `ok` is session-scoped participation: a node whose
// camera failed is dropped from the combine and stays dropped for the rest of
// the session — it is rebooted, and rejoins by discovery at the next one.
typedef struct {
    char     ip[16];        // discovered by broadcast; "" for the master
    bool     ok;            // still contributing to the combined z
    uint8_t  cam_fault;     // this node's camera stopped delivering bits. It was
                            // dropped and rebooted; the flag stays set for the
                            // rest of the session so the UI can say WHICH node
                            // failed rather than only that the array shrank
    uint32_t reboots;       // times the master power-cycled this node's firmware
                            // over the session. Repeated reboots mean the camera
                            // is not coming back and the hardware needs a look
    double   sigma;         // per-run σ over the session (ideal 1.0)
    double   z_mean;        // online mean of this node's raw per-run z
    uint32_t z_n;           // runs behind z_mean (for SE = 1/√n)
    uint32_t lost;          // runs this node failed to answer in time
    uint8_t  soft_down;     // 1 = excluded from combine after a block σ excursion
                            // (quality collapse, not a hard camera stall). Cleared
                            // when a later block is clean. Never reboots.
    float    cam_mbit;      // camera rate at the last per-loop 'D' query
    uint32_t cam_stalls;
    // What this node's camera calibration chose at the start of the current loop
    // (PLAN.md Task 1). Nodes land on DIFFERENT settings and that is correct —
    // the cameras are physically different units — which is exactly why the
    // setting has to be published per node rather than as one session number.
    uint32_t cam_exp;       // 0 = this node has not calibrated (yet, or at all)
    uint16_t cam_gain;
    uint8_t  cam_fold;      // XOR fold state chosen
    uint8_t  cam_cal_ok;    // 1 = a candidate passed every gate; 0 = the node
                            // kept its previous setting because none did
    float    cam_bias;      // bias of the window that chose it
    float    cam_cal_mbit;  // rate of that same window
    /* First 8 bytes of the node's app elf sha256, hex — the same 16 characters
     * /status publishes as fw_sha, so the two are directly comparable. From the
     * node's 'D' reply.
     * Empty for the master (its own identity comes from esp_app_get_description)
     * and for a node whose firmware predates the field. It is in the CSV header
     * because "all four run the same code" is a policy, not a fact: on
     * 2026-08-19 the master ran a -dirty build from 10:57 and the slaves one
     * from 09:59, and the archive of that session records neither. */
    char     fw_sha[17];
} NodeStatus;

// Per-BLOCK health record (v3; the struct and the /loops endpoint keep their
// historical names). A block is the span between two sweep+baseline
// insertions (~15 min); each closed block stores the numbers a drift check
// needs — raw offsets and σ per node, plus camera health at that moment — and
// /loops serves the whole table, one row per block. With raw z published,
// slow drift is the one thing that widens the extremes, so this table and the
// drift regression on it matter because raw values remain uncorrected.
/* ⚠ Raised from 128 on 2026-08-19, because compaction removed the stop that was
 * hiding this one. At 52 blocks in 11,6 h — every `calint` plus one per round —
 * 128 was ~28 h, so the FIRST session able to run longer than the buffer would
 * have gone blind here instead: the drift regression survives (running sums,
 * exact past the table) but /loops, the per-block camera settings and the
 * exclusion verdicts simply stop being recorded. 1024 is ~9 days at that rate,
 * and costs 1024 x ~180 B = ~185 KB of PSRAM, which the same session already
 * has spare. */
#define LOOP_HIST 1024           // blocks kept in the table; the drift regression
                                 // runs on running sums and is exact beyond it
typedef struct {
    float    base;         // master baseline_mean of this loop = raw per-run z offset
    float    mean;         // combined per-run raw z mean over the loop
    float    sigma;        // combined per-run σ (== loop_sigma), ideal 1.0
    uint8_t  nodes;        // nodes contributing to this loop (master included)
    // Per node, index 0 = master. A node that did not take part leaves zeros,
    // which is distinguishable from a measured 0 by `nodes` and by sig_n == 0.
    float    mean_n[MAX_NODES];   // per-node mean z (after its own baseline)
    float    sig_n[MAX_NODES];    // per-node per-run σ over this loop, ideal 1.0
    float    cam_mbit[MAX_NODES]; // camera rate at loop end, 0 = not answered
    uint32_t cam_stalls[MAX_NODES];
    uint32_t t_s;          // elapsed seconds at loop end
    // Camera settings this loop was MEASURED AT (PLAN.md Task 1 §1.5.2, and
    // mandatory there rather than optional). Per-loop re-tuning is what tracks
    // thermal drift, and recording the setting keeps the statistics auditable — but a
    // per-loop change nobody logged is indistinguishable from drift in the data,
    // so the setting travels with the loop it produced.
    uint32_t cam_exp[MAX_NODES];    // 0 = not calibrated this loop
    uint16_t cam_gain[MAX_NODES];
    uint8_t  cam_fold[MAX_NODES];
    uint8_t  cam_cal_ok[MAX_NODES]; // 0 = kept its previous setting, no gate passed
    float    cam_bias[MAX_NODES];   // bias of the window that chose it
    uint16_t cal_ms;       // wall time the calibration cost AT THE TOP OF THIS
                           // LOOP. 0 = no sweep ran here (interval not elapsed,
                           // or ?cal=0): the cam_* fields above are then the
                           // setting carried over from an earlier loop, which
                           // is still the operating point this loop measured at
    // The measured run window and inter-run gap OF THIS LOOP. Recorded per loop
    // because the count→duration conversion is not stable (open item 4) and
    // per-loop calibration moves the camera's rate on purpose (§1.5.3), so the
    // series across loops is the only way to see the window drift rather than
    // average it away. Measured in every session, attended or not.
    float    win_ms, gap_ms;
    /* ── Who was in the combine, and why (2026-08-19) ──────────────────────
     * The exclusion state was published only as a live flag in /status and
     * printed to a console nobody reads, so a finished session could not say
     * WHEN an arm went down or what bar it was judged against. On 2026-08-19
     * that had to be reconstructed by replaying the rule against the block
     * table — which works only as long as the rule has not changed since, i.e.
     * exactly when it is least useful. A block row now carries its own verdict.
     *
     * soft_mask  bit i = node i was soft-down at the close of THIS block
     * trip_mask  bit i = node i tripped IN this block (σ over the bar)
     * mean_mask  bit i = |mean| over NODE_MEAN_REPORT — a flag, never an
     *            exclusion; the offsets it marks are the ones the exposure
     *            ladder makes and centring removes
     * clear_sig  the peer-referenced σ bar this block was judged against; it
     *            MOVES per block, so a clean/not-clean call cannot be rechecked
     *            without it
     * quarantined  this block's items were skipped for ranking */
    uint8_t  soft_mask, trip_mask, mean_mask;
    uint8_t  quarantined;
    float    clear_sig;
} LoopStat;

typedef struct {
    ElottoState      state;
    ElottoPhase      phase;
    ElottoMode       mode;
    /* ROWS CURRENTLY IN results[] — not the session's item count. The two were
     * the same number until round-boundary compaction (2026-08-19) made
     * results[] a subset rather than a prefix. Everything that walks the array
     * bounds itself with this; everything that reports PROGRESS uses
     * items_done. ⚠ Using this one for progress makes the counter go backwards
     * the first time a compaction runs. */
    volatile int     runs_completed;
    /* Items measured this session, across every round. Monotone: a compaction
     * never lowers it, because the measurement happened. This is what /status
     * publishes as `completed`, what round_base is taken from, and what the CSV
     * header counts in `items=`. */
    volatile int     items_done;
    /* Items dropped by compaction, i.e. measured and folded into the pass
     * statistics but no longer individually in results[] or the CSV. 0 for any
     * session that never filled the buffer, which is most of them.
     * ⚠ Published so an archive can say what it is NOT: a CSV with
     * `compacted=` non-zero holds the extremes plus whatever else survived, and
     * is not a sample of the session. Never compute a distribution from it. */
    int              compacted;
    int              runs_total;       // combinations in the CURRENT round
    /* ── Unlimited mode (see the block near the top of this file) ──────
     * `unlimited` and `runs_cap` are session parameters written by /start and
     * NOT reset by elotto_task — they are the session's tag, like focus_mode.
     * The rest is per-round bookkeeping the UI and the CSV read. */
    bool             unlimited;        // rounds repeat until Abort / results full
    int              runs_cap;         // measurement runs a round may spend
    int              round;            // 1-based; 0 before the first round starts
    int              round_base;       // results[] index this round started at
    int              round_total;      // == runs_total, published separately so a
                                       // reader never has to know which one moved
    volatile int     baseline_done;
    int              baseline_total;
    double           baseline_mean;
    int64_t          elapsed_ms;
    volatile int     scoring_done;
    int              scoring_total;
    double           best_z;              // most extreme |Z*| in the studentized ranking
    double           p_corrected;         // Bonferroni two-sided p of best_z (on Z*)
    int              comparisons;         // == VALID items so far (voids excluded)
    /* ── Pass-level health (GCP primary endpoints) ─────────────────────
     * Under H₀ with a working instrument: mean ≈ 0, σ ≈ 1, Σz² ≈ n.
     * Ranking is secondary; when null_flags ≠ 0 the extremes are not
     * decisive. Updated after every valid item from the valid prefix. */
    double           pass_mean;           // mean of valid raw z so far
    double           pass_sigma;          // sample σ (df = n−1) of valid raw z
    double           pass_chi2;           // Σ z² over valid items (≈ χ²(n) under H₀)
    double           pass_stouffer;       // mean · √n — test of a common offset
    int              pass_n_valid;        // ranked items (k>0 and not skip_rank)
    int              pass_n_void;         // incomplete combines (k=0), archived only
    int              pass_n_excl;         // k>0 but skip_rank (trigger-block quarantine)
    double           v_eff;               // Var(Σz_i/√k) under measured σ and r
                                          // (1.0 = independent unit nodes)
    uint8_t          null_flags;          // NULL_FLAG_* — non-zero ⇒ ranking not decisive
    double           loop_sigma;          // per-run σ of the LAST CLOSED BLOCK (1.0 = ideal)
    int              loops_done;          // BLOCKS closed and folded into the drift stats
    int              loop_hist_n;         // entries valid in loop_hist[] (<= LOOP_HIST)
    double           drift_slope;         // z-offset change per block (linear regression on
                                          // the master's raw per-run offset per block)
    double           drift_t;             // slope / SE(slope); |t| > 3 = real drift, not noise
    double           off_first, off_last; // master raw per-run z offset, first / latest block
    double           sigma_lo, sigma_hi;  // min / max per-block combined σ across the session
    ScoreDir         score_dir;           // Phase-0 pool selection rule (pre-registered)
    // Independence check across ALL node pairs (6 of them at n=4). Only the
    // worst is published as a scalar: the √n gain fails if ANY pair correlates,
    // so the maximum is the number that decides, not an average that would
    // dilute one bad pair among five good ones.
    double           pair_r_max;          // largest |r| over the pairs (signed value kept)
    int              pair_r_i, pair_r_j;  // which two nodes produced it
    int              pair_n;              // runs behind that worst pair
    int              pair_count;          // pairs actually evaluated
    // The FULL matrix, not only the worst pair. The measurement topology is the
    // Risk 1 control — master on isolated power, slaves on one PoE rail — so
    // which pairs correlate is the whole question: slaves-only implicates the
    // shared rail, everything-with-everything implicates the room. Publishing a
    // maximum answers neither. Upper triangle used; index 0 is the master.
    double           pair_r[MAX_NODES][MAX_NODES];
    int              result_count;       // valid entries in top[] (published)
    RunResult        top[TOP_N];          // highest raw z measured so far, desc
    int              low_count;           // valid entries in low[] (published)
    RunResult        low[TOP_N];          // lowest raw z measured so far, asc
    volatile bool    abort_requested;
    // ── Focus display (PLAN_4NODE Phase 5) ─────────────────────────────
    bool             focus_mode;          // this session is ATTENDED: the panel is
                                          // live and the session is tagged as such.
                                          // A focus session is not equivalent to an
                                          // unattended one, so the two must never be
                                          // pooled later — hence a recorded flag
                                          // rather than "whether someone was watching"
    volatile bool    paused;              // hold BETWEEN runs (never inside one):
                                          // attention is the scarce resource here, and
                                          // without a pause the only way to stop
                                          // attending is to abort and lose the loop
    int64_t          paused_ms;           // total time held, excluded from elapsed_ms
                                          // so a session with a 40-min break is not
                                          // later read as continuous
    float            focus_win_ms;        // measured mean lit window (the run)
    float            focus_gap_ms;        // measured mean dark gap between runs —
                                          // the gate asks whether the ~200 ms was
                                          // free (existing overhead) or paid for
    int              run_target_ms;       // requested window (from ?run=), for status
    int              gap_ms;              // intentional blank between runs (?gap=)
    /* Runs voided because the pre-window ring flush did not finish in
     * ONSET_SETTLE_MS. Published in /status and the CSV header: a silent
     * safeguard that fires is indistinguishable from one that never had to. */
    uint32_t         flush_timeouts;   /* per SESSION -- cleared at session start */
    int              run_segments;        // segment count derived for this session
    FocusState       focus;
    bool             slave_connected;     // at least one slave answered discovery
    int              node_count;          // nodes discovered, master included (>= 1)
    int              node_ok;             // of those, still contributing
    NodeStatus       nodes[MAX_NODES];    // [0] = master
    // UDP transport health (PLAN_NETWORK Phase C / Risk 3: "UDP loss must be
    // handled explicitly, not assumed away"). Per session.
    uint32_t         net_retries;         // commands resent because no reply came
    uint32_t         net_lost;            // triggers with no reply even after the
                                          // resend — the gate wants 0
    uint32_t         net_stale;           // replies dropped for a mismatched
                                          // sequence number, i.e. answers that
                                          // arrived after we stopped waiting.
                                          // Silently accepting one would pair
                                          // z_slave of run k with z_master of
                                          // run k+1 — correlation dressed as
                                          // physics, so they are counted, not used
    // ── Per-loop camera calibration (PLAN.md Task 1) ───────────────────
    int              cal_budget_ms;       // sweep budget per loop, 0 = do not
                                          // calibrate. A no-calibration session
                                          // is the matched control this change
                                          // has to be compared against, so it is
                                          // a session parameter, not a #define
    int              cal_ms;              // what the last loop's calibration
                                          // actually cost, master + ack wait —
                                          // the §1.6 gate is a measured number
    int              cal_interval_ms;      // minimum wall time between sweeps.
                                          // A loop that starts sooner than this
                                          // after the last one SKIPS calibration:
                                          // the sweep costs ~24 s, a Runs-capped
                                          // loop can be shorter than that, and
                                          // thermal drift moves on wall-clock
                                          // time rather than per loop. 0 = the
                                          // old behaviour, sweep every loop
    bool             cal_did_sweep;       // did THIS loop calibrate? Recorded per
                                          // loop (LoopStat.cal_ms = 0 when not),
                                          // because "the setting was re-derived
                                          // here" and "it was carried over" are
                                          // different facts about the data
    volatile int64_t cal_start_us;        // when the sweep in flight began, 0 when
                                          // none is. Published as cal_elapsed_ms so
                                          // the UI can show a live bar: a silent
                                          // ~25 s gap at the head of every loop
                                          // reads as a crash otherwise. cal_ms is
                                          // only written when the sweep ENDS, so it
                                          // cannot drive progress while one runs
    volatile bool    noise_stalled;       // the array lost too many cameras to carry
                                          // on. There is no substitute source to fall
                                          // back to by design, so at n >= 3 a failed
                                          // node is dropped and rebooted and the rest
                                          // continue over √(n−1); below the floor the
                                          // session ABORTS.
    char             fault[112];          // human-readable reason, "" when healthy.
                                          // A camera failure has to reach the operator
                                          // as words — a node silently missing from
                                          // the combine is the failure mode this whole
                                          // policy exists to prevent
    /* ── Attended pool confirmation ────────────────────────────────────
     * Scoring proposes a pool; with `pool_confirm` set the session STOPS at
     * PHASE_POOL_CONFIRM and publishes the proposal here so the operator can
     * edit it before thousands of runs are spent measuring it. Opt-in per
     * session (`POST /start?confirm=1`) because a device that waits for a
     * human would hang every scripted or overnight run — the web UI sends it,
     * curl does not.
     *
     * Keeping fewer numbers is legitimate and shrinks the combination space
     * exactly: at pool_n_main == pool_need_main (and, for Eurojackpot,
     * pool_n_euro == 2) there is exactly ONE combination — measured exactly
     * once, like everything else in the v3 single pass. */
    uint8_t          pool_main[POOL_MAIN_49];   // proposed/confirmed main numbers
    float            pool_main_z[POOL_MAIN_49]; // their scoring z, for display
    uint8_t          pool_euro[POOL_EURO_12];   // Eurojackpot bonus pool
    float            pool_euro_z[POOL_EURO_12];
    uint8_t          pool_n_main;         // slots filled
    uint8_t          pool_n_euro;
    uint8_t          pool_need_main;      // a draw needs this many (5 or 6)
    uint8_t          pool_need_euro;      // 2 for Eurojackpot, 0 for 6-of-49
    uint8_t          pool_confirm;        // 1 = this session stops and asks
    uint8_t          pool_auto;           // 1 = nobody answered; the proposal was
                                          // taken unchanged after the timeout, and
                                          // that fact belongs in the record
    LoopStat        *loop_hist;           // per-block health table (LOOP_HIST entries,
                                          // PSRAM — internal RAM is full with results[];
                                          // NULL if the allocation failed, in which case
                                          // only the drift/σ aggregates are available)
    /* The pass, in MEASUREMENT order: results[j] is the j-th item measured
     * (its combination id is results[j].index). Compact by construction, so
     * the prefix [0 .. runs_completed) is always the complete record — an
     * abort needs no compaction and /results.csv streams it directly. */
    RunResult        results[NUM_RUNS];
} ElottoStatus;

extern ElottoStatus g_status;

void elotto_task(void *pvParam);

/* Administrative randomness only — measurement order, and the link's initial
 * sequence number. Seeded from the camera once per session, then run forward
 * arithmetically. It NEVER enters a z-score: spending rate-limited camera
 * entropy on a shuffle would stall the session for bits nobody measures. */
uint32_t fast_rng(void);

/* The master's most recent calibration sweep, or NULL if it has never run one.
 * The whole per-candidate table, not just the winner: the Task 1 gate is a
 * bias-vs-exposure CURVE, and a single chosen point cannot show whether bias
 * responded to exposure at all. Served by GET /calibrate. */
const camera_cal_t *elotto_last_calibration(void);

/* The `cap` VALID items sitting CLOSEST TO THE PASS MEAN, i.e. the least
 * remarkable measurements of the session — the third published group beside
 * Top-N and Bottom-N. Writes up to `cap` entries to out[] (nearest first) and
 * returns how many; *out_mean / *out_sigma receive the pass statistics the
 * selection was made against (sample σ, df = n−1), or 0 when n < 2.
 *
 * ⚠ Nearest the MEAN, not nearest raw zero. Raw z carries the array's common
 * offset — the 2026-08-05 pass ran at mean −1.82 — so |z_raw| ≈ 0 would select
 * items about +1.8σ ABOVE the array's own centre: the opposite of neutral.
 * Ordering by |z − m| is identical to ordering by the studentized |z − m|/σ,
 * so σ only scales what is displayed, never which items are chosen.
 *
 * Void rows (k == 0) are skipped. Computed on demand from the measured prefix
 * rather than maintained incrementally: the mean moves as the pass proceeds.
 * Safe against the running session: runs_completed is bumped only after a row
 * is complete. */
int results_near_mean(RunResult *out, int cap, double *out_mean, double *out_sigma);

/* Per-node raw z for measured item j (measurement order). Returns false if
 * the archive is missing or j is out of range. out[MAX_NODES] gets NaN for
 * nodes that did not contribute that run. Lives in PSRAM so results[] itself
 * stays lean. */
bool results_node_z(int j, float out[MAX_NODES]);
