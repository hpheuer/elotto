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

// Segments per run is source-dependent (PLAN_4NODE Phase 1). The TRNG is
// oversampled and effectively free, so it keeps the historical 6.4 Mbit/run.
// Camera bits are honest and rate-limited (~3.4 Mbit/s measured), so a run is
// sized for ~0.5 s instead. Run length only sets granularity — statistical
// power per second is rate-limited either way, and z stays N(0,1) because it
// is normalised by √segments.
#define TRNG_SEGMENTS  NUM_SEGMENTS      // 6.4 Mbit/run
#define CAM_SEGMENTS   8000              // 1.6 Mbit/run ≈ 0.47 s at 3.4 Mbit/s

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

static double gcp_zscore_raw(void)
{
    const int nseg = (s_active_source == NOISE_CAMERA) ? CAM_SEGMENTS : TRNG_SEGMENTS;
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
        // TRNG path needs explicit yields (4/run, ~40 ms); the camera path
        // already yields inside camera_read_word() whenever it waits on the
        // producer, which is most of the time.
        if (seg % 8000 == 0) vTaskDelay(1);
    }
    return z_sum / sqrt((double)nseg);
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
static double score_one_run(void);   // forward (defined after the slave UART block)

static void score_and_build_pool(int max_val, int pool_size, uint8_t *pool)
{
    double scores[51] = {0};
    for (int k = 1; k <= max_val; k++) {
        double sum = 0.0;
        for (int r = 0; r < SCORE_REPS; r++) {
            if (g_status.abort_requested) return;
            sum += score_one_run();
            g_status.scoring_done++;
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

static void slave_baseline_start(int n)
{
    if (!s_slave_ok) return;
    char cmd[16];
    snprintf(cmd, sizeof(cmd), "B%d", n);
    nodes_send(cmd);
}

static void slave_baseline_wait(void)
{
    if (!s_slave_ok) return;
    nodes_collect(g_status.baseline_total * 800 + 15000, true);
}

static void slave_trigger(void)
{
    if (s_slave_ok) nodes_send("M");
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
    slave_trigger();
    double zm = gcp_zscore_raw();
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

    double best = 0.0;
    int    bi = 0, bj = 0, bn = 0, count = 0;
    for (int i = 0; i < g_status.node_count; i++)
        for (int j = i + 1; j < g_status.node_count; j++) {
            const PairAcc *p = &s_pair[i][j];
            if (p->cn - p->cloops < 1 || p->cxx <= 0.0 || p->cyy <= 0.0) continue;
            count++;
            double r = p->cxy / sqrt(p->cxx * p->cyy);
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
    int64_t t0 = esp_timer_get_time();

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
        g_status.phase = PHASE_BASELINE;
        slave_baseline_start(g_status.baseline_total);
        {
            double bsum = 0.0;
            for (int i = 0; i < g_status.baseline_total; i++) {
                if (g_status.abort_requested) {
                    slave_abort();
                    goto done;
                }
                bsum += gcp_zscore_raw();
                g_status.baseline_done = i + 1;
                g_status.elapsed_ms = (esp_timer_get_time() - t0) / 1000;
            }
            g_status.baseline_mean = bsum / g_status.baseline_total;
        }
        if (s_slave_ok && !g_status.abort_requested)
            slave_baseline_wait();

        /* ── Phase 0: Individual number scoring (cumulative: loop 0 only) ── */
        g_status.phase = PHASE_SCORING;
        if (do_score) {
            score_and_build_pool(mx, pool_nm, pool_main);
            if (g_status.abort_requested) goto done;
            if (euro) score_and_build_pool(12, POOL_EURO_12, pool_euro);
            if (g_status.abort_requested) goto done;
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

            // One broadcast starts every node, then measure locally — all of
            // them integrate the same window, which is the premise the √n
            // combine rests on.
            slave_trigger();
            double zm = gcp_zscore_raw() - g_status.baseline_mean;
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
            g_status.elapsed_ms         = (esp_timer_get_time() - t0) / 1000;
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
    g_status.elapsed_ms = (esp_timer_get_time() - t0) / 1000;
    g_status.state = g_status.abort_requested ? ELOTTO_ABORTED : ELOTTO_DONE;
    vTaskDelete(NULL);
}
