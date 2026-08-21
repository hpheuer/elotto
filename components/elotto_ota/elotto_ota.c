/* Shared network-update endpoint + boot-safety logic — see include/elotto_ota.h.
 *
 * esp_ota sequence follows examples/system/ota/native_ota_example; the body
 * handling follows examples/protocols/http_server/file_serving. IDF's examples
 * all *pull* from an HTTPS URL — this pushes instead, which needs no server on
 * the PC and reuses the webserver a node already runs.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "elotto_ota.h"

static const char *TAG = "ota";

#define BOOT_FAIL_LIMIT   3
#define HEALTHY_UPTIME_MS 30000
#define UPLOAD_CHUNK      4096

#define NVS_NS         "otaboot"
#define NVS_KEY_FAILS  "fails"
#define NVS_KEY_POISON "poison"

/* Test hooks for the recovery gates. Skipped when running from factory, so the
 * recovery image can never poison itself into a loop.
 *   LATE (1): crash after validating -> only the boot counter can help (gate 3).
 *             Persistent: the point is to fail repeatedly.
 *   EARLY(2): crash before validating -> the bootloader rolls back (gate 2).
 *             One-shot, or the image we roll back TO would crash as well and
 *             the two gates would blur into "everything dies". */
#define POISON_LATE  1
#define POISON_EARLY 2

static elotto_ota_busy_fn s_busy;
static volatile bool      s_update_busy;
static bool               s_from_slot;
static bool               s_checked;

/* ── NVS helpers ──────────────────────────────────────────────────────── */

static uint8_t nvs_get_u8_or(const char *key, uint8_t dflt)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return dflt;
    uint8_t v = dflt;
    if (nvs_get_u8(h, key, &v) != ESP_OK) v = dflt;
    nvs_close(h);
    return v;
}

static void nvs_set_u8_commit(const char *key, uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

bool elotto_ota_running_from_slot(void) { return s_from_slot; }

/* ── boot safety ──────────────────────────────────────────────────────── */

/* Cleared only after sustained uptime, never at startup: an image that dies
 * five seconds in must not get to reset its own failure count on the way past. */
static void health_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(HEALTHY_UPTIME_MS));
    nvs_set_u8_commit(NVS_KEY_FAILS, 0);
    ESP_LOGI(TAG, "healthy for %d s — boot-failure counter cleared", HEALTHY_UPTIME_MS / 1000);
    vTaskDelete(NULL);
}

static void poison_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP_LOGE(TAG, "poison(late) — aborting to simulate a validated app that crash-loops");
    abort();
}

void elotto_ota_boot_check(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    s_from_slot = run && run->subtype != ESP_PARTITION_SUBTYPE_APP_FACTORY;
    s_checked   = true;
    ESP_LOGI(TAG, "running from %s (%s)", run ? run->label : "?",
             s_from_slot ? "OTA slot" : "factory / recovery");
    if (!s_from_slot) return;

    if (nvs_get_u8_or(NVS_KEY_POISON, 0) == POISON_EARLY) {
        nvs_set_u8_commit(NVS_KEY_POISON, 0);   /* one-shot */
        ESP_LOGE(TAG, "poison(early) — aborting before validation; expect rollback");
        abort();
    }

    uint8_t fails = nvs_get_u8_or(NVS_KEY_FAILS, 0) + 1;
    nvs_set_u8_commit(NVS_KEY_FAILS, fails);
    ESP_LOGI(TAG, "boot attempt %u since last healthy run", fails);
    if (fails >= BOOT_FAIL_LIMIT) {
        const esp_partition_t *fac = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
        if (fac) {
            ESP_LOGE(TAG, "%u failed boots — falling back to factory updater", fails);
            nvs_set_u8_commit(NVS_KEY_FAILS, 0);
            esp_ota_set_boot_partition(fac);
            esp_restart();
        }
    }
}

void elotto_ota_mark_valid(void)
{
    if (!s_from_slot) return;
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "image marked valid (network reachable)");
    }
    xTaskCreate(health_task, "ota_health", 3072, NULL, 3, NULL);
    if (nvs_get_u8_or(NVS_KEY_POISON, 0) == POISON_LATE)
        xTaskCreate(poison_task, "ota_poison", 3072, NULL, 3, NULL);
}

/* ── status fragment ──────────────────────────────────────────────────── */

int elotto_ota_status_json(char *buf, int cap)
{
    const esp_partition_t *run  = esp_ota_get_running_partition();
    const esp_app_desc_t  *desc = esp_app_get_description();
    esp_ota_img_states_t   st   = ESP_OTA_IMG_UNDEFINED;
    if (s_from_slot && run) esp_ota_get_state_partition(run, &st);

    /* elf sha256 identifies the exact image — the one thing you must be able to
     * check after an update that reported success. */
    char sha[17] = {0};
    for (int i = 0; i < 8; i++) snprintf(sha + i * 2, 3, "%02x", desc->app_elf_sha256[i]);

    return snprintf(buf, cap,
        "\"fw_version\":\"%s\",\"fw_built\":\"%s %s\",\"fw_sha\":\"%s\","
        "\"fw_slot\":\"%s\",\"fw_state\":%d,\"fw_boot_fails\":%d",
        desc->version, desc->date, desc->time, sha,
        run ? run->label : "?", (int)st, (int)nvs_get_u8_or(NVS_KEY_FAILS, 0));
}

/* ── handlers ─────────────────────────────────────────────────────────── */

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(700));   /* let the HTTP response drain first */
    esp_restart();
}

static void reboot_soon(void) { xTaskCreate(restart_task, "ota_restart", 2048, NULL, 5, NULL); }

static const esp_partition_t *slot_from_query(httpd_req_t *req)
{
    char qry[48], val[16];
    if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) == ESP_OK &&
        httpd_query_key_value(qry, "slot", val, sizeof(val)) == ESP_OK) {
        if (!strcmp(val, "0"))
            return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                            ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
        if (!strcmp(val, "1"))
            return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                            ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
        if (!strcmp(val, "factory"))
            return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                            ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    }
    return esp_ota_get_next_update_partition(NULL);
}

/* IDF has no HTTPD_409_CONFLICT. Set it by hand rather than reusing 400/403:
 * a script must be able to tell "refused, try later" from "your image is bad". */
static esp_err_t refuse_409(httpd_req_t *req, const char *why)
{
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, why);
    return ESP_OK;
}

/* ── CSRF guard ─────────────────────────────────────────────────────────
 * See elotto_ota.h. Shared here so master, slave and updater apply the same
 * rule to their state-changing handlers. */
bool elotto_httpd_same_origin(httpd_req_t *req)
{
    char origin[128] = "", host[128] = "";
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK)
        return true;   /* no Origin header: curl / script / same-machine tool */
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK)
        return false;  /* cannot verify — refuse rather than trust */

    /* Host part of the Origin URL: skip "http://" / "https://", stop at ':' or
     * '/'. No port is expected on this LAN, but tolerate it by cutting both. */
    const char *o = origin;
    if      (strncmp(o, "http://",  7) == 0) o += 7;
    else if (strncmp(o, "https://", 8) == 0) o += 8;
    else return false;

    size_t ol = 0;
    while (o[ol] && o[ol] != ':' && o[ol] != '/') ol++;
    if (ol == 0) return false;

    /* Host header, also cut at an optional port. */
    size_t hl = 0;
    while (host[hl] && host[hl] != ':') hl++;
    if (hl == 0) return false;

    return ol == hl && strncasecmp(o, host, ol) == 0;
}

static esp_err_t refuse_403(httpd_req_t *req, const char *why)
{
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, why);
    return ESP_OK;
}

static bool origin_ok(httpd_req_t *req)
{
    if (elotto_httpd_same_origin(req)) return true;
    refuse_403(req, "cross-origin request refused");
    return false;
}

static esp_err_t update_post_handler(httpd_req_t *req)
{
    if (!origin_ok(req)) return ESP_OK;
    /* Refusing beats destroying: a flash mid-measurement silently throws away
     * however many hours the session had accumulated. */
    if (s_busy && s_busy())
        return refuse_409(req, "busy: a session is running — abort it first\n");
    if (s_update_busy)
        return refuse_409(req, "busy: an update is already running\n");
    const esp_partition_t *target = slot_from_query(req);
    if (!target) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no target partition");
        return ESP_FAIL;
    }
    /* Never overwrite the slot we execute from — this is what keeps a failed
     * transfer harmless: the running image stays intact throughout. */
    if (target == esp_ota_get_running_partition()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "refusing to overwrite running slot");
        return ESP_FAIL;
    }
    int total = req->content_len;
    if (total <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body / no Content-Length");
        return ESP_FAIL;
    }
    if (total > (int)target->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image larger than partition");
        return ESP_FAIL;
    }

    char *buf = malloc(UPLOAD_CHUNK);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    s_update_busy = true;
    ESP_LOGI(TAG, "update -> %s (%d bytes)", target->label, total);

    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(target, total, &h);   /* real size => erase only what is needed */
    if (err != ESP_OK) {
        free(buf);
        s_update_busy = false;
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    int remaining = total;
    while (remaining > 0) {
        int want = remaining < UPLOAD_CHUNK ? remaining : UPLOAD_CHUNK;
        int got  = httpd_req_recv(req, buf, want);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) { err = ESP_FAIL; break; }
        err = esp_ota_write(h, buf, got);
        if (err != ESP_OK) break;
        remaining -= got;
    }
    free(buf);

    if (err != ESP_OK) {
        esp_ota_abort(h);
        s_update_busy = false;
        ESP_LOGE(TAG, "transfer failed, %d bytes left: %s", remaining, esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "transfer failed");
        return ESP_FAIL;
    }
    err = esp_ota_end(h);          /* validates the image before we trust it */
    if (err != ESP_OK) {
        s_update_busy = false;
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    err = esp_ota_set_boot_partition(target);
    s_update_busy = false;
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    nvs_set_u8_commit(NVS_KEY_FAILS, 0);   /* a fresh image gets a fresh budget */

    char msg[128];
    snprintf(msg, sizeof(msg), "ok: wrote %d bytes to %s, rebooting\n", total, target->label);
    httpd_resp_sendstr(req, msg);
    ESP_LOGI(TAG, "%s", msg);
    reboot_soon();
    return ESP_OK;
}

static esp_err_t boot_post_handler(httpd_req_t *req)
{
    if (!origin_ok(req)) return ESP_OK;
    const esp_partition_t *p = slot_from_query(req);
    if (!p) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no such slot"); return ESP_FAIL; }
    esp_err_t err = esp_ota_set_boot_partition(p);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    nvs_set_u8_commit(NVS_KEY_FAILS, 0);
    char msg[64];
    snprintf(msg, sizeof(msg), "ok: next boot = %s\n", p->label);
    httpd_resp_sendstr(req, msg);
    reboot_soon();
    return ESP_OK;
}

static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    if (!origin_ok(req)) return ESP_OK;
    httpd_resp_sendstr(req, "ok: rebooting\n");
    reboot_soon();
    return ESP_OK;
}

/* Arms the recovery-path tests: ?on=1 late crash (boot counter), ?on=2 early
 * crash (rollback), ?on=0 clears. Kept in the shipped firmware on purpose —
 * these paths must be re-provable on every node, not just the first one. */
static esp_err_t poison_post_handler(httpd_req_t *req)
{
    if (!origin_ok(req)) return ESP_OK;
    char qry[32], val[8] = "1";
    if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) == ESP_OK)
        httpd_query_key_value(qry, "on", val, sizeof(val));
    uint8_t on = (uint8_t)atoi(val);
    if (on > POISON_EARLY) on = 0;
    nvs_set_u8_commit(NVS_KEY_POISON, on);
    httpd_resp_sendstr(req, (on == POISON_LATE)  ? "ok: poison=late (crash after validate)\n"
                          : (on == POISON_EARLY) ? "ok: poison=early (crash before validate)\n"
                                                 : "ok: poison cleared\n");
    return ESP_OK;
}

static esp_err_t otainfo_get_handler(httpd_req_t *req)
{
    char buf[512];
    int  pos = snprintf(buf, sizeof(buf), "{\"role\":\"%s\",",
                        s_from_slot ? "app" : "factory");
    pos += elotto_ota_status_json(buf + pos, sizeof(buf) - pos);
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    pos += snprintf(buf + pos, sizeof(buf) - pos,
                    ",\"next_slot\":\"%s\",\"poison\":%d,\"heap_free\":%lu}",
                    next ? next->label : "?", (int)nvs_get_u8_or(NVS_KEY_POISON, 0),
                    (unsigned long)esp_get_free_heap_size());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

esp_err_t elotto_ota_register(httpd_handle_t server, elotto_ota_busy_fn busy)
{
    if (!server) return ESP_ERR_INVALID_ARG;
    if (!s_checked)
        ESP_LOGW(TAG, "elotto_ota_boot_check() was not called — no crash-loop protection");
    s_busy = busy;

    static const httpd_uri_t uris[] = {
        {"/update",  HTTP_POST, update_post_handler,  NULL},
        {"/boot",    HTTP_POST, boot_post_handler,    NULL},
        {"/reboot",  HTTP_POST, reboot_post_handler,  NULL},
        {"/poison",  HTTP_POST, poison_post_handler,  NULL},
        {"/otainfo", HTTP_GET,  otainfo_get_handler,  NULL},
    };
    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s: %s", uris[i].uri, esp_err_to_name(err));
            return err;
        }
    }
    ESP_LOGI(TAG, "update endpoints registered");
    return ESP_OK;
}
