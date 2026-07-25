#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "sensor.h"
#include "camera.h"
#include "elotto_link.h"

ElottoStatus g_status = { .state = ELOTTO_IDLE };

// Per-combination running Σz across loops (cumulative / Stouffer ranking mode)
static double s_zsum[NUM_RUNS];

// Random measurement order for the current loop (Fisher–Yates, rebuilt per
// loop) — decouples slow TRNG drift from fixed combination indices, so drift
// cannot accumulate coherently on specific combinations across loops
static uint16_t s_perm[NUM_RUNS];

// Direct TRNG register access (75× faster than esp_random)
#define RNG_REG  (*((volatile uint32_t *)0x501101A4UL))

static inline uint32_t fast_rng(void) { return RNG_REG; }

#define TRNG_PER_RUN   200000
#define SEGMENT_BITS   200
#define NUM_SEGMENTS   ((TRNG_PER_RUN * 32) / SEGMENT_BITS)   // 32000

/* Segments per run is source- AND phase-dependent (PLAN_4NODE Phase 1 + 5).
 *
 * Phase 5 made the run length the *display* window: the Focus panel holds a
 * target for exactly as long as its bits are collected, so "1000 ms per
 * candidate number, 500 ms per draw" is not a delay bolted onto a run — it IS
 * the run. Padding with a vTaskDelay would halve the bit rate for nothing.
 *
 * The camera counts are CALIBRATED ON HARDWARE, not derived on paper — the
 * Phase 5 gate insists on that, and the paper answer was wrong by 60 %.
 *
 * What makes the paper answer wrong is that the sustained rate is not a
 * constant: the measurement loop runs one priority ABOVE the camera extraction
 * task it consumes from, so the harder it consumes the more it starves its own
 * producer. Measured on the live 4-node array (2026-07-25, all four on camera,
 * a client polling /focus at 10 Hz), per-cycle sustained rate against duty
 * cycle (run / (run + gap)):
 *
 *      n=6350   run  277 ms   duty 57 %   2.63 Mbit/s
 *      n=11600  run  588 ms   duty 72 %   2.82 Mbit/s
 *      n=17000  run 1519 ms   duty 88 %   1.98 Mbit/s   <- collapsed
 *
 * So it is flat near 2.7 Mbit/s and falls off a cliff somewhere past ~72 %, at
 * which point longer runs feed back into a slower producer and get longer
 * still. None of this is visible in Phase 0's 3.49 Mbit/s idle figure. The
 * counts below are solved from the flat part — n = rate × (window + gap) —
 * which also keeps the source out of the starved regime, where PLAN_4NODE's
 * open item 3 (camera bias degrading under sustained load) lives.
 *
 * This is the other reason RUN_GAP_MS is not merely cosmetic: the gap is when
 * the producer gets the CPU back, so it buys back most of what it costs.
 *
 * The TRNG counts are NOT re-measured; they are carried from Phase 1's "a run
 * is ~1 s at 32000 segments" and halved for the 500 ms window. The camera is
 * the default source, and the TRNG path exists for A/B — if a TRNG Focus
 * session is ever run for real, measure focus_win_ms and correct these.
 *
 * z stays N(0,1) at any of these because it is normalised by √segments; run
 * length only sets granularity, and statistical power per second is
 * rate-limited either way. */
#define TRNG_SCORE_SEGMENTS  NUM_SEGMENTS   // 6.4 Mbit/run ≈ 1000 ms (extrapolated)
#define TRNG_MEAS_SEGMENTS   16000          // 3.2 Mbit/run ≈  500 ms (extrapolated)
#define CAM_SCORE_SEGMENTS   11950          // 2.4 Mbit/run ~ 1000 ms measured,
                                            // at the 350 ms scoring gap
#define CAM_MEAS_SEGMENTS     6400          // 1.3 Mbit/run ~  500 ms measured

// Which source the running session is actually drawing measurement bits from.
// Distinct from g_status.noise_source (what was *requested*).
static volatile int s_active_source = NOISE_TRNG;

/* Camera stall policy (PLAN_4NODE "Fallback policy" + PLAN_NETWORK §5).
 *
 * Substituting the TRNG for a node's camera would swap the measured physics —
 * the whole point of camera entropy is replacing the opaque whitened TRNG with
 * raw quantum noise — and a session-level flag cannot say *which* runs were
 * affected. So a node whose source degrades is DROPPED from the combine rather
 * than averaged in; the nodes that remain are all still camera-sourced, which
 * keeps the session source-clean by construction.
 *
 * Aborting is the right response only while dropping would leave too few nodes.
 * At n=2 losing one halves the instrument, which is why Phase C aborted; at
 * n>=3 the rest carry on over √(n−1) and aborting would be needless. Both are
 * the same rule with a different floor. */
static void node_source_lost(int node)
{
    if (node < 0 || node >= g_status.node_count || !g_status.nodes[node].ok) return;
    g_status.nodes[node].ok = false;
    g_status.node_ok--;
    printf("node %d (%s): source fell back to TRNG -- dropped, %d node(s) left\n",
           node, node ? g_status.nodes[node].ip : "master", g_status.node_ok);

    // A solo master has nothing to fall back to, so its floor is 1; any array
    // that drops below two nodes has stopped being the instrument it started as.
    int floor_n = (g_status.node_count >= 2) ? 2 : 1;
    if (g_status.node_ok < floor_n) {
        g_status.noise_stalled   = true;
        g_status.abort_requested = true;
    }
}

static void noise_camera_stalled(void)
{
    node_source_lost(0);
    // The master keeps running full-length runs either way (it paces the loop),
    // but switching its local source stops every remaining word from re-paying
    // the 2 s stall timeout. Its z is simply no longer folded into the combine.
    s_active_source = NOISE_TRNG;
}

static inline uint32_t noise_word(void)
{
    if (s_active_source == NOISE_CAMERA) {
        uint32_t w;
        if (camera_read_word(&w)) return w;
        noise_camera_stalled();
    }
    return RNG_REG;
}

// Called once per session, after discovery, before any measurement.
static void noise_source_begin(void)
{
    g_status.noise_stalled = false;
    g_status.nodes[0].src  = g_status.noise_source;
    if (g_status.noise_source == NOISE_CAMERA) {
        if (camera_is_ready()) {
            s_active_source = NOISE_CAMERA;
        } else {
            // Refuse to run a "camera" session on TRNG bits.
            noise_camera_stalled();
        }
    } else {
        s_active_source = NOISE_TRNG;
    }
}

static const char *p_label(double absZ)
{
    if (absZ > 3.29) return "p&lt;0.001";
    if (absZ > 2.58) return "p&lt;0.01";
    if (absZ > 1.96) return "p&lt;0.05";
    if (absZ > 1.28) return "p&lt;0.10";
    return "n.s.";
}

/* Segments for one run of the given phase. Scoring holds a single candidate
 * number twice as long as a draw, because that is the Focus spec's window. */
static int segments_for(bool scoring)
{
    if (s_active_source == NOISE_CAMERA)
        return scoring ? CAM_SCORE_SEGMENTS : CAM_MEAS_SEGMENTS;
    return scoring ? TRNG_SCORE_SEGMENTS : TRNG_MEAS_SEGMENTS;
}

static double gcp_zscore_raw(int nseg)
{
    double z_sum = 0.0;
    for (int seg = 0; seg < nseg; seg++) {
        int ones = __builtin_popcount(noise_word())
                 + __builtin_popcount(noise_word())
                 + __builtin_popcount(noise_word())
                 + __builtin_popcount(noise_word())
                 + __builtin_popcount(noise_word())
                 + __builtin_popcount(noise_word())
                 + __builtin_popcount(noise_word() & 0xFF);
        z_sum += (ones - 100.0) / 7.07106781;
        // TRNG path needs explicit yields (4/run); the camera path already
        // yields inside camera_read_word() whenever it waits on the producer,
        // which is most of the time. Kept as a fraction of the run rather than
        // a fixed count so it stays matched to the slave's cadence at every
        // run length — per-run wall time is the max over nodes, so a mismatch
        // would slow every measurement to the slowest device.
        if (seg % (nseg / 4 + 1) == 0) vTaskDelay(1);
    }
    return z_sum / sqrt((double)nseg);
}

/* ── Focus display + pause (docs/PLAN_4NODE.md Phase 5) ───────────────────
 *
 * The panel shows what is being measured, WHILE it is being measured. That is
 * the whole content of the experiment: it makes the observer part of the
 * measurement window, as in the original GCP/PEAR protocol. Statistically it
 * changes nothing (z is normalised by √segments at any run length), which is
 * why the session is merely *tagged* rather than analysed differently.
 *
 * The invariant that has to hold is: panel lit ⟺ this run's bits are being
 * collected. So focus_publish() runs immediately before the trigger goes out
 * and focus_off() immediately after the local run returns — the reply wait and
 * the per-run bookkeeping are dark time, and the observer should not spend them
 * attending to a target whose measurement has already finished. */
/* Dark time between targets. Phase 5 asked for this to be *aligned* with
 * overhead that already existed rather than added on top — the estimate was
 * ~190 ms per run spent on the slave round-trip and bookkeeping. Measured on
 * the 4-node array, that overhead is **2.3 ms**: the slaves integrate the same
 * window concurrently and are already answering by the time the master's own
 * run returns, so nodes_collect() costs nothing. The gap therefore has to be
 * paid for, which the plan anticipated ("then pay for it in the run budget"),
 * and `Runs = 850` is what pays for it.
 *
 * It is not only a display concern. At ~100 % duty cycle the measurement loop
 * starves the camera extraction task it consumes from (it runs one priority
 * above it), and the sustained rate collapses from 3.49 to 2.68 Mbit/s —
 * measured, with ring `waits` climbing. The blank period is when the producer
 * gets the CPU back, so the gap buys back most of the throughput it costs.
 *
 * Applied to EVERY run — baseline, scoring and measurement — and independently
 * of focus_mode. Two reasons: a matched no-focus control must differ from an
 * attended session in the display and nothing else, and the baseline has to be
 * the same instrument as the runs it is subtracted from, down to duty cycle.
 *
 * SCORING GETS A LONGER GAP, and this is forced, not a preference. The spec's
 * 1000 ms scoring window at a 200 ms gap is 83 % duty *by construction* —
 * already past the ~72 % cliff above — so no segment count reaches 1000 ms
 * there: every candidate value lands in the collapsed regime and stretches to
 * ~1500 ms instead (measured at both 16700 and 17000 segments). Widening the
 * scoring gap to 350 ms puts the same 1000 ms window at 74 % duty, back on the
 * flat part, where a segment count *does* solve.
 *
 * The alternative was to shorten the scoring window until it fitted a 200 ms
 * gap. That was rejected: the gap is the soft parameter the plan already
 * marked as adjustable ("then pay for it in the run budget"), whereas the hold
 * times are the spec, and buying a display number by running the entropy
 * source in a starved regime trades physics for cosmetics — that regime is
 * where PLAN_4NODE's open item 3 lives. Scoring is loop-0 only, so the extra
 * 150 ms costs one session ~37 s (245 runs at 6/49).
 *
 * Safe to differ per phase because scoring does not use the baseline at all
 * (score_one_run() subtracts nothing — the offset is common to every number
 * and scoring only ranks them). Measurement and its baseline both stay at
 * 200 ms, which is the pairing that has to match. */
#define RUN_GAP_MS        200
#define SCORE_RUN_GAP_MS  350

static void run_gap(bool scoring)
{
    vTaskDelay(pdMS_TO_TICKS(scoring ? SCORE_RUN_GAP_MS : RUN_GAP_MS));
}

static int64_t s_t0;               // session start
static int64_t s_paused_us;        // total time held by pause, excluded from elapsed
static int64_t s_focus_on_us, s_focus_off_us;
static double  s_win_sum, s_gap_sum;
static uint32_t s_win_n, s_gap_n;

static int64_t elapsed_ms_now(void)
{
    return (esp_timer_get_time() - s_t0 - s_paused_us) / 1000;
}

static void focus_reset(void)
{
    memset(&g_status.focus, 0, sizeof(g_status.focus));
    s_focus_on_us = s_focus_off_us = 0;
    s_win_sum = s_gap_sum = 0.0;
    s_win_n = s_gap_n = 0;
    g_status.focus_win_ms = g_status.focus_gap_ms = 0.0f;
    g_status.paused    = false;
    g_status.paused_ms = 0;
    s_paused_us = 0;
}

/* Put a target on screen. `seq` is bumped LAST and `active` with it, so a
 * reader that sees a given seq sees the numbers that belong to it (the /focus
 * handler re-reads seq to detect the race rather than taking a lock on the
 * measurement path). */
static void focus_publish(FocusKind kind, const uint8_t *nums, int n,
                          const uint8_t *euro, int ne)
{
    if (!g_status.focus_mode) return;      // unattended session: panel stays dark
    FocusState *F = &g_status.focus;
    // Scoring and measurement have different windows by design (1000 ms vs
    // 500 ms), so the timing means are per phase — pooled they would describe
    // neither, and the gate is stated separately for each.
    if (F->kind != (uint8_t)kind) {
        s_win_sum = s_gap_sum = 0.0;
        s_win_n = s_gap_n = 0;
        s_focus_off_us = 0;
    }
    F->active = 0;
    __sync_synchronize();
    F->kind = (uint8_t)kind;
    F->n    = (uint8_t)n;
    F->ne   = (uint8_t)ne;
    for (int i = 0; i < n  && i < 6; i++) F->nums[i] = nums[i];
    for (int i = 0; i < ne && i < 2; i++) F->euro[i] = euro[i];
    __sync_synchronize();
    F->seq++;
    F->active = 1;

    int64_t now = esp_timer_get_time();
    if (s_focus_off_us) { s_gap_sum += (double)(now - s_focus_off_us); s_gap_n++; }
    s_focus_on_us = now;
}

static void focus_show_number(int value, bool is_euro)
{
    uint8_t v = (uint8_t)value;
    if (is_euro) focus_publish(FOCUS_NUMBER, NULL, 0, &v, 1);
    else         focus_publish(FOCUS_NUMBER, &v, 1, NULL, 0);
}

/* Blank to the fixation mark. Not "nothing": the gaze stays anchored where the
 * next target will appear, and onset is the payload. */
static void focus_off(void)
{
    FocusState *F = &g_status.focus;
    if (!F->active) return;
    F->active = 0;
    int64_t now = esp_timer_get_time();
    if (s_focus_on_us) { s_win_sum += (double)(now - s_focus_on_us); s_win_n++; }
    s_focus_off_us = now;
    if (s_win_n) g_status.focus_win_ms = (float)(s_win_sum / s_win_n / 1000.0);
    if (s_gap_n) g_status.focus_gap_ms = (float)(s_gap_sum / s_gap_n / 1000.0);
}

/* Hold BETWEEN runs. Called where abort_requested is already tested, so the
 * current run always finishes and is kept: stopping mid-run would leave bits
 * sampled while nobody was watching inside a run labelled as attended, which is
 * the one contamination this phase exists to avoid.
 *
 * This is not abort — state stays running, nothing is published, and the
 * permutation index and Σz accumulation resume exactly where they left off.
 * The held time is subtracted from elapsed_ms so the ETA does not absorb the
 * break and the session is not later read as continuous. */
static void pause_gate(void)
{
    if (!g_status.paused) return;
    focus_off();                       // panel switches to its paused state
    int64_t p0 = esp_timer_get_time();
    while (g_status.paused && !g_status.abort_requested) {
        vTaskDelay(pdMS_TO_TICKS(100));
        g_status.paused_ms = (s_paused_us + esp_timer_get_time() - p0) / 1000;
    }
    s_paused_us += esp_timer_get_time() - p0;
    g_status.paused_ms  = s_paused_us / 1000;
    // The gap timer would otherwise charge the whole break to the inter-run gap
    // and make focus_gap_ms meaningless.
    s_focus_off_us = esp_timer_get_time();
}

// Binomial coefficient C(n, r) for small values (max n=15, r=6)
static int comb(int n, int r)
{
    if (r < 0 || r > n) return 0;
    if (r == 0) return 1;
    if (r > n - r) r = n - r;
    int res = 1;
    for (int i = 0; i < r; i++)
        res = res * (n - i) / (i + 1);
    return res;
}

// k-th combination (0-based, lexicographic) from sorted pool[0..n-1], r elements
static void nth_combination(const uint8_t *pool, int n, int r, int k, uint8_t *out)
{
    int start = 0;
    for (int i = 0; i < r; i++) {
        for (int j = start; j <= n - (r - i); j++) {
            int c = comb(n - 1 - j, r - 1 - i);
            if (k < c) {
                out[i] = pool[j];
                start = j + 1;
                break;
            }
            k -= c;
        }
    }
}

// Scores each number 1..max_val with SCORE_REPS GCP runs (Stouffer per number),
// node-combined like Phase 2 (÷√k), picks the top pool_size numbers (sorted
// ascending). The pool is locked for the whole cumulative session, so this is
// where selection confidence matters most: per-number SE = 1/√(REPS·k) over k
// nodes, i.e. 0.32 at 5 reps on two nodes, 0.22 at 10, 0.11 at 40 — against 1.0
// for a single master-only run. More nodes buy this back: 5 reps on four nodes
// is 0.22, the same as 10 reps on two.
//
// 5 keeps the one-time scoring phase short (~2 min for 6/49 at two nodes) while
// the array is being brought up and sessions are started often. It changes only
// WHICH numbers enter the pool, never the Phase-2 statistics measured on them —
// raise it for a session whose pool choice has to be trusted on its own.
#define SCORE_REPS 5
static double score_one_run(void);   // forward (defined after the slave link block)

// `euro_pool` selects the bonus-number pool; it only reaches the Focus panel,
// which styles a euro candidate differently from a main one.
static void score_and_build_pool(int max_val, int pool_size, uint8_t *pool,
                                 bool euro_pool)
{
    double scores[51] = {0};
    for (int k = 1; k <= max_val; k++) {
        double sum = 0.0;
        for (int r = 0; r < SCORE_REPS; r++) {
            if (g_status.abort_requested) return;
            pause_gate();
            if (g_status.abort_requested) return;
            // On screen before the run starts and until it ends — display and
            // bits cover the same interval, or the panel is decoration.
            focus_show_number(k, euro_pool);
            sum += score_one_run();
            g_status.scoring_done++;
            // Scoring never updated the clock, so elapsed_ms (and with it the
            // ETA) froze for the whole phase. That was survivable when scoring
            // was a preamble; Phase 5 makes it ~5.6 min of attended screen time
            // with a pause button, so the clock has to run here too.
            g_status.elapsed_ms = elapsed_ms_now();
            run_gap(true);
        }
        scores[k] = sum;   // ranking by Σz ≡ ranking by Stouffer Σz/√R
    }
    bool used[51] = {false};
    for (int i = 0; i < pool_size; i++) {
        int b = 0; double bs = -1e18;
        for (int j = 1; j <= max_val; j++)
            if (!used[j] && scores[j] > bs) { b = j; bs = scores[j]; }
        pool[i] = (uint8_t)b;
        if (b) used[b] = true;
    }
    // Sort ascending (for consistent combination enumeration)
    for (int i = 1; i < pool_size; i++) {
        uint8_t key = pool[i]; int j = i - 1;
        while (j >= 0 && pool[j] > key) { pool[j+1] = pool[j]; j--; }
        pool[j+1] = key;
    }
}

static int cmp_desc(const void *a, const void *b)
{
    const RunResult *ra = (const RunResult *)a;
    const RunResult *rb = (const RunResult *)b;
    if (rb->z_score > ra->z_score) return  1;
    if (rb->z_score < ra->z_score) return -1;
    return 0;
}

static int cmp_asc(const void *a, const void *b)
{
    return -cmp_desc(a, b);
}

/* ── Slave link — UDP broadcast (docs/PLAN_NETWORK.md §4, Phase C) ────
 * Replaces the UART1 point-to-point pair (was TX=GPIO14 / RX=GPIO15,
 * 460800 baud). A command leaves as ONE broadcast datagram, so every node
 * starts within microseconds of the others instead of N sequential UART
 * writes — the one difference that matters physically, since the premise is
 * that all nodes integrate the *same* window.
 *
 * The command semantics are byte-for-byte the ones the UART link carried
 * ('P'/'B'/'M'/'D'/'A' and their 'OK' / 'Z:' / 'D:' answers). Nothing above
 * this block changed, which is what makes Phase C a controlled A/B: if pair_r
 * or sigma move against the UART-era numbers, the transport moved them.
 *
 * Loss is handled explicitly, never assumed away (Risk 3). See elotto_link.h
 * for why every frame carries the sequence number it answers.
 * ─────────────────────────────────────────────────────────────────── */
#define LINK_PROBE_TRIES   4      // discovery broadcasts before declaring solo
#define LINK_PROBE_MS    600
#define LINK_MEAS_MS    4000      // a run is ~0.5 s (camera) / ~1 s (TRNG)
#define LINK_DIAG_MS    1500

// Consecutive missed replies before a node leaves the session. Without it an
// unplugged node would cost every remaining run the full retry budget forever;
// the Phase D gate wants an unplug to *degrade* the array, not to slow it down.
#define NODE_MISS_LIMIT 3

static bool     s_slave_ok = false;   // at least one slave is participating
static int      s_sock     = -1;
static uint32_t s_seq;
static struct sockaddr_in s_bcast;
static struct { uint32_t seq; char cmd[24]; } s_pending;

/* Per-slave link state. g_status.nodes[k+1] is the published view of s_link[k];
 * this holds what only the transport needs. */
typedef struct {
    struct sockaddr_in addr;
    bool   replied;                    // answered the command in flight
    int    miss_streak;                // consecutive commands unanswered
    double z;                          // its z for the current run
    char   reply[ELOTTO_LINK_MAX];
} SlaveLink;

static SlaveLink s_link[MAX_SLAVES];
static int       s_nslaves;

static bool link_open(void)
{
    if (s_sock >= 0) return true;
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) { printf("link: socket() failed\n"); return false; }

    int on = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    struct sockaddr_in me = {
        .sin_family      = AF_INET,
        .sin_port        = htons(ELOTTO_LINK_MASTER_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_sock, (struct sockaddr *)&me, sizeof(me)) < 0) {
        printf("link: bind(%d) failed\n", ELOTTO_LINK_MASTER_PORT);
        close(s_sock);
        s_sock = -1;
        return false;
    }
    s_bcast.sin_family      = AF_INET;
    s_bcast.sin_port        = htons(ELOTTO_LINK_CMD_PORT);
    s_bcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    // Seeded, not started at zero: after a master reboot a slave must not
    // mistake a fresh command for a repeat of one it already answered and
    // serve a cached reply instead of measuring.
    s_seq = fast_rng();
    return true;
}

/* Discard whatever is queued. Called when a session starts, for the reason the
 * UART path called uart_flush_input(): the OK to the abort that ended the
 * previous session must not be waiting when this one begins. The sequence check
 * would drop it anyway — draining keeps net_stale meaningful as a live
 * indicator rather than a tally of last session's leftovers. */
static void link_drain(void)
{
    if (s_sock < 0) return;
    char buf[ELOTTO_LINK_MAX];
    struct sockaddr_in from;
    socklen_t fl;
    for (;;) {
        fl = sizeof(from);
        if (recvfrom(s_sock, buf, sizeof(buf) - 1, MSG_DONTWAIT,
                     (struct sockaddr *)&from, &fl) <= 0) return;
    }
}

static void link_send(uint32_t seq, const char *cmd)
{
    char msg[ELOTTO_LINK_MAX];
    int  n = elotto_link_pack(msg, sizeof(msg), seq, cmd);
    if (n > 0)
        sendto(s_sock, msg, n, 0, (struct sockaddr *)&s_bcast, sizeof(s_bcast));
}

/* Arm the socket's receive timeout for what is left of `deadline`. Returns false
 * when too little remains to wait on, so the caller stops instead of receiving.
 *
 * The 1 ms floor is load-bearing, not tidiness. lwIP converts SO_RCVTIMEO to
 * whole milliseconds — ((tv_usec + 500) / 1000) — and a resulting 0 means "no
 * timeout": sys_arch_mbox_fetch() documents zero as "wait infinitely". So a
 * window with under 500 µs left silently turns a bounded wait into a permanent
 * one. That is exactly how discovery hung after it had already found its node,
 * and the same pattern was latent in the Phase C code from the moment it
 * shipped — it would have fired the first time the master booted with no slave
 * powered on. */
static bool link_arm_timeout(int64_t deadline)
{
    int64_t left = deadline - esp_timer_get_time();
    if (left < 1000) return false;
    struct timeval tv = { .tv_sec  = (time_t)(left / 1000000),
                          .tv_usec = (suseconds_t)(left % 1000000) };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return true;
}

/* Which slave a datagram came from. Identity is the source address: replies are
 * unicast, so this is exact and needs no node id on the wire. */
static int link_node_of(const struct sockaddr_in *from)
{
    for (int k = 0; k < s_nslaves; k++)
        if (s_link[k].addr.sin_addr.s_addr == from->sin_addr.s_addr) return k;
    return -1;
}

/* Receive one reply to `seq` before `deadline`, from any known slave. Returns
 * the slave index, or -1 on timeout. A frame carrying a different sequence
 * number is a late answer to a command already given up on — counted and
 * dropped, never attributed to the run in flight. */
static int link_recv_any(uint32_t seq, char *out, int cap, int64_t deadline)
{
    for (;;) {
        if (!link_arm_timeout(deadline)) return -1;

        char buf[ELOTTO_LINK_MAX];
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s_sock, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;             // timeout — the deadline decides
        buf[n] = '\0';

        uint32_t rseq;
        char    *payload;
        if (!elotto_link_parse(buf, &rseq, &payload)) continue;   // foreign traffic
        if (rseq != seq) { g_status.net_stale++; continue; }

        int k = link_node_of(&from);
        if (k < 0) continue;              // answered, but not a node of this session
        snprintf(out, cap, "%s", payload);
        return k;
    }
}

/* Send a command to every node at once. The caller measures locally in parallel
 * and collects with nodes_collect(), so the trigger still goes out *before* the
 * master's own run starts — and one datagram starts all of them, which is the
 * whole reason this is not N sequential writes. */
static void nodes_send(const char *cmd)
{
    s_pending.seq = ++s_seq;
    snprintf(s_pending.cmd, sizeof(s_pending.cmd), "%s", cmd);
    for (int k = 0; k < s_nslaves; k++) s_link[k].replied = false;
    link_send(s_pending.seq, s_pending.cmd);
}

/* Gather replies from every node still marked ok. Returns how many answered.
 *
 * The resend is a broadcast, which sounds wasteful at n=4 but is not: a node
 * that already answered this sequence number replies from its cache without
 * measuring again, so only the node that actually missed it pays anything.
 *
 * A node that stays silent is dropped FOR THIS RUN (PLAN_NETWORK §4) — at n>=3
 * that is a degraded run over √(n−1), not a reason to end the session. After
 * NODE_MISS_LIMIT consecutive misses it leaves the session altogether, so an
 * unplugged node degrades the array instead of taxing every later run with the
 * full retry budget. */
static int nodes_collect(int timeout_ms, bool critical)
{
    int want = 0;
    for (int k = 0; k < s_nslaves; k++)
        if (g_status.nodes[k + 1].ok) want++;
    if (want == 0) return 0;

    int got = 0;
    for (int attempt = 0; attempt < 2 && got < want; attempt++) {
        if (attempt) {
            g_status.net_retries++;
            link_send(s_pending.seq, s_pending.cmd);
        }
        int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
        while (got < want) {
            char payload[ELOTTO_LINK_MAX];
            int k = link_recv_any(s_pending.seq, payload, sizeof(payload), deadline);
            if (k < 0) break;                                  // deadline reached
            if (s_link[k].replied || !g_status.nodes[k + 1].ok) continue;  // duplicate
            s_link[k].replied = true;
            snprintf(s_link[k].reply, sizeof(s_link[k].reply), "%s", payload);
            got++;
        }
    }

    for (int k = 0; k < s_nslaves; k++) {
        if (!g_status.nodes[k + 1].ok) continue;
        if (s_link[k].replied) { s_link[k].miss_streak = 0; continue; }
        g_status.nodes[k + 1].lost++;
        if (critical) g_status.net_lost++;
        if (++s_link[k].miss_streak >= NODE_MISS_LIMIT) {
            g_status.nodes[k + 1].ok = false;
            g_status.node_ok--;
            printf("node %d (%s): %d missed replies -- dropped, %d node(s) left\n",
                   k + 1, g_status.nodes[k + 1].ip, s_link[k].miss_streak,
                   g_status.node_ok);
            int floor_n = (g_status.node_count >= 2) ? 2 : 1;
            if (g_status.node_ok < floor_n) g_status.abort_requested = true;
        }
    }
    s_slave_ok = (g_status.node_ok > (g_status.nodes[0].ok ? 1 : 0));
    return got;
}

/* Discovery by broadcast: no static IP table to maintain, and a node on a
 * dynamic DHCP lease joins exactly like one on a static one. Every probe round
 * is a single datagram; several rounds because one can be lost, and every
 * distinct responder is a node. */
static void nodes_discover(void)
{
    s_nslaves            = 0;
    g_status.node_count  = 1;                 // the master itself
    g_status.node_ok     = 1;
    memset(s_link, 0, sizeof(s_link));
    memset(g_status.nodes, 0, sizeof(g_status.nodes));
    for (int i = 0; i < MAX_NODES; i++) g_status.nodes[i].src = -1;   // not yet reported
    g_status.nodes[0].ok = true;

    if (!link_open()) { g_status.slave_connected = s_slave_ok = false; return; }
    link_drain();

    for (int round = 0; round < LINK_PROBE_TRIES && s_nslaves < MAX_SLAVES; round++) {
        s_pending.seq = ++s_seq;
        snprintf(s_pending.cmd, sizeof(s_pending.cmd), "P");
        link_send(s_pending.seq, s_pending.cmd);

        // Collect for the WHOLE window rather than stopping at the first answer:
        // at n>1 the other nodes are exactly the ones that would be missed.
        int64_t deadline = esp_timer_get_time() + (int64_t)LINK_PROBE_MS * 1000;
        for (;;) {
            if (!link_arm_timeout(deadline)) break;

            char buf[ELOTTO_LINK_MAX];
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            int n = recvfrom(s_sock, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr *)&from, &fl);
            if (n <= 0) continue;
            buf[n] = '\0';

            uint32_t rseq;
            char    *payload;
            if (!elotto_link_parse(buf, &rseq, &payload)) continue;
            if (rseq != s_pending.seq || payload[0] != 'O') continue;
            if (link_node_of(&from) >= 0) continue;          // already known
            if (s_nslaves >= MAX_SLAVES) break;

            int k = s_nslaves++;
            s_link[k].addr = from;
            int idx = k + 1;
            inet_ntoa_r(from.sin_addr, g_status.nodes[idx].ip,
                        sizeof(g_status.nodes[idx].ip));
            g_status.nodes[idx].ok = true;
            g_status.node_count++;
            g_status.node_ok++;
        }
    }

    s_slave_ok = (s_nslaves > 0);
    g_status.slave_connected = s_slave_ok;
    printf("Nodes: %d (master", g_status.node_count);
    for (int k = 0; k < s_nslaves; k++) printf(" + %s", g_status.nodes[k + 1].ip);
    printf(")\n");
}

/* The segment count now travels ON THE WIRE (PLAN_4NODE Phase 5): it is no
 * longer a constant each side keeps its own copy of. With run length made
 * phase-dependent, a duplicated constant would let master and slave integrate
 * different windows while every published number stayed plausible — the exact
 * silent failure CLAUDE.md warned about. A slave told the length cannot
 * disagree about it. */
static void slave_baseline_start(int n, int nseg)
{
    if (!s_slave_ok) return;
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "B%d,%d", n, nseg);
    nodes_send(cmd);
}

static void slave_baseline_wait(void)
{
    if (!s_slave_ok) return;
    nodes_collect(g_status.baseline_total * 800 + 15000, true);
}

static void slave_trigger(int nseg)
{
    if (!s_slave_ok) return;
    char cmd[16];
    snprintf(cmd, sizeof(cmd), "M%d", nseg);
    nodes_send(cmd);
}

static void slave_abort(void)
{
    if (!s_slave_ok) return;
    // Fire and forget, twice: nothing waits for the answer (the caller is on its
    // way out), and a single dropped datagram would otherwise leave a node
    // grinding through a run whose result no one will read.
    nodes_send("A");
    link_send(s_pending.seq, s_pending.cmd);
}

/* Parse one node's "Z:<float>,<C|T>" reply into s_link[k].z. Returns false if
 * the node did not contribute a usable value this run. */
static bool node_take_z(int k)
{
    if (!s_link[k].replied) return false;
    const char *resp = s_link[k].reply;
    if (resp[0] != 'Z' || resp[1] != ':') return false;

    // The trailing source tag is optional so a pre-camera slave still parses;
    // atof() stops at the comma either way.
    const char *tag = strchr(resp, ',');
    int src = (tag && tag[1] == 'C') ? NOISE_CAMERA : NOISE_TRNG;
    g_status.nodes[k + 1].src = src;
    if (g_status.noise_source == NOISE_CAMERA && src == NOISE_TRNG) {
        node_source_lost(k + 1);
        return false;               // its bits came from the wrong physics
    }
    s_link[k].z = atof(resp + 2);
    return true;
}

/* Ask each node for its camera health (protocol 'D'). Only ever called between
 * loops, when the nodes are idle — never between an 'M' and its 'Z:' reply.
 * A missing/garbled answer is NOT treated as a disconnect: this is diagnostics,
 * and dropping a node over it would cost the session part of its SNR. */
static void slaves_diag(void)
{
    if (!s_slave_ok) return;
    nodes_send("D");
    nodes_collect(LINK_DIAG_MS, false);
    for (int k = 0; k < s_nslaves; k++) {
        NodeStatus *N = &g_status.nodes[k + 1];
        N->cam_mbit = 0.0f;
        if (!s_link[k].replied) continue;
        const char *resp = s_link[k].reply;
        if (resp[0] != 'D' || resp[1] != ':') continue;
        // "D:<ready>,<bias>,<sigma>,<mbit_s>,<stalls>,<stuck>,<C|T>"
        int   ready = 0;
        float bias = 0, sigma = 0, mb = 0;
        unsigned long st = 0, stuck = 0;
        if (sscanf(resp + 2, "%d,%f,%f,%f,%lu,%lu",
                   &ready, &bias, &sigma, &mb, &st, &stuck) < 5) continue;
        N->cam_mbit   = mb;
        N->cam_stalls = (uint32_t)st;
    }
    // A 'D' miss must not count toward the drop rule — it says nothing about
    // whether the node can still measure.
    for (int k = 0; k < s_nslaves; k++) s_link[k].miss_streak = 0;
}

void slave_probe(void) { nodes_discover(); }

/* Combine this run's per-node z into one N(0,1) value: Σz / √k over the k nodes
 * that actually contributed. k varies run to run when a node is dropped, and
 * that is fine — each combined z is still unit-variance under the null, which
 * is the only property the statistics above depend on. Returns k via *n_used. */
static double combine_z(double z_master, int *n_used)
{
    double sum = 0.0;
    int    k   = 0;
    if (g_status.nodes[0].ok) { sum += z_master; k++; }
    for (int i = 0; i < s_nslaves; i++)
        if (g_status.nodes[i + 1].ok && node_take_z(i)) { sum += s_link[i].z; k++; }
    if (n_used) *n_used = k;
    return k > 0 ? sum / sqrt((double)k) : 0.0;
}

/* One scoring run, node-combined like Phase 2: trigger every node, measure
 * locally in parallel, combine ÷√k. No baseline subtraction — the offset is
 * common to every number and scoring only ranks them. */
static double score_one_run(void)
{
    int nseg = segments_for(true);
    slave_trigger(nseg);
    double zm = gcp_zscore_raw(nseg);
    // Sampling is over the moment the local run returns; the reply wait below
    // is dark time. The caller lit the panel — see focus_publish().
    focus_off();
    if (s_slave_ok) nodes_collect(LINK_MEAS_MS, true);
    return combine_z(zm, NULL);
}

/* Select the most-frequent numbers from the accumulated Z>2 histograms and
 * publish them (sorted ascending) to g_status.freq_*. */
static void publish_frequency(int *fm, int *fe, int z2, int nm, int mx, bool euro)
{
    g_status.freq_z2_count = z2;
    if (z2 <= 0) return;
    bool used[51] = {false};
    for (int k = 0; k < nm; k++) {
        int b = 0, bf = -1;
        for (int j = 1; j <= mx; j++)
            if (!used[j] && fm[j] > bf) { b = j; bf = fm[j]; }
        g_status.freq_nums[k] = (uint8_t)b;
        if (b) used[b] = true;
    }
    for (int i = 1; i < nm; i++) {
        uint8_t key = g_status.freq_nums[i]; int j = i - 1;
        while (j >= 0 && g_status.freq_nums[j] > key) { g_status.freq_nums[j+1] = g_status.freq_nums[j]; j--; }
        g_status.freq_nums[j+1] = key;
    }
    if (euro) {
        bool eu[13] = {false};
        for (int k = 0; k < 2; k++) {
            int b = 0, bf = -1;
            for (int j = 1; j <= 12; j++)
                if (!eu[j] && fe[j] > bf) { b = j; bf = fe[j]; }
            g_status.freq_euro[k] = (uint8_t)b;
            if (b) eu[b] = true;
        }
        if (g_status.freq_euro[0] > g_status.freq_euro[1]) {
            uint8_t t = g_status.freq_euro[0];
            g_status.freq_euro[0] = g_status.freq_euro[1];
            g_status.freq_euro[1] = t;
        }
    }
}

/* Bonferroni-corrected significance of the most extreme |Z| in the published
 * ranking — honest about the multiple-comparison search over `comparisons`. */
static void compute_significance(int comparisons)
{
    if (comparisons <= 0) {
        g_status.best_z = 0.0; g_status.p_corrected = 1.0; g_status.comparisons = 0;
        return;
    }
    double zt = (g_status.result_count > 0) ? fabs(g_status.top[0].z_score) : 0.0;
    double zb = (g_status.low_count   > 0) ? fabs(g_status.low[0].z_score) : 0.0;
    double zmax = zt > zb ? zt : zb;
    double p1 = erfc(zmax / 1.41421356237);   // two-sided single-test tail prob
    double pc = (double)comparisons * p1;      // Bonferroni
    if (pc > 1.0) pc = 1.0;
    g_status.best_z      = zmax;
    g_status.p_corrected = pc;
    g_status.comparisons = comparisons;
}

/* Studentize one loop's measurements: center on the loop's own mean and scale
 * by the loop's own empirical σ. This (a) removes the common bias offset with
 * a 5005-sample estimate instead of the noisy 100-run baseline (whose error
 * would otherwise accumulate √k-coherently in cumulative mode), and (b) makes
 * per-run Z exactly N(0,1) under the null even if raw source reads are
 * correlated and true σ ≠ 1. Reports the pre-scaling σ as a quality metric. */
static void studentize(int n, double *out_mean)
{
    if (out_mean) *out_mean = 0.0;
    if (n < 4) { g_status.loop_sigma = 0.0; return; }
    double m = 0.0;
    for (int i = 0; i < n; i++) m += g_status.results[i].z_score;
    m /= n;
    double v = 0.0;
    for (int i = 0; i < n; i++) {
        double d = g_status.results[i].z_score - m;
        v += d * d;
    }
    double s = sqrt(v / (n - 1));
    g_status.loop_sigma = s;
    if (out_mean) *out_mean = m;   // the offset removed here — Phase 3 drift check
    if (s < 1e-9) s = 1.0;
    for (int i = 0; i < n; i++) {
        double z = (g_status.results[i].z_score - m) / s;
        g_status.results[i].z_score = z;
        g_status.results[i].chi_sq  = z * z;
        g_status.results[i].p_value = p_label(fabs(z));
    }
}

static int cmp_u16(const void *a, const void *b)
{
    return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b;
}

/* After a mid-measurement abort the measured entries sit scattered at
 * results[s_perm[0..done-1]] (random order). Compact them into
 * results[0..done-1]: sorted ascending each source index is >= its
 * destination, so the stable forward copy never clobbers unread data. */
static void compact_partial(int done)
{
    qsort(s_perm, done, sizeof(uint16_t), cmp_u16);
    for (int j = 0; j < done; j++)
        g_status.results[j] = g_status.results[s_perm[j]];
}

/* Independence diagnostics over every pair of nodes — free bookkeeping that
 * verifies the √n combine assumption. At n=4 there are six pairs, i.e. six ways
 * for the assumption to be false, and ONE correlated pair invalidates the
 * combine. So the worst pair is what gets published; averaging the six would
 * let one bad pair hide behind five good ones.
 *
 * Moments are centered PER LOOP before folding into the session totals: pooling
 * raw values across loops would let each loop's random baseline offset
 * (SE = 1/√n_baseline per node) masquerade as correlation.
 *
 * A pair only accumulates on runs where BOTH nodes contributed, so a node that
 * was dropped part-way through does not silently shift the other's mean. */
typedef struct {
    double sx, sy, sxx, syy, sxy;   // this loop
    int    n;
    double cxx, cyy, cxy;           // session, per-loop centered
    int    cn, cloops;
} PairAcc;

typedef struct {
    double s, ss;                   // this loop
    int    n;
    double css;                     // session, per-loop centered
    int    cn, cloops;
} NodeAcc;

static PairAcc s_pair[MAX_NODES][MAX_NODES];   // upper triangle, i < j
static NodeAcc s_nacc[MAX_NODES];

static void pairs_reset(void)
{
    memset(s_pair, 0, sizeof(s_pair));
    memset(s_nacc, 0, sizeof(s_nacc));
}

/* Fold one run's per-node values in. `z[]` holds each node's own z and `have[]`
 * says which of them are real this run. */
static void pairs_add_run(const double *z, const bool *have)
{
    for (int i = 0; i < g_status.node_count; i++) {
        if (!have[i]) continue;
        s_nacc[i].s  += z[i];
        s_nacc[i].ss += z[i] * z[i];
        s_nacc[i].n++;
        for (int j = i + 1; j < g_status.node_count; j++) {
            if (!have[j]) continue;
            PairAcc *p = &s_pair[i][j];
            p->sx  += z[i];       p->sy  += z[j];
            p->sxx += z[i] * z[i]; p->syy += z[j] * z[j];
            p->sxy += z[i] * z[j];
            p->n++;
        }
    }
}

static void pairs_fold_loop(void)
{
    for (int i = 0; i < MAX_NODES; i++) {
        NodeAcc *a = &s_nacc[i];
        if (a->n >= 2) {
            double n = (double)a->n, m = a->s / n;
            a->css += a->ss - n * m * m;
            a->cn  += a->n;
            a->cloops++;
        }
        a->s = a->ss = 0.0; a->n = 0;

        for (int j = i + 1; j < MAX_NODES; j++) {
            PairAcc *p = &s_pair[i][j];
            if (p->n >= 2) {
                double n = (double)p->n, mx = p->sx / n, my = p->sy / n;
                p->cxx += p->sxx - n * mx * mx;
                p->cyy += p->syy - n * my * my;
                p->cxy += p->sxy - n * mx * my;
                p->cn  += p->n;
                p->cloops++;
            }
            p->sx = p->sy = p->sxx = p->syy = p->sxy = 0.0; p->n = 0;
        }
    }
}

static void publish_pair_stats(void)
{
    for (int i = 0; i < g_status.node_count; i++) {
        const NodeAcc *a = &s_nacc[i];
        int df = a->cn - a->cloops;          // one mean estimated per loop
        double v = (df >= 1) ? a->css / df : 0.0;
        g_status.nodes[i].sigma = v > 0.0 ? sqrt(v) : 0.0;
    }

    memset(g_status.pair_r, 0, sizeof(g_status.pair_r));
    double best = 0.0;
    int    bi = 0, bj = 0, bn = 0, count = 0;
    for (int i = 0; i < g_status.node_count; i++)
        for (int j = i + 1; j < g_status.node_count; j++) {
            const PairAcc *p = &s_pair[i][j];
            if (p->cn - p->cloops < 1 || p->cxx <= 0.0 || p->cyy <= 0.0) continue;
            count++;
            double r = p->cxy / sqrt(p->cxx * p->cyy);
            g_status.pair_r[i][j] = r;
            // |r| decides, but the signed value is what gets published — the
            // sign says whether nodes move together or against each other, and
            // that is the first clue to a mechanism if one ever shows up.
            if (fabs(r) >= fabs(best)) { best = r; bi = i; bj = j; bn = p->cn; }
        }
    g_status.pair_r_max = best;
    g_status.pair_r_i   = bi;
    g_status.pair_r_j   = bj;
    g_status.pair_n     = bn;
    g_status.pair_count = count;
}

/* ── Phase 3: cross-loop drift instrumentation ─────────────────────────
 * studentize() removes each loop's own mean exactly, so a *constant* offset
 * (e.g. the camera's residual LSB bias, ≈ −0.33 z/run in Phase 1) is harmless.
 * A trend across loops is not: it survives centering and, over a 20 h session,
 * has far more room to develop than the three loops of Phase 1/2. So regress
 * the master's raw per-run offset on the loop index and publish slope + t.
 * Running sums, so the test stays exact past LOOP_HIST stored loops. */
static struct { double n, sx, sxx, sy, sxy, syy; } s_drift;

static void drift_add(double x, double y)
{
    s_drift.n++;
    s_drift.sx += x; s_drift.sxx += x * x;
    s_drift.sy += y; s_drift.sxy += x * y; s_drift.syy += y * y;
    if (s_drift.n < 3) return;
    double n   = s_drift.n;
    double sxx = s_drift.sxx - s_drift.sx * s_drift.sx / n;
    double sxy = s_drift.sxy - s_drift.sx * s_drift.sy / n;
    double syy = s_drift.syy - s_drift.sy * s_drift.sy / n;
    if (sxx <= 0.0) return;
    double b     = sxy / sxx;
    double resid = syy - b * sxy;              // Σ of squared residuals
    if (resid < 0.0) resid = 0.0;
    double var_b = resid / (n - 2.0) / sxx;    // SE(slope)²
    g_status.drift_slope = b;
    g_status.drift_t     = (var_b > 0.0) ? b / sqrt(var_b) : 0.0;
}

/* Append one completed loop to the health table. Must run BEFORE
 * pairs_fold_loop(), which clears the per-loop sums this reads. */
static void record_loop(double loop_mean, int loop_idx)
{
    // Per-node mean and σ for this loop, straight from the un-folded sums.
    double mean_n[MAX_NODES] = {0}, sig_n[MAX_NODES] = {0};
    for (int i = 0; i < g_status.node_count; i++) {
        const NodeAcc *a = &s_nacc[i];
        if (a->n < 2) continue;
        double n = (double)a->n;
        mean_n[i] = a->s / n;
        double v = (a->ss - n * mean_n[i] * mean_n[i]) / (n - 1.0);
        sig_n[i] = v > 0.0 ? sqrt(v) : 0.0;
    }
    // Solo master: no per-node accumulation happened, so fall back to the
    // combined mean, which is the master's own mean in that case.
    if (s_nacc[0].n < 2) mean_n[0] = loop_mean;

    camera_stats_t cs;
    camera_get_stats(&cs);
    slaves_diag();                    // nodes are idle between loops

    if (g_status.loop_hist && g_status.loop_hist_n < LOOP_HIST) {
        LoopStat *L = &g_status.loop_hist[g_status.loop_hist_n++];
        memset(L, 0, sizeof(*L));
        L->base  = (float)g_status.baseline_mean;
        L->mean  = (float)loop_mean;
        L->sigma = (float)g_status.loop_sigma;
        L->nodes = (uint8_t)g_status.node_count;
        L->t_s   = (uint32_t)(g_status.elapsed_ms / 1000);
        for (int i = 0; i < g_status.node_count && i < MAX_NODES; i++) {
            L->mean_n[i] = (float)mean_n[i];
            L->sig_n[i]  = (float)sig_n[i];
            L->cam_mbit[i]   = i ? g_status.nodes[i].cam_mbit : (float)cs.mbit_per_sec;
            L->cam_stalls[i] = i ? g_status.nodes[i].cam_stalls : cs.stalls;
        }
    }
    g_status.nodes[0].cam_mbit   = (float)cs.mbit_per_sec;
    g_status.nodes[0].cam_stalls = cs.stalls;

    double s = g_status.loop_sigma;
    if (s > 0.0) {
        if (g_status.loops_done == 0 || s < g_status.sigma_lo) g_status.sigma_lo = s;
        if (g_status.loops_done == 0 || s > g_status.sigma_hi) g_status.sigma_hi = s;
    }
    // The master's RAW per-run offset: what its source actually produced this
    // loop, before studentization removed it (baseline + post-baseline residual)
    double raw_off = g_status.baseline_mean + mean_n[0];
    if (g_status.loops_done == 0) g_status.off_first = raw_off;
    g_status.off_last = raw_off;
    g_status.loops_done++;

    drift_add((double)loop_idx, raw_off);
}

/* Greedy diversified "coverage" picks: from the COVER_POOL most extreme
 * combinations (highest Z if !lowest, most-negative if lowest), choose up to
 * TOP_N that each share at most nm/2 numbers with every already-chosen one —
 * strong by Z but spread out, so the set collectively covers more of the draw
 * space than the (often near-duplicate) raw top-N / bottom-N. Operates on
 * g_status.results[] in combination-index order (cumulative mode). */
#define COVER_POOL 48
static void publish_coverage(int n, int nm, bool lowest,
                             RunResult *out, int *out_count)
{
    if (n <= 0) { *out_count = 0; return; }

    // Gather the COVER_POOL most extreme combinations by Z (best candidate first)
    int candIdx[COVER_POOL];
    int mc = 0;
    for (int i = 0; i < n; i++) {
        double z = g_status.results[i].z_score;
        double worst = (mc > 0) ? g_status.results[candIdx[mc - 1]].z_score : 0.0;
        bool better = (mc < COVER_POOL) || (lowest ? (z < worst) : (z > worst));
        if (!better) continue;
        if (mc < COVER_POOL) mc++;
        int p = mc - 1;
        while (p > 0 && (lowest ? (z < g_status.results[candIdx[p - 1]].z_score)
                                : (z > g_status.results[candIdx[p - 1]].z_score))) {
            candIdx[p] = candIdx[p - 1]; p--;
        }
        candIdx[p] = i;
    }

    int  maxov = nm / 2;
    bool chosen[COVER_POOL] = {false};
    int  sel = 0;
    // Pass 1: greedy by Z, enforce the pairwise overlap constraint
    for (int j = 0; j < mc && sel < TOP_N; j++) {
        RunResult *cj = &g_status.results[candIdx[j]];
        bool ok = true;
        for (int s = 0; s < sel && ok; s++) {
            int shared = 0;
            for (int a = 0; a < nm; a++)
                for (int b = 0; b < nm; b++)
                    if (out[s].nums[a] == cj->nums[b]) shared++;
            if (shared > maxov) ok = false;
        }
        if (ok) { out[sel++] = *cj; chosen[j] = true; }
    }
    // Pass 2: if the constraint was too tight, fill remaining slots by Z
    for (int j = 0; j < mc && sel < TOP_N; j++) {
        if (!chosen[j]) out[sel++] = g_status.results[candIdx[j]];
    }
    *out_count = sel;
}

/* Cumulative (Stouffer) ranking: each of the n fixed combinations has its
 * running Σz in zsum[] over k measured loops. Rank by Z = Σz/√k, publish the
 * top-N / bottom-N and most-frequent (over cumulative Z>2). */
static void publish_cumulative(double *zsum, int n, int k,
                               int *fm, int *fe, int *z2, int nm, int mx, bool euro)
{
    if (k <= 0 || n <= 0) return;
    double sk = sqrt((double)k);
    for (int i = 0; i < n; i++) {
        double z = zsum[i] / sk;                  // Stouffer Z = Σz / √k
        g_status.results[i].z_score = z;
        g_status.results[i].chi_sq  = z * z;
        g_status.results[i].p_value = p_label(fabs(z));
    }

    // Select top-N (highest) and bottom-N (lowest) by insertion, WITHOUT
    // sorting results[] — it must stay in combination-index order so the next
    // loop's Σz accumulation and nums stay aligned by index.
    RunResult top[TOP_N], low[TOP_N];
    int tn = 0, ln = 0;
    for (int i = 0; i < n; i++) {
        RunResult *r = &g_status.results[i];
        if (tn < TOP_N || r->z_score > top[tn ? tn - 1 : 0].z_score) {
            if (tn < TOP_N) tn++;
            int p = tn - 1;
            while (p > 0 && top[p - 1].z_score < r->z_score) { top[p] = top[p - 1]; p--; }
            top[p] = *r;
        }
        if (ln < TOP_N || r->z_score < low[ln ? ln - 1 : 0].z_score) {
            if (ln < TOP_N) ln++;
            int p = ln - 1;
            while (p > 0 && low[p - 1].z_score > r->z_score) { low[p] = low[p - 1]; p--; }
            low[p] = *r;
        }
    }
    for (int i = 0; i < tn; i++) g_status.top[i] = top[i];
    g_status.result_count = tn;
    for (int i = 0; i < ln; i++) g_status.low[i] = low[i];
    g_status.low_count = ln;

    // Most-frequent over cumulative Z>2 (scan, order-independent)
    for (int j = 0; j <= 50; j++) fm[j] = 0;
    for (int j = 0; j <= 12; j++) fe[j] = 0;
    *z2 = 0;
    for (int i = 0; i < n; i++) {
        if (g_status.results[i].z_score <= 2.0) continue;
        (*z2)++;
        for (int j = 0; j < nm; j++) fm[g_status.results[i].nums[j]]++;
        if (euro) { fe[g_status.results[i].euro[0]]++; fe[g_status.results[i].euro[1]]++; }
    }
    publish_frequency(fm, fe, *z2, nm, mx, euro);

    // Diversified max-spread picks from the top-Z and bottom-Z pools
    publish_coverage(n, nm, false, g_status.cover,     &g_status.cover_count);
    publish_coverage(n, nm, true,  g_status.cover_low, &g_status.cover_low_count);
}

/* Fold one completed (or partial) loop's results into the cumulative top-N
 * carry, accumulate the cross-loop Z>2 frequency histograms, and publish the
 * current cumulative top-N + most-frequent so /status can show them between
 * loops (intermediate results), not only at the very end. */
static void absorb_loop(RunResult *carry, int *carry_n,
                        RunResult *low, int *low_n,
                        int *fm, int *fe, int *z2, int nm, int mx, bool euro)
{
    int done = g_status.runs_completed;
    if (done > 0) {
        qsort(g_status.results, done, sizeof(RunResult), cmp_desc);

        // Frequency accumulation over Z>2 runs (sorted desc → stop at <=2)
        for (int i = 0; i < done; i++) {
            if (g_status.results[i].z_score <= 2.0) break;
            (*z2)++;
            for (int j = 0; j < nm; j++) fm[g_status.results[i].nums[j]]++;
            if (euro) { fe[g_status.results[i].euro[0]]++; fe[g_status.results[i].euro[1]]++; }
        }

        int take = done < TOP_N ? done : TOP_N;
        RunResult tmp[2 * TOP_N];
        int tn;

        // Merge this loop's highest-Z into the top carry, keep global top-N
        tn = 0;
        for (int i = 0; i < *carry_n; i++) tmp[tn++] = carry[i];
        for (int i = 0; i < take; i++) tmp[tn++] = g_status.results[i];   // results[] sorted desc
        qsort(tmp, tn, sizeof(RunResult), cmp_desc);
        *carry_n = tn < TOP_N ? tn : TOP_N;
        for (int i = 0; i < *carry_n; i++) carry[i] = tmp[i];

        // Merge this loop's lowest-Z into the low carry, keep global bottom-N
        tn = 0;
        for (int i = 0; i < *low_n; i++) tmp[tn++] = low[i];
        for (int i = 0; i < take; i++) tmp[tn++] = g_status.results[done - take + i];
        qsort(tmp, tn, sizeof(RunResult), cmp_asc);
        *low_n = tn < TOP_N ? tn : TOP_N;
        for (int i = 0; i < *low_n; i++) low[i] = tmp[i];
    }

    // Publish cumulative top-N + bottom-N (entries first, then count)
    for (int i = 0; i < *carry_n; i++) g_status.top[i] = carry[i];
    g_status.result_count = *carry_n;
    for (int i = 0; i < *low_n; i++) g_status.low[i] = low[i];
    g_status.low_count = *low_n;
    g_status.cover_count = g_status.cover_low_count = 0;   // coverage is cumulative-only

    // Publish most-frequent numbers across all loops' Z>2 runs
    publish_frequency(fm, fe, *z2, nm, mx, euro);
}

void elotto_task(void *pvParam)
{
    g_status.state           = ELOTTO_RUNNING;
    g_status.runs_completed  = 0;
    g_status.baseline_done   = 0;
    g_status.scoring_done    = 0;
    g_status.abort_requested = false;
    g_status.elapsed_ms      = 0;
    g_status.baseline_mean   = 0.0;
    g_status.slave_connected = false;
    g_status.result_count    = 0;
    g_status.low_count       = 0;
    g_status.cover_count     = 0;
    g_status.cover_low_count = 0;
    g_status.freq_z2_count   = 0;
    g_status.loop_current    = 0;
    g_status.best_z          = 0.0;
    g_status.p_corrected     = 1.0;
    g_status.comparisons     = 0;
    g_status.loop_sigma      = 0.0;
    g_status.loops_done      = 0;
    g_status.loop_hist_n     = 0;
    g_status.drift_slope     = 0.0;
    g_status.drift_t         = 0.0;
    g_status.off_first       = 0.0;
    g_status.off_last        = 0.0;
    g_status.sigma_lo        = 0.0;
    g_status.sigma_hi        = 0.0;
    memset(&s_drift, 0, sizeof(s_drift));
    // focus_mode is set by /start and NOT reset here — it is the session's tag.
    // Everything else about the panel starts clean, including a pause left over
    // from a session that was aborted while held.
    focus_reset();
    // Loop history lives in PSRAM: results[] already fills internal RAM. Kept
    // for the lifetime of the app (allocated once, never freed) so the table of
    // a finished session survives for inspection.
    if (!g_status.loop_hist)
        g_status.loop_hist = heap_caps_calloc(LOOP_HIST, sizeof(LoopStat), MALLOC_CAP_SPIRAM);
    g_status.pair_r_max      = 0.0;
    g_status.pair_n          = 0;
    g_status.pair_count      = 0;
    g_status.pair_r_i        = g_status.pair_r_j = 0;
    // Per-session transport health. "Zero lost triggers" is the Phase C gate,
    // so it has to be a counted number rather than an impression.
    g_status.net_retries     = 0;
    g_status.net_lost        = 0;
    g_status.net_stale       = 0;
    if (g_status.baseline_total <= 0 || g_status.baseline_total > 5000)
        g_status.baseline_total = 100;
    if (g_status.loops_total <= 0 || g_status.loops_total > 500)
        g_status.loops_total = 1;

    // Re-discover every session: a node that was powered off last time joins
    // this one, and one that vanished does not sit in the table as a phantom.
    nodes_discover();

    // After abort_requested is cleared, so a camera that is not ready can abort
    // the session immediately instead of having the flag wiped by the reset above.
    noise_source_begin();

    bool euro    = (g_status.mode == MODE_EUROJACKPOT);
    int  nm      = euro ? 5 : 6;
    int  mx      = euro ? 50 : 49;
    int  pool_nm = euro ? POOL_MAIN_50 : POOL_MAIN_49;

    g_status.scoring_total = (mx + (euro ? 12 : 0)) * SCORE_REPS;

    uint8_t pool_main[POOL_MAIN_49] = {0};   // 15 slots, enough for both modes
    uint8_t pool_euro[POOL_EURO_12] = {0};
    int     main_combos = comb(pool_nm, nm);
    int     euro_combos = euro ? comb(POOL_EURO_12, 2) : 1;
    int     full_combos = main_combos * euro_combos;
    g_status.runs_total = (g_status.runs_limit > 0 && g_status.runs_limit < full_combos)
                          ? g_status.runs_limit : full_combos;

    // Peak-mode carries (best/worst single-run Z across loops) + freq histograms
    RunResult carry[TOP_N];
    RunResult low_carry[TOP_N];
    int carry_n = 0, low_n = 0;
    int fm[51] = {0}, fe[13] = {0}, z2 = 0;

    // Cumulative (Stouffer) mode: fixed pool, Σz per combination over meas_k loops
    bool cumulative = (g_status.rank_mode == RANK_CUMULATIVE);
    int  meas_k = 0;
    if (cumulative)
        memset(s_zsum, 0, sizeof(double) * (size_t)g_status.runs_total);

    // Pairwise independence check across all nodes (per-loop centered)
    pairs_reset();

    int comparisons = 0;
    s_t0 = esp_timer_get_time();

    for (int loop = 0; loop < g_status.loops_total; loop++) {
        g_status.loop_current   = loop + 1;
        g_status.baseline_done  = 0;
        g_status.scoring_done   = 0;
        g_status.runs_completed = 0;
        g_status.baseline_mean  = 0.0;
        // Cumulative mode locks the pool after loop 0; peak mode rebuilds it
        bool do_score = (!cumulative || loop == 0);
        if (do_score) {
            memset(pool_main, 0, sizeof(pool_main));
            memset(pool_euro, 0, sizeof(pool_euro));
        }

        /* ── Phase 1: Baseline calibration (every node in parallel) ────── */
        // Baseline runs at MEASUREMENT length: it estimates the offset of the
        // runs it will be subtracted from, so it has to be the same instrument.
        // Nothing is displayed — the panel is hidden during baseline — and
        // pause is deliberately not offered here: one 'B' command sets every
        // slave running its whole baseline autonomously, so a master-side hold
        // would desynchronise them rather than pause them.
        g_status.phase = PHASE_BASELINE;
        slave_baseline_start(g_status.baseline_total, segments_for(false));
        {
            double bsum = 0.0;
            for (int i = 0; i < g_status.baseline_total; i++) {
                if (g_status.abort_requested) {
                    slave_abort();
                    goto done;
                }
                bsum += gcp_zscore_raw(segments_for(false));
                run_gap(false);     // same duty cycle as the runs this calibrates
                g_status.baseline_done = i + 1;
                g_status.elapsed_ms = elapsed_ms_now();
            }
            g_status.baseline_mean = bsum / g_status.baseline_total;
        }
        if (s_slave_ok && !g_status.abort_requested)
            slave_baseline_wait();

        /* ── Phase 0: Individual number scoring (cumulative: loop 0 only) ── */
        g_status.phase = PHASE_SCORING;
        if (do_score) {
            score_and_build_pool(mx, pool_nm, pool_main, false);
            if (g_status.abort_requested) goto done;
            if (euro) score_and_build_pool(12, POOL_EURO_12, pool_euro, true);
            if (g_status.abort_requested) goto done;
            focus_off();
        } else {
            g_status.scoring_done = g_status.scoring_total;   // pool already locked
        }

        /* ── Phase 2: Measure all combinations in fresh random order ───── */
        // Fisher–Yates: with a fixed order, slow drift (temperature ramp over
        // the ~20-min loop) would hit each combination at the same position
        // every loop and accumulate √k-coherently — exactly like a real signal.
        for (int i = 0; i < g_status.runs_total; i++) s_perm[i] = (uint16_t)i;
        for (int i = g_status.runs_total - 1; i > 0; i--) {
            // Deliberately fast_rng(), not noise_word(): this is administrative
            // randomness (measurement order), not measured data. Spending
            // rate-limited camera entropy here would stall the session for
            // bits that never enter a z-score.
            int j = (int)(fast_rng() % (uint32_t)(i + 1));
            uint16_t t = s_perm[i]; s_perm[i] = s_perm[j]; s_perm[j] = t;
        }

        g_status.phase = PHASE_MEASURING;
        for (int j = 0; j < g_status.runs_total; j++) {
            if (g_status.abort_requested) {
                slave_abort();
                goto done;
            }
            // Between runs, never inside one: the run just finished is kept and
            // j does not advance, so nothing is lost or duplicated.
            pause_gate();
            if (g_status.abort_requested) {
                slave_abort();
                goto done;
            }
            int i = s_perm[j];   // slot index; results[] stays slot-indexed

            // Slot → combination: spread slots evenly over the full space so a
            // Runs cap samples across all combinations instead of taking the
            // lexicographic prefix (which all shares the pool's lowest numbers).
            // Uncapped, runs_total == full_combos and c == i exactly.
            int c  = (int)(((int64_t)i * full_combos) / g_status.runs_total);
            int mi = c % main_combos;
            int ei = euro ? (c / main_combos) : 0;
            nth_combination(pool_main, pool_nm, nm, mi, g_status.results[i].nums);
            if (euro)
                nth_combination(pool_euro, POOL_EURO_12, 2, ei, g_status.results[i].euro);
            else
                g_status.results[i].euro[0] = g_status.results[i].euro[1] = 0;

            // The draw goes on screen BEFORE the trigger, so the observer is
            // already attending when the first bit is sampled.
            focus_publish(FOCUS_DRAW, g_status.results[i].nums, nm,
                          g_status.results[i].euro, euro ? 2 : 0);

            // One broadcast starts every node, then measure locally — all of
            // them integrate the same window, which is the premise the √n
            // combine rests on.
            int nseg = segments_for(false);
            slave_trigger(nseg);
            double zm = gcp_zscore_raw(nseg) - g_status.baseline_mean;
            focus_off();          // sampling done; the reply wait is dark time
            if (s_slave_ok) nodes_collect(LINK_MEAS_MS, true);

            // Per-node values for the independence check, gathered before the
            // combine so a dropped node is excluded from both consistently.
            double znode[MAX_NODES] = {0};
            bool   have[MAX_NODES]  = {false};
            double sum = 0.0;
            int    k   = 0;
            if (g_status.nodes[0].ok) {
                znode[0] = zm; have[0] = true; sum += zm; k++;
            }
            for (int s = 0; s < s_nslaves; s++) {
                if (!g_status.nodes[s + 1].ok || !node_take_z(s)) continue;
                znode[s + 1] = s_link[s].z; have[s + 1] = true;
                sum += s_link[s].z; k++;
            }
            pairs_add_run(znode, have);
            double z = (k > 0) ? sum / sqrt((double)k) : 0.0;

            g_status.results[i].index   = i + 1;
            g_status.results[i].z_score = z;
            g_status.results[i].chi_sq  = z * z;
            g_status.results[i].p_value = p_label(fabs(z));
            g_status.runs_completed     = j + 1;
            g_status.elapsed_ms         = elapsed_ms_now();
            run_gap(false);
        }

        // Loop finished cleanly: studentize on the loop's own mean/σ, then
        // fold + publish the ranking
        double loop_mean = 0.0;
        studentize(g_status.runs_total, &loop_mean);
        record_loop(loop_mean, loop);   // before the fold clears the per-loop sums
        pairs_fold_loop();
        publish_pair_stats();
        if (cumulative) {
            for (int i = 0; i < g_status.runs_total; i++)
                s_zsum[i] += g_status.results[i].z_score;   // Σz per fixed combination
            meas_k++;
            publish_cumulative(s_zsum, g_status.runs_total, meas_k, fm, fe, &z2, nm, mx, euro);
            comparisons = g_status.runs_total;
        } else {
            absorb_loop(carry, &carry_n, low_carry, &low_n, fm, fe, &z2, nm, mx, euro);
            comparisons = g_status.runs_total * g_status.loop_current;
        }
        compute_significance(comparisons);
    }
    goto finalize;

done:
    // Aborted mid-loop: publish whatever was completed so far. Measured
    // entries of the aborted loop sit scattered at results[s_perm[...]], so
    // compact them to the front before using them as a partial prefix.
    pairs_fold_loop();
    publish_pair_stats();
    if (cumulative) {
        if (meas_k > 0) {
            // Complete loops exist: discard the partial loop, publish those
            publish_cumulative(s_zsum, g_status.runs_total, meas_k, fm, fe, &z2, nm, mx, euro);
            comparisons = g_status.runs_total;
        } else if (g_status.runs_completed > 0) {
            // Aborted during the first measurement loop: treat the measured
            // subset as a single sample so partial results are still shown
            int pdone = g_status.runs_completed;
            compact_partial(pdone);
            studentize(pdone, NULL);
            for (int i = 0; i < pdone; i++)
                s_zsum[i] = g_status.results[i].z_score;
            publish_cumulative(s_zsum, pdone, 1, fm, fe, &z2, nm, mx, euro);
            comparisons = pdone;
        }
    } else {
        int pdone = g_status.runs_completed;
        if (pdone > 0 && pdone < g_status.runs_total) {
            compact_partial(pdone);
            studentize(pdone, NULL);
        }
        absorb_loop(carry, &carry_n, low_carry, &low_n, fm, fe, &z2, nm, mx, euro);
        comparisons = g_status.runs_total * (g_status.loop_current > 0 ? g_status.loop_current : 1);
    }
    compute_significance(comparisons);

finalize:
    focus_off();
    g_status.paused     = false;
    g_status.elapsed_ms = elapsed_ms_now();
    g_status.state = g_status.abort_requested ? ELOTTO_ABORTED : ELOTTO_DONE;
    vTaskDelete(NULL);
}
