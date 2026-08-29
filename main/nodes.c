/* ── The node array — see nodes.h for what this module is ───────────────
 *
 * Moved out of sensor.c on 2026-07-27 with no functional change: the UDP link,
 * discovery, the calibration handshake, the drop/reboot policy and the
 * diagnostics poll were ~520 lines that shared seven file-scope statics with
 * each other and almost nothing with the GCP statistics around them.
 *
 * The only edits made in the move were the ones the file boundary forced:
 * ten functions lost `static`, node_take_z() now returns its z through an
 * out-parameter instead of the caller reaching into s_link[], and s_slave_ok /
 * s_nslaves are read through accessors. The protocol, the timeouts, the retry
 * and drop rules are untouched.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "sensor.h"
#include "nodes.h"
#include "camera.h"
#include "gcp.h"
#include "elotto_link.h"

/* Defined below, beside the miss rule it belongs to; the camera-fault path
 * further down is the other caller. */
static void note_first_drop(int node);

/* Camera failure policy — REPORT AND REBOOT, never substitute.
 *
 * There is no second source in this firmware, so "degrade gracefully" is not on
 * the menu and that is deliberate (see sensor.h). A node whose camera stops
 * delivering has stopped being an instrument, so it is:
 *   1. named in g_status.fault, where the operator can actually see it,
 *   2. dropped from the combine, and
 *   3. rebooted — the camera is brought up in app_main, so a restart is the one
 *      recovery available to software, and a node that comes back rejoins the
 *      next session by discovery.
 *
 * Dropping suffices while enough nodes remain: at n>=3 the rest carry on over
 * √(n−1). Below that floor the array has stopped being the instrument it
 * started as, so the session aborts rather than quietly continuing at half
 * strength. */
void node_camera_failed(int node, const char *why)
{
    if (node < 0 || node >= g_status.node_count) return;
    bool first = !g_status.nodes[node].cam_fault;
    g_status.nodes[node].cam_fault = 1;

    if (g_status.nodes[node].ok) {
        g_status.nodes[node].ok = false;
        g_status.node_ok--;
    }
    const char *name = node ? g_status.nodes[node].ip : "master";
    printf("node %d (%s): CAMERA FAULT (%s) -- dropped, %d node(s) left\n",
           node, name, why, g_status.node_ok);

    // First failure wins the message: the operator needs the node that went
    // first, not whichever happened to fail last.
    if (first && !g_status.fault[0])
        snprintf(g_status.fault, sizeof(g_status.fault),
                 "camera fault on %s (%s) - node dropped and rebooted", name, why);

    if (node > 0) {
        g_status.nodes[node].reboots++;
        slave_reboot(node - 1);
    }
    note_first_drop(node);

    int floor_n = (g_status.node_count >= 2) ? 2 : 1;
    if (g_status.node_ok < floor_n) {
        g_status.noise_stalled   = true;
        g_status.abort_requested = true;
        snprintf(g_status.fault, sizeof(g_status.fault),
                 "camera fault on %s (%s) - only %d node(s) left, session aborted",
                 name, why, g_status.node_ok);
    }
}

/* ── Slave link — UDP broadcast ───────────────────────────────────────
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
#define LINK_MEAS_MS    4000      // a run is ~1 s, so this is generous headroom
#define LINK_DIAG_MS    1500

/* Slack added on top of a phase's own expected duration before its ack wait
 * gives up — settle time, flush and scheduling jitter, none of which scales
 * with the phase. Used by the calibration wait, so a
 * node that is merely slow is not mistaken for a missing one in either. */
#define LINK_ACK_SLACK_MS 15000

// Consecutive missed replies before a node leaves the session. Without it an
// unplugged node would cost every remaining run the full retry budget forever;
// the Phase D gate wants an unplug to *degrade* the array, not to slow it down.
#define NODE_MISS_LIMIT 3

/* Stamp the first drop of the session with the master's OWN link state.
 * Called from both drop paths (missed replies here, camera fault above), and
 * only the first one lands: what a diagnosis needs is the state at the moment
 * the array started falling apart, not the moment the last node left.
 *
 * Reading g_status.eth_up rather than polling the PHY is deliberate -- the
 * event handler in elotto.c already holds the authoritative value, and a drop
 * is decided from the measurement task, which must not block on MDIO. */
static void note_first_drop(int node)
{
    if (g_status.drop_uptime_ms >= 0) return;
    g_status.drop_uptime_ms = esp_timer_get_time() / 1000;
    g_status.drop_eth_up    = g_status.eth_up;
    g_status.drop_eth_downs = g_status.eth_downs;
    g_status.drop_node      = node;
    printf("drop forensics: node %d at uptime %lld ms, master eth %s, "
           "%lu link down(s) since boot\n",
           node, (long long)g_status.drop_uptime_ms,
           g_status.drop_eth_up ? "UP" : "DOWN",
           (unsigned long)g_status.drop_eth_downs);
}

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
 * A node that stays silent is dropped FOR THIS RUN — at n>=3
 * that is a degraded run over √(n−1), not a reason to end the session. After
 * NODE_MISS_LIMIT consecutive misses it leaves the session altogether, so an
 * unplugged node degrades the array instead of taxing every later run with the
 * full retry budget. */
int nodes_collect(int timeout_ms, bool critical)
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
            note_first_drop(k + 1);
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
void nodes_discover(void)
{
    s_nslaves            = 0;
    g_status.node_count  = 1;                 // the master itself
    g_status.node_ok     = 1;
    memset(s_link, 0, sizeof(s_link));
    memset(g_status.nodes, 0, sizeof(g_status.nodes));
    g_status.nodes[0].ok = true;
    /* memset gives 0, and 0,0 °C is a plausible temperature that would be
     * regressed on as if it were real. NAN is the only value that means
     * "this node reported none". */
    for (int i = 0; i < MAX_NODES; i++)
        g_status.nodes[i].die_temp_c = NAN;

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

/* ── Per-loop camera calibration (docs/PLAN.md Task 1) ────────────────────
 *
 * One broadcast starts every node's sweep at once, the master runs its own in
 * parallel, then waits for every ack. Not pausable: one 'K' sets every slave
 * running autonomously, so a master-side hold would desynchronise them rather
 * than pause them. */
/* "K<budget_ms>,<segments>". The segment count travels for the same reason it
 * travels on 'M': the bias gate is scaled by it (camera_cal_set_z_scale),
 * so a slave that guessed would apply a different gate to the same camera and
 * nothing would look wrong. A slave too old to parse the second field falls back
 * to its legacy fixed bar and says so on its own console -- tolerable ONLY
 * because this field decides a rung, never a combine, and because the two
 * firmwares are flashed together by policy. Do not copy the pattern to a field
 * that feeds a z. */
static void slave_calibrate_start(int budget_ms, int segments)
{
    if (!s_slave_ok) return;
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "K%d,%d", budget_ms, segments);
    nodes_send(cmd);
}

/* Record what one node reported: "OK:<exp>,<gain>,<fold>,<bias>,<mbit_s>,<G|U>".
 * The tag is appended AFTER the numbers so it cannot disturb a field-order
 * parse, and an older master that stops reading early still gets valid
 * numbers. It says whether
 * the node actually adopted a GATED setting or fell back to its previous one.
 * Without it a node that certified nothing would be indistinguishable in
 * /status from one that certified the setting it happens to be running. */
static void node_take_cal(int k)
{
    NodeStatus *N = &g_status.nodes[k + 1];
    N->cam_exp = 0;
    N->cam_cal_ok = 0;
    if (!s_link[k].replied) return;
    const char *resp = s_link[k].reply;
    if (resp[0] != 'O' || resp[1] != 'K' || resp[2] != ':') return;

    unsigned long e = 0, g = 0;
    int   fold = 0;
    float bias = 0.0f, mb = 0.0f;
    if (sscanf(resp + 3, "%lu,%lu,%d,%f,%f", &e, &g, &fold, &bias, &mb) < 5) return;
    const char *tag = strrchr(resp, ',');
    N->cam_exp      = (uint32_t)e;
    N->cam_gain     = (uint16_t)g;
    N->cam_fold     = fold ? 1 : 0;
    N->cam_bias     = bias;
    N->cam_cal_mbit = mb;
    N->cam_cal_ok   = (tag && tag[1] == 'G') ? 1 : 0;
    printf("node %d (%s): cal exposure=%lu gain=%lu fold=%d bias=%.6f %.2f Mbit/s %s\n",
           k + 1, N->ip, e, g, fold, bias, mb,
           N->cam_cal_ok ? "" : "(no gated setting -- kept previous)");
}

/* Wait for every node's ack. The timeout is derived from the budget the nodes
 * were given plus the settle-and-flush overhead the sweep pays on top of it, so
 * a slower node is not mistaken for a missing one. A node that really does not
 * answer is handled by the existing rule — dropped after NODE_MISS_LIMIT, the
 * session continues over √(k−1). */
static void slave_calibrate_wait(int budget_ms)
{
    if (!s_slave_ok) return;
    nodes_collect(budget_ms + LINK_ACK_SLACK_MS, true);
    for (int k = 0; k < s_nslaves; k++) node_take_cal(k);
}

/* The master's own sweep, kept in PSRAM: the table is ~1.2 KB and internal RAM
 * is already full with results[] (adding it as .bss fails the LINK, not the
 * run). Allocated once and never freed, so GET /calibrate can still serve the
 * last sweep long after the session that produced it finished. */
static camera_cal_t *s_cal;

const camera_cal_t *elotto_last_calibration(void)
{
    return (s_cal && s_cal->nsteps > 0) ? s_cal : NULL;
}

static bool cal_abort_cb(void) { return g_status.abort_requested; }

static void calibrate_master(int budget_ms)
{
    NodeStatus *N = &g_status.nodes[0];
    N->cam_exp = 0;
    N->cam_cal_ok = 0;
    if (!s_cal)
        s_cal = heap_caps_calloc(1, sizeof(camera_cal_t), MALLOC_CAP_SPIRAM);
    if (!s_cal) { printf("cal: no PSRAM for the sweep table -- skipped\n"); return; }

    camera_cal_set_z_scale(gcp_z_per_bias(g_status.run_segments));
    bool ok = camera_calibrate(budget_ms, cal_abort_cb, s_cal);
    N->cam_exp      = s_cal->exposure;
    N->cam_gain     = (uint16_t)s_cal->gain;
    N->cam_fold     = s_cal->xor_fold ? 1 : 0;
    N->cam_bias     = (float)s_cal->bias;
    N->cam_cal_mbit = (float)s_cal->mbit_per_sec;
    N->cam_cal_ok   = ok ? 1 : 0;
    printf("master: cal exposure=%lu gain=%lu fold=%d %s (%lu ms, %d steps)\n",
           (unsigned long)s_cal->exposure, (unsigned long)s_cal->gain,
           (int)s_cal->xor_fold, ok ? "" : "(no gated setting -- kept previous)",
           (unsigned long)s_cal->elapsed_ms, s_cal->nsteps);
}

/* When the last sweep finished. 0 = none this session, which forces the first
 * loop to calibrate however short the interval is. */
static int64_t s_cal_last_us;

void calibrate_forget(void) { s_cal_last_us = 0; }

/* One calibration phase: broadcast, sweep locally in parallel, wait for acks.
 * Skipped when the budget is 0 — that is the matched no-calibration control —
 * and when nothing is left to calibrate.
 *
 * Also skipped when the last sweep is younger than `cal_interval_ms`. The sweep
 * costs ~24 s, which is ~4 % of a full ~10 min loop but can be MOST of a short
 * one (a Runs-capped test loop runs in seconds), and what it corrects — thermal
 * drift of the sensors — moves on a wall-clock scale, not a per-loop one.
 * Re-tuning a camera that was tuned 20 s ago measures nothing but the sweep's
 * own noise. So the trigger is elapsed time since the last sweep, not the loop
 * boundary; at the default interval a full-length loop still calibrates every
 * loop, which is the behaviour §1.5.1 measured.
 *
 * ⚠ camera_calibrate() resets the camera statistics, so `mbit_s`/`bias` in
 * /status and /loops are "since the last sweep" — on a skipped loop they now
 * span several loops rather than one. */
bool calibrate_all(void)
{
    if (g_status.cal_budget_ms <= 0) return false;
    if (g_status.node_ok == 0) return false;

    if (s_cal_last_us && g_status.cal_interval_ms > 0) {
        int64_t age_ms = (esp_timer_get_time() - s_cal_last_us) / 1000;
        if (age_ms < (int64_t)g_status.cal_interval_ms) {
            printf("calibration: skipped, last sweep %d s ago (interval %d s)\n",
                   (int)(age_ms / 1000), g_status.cal_interval_ms / 1000);
            return false;
        }
    }

    g_status.phase = PHASE_CALIBRATE;
    int64_t t0 = esp_timer_get_time();
    g_status.cal_start_us = t0;          // publishes the live bar; cleared below
    slave_calibrate_start(g_status.cal_budget_ms, g_status.run_segments);  // trigger first, then measure
    calibrate_master(g_status.cal_budget_ms);
    if (!g_status.abort_requested) slave_calibrate_wait(g_status.cal_budget_ms);
    g_status.cal_ms = (int)((esp_timer_get_time() - t0) / 1000);
    g_status.cal_start_us = 0;           // no sweep in flight
    // Timed from the END of the sweep: the interval is dark time between
    // sweeps, and charging it from the start would make a slow sweep shorten
    // the gap to the next one.
    s_cal_last_us = esp_timer_get_time();
    printf("calibration: %d ms for %d node(s)\n", g_status.cal_ms, g_status.node_ok);
    return true;
}

void slave_trigger(int nseg)
{
    if (!s_slave_ok) return;
    char cmd[16];
    snprintf(cmd, sizeof(cmd), "M%d", nseg);
    nodes_send(cmd);
}

/* Restart a node whose camera failed. Fire and forget, twice: the node reboots
 * on receipt so there is no reply to wait for, and a single dropped datagram
 * would leave a dead instrument sitting on the switch answering discovery.
 *
 * Rebooting is the only recovery software has here — the camera is brought up
 * in app_main — and it is safe by construction: the node comes back running the
 * recovery-validated image and rejoins the next session through discovery. It
 * is NOT re-added to the running session; this session already lost it. */
void slave_reboot(int k)
{
    if (k < 0 || k >= s_nslaves || s_sock < 0) return;
    char msg[ELOTTO_LINK_MAX];
    int  n = elotto_link_pack(msg, sizeof(msg), ++s_seq, "R");
    if (n <= 0) return;
    for (int i = 0; i < 2; i++)
        sendto(s_sock, msg, n, 0, (struct sockaddr *)&s_link[k].addr,
               sizeof(s_link[k].addr));
    printf("node %d (%s): reboot sent\n", k + 1, g_status.nodes[k + 1].ip);
}

void slave_abort(void)
{
    if (!s_slave_ok) return;
    // Fire and forget, twice: nothing waits for the answer (the caller is on its
    // way out), and a single dropped datagram would otherwise leave a node
    // grinding through a run whose result no one will read.
    nodes_send("A");
    link_send(s_pending.seq, s_pending.cmd);
}

/* Parse one node's reply into s_link[k].z. Returns false if the node did not
 * contribute a usable value this run.
 *
 *   "Z:<float>[,<H_norm>]"  the run completed on camera bits
 *   "E:<reason>" the camera stopped delivering — the node is faulted and
 *                rebooted, not silently omitted
 *
 * The old ",<C|T>" source tag is gone with the TRNG: there is only one source
 * now, so a completed run cannot have come from anywhere else, and a run that
 * could not complete says so explicitly instead of reporting a substituted z.
 *
 * ⚠ Trailing fields optional. Z:<z>[,H[,z_pre[,h1,h2]]] — H ignored (D53);
 * z_pre past 2nd comma; half-window pre past 3rd/4th (D56). A single 4th
 * field with no 5th is an old D54 runs z and is ignored. Absent ≠ 0. */
bool node_take_z(int k, double *out_z,
                 bool *out_have_pre, double *out_pre,
                 bool *out_have_h, double *out_h1, double *out_h2)
{
    if (out_have_pre) *out_have_pre = false;
    if (out_have_h) *out_have_h = false;
    if (!s_link[k].replied) return false;
    const char *resp = s_link[k].reply;

    /* ⚠ 'V:' is a VOID, not a fault: the node withheld this run's z on purpose
     * (its pre-window ring flush did not settle) and is otherwise healthy. It
     * must not go down the 'E:' path, which reboots the node -- escalating a
     * transient to a reboot would cost an arm for the rest of the session, and
     * the whole point of voiding one run is that it costs only that run. */
    if (resp[0] == 'V' && resp[1] == ':') {
        g_status.flush_timeouts++;
        printf("node %d (%s): run voided -- %s" "\n", k + 1,
               g_status.nodes[k + 1].ip, resp + 2);
        return false;
    }
    if (resp[0] == 'E' && resp[1] == ':') {
        node_camera_failed(k + 1, resp + 2);
        return false;
    }
    if (resp[0] != 'Z' || resp[1] != ':') return false;
    s_link[k].z = atof(resp + 2);
    if (out_z) *out_z = s_link[k].z;

    const char *comma = strchr(resp + 2, ',');
    if (comma) {
        const char *c2 = strchr(comma + 1, ',');
        if (c2) {
            if (out_have_pre && out_pre) {
                double p = atof(c2 + 1);
                if (isfinite(p)) { *out_pre = p; *out_have_pre = true; }
            }
            const char *c3 = strchr(c2 + 1, ',');
            const char *c4 = c3 ? strchr(c3 + 1, ',') : NULL;
            if (c3 && c4 && out_have_h && out_h1 && out_h2) {
                double a = atof(c3 + 1), b = atof(c4 + 1);
                if (isfinite(a) && isfinite(b)) {
                    *out_h1 = a; *out_h2 = b; *out_have_h = true;
                }
            }
        }
    }
    return true;
}

/* Ask each node for its camera health (protocol 'D'). Only ever called between
 * loops, when the nodes are idle — never between an 'M' and its 'Z:' reply.
 * A missing/garbled answer is NOT treated as a disconnect: this is diagnostics,
 * and dropping a node over it would cost the session part of its SNR. */
void slaves_diag(void)
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
        N->cam_mbit      = mb;
        N->cam_stalls    = (uint32_t)st;
        N->cam_bias_now  = bias;
        N->cam_sigma_now = sigma;
        /* ",fw=<16 hex>" = the node's image, the same 16 characters its own
         * /status calls fw_sha. TAGGED and appended, so the field-order parse
         * above cannot trip over it and a slave too old to send it is simply
         * absent rather than misread — a positional last field would have had
         * to be told apart from the stuck-frame count by shape alone. */
        const char *fw = strstr(resp, ",fw=");
        if (fw && strspn(fw + 4, "0123456789abcdef") == 16) {
            memcpy(N->fw_sha, fw + 4, 16);
            N->fw_sha[16] = '\0';
        }
        /* ",raw=<bias>,<sigma>" — the PRE-FOLD pair (D43), tagged for the
         * same reason ,fw= is. Cleared first: a node that stops reporting it
         * must read as absent, not as whatever it said last time. */
        N->cam_raw_bias = 0.0f; N->cam_raw_sigma = 0.0f;
        const char *rw = strstr(resp, ",raw=");
        if (rw) {
            float rb = 0, rs = 0;
            if (sscanf(rw + 5, "%f,%f", &rb, &rs) == 2) {
                N->cam_raw_bias = rb; N->cam_raw_sigma = rs;
            }
        }
        /* ",exp=<exposure>,<gain>" — what the node is running on RIGHT NOW,
         * which is NOT cam_exp: that one is what the last sweep CHOSE, and a
         * manual POST /expose or a sweep that certified nothing makes the two
         * differ. A diagnostics view needs the live one. */
        /* ",t=<celsius>" — this node's OWN die temperature, the covariate the
         * offset monitor is regressed on. NAN when the node has no driver, and
         * NAN is what must travel: a plausible 0,0 would be regressed on. */
        N->die_temp_c = NAN;
        const char *tp = strstr(resp, ",t=");
        if (tp) {
            float t = 0;
            if (sscanf(tp + 3, "%f", &t) == 1) N->die_temp_c = t;
        }
        /* ",px=<mean pixel level>" — the light covariate (2026-08-28), stamped
         * into LoopStat.cam_px at every block close. Cleared first, and 0 is
         * how "this node does not send it" reads: a slave on older firmware
         * must be absent, not dark. Unlike die_temp this cannot travel as NAN,
         * because the archive column is a float that gets plotted. */
        N->cam_mean_px = 0.0f;
        const char *px = strstr(resp, ",px=");
        if (px) {
            float mp = 0;
            if (sscanf(px + 4, "%f", &mp) == 1) N->cam_mean_px = mp;
        }
        N->cam_exp_now = 0; N->cam_gain_now = 0;
        const char *ex = strstr(resp, ",exp=");
        if (ex) {
            unsigned long e = 0, g = 0;
            if (sscanf(ex + 5, "%lu,%lu", &e, &g) == 2) {
                N->cam_exp_now = (uint32_t)e; N->cam_gain_now = (uint16_t)g;
            }
        }
    }
    // A 'D' miss must not count toward the drop rule — it says nothing about
    // whether the node can still measure.
    for (int k = 0; k < s_nslaves; k++) s_link[k].miss_streak = 0;
}

void slave_probe(void) { nodes_discover(); }

/* Accessors, so the transport's state stays inside the transport. Both were
 * read directly from sensor.c before the split. */
bool nodes_have_slaves(void) { return s_slave_ok; }
int  nodes_slave_count(void) { return s_nslaves; }
