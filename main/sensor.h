#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "camera.h"
#include "elotto_link.h"

/* v3: rounds until Abort. Inside a round every combination is measured
 * exactly ONCE; results[] holds it in MEASUREMENT order and ACCUMULATES across
 * rounds, so NUM_RUNS is the hard cap on the buffer, not on the session.
 * 500 rows. A row is 48 B (the double in RunResult forces 8-byte
 * alignment). Compaction keeps at most PASS_KEEP_EXTREME rows (100) and the
 * next round is appended after them, so the resident peak is 100 + ?maxruns=;
 * UNLIM_RUNS_MAX 400 is what keeps every round whole. A combination space
 * larger than NUM_RUNS aborts — the shuffle buffer is this wide. */
#define NUM_RUNS       500
#define TOP_N            5
/* ── Round-boundary compaction  ──────────────────────────────────────
 * Unlimited rounds keep the 100 most extreme RANKED items by |rank_key| (both
 * tails). Quarantined rows are not kept: nothing reads them once their
 * block is closed. Everything else merges into pass moments. Offline re-analysis of the dropped
 * rows is not a goal. Since D67 every session is rounds, so every round
 * boundary calls pass_compact() (no-op while n ≤ 100). */
#define PASS_KEEP_EXTREME 100
#define POOL_MAIN_49    15   // C(15,6) = 5005 combinations
#define POOL_MAIN_50    12   // C(12,5) =  792 combinations
#define POOL_EURO_12     5   // C(5,2)  =   10 combinations
/* Phase-0 scoring: every number this many times, fresh shuffle each pass,
 * keys summed, then the pool is the top by that sum. Not back-to-back
 *.
 * ⚠ 20 since, and a camera sweep runs after pass SCORE_PASSES/2 of
 * EVERY scoring run — which in Eurojackpot is two runs, the main numbers and
 * the euro numbers. Each pass is centred and scaled on its own (score_build_keys
 * runs per pass), so the sweep lands on a centring boundary, not inside one.
 * ⚠ It doubles the scoring, which already dominates the round: 20 passes put a
 * Eurojackpot round near 2,5 h at ?run=5. That is the operator's call. */
#define SCORE_PASSES    20
#define SCORE_START_PAUSE_MS 2000   // idle break before each round's scoring 
// Eurojackpot: C(12,5)·C(5,2) = 7920 — the largest configuration under the
// ~10000 the user set as the ceiling (13+5 would be 12870). 6-of-49: 5005.

/* ── Session parameters: ONE definition each ──────────────────────────────
 *
 * Every value below is defined HERE and nowhere else. The HTML input
 * attribute, the JavaScript clamp and the C validator in the /start handler all
 * read from this one place.
 *
 * The page is a C string literal, so the UI copies are made to reference these
 * through EL_STR() rather than being written out again. A limit changed here
 * now changes the form field, the clamp and the validator together.
 *
 * ⚠ Defaults only. Every one is overridable per session on /start, so
 * changing a default here does not describe a session that overrode it. */
/* ⛔ No round-length warning. The form prints the scoring/pass split
 * instead — which names `?run=`, the parameter that actually sets the
 * length. */
#define CAL_BUDGET_DEFAULT_MS 10000      // exposure-sweep CAP, split over 9 rungs
#define CAL_BUDGET_MAX_MS   120000
/* Dynamic sweep interval. A sweep runs at a CANDIDATE point — the end
 * of every scoring pass, before the pass, the round boundary: the only places
 * no centring mean straddles — once the interval since the last sweep has run
 * out. An ok sweep (every node certified, no exposure moved) doubles the
 * interval up to the max; anything else, and every soft-down trip, puts it
 * back to the min. The session-start sweep always runs. */
#define CAL_DYN_MIN_MS      (15 * 60 * 1000)
#define CAL_DYN_MAX_MS      (120 * 60 * 1000)
/* The settle pause after a sweep that moved any node's exposure: the
 * whole array waits this long before the next window. calibrate_all() owns it;
 * here so the page's countdown bar reads the same number. */
#define CAL_SETTLE_AFTER_MS  60000
/* ⛔ ONE BLOCK IS ONE ROUND, and there is no time trigger. The round
 * boundary parks the pass, closes the block and runs the camera sweep; the block
 * is the unit that carries the drift point, the pairwise close and the /loops
 * row. `?cal=0` turns the sweep off; nothing else changes the boundary.
 *
 * ⚠ The sweep runs TWICE per round since  — at the boundary and again
 * between the scoring and the pass — but the BLOCK boundary is still only the
 * round boundary. The second sweep sits between two spans that are already
 * separate blocks (the scoring span and the pass), so it moves no operating
 * point underneath a centring and leaves n untouched.
 *
 * Why: `rank_key()` divides by the block's own σ, and the largest value a block
 * can produce is (n-1)/√n with n its item count. Round = block makes n the same
 * in every round, because every round measures the same `maxruns`-sized space.
 * ⚠ The last round is cut short by Abort, so its block is the one exception. */
/* Per-run window (one continuous window per item) and the
 * intentional blank after it. Both are session parameters on /start (?run= &
 * ?gap=). Gap defaults to 40 % of the
 * window (5 s → 2 s, 7 s → 2.8 s ≈ 3 s) so duty stays ~70 % of the intentional
 * cycle; measured focus_gap_ms is larger because it also includes slave collect.
 * Segment count is derived from run_s via the segs↔ms cal — wall time can stretch
 * if the camera rate collapses at long windows (that IS the limit). */
/* ⚠ 0,2..5 s: 0,2 s is ~10435 segs / ~2,3 Mbit, the
 * window's ~0,52 Mbit first-pair fragment plus about one whole pair.
 * Auto-gap is 40 % of the window with no floor above GAP_S_MIN 0, and an
 * explicit ?gap= may be 0: the item boundary is the flush on M, not the gap
 *, so a short gap never credits one item's bits to the next. */
#define RUN_S_DEFAULT            5
/* Cap on the per-item ring flush (sensor.c onset_settle). A fresh pair costs
 * ~56 ms idle and ~85 ms under load, so the wait is normally well under this;
 * the cap only bounds the damage if the camera has stopped delivering. */
#define ONSET_SETTLE_MS        700   // two pairs now: one discarded 

#define RUN_S_MIN              0.2
#define RUN_S_MAX                5
#define GAP_S_MIN                0
#define GAP_S_MAX               10
/* Live 4-node cal. ⚠ THIS PAIR IS A MEASUREMENT, and it must be re-measured
 * after anything that changes the extraction rate — otherwise the window the
 * operator asks for and the window the observer actually gets drift apart
 * silently, which is the one thing the Focus protocol cannot tolerate.
 * ⚠ Do not pool sessions measured against different pairs: same nominal
 * ?run=, different bit count per item. */
#define RUN_SEGS_REF        141026
#define RUN_MS_REF            2703
/* The wire caps the segment count at EL_SEG_MAX, and a receiver does NOT clamp
 * an out-of-range value — it falls back to its own default, which would put the
 * nodes on different window lengths without anything looking wrong. The master
 * must therefore never ASK for more than the wire allows. With RUN_S_MAX at 5 s
 * that holds with room to spare (260871 of 400000), and the assertion below
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
 *   run_bits  = GCP_SEGMENT_BITS * segments      (segments from run_s)
 *   cycle_ms ~= run_bits / rate + CYCLE_FIXED_MS + gap_ms
 *
 * CYCLE_LOAD_MBIT_X100 is the per-node rate UNDER LOAD, not the idle rate:
 * during a session the GCP consumer outranks the extraction task. The model
 * must use the SLOWEST node, not an average — the slowest node sets how long a
 * run takes.
 *
 * CYCLE_FIXED_MS is what is left over per run and does not scale with the
 * window: the ring flush, the trigger, the reply collect and the publish.
 *
 * ⚠ Still an ESTIMATE, and the live UI prefers the measured pace wherever it
 * has one — including the slowest node's own cam_mbit from /status, which makes
 * the constant a cold-start value rather than the answer. */
#define CYCLE_LOAD_MBIT_X100   732   // per-node rate under load x100; 2x words, LSB-as-is 
#define CYCLE_FIXED_MS         780   // per-run overhead that does not scale

/* ── Unlimited mode ───────────────────────────────────────────────────────
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
 * meaningful WITHIN a round: the pool it enumerates changes every round. */
#define UNLIM_RUNS_DEFAULT     100   // measurement runs per round
#define UNLIM_RUNS_MIN          10
#define UNLIM_RUNS_MAX         400   // NUM_RUNS minus the compaction survivors
_Static_assert(UNLIM_RUNS_MAX + PASS_KEEP_EXTREME <= NUM_RUNS,
               "a round of ?maxruns= must fit beside the compaction survivors");
#define UNLIM_RUNS_STEP         10
/* ⚠ The pool split is NOT a free choice: maximise the combinations measured.
 * A pool rule is neutral exactly when it spends the whole run budget. The
 * bonus-number preference survives only as the TIE-BREAK, where it is free.
 * No weights — see unlimited_pool_sizes(). */

/* Diagnostic thresholds that appear in more than one place. */
#define PAIR_FLAG_T            3.0   // |r|·√n above this = nodes not independent
#define DRIFT_FLAG_T           3.0   // |drift_t| above this = real cross-block drift
/* ── When the OPEN block joins the ranking ────────────────────────────────
 * Items are ranked on z_ctr, and z_ctr only means anything once each node's own
 * offset has been subtracted. For a closed block that mean comes from ~100
 * items; for the open block it comes from however many it holds so far. Below
 * this count the estimate is worse than useless — with one item per node the
 * mean IS that item and centring zeroes it — so the open block stays out and
 * the tables say so. Above it, the open block is centred live at every publish
 * and ranks alongside the closed ones.
 * ⚠ A block-centred value is deflated by √(1−1/n) per node, which at n = 4 is
 * 13 %. That is real but it is the SAME bargain every closed block already
 * makes, only noisier, and it shrinks as the block fills. */
#define PASS_OPEN_MIN_N        4    // items in the open block before it is ranked
#define NODE_SOFT_TRIP_K       1.35  // trip if block σ > K × peer-median σ 
#define NODE_SIGMA_SOFT        NODE_SOFT_TRIP_K  /* alias: old absolute 1.25 is gone */
/* ── |block mean| REPORTS, it no longer excludes ──────────────────────────
 *
 * Centring (center_block()) removes each node's own block mean, so a constant
 * offset is already gone from every ranked number; excluding the arm buys
 * nothing. An arm over this is still flagged in the block's /loops row and
 * printed, because a big offset is worth seeing. It does not exclude, does not
 * quarantine, and does not block a clear.
 * ⚠ σ remains a trip wire and must: it is invariant to the run length by
 * construction, and centring does NOT fix it. */
#define NODE_MEAN_REPORT       1.50  // |block mean z| above this → flagged, not excluded
#define NODE_SOFT_MIN_N       20     // min runs in the block before soft-exclude
/* ── Clearing a soft-down: bars RELATIVE to the peers in the same block ────
 *
 * The bar is the peers' own median σ for that block, times a factor. That is
 * real hysteresis (trip and clear stay far apart) AND it makes a common-mode
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
/* ⚠ There is no |mean| clear bar. A criterion that cannot trip a node must
 * not be able to keep it down either. */

/* ⚠ The σ FLOOR is 1,10 and NOT the old 1,05. */
#define NODE_SOFT_CLEAR_BLOCKS 4     // consecutive clean blocks required to lift soft-down
/* Never soft-exclude below this many live nodes. ⚠ 1, not 3: at four nodes a
 * floor of 3 allows exactly one exclusion, so when two nodes misbehave at once
 * the second would stay in the combine. A bad arm in the combine costs more
 * than a small k does. */
#define NODE_SOFT_MIN_COMBINE  1     // never soft-exclude below this many live nodes
/* Concordance weight of the ranking key, ?wpre=<0..1>.
 * DEFAULT 0: z alone. The form pre-fills 0,8. Curl must not silently pick a
 * weight; the page does, and /status `pre_w` says which one ran. */
#define ENT_W_PRE_DEFAULT    0.0
#define ENT_W_PRE_FORM       0.8

/* Phase-0 scoring direction (pre-registered). Only affects WHICH numbers enter
 * the pool — never the Phase-2 measurement statistics. Default HIGH matches
 * the historical "largest positive key" rule. */
typedef enum {
    SCORE_DIR_HIGH = 0,   // pick largest rank_key
    SCORE_DIR_LOW  = 1,   // pick smallest rank_key
    SCORE_DIR_ABS  = 2,   // pick largest |rank_key|
} ScoreDir;

/* Stringify, so the HTML/JS copies of the numbers above are the SAME token the
 * C code compiles. Two levels are required: the inner one would otherwise
 * stringify the macro's name instead of its value. */
#define EL_STR2(x) #x
#define EL_STR(x)  EL_STR2(x)

typedef enum { MODE_EUROJACKPOT = 0, MODE_LOTTO_649 = 1 } ElottoMode;
typedef enum { ELOTTO_IDLE, ELOTTO_RUNNING, ELOTTO_DONE, ELOTTO_ABORTED } ElottoState;
// PHASE_CALIBRATE is appended, not inserted: it runs FIRST in a round but
// the enum values are wired into the UI and /status by number.
typedef enum { PHASE_SCORING, PHASE_MEASURING,
               PHASE_CALIBRATE } ElottoPhase;

/* v3: NO ranking modes. Each item's published Z is its own single raw
 * measurement, stored in results[] untouched: no rewrite, no subtraction, no
 * cross-item normalization. A recurrence in a later round is a separate row,
 * never an average. */

/* Entropy is photons. A node whose camera stops is reported and rebooted. */

typedef struct {
    int        index;      // combination id (1-based slot in the enumeration)
    /* BIT AUTOCORRELATION of the bits this item was measured from (bAC):
     * per node a = Σ_{L=1..4} z_L over its window (,ac= on the wire, the
     * master's own win_ac_z), centred on that node's mean over the block,
     * Stouffer over the combined nodes, then divided by the block's own σ of
     * that combine — unit variance within the block, read against 0.
     * + = the bits clumped more than in the rest of the block, − = alternated.
     * Never enters Z*; its scoring SUM may pick the pool (research).
     * NaN until the block (scoring: the pass) is centred, or no node reported.
     * Sits here to fill the padding before z_score: the row stays 48 bytes. */
    float      bac;
    double     z_score;   // RAW combined Stouffer z (Σz_i/√k) — never rewritten
    /* BLOCK-CENTRED combine: Σ(z_i − m_i,block)/√k over the same nodes that
     * entered z_score, where m_i,block is node i's own mean over this block.
     * This is what the ranking and pass mean/σ run on.
     *
     * ⚠ It also removes any real effect that is CONSTANT across a whole block,
     * which is a pre-registration decision, not a detail: what this instrument
     * can still see is an effect that varies BETWEEN items inside a block.
     * z_score stays raw and untouched, so the uncentred view survives beside
     * it. Provisional (= z_score) until the block closes. */
    float      z_ctr;
    uint16_t   block;      // which block this item was measured in (v3)
    /* Which ROUND measured it, 1-based. The pool is re-scored every round, so `index` enumerates a
     * DIFFERENT combination space per round — the pair (round, index) is the
     * identity, and nums[]/euro[] are what a reader should actually key on. */
    uint16_t   round;
    uint8_t    k;          // nodes that entered the combine; 0 = VOID (incomplete)
    uint8_t    have_mask;  // bit i set ⇒ node i contributed (master = bit 0)
    uint8_t    skip_rank;  // 1 = exclude from pass mean/σ/Top-Bottom (trigger block)
    uint8_t    nums[6];
    uint8_t    euro[2];
    /* ⛔ `zp_ctr` (the old second LSB channel) was DELETED: it had become a
     * bit-for-bit alias of z_ctr and its per-node archive a copy of the
     * per-node z. Nothing was lost.
     * ⚠ `raw_sigma` in /diag is a DIFFERENT number: the per-mini-run sigma of
     * one node's stream. rank_key() divides by the item's BLOCK σ,
     * s_bsig[block].sig_p, never by raw_sigma. */
    /* Concordance z: leave-one-out Stouffer of per-node half-window z.
     * Each HALF is centred on its own per-node block mean BEFORE the sign
     * test  — on raw halves the node offset makes
     * both halves always agree and the channel was z plus noise. 0 = halves
     * disagreed on every surviving node, or fewer than two nodes to
     * corroborate; under H₀ that is about half of all items.
     * ⚠ This is the ONLY channel beside z  — `?wpre=` is its weight p and
     * the key is ((1-p)·z_ctr/σ_z + p·zc_ctr/σ_c)/√((1-p)²+p²), each term on
     * that item's own BLOCK σ. Not a half-and-half split of pre_w. */
    float      zc_ctr;
    /* NODE AGREEMENT on this item: sample σ across the contributing nodes of
     * their own block-centred z, each standardised by that NODE's own σ over
     * the same block. Small = the cameras moved together on this item, large =
     * one node carried the combined z alone.
     *
     * Why standardise per node before comparing: the per-node LSB σ is NOT 1
     * and differs between nodes, so the raw spread of z_i would
     * mostly measure which nodes happened to contribute. After the division the
     * null is ≈ 1 for independent nodes whatever their scale, which is the same
     * bargain rank_key() makes with the block σ.
     *
     * The mean the spread is taken around IS the combined z: Σz_i/√k = √k·mean,
     * so "deviation from the combined Z*" and "deviation from each other" are
     * the same number here.
     *
     * ⚠ It is a per-BLOCK quantity like z_ctr: provisional (NaN) until the
     * block has been centred, recomputed by center_block() every time.
     * ⚠ NaN when fewer than two nodes have a usable block σ — k < 2, or a node
     * with fewer than 2 runs in the block. It is never 0 for "unknown".
     * ⚠ It says nothing about whether the item is HIGH or LOW; it is a
     * confidence column beside Z*, not a second ranking key. Nothing selects,
     * excludes or reorders on it. */
    float      node_sd;
    /* ITEM AUTOCORRELATION (AC): this item's centred z times the 1..4 items
     * measured before it, / √m — see series_ac(). NaN until the block closes
     * and for its first item. Never enters Z*. */
    float      acz;
} RunResult;
_Static_assert(sizeof(RunResult) == 48, "results[] row is the internal-RAM budget");

// Current-item display: what is on screen right now, for exactly the window
// its bits are collected in. The session is unattended; the one
// property that must hold is `active` ⟺ a run is sampling.
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

// Nodes in the array, master included as index 0.
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
    float    cam_mbit;      // PRODUCTION rate at the last per-loop 'D' query --
                            // what extraction wrote into the ring, discarded
                            // surplus included. Not device performance.
    float    cam_cons_mbit; // CONSUMPTION rate: bits a measurement actually read,
                            // per second SPENT READING (gaps excluded). This is
                            // the number that compares nodes. 0 = the node did
                            // not report it (slave older than 2026-08-30) or no
                            // run has completed yet -- never "reads nothing".
    uint32_t cam_stalls;
    // What this node's camera calibration chose at the last sweep (round
    // boundary). Nodes land on DIFFERENT settings and that is correct —
    // the cameras are physically different units — which is exactly why the
    // setting has to be published per node rather than as one session number.
    uint32_t cam_exp;       // 0 = this node has not calibrated (yet, or at all)
    uint16_t cam_gain;
    uint8_t  cam_cal_ok;    // 1 = a candidate passed every gate; 0 = the node
                            // kept its previous setting because none did
    float    cam_bias;      // bias of the window that chose it
    /* LSB health from the last 'D' query. 0 = this node did not
     * report it, which is NOT the same as a raw bias of zero. */
    float    cam_raw_bias;
    float    cam_raw_sigma;
    /* The LIVE operating point from that same 'D' query. ⚠ Not cam_exp: that is
     * what the last SWEEP chose, and the two differ after a manual
     * POST /expose or a sweep that certified nothing. */
    uint32_t cam_exp_now;
    uint16_t cam_gain_now;
    /* The bias/sigma pair from the same 'D' query, i.e. this node's own /diag
     * values. The wire always carried them; they were parsed and dropped
     * until the collector needed them. Same bits as cam_raw_*. */
    float    cam_bias_now;
    float    cam_sigma_now;
    /* The camera sigma of the LAST MEASUREMENT WINDOW on this node,
     * from ,wsig= on the 'Z' reply — not from the 'D' query the two fields
     * above come from.
     * ⚠ cam_sigma_now spans everything since the last sweep, up to three
     * blocks on this rig, and therefore cannot localise anything. This one
     * covers exactly the bits one item was scored from, which is what makes a
     * per-item jump meaningful.
     * ⚠ NAN = the node did not report one, NOT a quiet window. */
    float    cam_wsig_now;
    /* The same window's autocorrelation,,ac= on the 'Z' reply: the sum
     * of the lag-1..4 z, variance 4 under independence. NAN = not reported. */
    float    cam_ac_now;
    /* Mean raw pixel level from that same 'D' query (,px=). The one covariate
     * that separates a light change from a sensor change.
     * ⚠ 0 = this node did not report it (firmware older than 2026-08-28), not
     * a dark frame. */
    float    cam_mean_px;
    float    die_temp_c;    // this node's P4 die temperature; NAN = not reported
    float    cam_cal_mbit;  // rate of that same window
    /* First 8 bytes of the node's app elf sha256, hex — the same 16 characters
     * /status publishes as fw_sha, so the two are directly comparable. From the
     * node's 'D' reply.
     * Empty for the master (its own identity comes from esp_app_get_description)
     * and for a node whose firmware predates the field. /diagjson?all=1 shows
     * it per node because "all four run the same code" is a policy, not a
     * fact. */
    char     fw_sha[17];
} NodeStatus;

// Per-BLOCK health record (v3; the struct and the /loops endpoint keep their
// historical names). A block is the span between two camera sweeps
// (one round); each closed block stores the numbers a drift check
// needs — raw offsets and σ per node, plus camera health at that moment — and
// /loops serves the whole table, one row per block. With raw z published,
// slow drift is the one thing that widens the extremes, so this table and the
// drift regression on it matter because raw values remain uncorrected.
/* ⚠ 1024 blocks: the drift regression survives beyond the table (running sums,
 * exact past it) but /loops, the per-block camera settings and the exclusion
 * verdicts simply stop being recorded once it is full. Costs 1024 x ~180 B =
 * ~185 KB of PSRAM. */
#define LOOP_HIST 1024           // blocks kept in the table; the drift regression
                                 // runs on running sums and is exact beyond it
typedef struct {
    float    mean;         // combined per-run raw z mean over the loop
    float    sigma;        // combined per-run σ (== loop_sigma), ideal 1.0
    uint8_t  nodes;        // nodes contributing to this loop (master included)
    // Per node, index 0 = master. A node that did not take part leaves zeros,
    // which is distinguishable from a measured 0 by `nodes` and by sig_n == 0.
    float    mean_n[MAX_NODES];   // per-node mean raw z over this block
    float    sig_n[MAX_NODES];    // per-node per-run σ over this loop, ideal 1.0
    float    cam_mbit[MAX_NODES]; // camera rate at loop end, 0 = not answered
    uint32_t cam_stalls[MAX_NODES];
    uint32_t t_s;          // elapsed seconds at loop end
    // Camera settings this loop was MEASURED AT (§1.5.2, and
    // mandatory there rather than optional). Per-loop re-tuning is what tracks
    // thermal drift, and recording the setting keeps the statistics auditable — but a
    // per-loop change nobody logged is indistinguishable from drift in the data,
    // so the setting travels with the loop it produced.
    /* Per-node die temperature at this block's close: the covariate any moved
     * offset has to be read against. Without it "something moved" is all a
     * block-to-block change can ever say.
     * ⚠ NAN = that node reported no temperature. */
    float    die_temp[MAX_NODES];
    uint32_t cam_exp[MAX_NODES];    // 0 = not calibrated this loop
    uint16_t cam_gain[MAX_NODES];
    uint8_t  cam_cal_ok[MAX_NODES]; // 0 = kept its previous setting, no gate passed
    float    cam_bias[MAX_NODES];   // bias of the window that chose it
    /* ── What the camera actually did DURING this block ───────────────────
     * Everything above is what the last SWEEP measured — `cam_bias` is the bias
     * of the window that chose the rung, so two consecutive blocks on the same
     * setting carry byte-identical values and say nothing about either. These
     * three are read at block CLOSE, from the master's own camera_get_stats()
     * and each slave's 'D' reply, i.e. they describe the block's own bits.
     * ⚠ `cam_px` separates "the lamp moved" from "the sensor moved". 0 = not
     * reported (a slave on firmware older than 2026-08-28 sends no,px= field),
     * which is NOT the same as a dark frame.
     * ⚠ `cam_rsig` is the LSB σ. It has no null of 1 to be read against
     * and is non-stationary per node minute to minute  — record it, plot
     * it against its own history, never gate on its absolute level. */
    float    cam_sig[MAX_NODES];    // per-mini-run σ during the block
    float    cam_rsig[MAX_NODES];   // LSB per-mini-run σ during the block
    float    cam_px[MAX_NODES];     // mean raw pixel level; 0 = not reported
    uint16_t cal_ms;       // wall time the calibration cost AT THE TOP OF THIS
                           // LOOP. 0 = no sweep ran here (interval not elapsed,
                           // or ?cal=0): the cam_* fields above are then the
                           // setting carried over from an earlier loop, which
                           // is still the operating point this loop measured at
    // The measured run window and inter-run gap OF THIS LOOP. Recorded per loop
    // because the count→duration conversion is not stable (open item 4) and
    // per-loop calibration moves the camera's rate on purpose (§1.5.3), so the
    // series across loops is the only way to see the window drift rather than
    // average it away. Measured in every session.
    float    win_ms, gap_ms;
    /* ── Who was in the combine, and why ──────────────────────────────────
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

/* One camera-sigma JUMP: a single measurement on a single node, where that
 * node's window sigma moved furthest from what the same node measured on the
 * item before it.
 *
 * Why the jump and not the level: the level drifts slowly with the operating
 * point, so an absolute bar would flag one node's rung rather than an event.
 * A jump is differenced against that node's own previous window and so is
 * blind to where it sits. A disturbance that lasts produces a jump up when it
 * starts and one down when it ends — both are real, both belong on the board,
 * which is why it ranks |jump| and keeps the sign in prev/now.
 *
 * ⚠ `prev` is the previous item THIS NODE reported a window sigma for, not
 * the previous item overall: a node that voided or was unreachable in
 * between leaves a gap, and the jump then spans it.
 * ⚠ nums/euro are copied in rather than looked up later, because results[]
 * is compacted at every round boundary and the row would be gone. This board
 * has to survive that — it exists precisely because the rows do not. */
/* What a soft-down trip was MADE OF.
 *
 * A trip is a property of a whole block: sigma is the spread of one node's z
 * over that round's ?maxruns= items, and it does not exist until the block
 * closes. The answer is taken at the only moment it exists: inside
 * record_loop(), where the block's items are still in results[] and their
 * per-node z is still in the archive. Same trick as the jump board — copy what
 * names the measurement, do not hope the row survives.
 *
 * ⚠ `dev` is (z - block mean) / block sigma: how far that item sat from the
 * middle of the very spread it helped create. It is NOT a z-score against the
 * null and must not be read as one. At n=`?maxruns=` (default 100) a value near 3 is ordinary;
 * the point is the SHAPE — ONE item far out is a single excursion, three of
 * them close together mean the block was simply wide. */
typedef struct {
    uint16_t round, index;
    uint8_t  nums[6], euro[2];
    float    z;          // that node's RAW z on this item
    float    dev;        // (z - block mean) / block sigma
} TripItem;

#define TRIPX_TOP_N 3    // items per trip: enough to tell one spike from a wide block
#define TRIPX_MAX   6    // trips remembered per session

typedef struct {
    uint16_t block;      // 1-based, the number /loops shows, NOT results[].block
    uint8_t  node;       // discovery order, 0 = master
    uint8_t  n;          // items filled, 0..TRIPX_TOP_N
    float    sigma;      // the block sigma that tripped
    float    mean;       // the block mean it was measured about
    /* Board uptime at the trip (ms). No RTC: the UI does
     * wall = now − (uptime_ms − t_ms). 0 = not stamped (pre-this-field). */
    int64_t  t_ms;
    TripItem it[TRIPX_TOP_N];
} TripRec;

typedef struct {
    uint16_t round;      // (round, index) is the identity in unlimited mode
    uint16_t index;      // combination id WITHIN that round
    uint8_t  node;       // discovery order, 0 = master
    uint8_t  counted;    // 1 = this node was in that item's combine
    uint8_t  nums[6];
    uint8_t  euro[2];
    /* 0 = an item of the pass; 1..SCORE_PASSES = a scoring run in that pass,
     * `index` is then the scored number (in nums[0], or euro[0] for a bonus
     * number) and (round, index) is NOT an item identity. */
    uint8_t  spass;
    float    prev;       // that node's window sigma on its previous item
    float    now;        // and on this one
    float    jump;       // now - prev; the board ranks |jump|
} WsigEvent;

/* One number of the scoring, in the pass's own row type: `r` is filled
 * exactly as a results[] row would be — index = the number, the number in
 * nums[0] (euro[0] for a bonus number), round, k, have_mask, z_score (raw),
 * z_ctr / zc_ctr (provisional raw until the pass closes, then centred on the
 * pass span, like an item on its block), node_sd (Δn), acz (AC), bac. What a
 * results[] row gets from its block instead is carried beside it: `key` (Z*,
 * the pass key in span-σ units — rank_key() would read s_bsig[] of a block
 * this row has none of) and `sum`, the running Σ the pool is picked on.
 * Holds the LATEST measurement of that number. `r.block` is unused.
 * ⚠ key is NaN while the pass holding the latest measurement is still open.
 * ⛔ Display only, and never in results[]: a scoring run is not an item and
 * must not reach pass statistics, blocks, the pairwise matrix or compaction.
 * The pool is picked from score_and_build_pool()'s own `acc`. Written by the
 * sensor task, read unlocked by the HTTP task: a torn row shows mixed values
 * for one poll, nothing else. */
/* What the pool is picked on: the running sum of ONE column over the
 * closed scoring passes, chosen by the operator on the page (POST /scoresum)
 * and read when the whole scoring ends. SUM_KEY (Z*) is the default. */
typedef enum { SUM_KEY = 0, SUM_Z, SUM_CONC, SUM_NSD, SUM_AC, SUM_BAC, SCORE_SUM_N } ScoreSum;

typedef struct {
    RunResult r;
    float     key;       // Z*: pass key in span-σ units, NaN while open
    /* Σ per column over the closed passes, indexed by ScoreSum. A pass whose
     * value is missing (Δn with < 2 nodes, AC not reported, no z) adds
     * nothing; sum_n says how many passes did add. */
    float     sums[SCORE_SUM_N];
    uint8_t   sum_n[SCORE_SUM_N];
    uint8_t   passes;    // closed passes of this number
} ScoreItem;

#define SCORE_ROWS_MAX 62   // 50 main + 12 bonus (Eurojackpot)

#define WSIG_TOP_N 5
/* ⚠ There is deliberately NO minimum jump. Noise-vs-finding is decided by
 * wsig_sd and the x-sigma column: five rows at 2..3 sigma ARE the quiet
 * session, stated in the units that say so, and a real event pushes one of
 * them into double digits. Publish the number, draw no verdict. */

typedef struct {
    ElottoState      state;
    ElottoPhase      phase;
    ElottoMode       mode;
    /* ROWS CURRENTLY IN results[] — not the session's item count. The two were
     * the same number until round-boundary compaction made results[] a subset
     * rather than a prefix. Everything that walks the array
     * bounds itself with this; everything that reports PROGRESS uses
     * items_done. ⚠ Using this one for progress makes the counter go backwards
     * the first time a compaction runs. */
    volatile int     runs_completed;
    /* Items measured this session, across every round. Monotone: a compaction
     * never lowers it, because the measurement happened. This is what /status
     * publishes as `completed` and what round_item_base is taken from. */
    volatile int     items_done;
    /* Items dropped by compaction, i.e. measured and merged into the pass
     * statistics but no longer individually in results[]. 0 for any session
     * that never filled the buffer, which is most of them.
     * ⚠ Non-zero means results[] holds the extremes plus whatever else
     * survived, not a sample of the session. Never compute a distribution
     * from its rows. */
    int              compacted;
    int              runs_total;       // combinations in the CURRENT round
    /* ── Unlimited mode (see the block near the top of this file) ──────
     * `unlimited` and `runs_cap` are session parameters written by /start and
     * NOT reset by elotto_task — they are the session's tag.
     * The rest is per-round bookkeeping the UI reads. */
    bool             unlimited;        // rounds repeat until Abort / results full
    int              runs_cap;         // measurement runs a round may spend
    int              round;            // 1-based; 0 before the first round starts
    /* elapsed_ms at the moment this round began. Same clock elapsed_ms itself
     * runs on, so paused time is already excluded from any difference taken
     * against it. 0 outside unlimited mode and before the first round.
     * The UI's progress panel is round-relative in items, percentage and ETA;
     * without this its clock was the only figure still session-relative. */
    uint32_t         round_start_ms;
    int              round_item_base;  // items_done when this round started. The
                                       // ITEM-space twin of round_base, and the
                                       // only one a progress figure may use:
                                       // round_base is an index and compaction
                                       // moves the two apart
    int              round_base;       // results[] index this round started at.
                                       // ⚠ An INDEX, so it comes from
                                       // runs_completed. items_done counts
                                       // ITEMS and compaction makes the two
                                       // diverge; see the note at the
                                       // assignment in sensor.c
    int              round_total;      // == runs_total, published separately so a
                                       // reader never has to know which one moved
    int64_t          elapsed_ms;
    volatile int     scoring_done;
    int              scoring_total;
    int              scoring_pass;        // 1..SCORE_PASSES while scoring, else 0
    int              scoring_passes;      // SCORE_PASSES, published so the UI does not hardcode it
    uint32_t         scoring_start_ms;    // elapsed_ms when this round's scoring began,
                                          // for the scoring's own Time / ETA card
    /* The scoring table (see ScoreItem): one row per number of this round's
     * scoring, cleared at every round start. score_sig_z / score_sig_c are the
     * last closed scoring pass's own channel σ (the span σ its keys divide by),
     * score_span_n the numbers in that pass — the scoring's health line. */
    /* How the pool now being measured was picked: its size (main +
     * bonus), the column its sum was on and the round. Set at the pick, kept
     * until the next one — unlike pool_main/_euro, which the next scoring
     * replaces live. 0 before the first pick. */
    int              pool_used_n, pool_used_sum, pool_used_round;
    volatile int     score_sum;           // ScoreSum picking the pool; loaded
                                          // from NVS at boot, kept across sessions,
                                          // set + stored by POST /scoresum
    ScoreItem       *score_rows;          // SCORE_ROWS_MAX, PSRAM (internal RAM is the
                                          // ring's); NULL = allocation failed
    int              score_rows_n;
    double           score_sig_z, score_sig_c;
    int              score_span_n;
    int              score_conc_n;        // numbers of that pass carrying a concordance
    /* Item autocorrelation: z_L = r_L·√(n−L) of the centred, block-σ
     * scaled z series in MEASUREMENT order, lags 1..4 — the last closed pass
     * block (item_ac_*) and the last closed scoring pass (score_ac_*). Unit
     * normal for independent items; Bancel's lag-1 test on the GCP series. */
    float            item_ac_z[4];
    int              item_ac_n, item_ac_block;   // items, 1-based block; 0 = none yet
    float            score_ac_z[4];
    int              score_ac_n;
    int              comparisons;         // == VALID items so far (voids excluded)
    /* ── Pass-level health (GCP primary endpoints) ─────────────────────
     * Under H₀ with a working instrument: mean ≈ 0, σ ≈ 1, Σz² ≈ n.
     * Ranking is secondary: read these three before any table. Updated after
     * every valid item from the valid prefix.
     * ⚠ They are PUBLISHED, not enforced. The software draws no
     * verdict from them and excludes nothing on them — exclusion is soft-down
     * and block quarantine, and neither reads this block. */
    double           pass_mean;           // mean of valid rank_z() (z_ctr) so far
    double           pass_sigma;          // sample σ (df = n−1) of valid z_ctr
    double           pass_chi2;           // Σ z² over valid items (≈ χ²(n) under H₀)
    double           pass_stouffer;       // mean · √n — test of a common offset
    int              pass_n_valid;        // ranked items in CLOSED blocks — the set
                                          // every pass statistic is computed over
    /* Ranked items whose block is still OPEN. Measured and archived, but not yet
     * assessable: z_ctr holds the provisional RAW value until close_block()
     * centres it, so these carry the per-node offsets and belong in no
     * statistic. They join pass_n_valid at the next block close. ⚠ Published so
     * "measured" and "assessed" can be told apart — they differ by up to one
     * block, and a reader who assumes they are the same will misread n. */
    int              pass_n_open;
    int              pass_n_void;         // incomplete combines (k=0), archived only
    int              pass_n_excl;         // k>0 but skip_rank (trigger-block quarantine)
    /* The camera-sigma jump board. Biggest |jump| first, session-wide.
     * ⚠ This is a SUSPICION list, not a ranking: what stands at the top is
     * the item whose bits were least quiet while they were taken, i.e. the
     * one whose z deserves the least trust. It must never be read like
     * top[]/low[], and it excludes nothing by itself — the software
     * publishes the number and draws no verdict. */
    /* What each soft-down trip was made of. Oldest kept, newest dropped
     * once full: the FIRST trip of a session is the one worth keeping — a
     * sticky node that keeps failing its gate produces all the rest. */
    TripRec          trip_hist[TRIPX_MAX];
    int              trip_n;
    WsigEvent        wsig_top[WSIG_TOP_N];
    int              wsig_n;              // entries in use, 0..WSIG_TOP_N
    /* Measured spread of the jump itself, over every node-item of this
     * session. It is what turns a jump into a judgement.
     * ⚠ Published so the READER can scale, not so the software can: nothing
     * gates on it. Without it a board full of 3-sigma noise looks exactly
     * like a board holding one real event. */
    double           wsig_sd;
    int              wsig_sd_n;           // node-items behind wsig_sd
    double           v_eff;               // Var(Σz_i/√k) under measured σ and r
                                          // (1.0 = independent unit nodes)
    /* ── Concordance ranking weight ────────────────────────────────
     * ⚠ It RANKS. It does not test. pass_mean/pass_sigma/pass_chi2 stay
     * on z alone. */
    double           pre_w;               // concordance weight, ?wpre=
                                          // (0 = pure-z ranking)
    int              pre_n;               // ranked items carrying a CONCORDANCE
                                          // value (zc_ctr != 0). 0 on an item
                                          // means the leave-one-out drop left
                                          // k < 2, so only the z term ranked it
    double           loop_sigma;          // per-run σ of the LAST CLOSED BLOCK (1.0 = ideal)
    int              loops_done;          // BLOCKS closed and merged into the drift stats
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
    RunResult        top[TOP_N];          // highest rank_key so far, desc
    int              low_count;           // valid entries in low[] (published)
    RunResult        low[TOP_N];          // lowest rank_key so far, asc
    volatile bool    abort_requested;
    // ── Current-item display (always on; session is unattended, D66) ──
    volatile bool    paused;              // hold BETWEEN runs (never inside one)
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
     * ONSET_SETTLE_MS. Published in /status: a silent
     * safeguard that fires is indistinguishable from one that never had to. */
    uint32_t         flush_timeouts;   /* per SESSION -- cleared at session start */
    int              run_segments;        // segment count derived for this session
    FocusState       focus;
    bool             slave_connected;     // at least one slave answered discovery
    int              node_count;          // nodes discovered, master included (>= 1)
    int              node_ok;             // of those, still contributing
    NodeStatus       nodes[MAX_NODES];    // [0] = master
    // UDP transport health. The rule it implements: "UDP loss must be
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

    /* ── Which side went quiet ─────────────────────────────────────────
     * A drop says a node stopped answering. It does NOT say whether the node
     * went away or the master's own link did.
     *
     * eth_* are LIFETIME, deliberately: the link event that ends a session is
     * often the one that happened before it started, and a per-session counter
     * would have been cleared by then.
     *
     * ⚠ Timestamps are esp_timer uptime, not wall clock — this rig has no RTC
     * and no SNTP. `uptime_ms` travels in every /status precisely so they can
     * be converted: wall = now − (uptime_ms − stamp). */
    bool             eth_up;              // PHY link as of the last ETHERNET_EVENT
    uint32_t         eth_downs;           // DISCONNECTED events since boot
    uint32_t         eth_lost_ips;        // IP_EVENT_ETH_LOST_IP since boot
    int64_t          eth_last_down_ms;    // uptime at the last DISCONNECTED, -1 never
    int64_t          eth_last_up_ms;      // uptime at the last CONNECTED, -1 never

    /* Stamped by the FIRST node drop of a session and then left alone: the
     * first one is the diagnostic, the rest are its consequences. Per session. */
    int64_t          drop_uptime_ms;      // uptime at that drop, -1 = none yet
    bool             drop_eth_up;         // master's own link at that moment
    uint32_t         drop_eth_downs;      // eth_downs as of that moment, so a
                                          // link bounce that already healed is
                                          // still visible after the fact
    int              drop_node;           // node index that went first, -1 none
    // ── Camera calibration (round-boundary sweep) ─────────────────────
    int              cal_budget_ms;       // sweep budget per round boundary, 0 = do not
                                          // calibrate. A no-calibration session
                                          // is the matched control this change
                                          // has to be compared against, so it is
                                          // a session parameter, not a #define
    int              cal_interval_ms;     // current dynamic interval 
    int              cal_due_ms;          // until the next sweep is due, 0 = due
                                          // (refreshed at every candidate point)
    int              cal_ms;              // what the last sweep actually cost,
                                          // master + ack wait —
                                          // the sweep-cost gate is a measured number
    bool             cal_did_sweep;       // did THIS round-boundary calibrate? Recorded per
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
    volatile int64_t settle_end_us;       // the post-sweep settle pause ends here,
                                          // 0 when none is running.
                                          // Published as settle_left_ms
    volatile bool    noise_stalled;      // the array lost too many cameras to carry
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
    /* ── The pool scoring proposed ─────────────────────────────────────
     * Scoring picks a pool and publishes it here for /status and the UI.
     * ⛔ There is NO confirmation gate: the pass starts on the proposal and
     * `POST /pool` answers 400. The web form's `confirm=1` is only a "save my
     * form values" marker, handled as a local in the /start handler.
     *
     * Keeping fewer numbers is legitimate and shrinks the combination space
     * exactly: at pool_n_main == pool_need_main (and, for Eurojackpot,
     * pool_n_euro == 2) there is exactly ONE combination — measured once per
     * round, like everything else. */
    uint8_t          pool_main[POOL_MAIN_49];   // proposed/confirmed main numbers
    float            pool_main_z[POOL_MAIN_49]; // their scoring z, for display
    uint8_t          pool_euro[POOL_EURO_12];   // Eurojackpot bonus pool
    float            pool_euro_z[POOL_EURO_12];
    uint8_t          pool_n_main;         // slots filled
    uint8_t          pool_n_euro;
    uint8_t          pool_need_main;      // a draw needs this many (5 or 6)
    uint8_t          pool_need_euro;      // 2 for Eurojackpot, 0 for 6-of-49
    LoopStat        *loop_hist;           // per-block health table (LOOP_HIST entries,
                                          // PSRAM; NULL if the allocation failed, in
                                          // which case only the drift/σ aggregates exist)
    /* The pass, in MEASUREMENT order: results[j] is the j-th item measured
     * (its combination id is results[j].index). Compact by construction, so
     * the prefix [0 .. runs_completed) is always the complete record — an
     * abort needs no compaction. */
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

/* Create the archive mutex that serialises pass_compact() against the archive
 * reader on the HTTP task (results_extremes). Called once from app_main before
 * any HTTP reader can run. Eager, not lazy: a heap failure here is a loud
 * startup error instead of a silent return to unlocked behaviour on the first
 * poll. Idempotent. */
void results_archive_init(void);

/* The combined ranking key of one row, in units of that item's own block σ
 *: block-centred z and concordance, weighted by pre_w. The ONE accessor
 * every table and every survivor choice goes through, for the same reason
 * rank_z() is the only reader of z_ctr. */
double rank_key(const RunResult *r);

/* The up-to `max` most extreme ranked & centred rows by |rank_key| (both
 * tails), copied into out[0..return) under the archive lock. The live form of
 * the compaction survivors, for the sortable Top-10. The
 * caller sorts on whichever column it displays; selection here is by |Z*|
 * only, so a newly measured item joins the set exactly as it did before. */
int results_extremes(RunResult *out, int max);
