#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Node link: the UDP transport that replaces the UART pair (docs/PLAN_NETWORK.md
 * §4, Phase C).
 *
 * SHARED between the master (elotto) and the slave (elotto_slave, which reaches
 * this through EXTRA_COMPONENT_DIRS=../elotto/components). The wire format lives
 * here and nowhere else — two hand-written parsers that disagree about framing
 * would produce a transport bug that looks exactly like a statistics bug.
 *
 * FRAME:  "EL1 <seq> <payload>"      (plain ASCII, one command per datagram)
 *
 * The payload is the *unchanged* UART command/reply text — "M", "B100", "D",
 * "A", "P" and the "Z:<z>,<C|T>" / "D:..." / "OK" answers. PLAN_NETWORK §4 asks
 * for exactly that: keep the command semantics, swap only the transport, so the
 * statistics layer needs no changes and the A/B compares like with like.
 *
 * WHY A SEQUENCE NUMBER: the UART link was effectively lossless and strictly
 * ordered, so a reply could only belong to the command just sent. UDP offers
 * neither guarantee. A reply that arrives after the master gave up would, in a
 * bare port of the UART code, be read as the answer to the *next* run — silently
 * pairing z_slave from run k with z_master from run k+1. That is a correlation
 * bug dressed as physics, and it is why every datagram carries the sequence
 * number it answers and mismatches are dropped and counted, never used.
 *
 * The master resends under the SAME sequence number, and a slave answers a
 * repeat of a command it has already completed from its one-entry reply cache.
 * So a lost *reply* costs a round trip rather than a second measurement, while a
 * lost *command* is re-executed exactly once.
 */

/* Slaves listen here for commands; the master binds the port below and sends to
 * the broadcast address, so a slave can simply answer the datagram's source. */
#define ELOTTO_LINK_CMD_PORT     5000
#define ELOTTO_LINK_MASTER_PORT  5001

#define ELOTTO_LINK_MAGIC        "EL1"
#define ELOTTO_LINK_MAX          160   /* longest frame: the 'D:' diagnostics reply */

/* Formats "EL1 <seq> <payload>" into buf; returns the length written (snprintf
 * semantics), or a negative value on error. */
int elotto_link_pack(char *buf, int cap, uint32_t seq, const char *payload);

/* Splits a NUL-terminated received frame in place. On success *payload points
 * into msg (not a copy) and *seq holds the sequence number. Returns false for
 * anything that is not a well-formed, non-empty frame — foreign broadcast
 * traffic on the same segment must be ignored, not misparsed. */
bool elotto_link_parse(char *msg, uint32_t *seq, char **payload);
