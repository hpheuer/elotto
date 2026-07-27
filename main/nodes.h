/* ── The node array: UDP transport, discovery, and per-node health ──────
 *
 * Everything about talking to the other boards lives behind this header —
 * the broadcast link, discovery, the per-loop calibration handshake, the
 * drop/reboot policy, and the diagnostics poll. sensor.c keeps the GCP
 * statistics and calls in here when it needs the array to do something.
 *
 * The split is along the seam the code already had: every static this module
 * owns (the socket, the sequence number, the per-slave table) was used only by
 * the functions moved with it. Nothing here is a new abstraction — it is the
 * same code, in a file named after what it does.
 *
 * The transport itself is deliberately NOT abstracted away: a command leaves as
 * ONE broadcast datagram so every node starts within microseconds of the
 * others, which is the premise the ÷√k combine rests on. See elotto_link.h for
 * the framing and why every frame carries the sequence number it answers.
 */
#pragma once

#include <stdbool.h>

/* Reply windows. LINK_MEAS_MS is here rather than private because sensor.c
 * waits on a measurement round itself, right after its own local run. */
#define LINK_MEAS_MS    4000      // a run is ~1 s, so this is generous headroom

/* ── Session lifecycle ─────────────────────────────────────────────── */

// Broadcast for slaves and rebuild the node table. Called at every session
// start: there is no IP table and no configured node count, so a node that
// missed the last session rejoins simply by answering this one.
void nodes_discover(void);

// Discovery, for the UI's probe button. Same thing under the name elotto.c uses.
void slave_probe(void);

// True once at least one slave is participating. sensor.c checks this before
// waiting on a reply round, so a solo master never pays a timeout.
bool nodes_have_slaves(void);

// Slaves in the table (NOT counting the master). Slave index i is published as
// g_status.nodes[i + 1].
int  nodes_slave_count(void);

/* ── A measurement round ───────────────────────────────────────────── */

// Broadcast 'M' so every node measures the same window as the local run.
void slave_trigger(int nseg);

// Wait for replies to the command in flight. `critical` marks a round whose
// loss should count toward the drop rule. Returns how many nodes answered.
int  nodes_collect(int timeout_ms, bool critical);

/* This slave's z for the round just collected. Returns false if it did not
 * answer or replied 'E:' (in which case the camera-fault policy has already
 * run and the node is dropped). The z travels through the out-parameter rather
 * than a struct the caller reaches into, so the per-slave table stays private. */
bool node_take_z(int k, double *out_z);

/* ── Baseline, calibration, diagnostics ────────────────────────────── */

void slave_baseline_start(int n, int nseg);
void slave_baseline_wait(void);

// Broadcast 'K' and sweep the master's own ladder in parallel, then collect
// each node's chosen setting. Nodes land on different exposures on purpose.
void calibrate_all(void);

// Per-node camera health via 'D'. Between loops only, never between an 'M' and
// its 'Z:'. A missing answer is diagnostics-only and never drops a node.
void slaves_diag(void);

/* ── Faults ────────────────────────────────────────────────────────── */

// Abort the round in flight on every node.
void slave_abort(void);

// Restart slave `k` (0-based slave index, not a node index).
void slave_reboot(int k);

/* Report, drop and reboot the node whose camera stopped. `node` is a NODE
 * index — 0 is the master, which is reported and dropped but never rebooted,
 * because a restart would destroy the /loops history the operator needs. The
 * session aborts only if the drop would leave fewer than two nodes. */
void node_camera_failed(int node, const char *why);
