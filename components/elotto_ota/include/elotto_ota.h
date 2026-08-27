#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

/* Shared network-update endpoint and boot-safety logic.
 *
 * SHARED between the factory updater (ota_firmware/), the master (elotto) and
 * later the slave — one implementation of the code that decides whether a node
 * can still be reached. Three copies of this would be three chances to get a
 * recovery path subtly wrong on one node only.
 *
 * Typical wiring in app_main():
 *
 *     elotto_ota_boot_check();            // FIRST, before networking
 *     ... bring up Ethernet, start httpd ...
 *     elotto_ota_register(server, is_busy);
 *     elotto_ota_mark_valid();            // only once the server answers
 */

/* Return true to refuse an incoming update — e.g. a measurement is running and
 * flashing would destroy it. Pass NULL to always accept. */
typedef bool (*elotto_ota_busy_fn)(void);

/* Counts boot attempts that never reached a healthy uptime and falls back to
 * the factory updater after BOOT_ATTEMPT_LIMIT of them.
 *
 * ⚠ The count published as `fw_boot_attempts` in /otainfo is ATTEMPTS, not
 * failures: every boot from a slot increments it and only HEALTHY_UPTIME_MS of
 * uptime clears it. A node reads 1 for its first 30 seconds after any flash.
 * Poll it later than that, or it always looks like something went wrong.
 *
 * This is the only escape from a validated image that crash-loops: rollback is
 * disarmed the moment an image is marked valid, so the bootloader would happily
 * re-launch it forever. Must run before anything that can itself crash.
 * No-op when running from the factory partition. */
void elotto_ota_boot_check(void);

/* Marks the running image valid and cancels rollback.
 *
 * The criterion is deliberately "can I still be updated?", NOT "is the app
 * correct?" — call it only once the network is up and the HTTP server is
 * answering. An image that validates is then by construction remotely
 * updatable however broken the rest of it is, and anything else stays
 * recoverable over the wire. No-op when not in PENDING_VERIFY. */
void elotto_ota_mark_valid(void);

/* Registers POST /update, /boot, /reboot and GET /otainfo on an existing
 * server. Needs 4 free URI handler slots. */
esp_err_t elotto_ota_register(httpd_handle_t server, elotto_ota_busy_fn busy);

/* JSON key/value fragment describing the running image — no surrounding braces,
 * no leading or trailing comma — for embedding in an existing status document.
 * Returns the number of characters written. */
int elotto_ota_status_json(char *buf, int cap);

/* True when this image is running from an OTA slot rather than factory. */
bool elotto_ota_running_from_slot(void);

/* CSRF guard for state-changing endpoints (POST /update, /boot, /reboot,
 * /poison, and every other state-changing handler in the firmwares).
 *
 * A browser cross-origin POST is a "simple request": CORS blocks the attacker
 * from READING the response, but the request itself is sent and executes. So a
 * page on any site could POST /abort to this node. The guard: when an Origin
 * header is present, its host must equal the request's own Host header
 * (same-origin). A request with no Origin header (curl, scripts, same-machine
 * tools) passes — the operator's whole update workflow runs over curl.
 * Returns true to allow the request. */
bool elotto_httpd_same_origin(httpd_req_t *req);
