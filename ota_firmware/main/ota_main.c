/* elotto network updater — the "OTA-Firmware" of docs/PLAN_NETWORK.md.
 *
 * Replaces the USB serial bootloader as the *workflow* for getting firmware onto
 * a node: Ethernet + HTTP + esp_ota, nothing else. No camera, no GCP, none of the
 * statistics tables — which is why it fits the ~130 KB RAM budget while the
 * application needs 421 KB static.
 *
 * ONE BINARY, TWO ROLES (deliberate):
 *   - in `factory`: the recovery updater. Never an OTA target, so no application
 *     image can destroy it. This is what makes "drop USB" safe.
 *   - in ota_0/ota_1: a known-good application that validates itself, so the
 *     update path can be proven end to end before a real app exists.
 *
 * Recovery layering (PLAN_NETWORK §3a) — reset alone recovers nothing, the
 * bootloader just re-boots whatever otadata points at:
 *   1. crash before validating   -> rollback (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)
 *   2. cannot be updated         -> never validate on mere liveness, see below
 *   3. validated, crash-loops    -> boot counter falls back to `factory`
 *   4. validated, hangs          -> GPIO factory reset (bootloader, not this code)
 *   5. corrupt bootloader        -> USB. Irreducible: the P4 has no
 *                                   SOC_RECOVERY_BOOTLOADER_SUPPORTED.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_desc.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "otafw";

/* Board wiring is identical to the master's (Waveshare ESP32-P4-ETH, IP101GRI
 * over RMII) — reused from elotto.c rather than the generic IDF example, because
 * this combination is already proven on this hardware. */
#define ETH_MDC_GPIO      31
#define ETH_MDIO_GPIO     52
#define ETH_PHY_RST_GPIO  51
#define ETH_PHY_ADDR       1
#define ETH_GOT_IP_BIT    BIT0

/* A validated image that later crash-loops cannot be rolled back — rollback is
 * disarmed the moment it is marked valid. So count boots that never reached a
 * healthy uptime and fall back to `factory` after this many. */
#define BOOT_FAIL_LIMIT   3
#define HEALTHY_UPTIME_MS 30000

#define NVS_NS            "otaboot"
#define NVS_KEY_FAILS     "fails"
#define NVS_KEY_POISON    "poison"

static EventGroupHandle_t s_eth_events;
static bool               s_from_ota;      /* running from an OTA slot, not factory */
static volatile bool      s_update_busy;   /* one upload at a time */

/* ── boot-failure counter ─────────────────────────────────────────────── */

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

/* Cleared only after HEALTHY_UPTIME_MS, never at startup: an app that crashes
 * five seconds in must not get to reset its own failure count on the way. */
static void health_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(HEALTHY_UPTIME_MS));
    nvs_set_u8_commit(NVS_KEY_FAILS, 0);
    ESP_LOGI(TAG, "healthy for %d s — boot-failure counter cleared",
             HEALTHY_UPTIME_MS / 1000);
    vTaskDelete(NULL);
}

/* Test hooks for the Phase A gates. Both are skipped when running from factory,
 * so the recovery image can never poison itself into a loop.
 *
 *   POISON_LATE  (1): crash 5 s AFTER validating. Rollback is already disarmed
 *                     at that point, so only the boot counter can save the node
 *                     — this is gate 3. Persists until cleared, because the
 *                     point is to fail repeatedly.
 *   POISON_EARLY (2): crash BEFORE Ethernet, so the image never validates and
 *                     the bootloader rolls back — this is gate 2. One-shot:
 *                     cleared the moment it is read, or the image we roll back
 *                     to would crash too and the two gates would blur together.
 */
#define POISON_LATE   1
#define POISON_EARLY  2

static void poison_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP_LOGE(TAG, "poison(late) — aborting to simulate a validated app that crash-loops");
    abort();
}

/* ── Ethernet ─────────────────────────────────────────────────────────── */

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_eth_events, ETH_GOT_IP_BIT);
    }
}

static void ethernet_init(void)
{
    esp_netif_config_t ncfg  = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t       *netif = esp_netif_new(&ncfg);

    eth_mac_config_t        mac_cfg  = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.smi_gpio.mdc_num  = ETH_MDC_GPIO;
    emac_cfg.smi_gpio.mdio_num = ETH_MDIO_GPIO;
    emac_cfg.interface         = EMAC_DATA_INTERFACE_RMII;
    emac_cfg.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr       = ETH_PHY_ADDR;
    phy_cfg.reset_gpio_num = ETH_PHY_RST_GPIO;
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_cfg);

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_hdl = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &eth_hdl));
    esp_netif_attach(netif, esp_eth_new_netif_glue(eth_hdl));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip_event, NULL, NULL));
    esp_eth_start(eth_hdl);
}

/* ── helpers ──────────────────────────────────────────────────────────── */

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(700));   /* let the HTTP response drain first */
    esp_restart();
}

static void reboot_soon(void) { xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL); }

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

/* ── GET /info ────────────────────────────────────────────────────────── */

static esp_err_t info_handler(httpd_req_t *req)
{
    char buf[1024];
    int  pos = 0;

    const esp_partition_t *run  = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    const esp_app_desc_t  *desc = esp_app_get_description();

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_ETH);
    uint32_t flash_sz = 0;
    esp_flash_get_size(NULL, &flash_sz);

    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (s_from_ota) esp_ota_get_state_partition(run, &st);

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "{\"role\":\"%s\",\"running\":\"%s\",\"next\":\"%s\","
        "\"ota_state\":%d,\"boot_fails\":%d,\"poison\":%d,"
        "\"project\":\"%s\",\"version\":\"%s\",\"built\":\"%s %s\",\"idf\":\"%s\","
        "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"flash_bytes\":%lu,"
        "\"heap_free\":%lu,\"heap_min\":%lu,\"partitions\":[",
        s_from_ota ? "app" : "factory",
        run ? run->label : "?", next ? next->label : "?",
        (int)st, (int)nvs_get_u8_or(NVS_KEY_FAILS, 0),
        (int)nvs_get_u8_or(NVS_KEY_POISON, 0),
        desc->project_name, desc->version, desc->date, desc->time, desc->idf_ver,
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        (unsigned long)flash_sz,
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)esp_get_minimum_free_heap_size());

    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY,
                                                     ESP_PARTITION_SUBTYPE_ANY, NULL);
    bool first = true;
    while (it) {
        const esp_partition_t *p = esp_partition_get(it);
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"label\":\"%s\",\"type\":%d,\"sub\":%d,\"off\":\"0x%lx\",\"size\":\"0x%lx\"}",
            first ? "" : ",", p->label, (int)p->type, (int)p->subtype,
            (unsigned long)p->address, (unsigned long)p->size);
        first = false;
        it = esp_partition_next(it);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ── POST /update ─────────────────────────────────────────────────────────
 * Push, not pull: `curl --data-binary @app.bin http://<node>/update`. Every IDF
 * OTA example pulls from an HTTPS URL, which would need a server on the PC and a
 * second HTTP client here; receiving the body is simpler and reuses the server
 * already running. Body handling follows examples/protocols/http_server/
 * file_serving, the esp_ota sequence follows examples/system/ota/native_ota_example.
 */
#define UPLOAD_CHUNK 4096

static esp_err_t update_post_handler(httpd_req_t *req)
{
    if (s_update_busy) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "update already running");
        return ESP_FAIL;
    }
    const esp_partition_t *target = slot_from_query(req);
    if (!target) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no target partition");
        return ESP_FAIL;
    }
    if (target == esp_ota_get_running_partition()) {
        /* Never overwrite the slot we are executing from — this is what keeps a
         * failed transfer harmless: the running image stays intact throughout. */
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
    /* Passing the real size lets esp_ota erase only what is needed. */
    esp_err_t err = esp_ota_begin(target, total, &h);
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
        ESP_LOGE(TAG, "transfer failed with %d bytes left: %s", remaining, esp_err_to_name(err));
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

    /* A fresh image gets a fresh failure budget. */
    nvs_set_u8_commit(NVS_KEY_FAILS, 0);

    char msg[128];
    snprintf(msg, sizeof(msg), "ok: wrote %d bytes to %s, rebooting\n", total, target->label);
    httpd_resp_sendstr(req, msg);
    ESP_LOGI(TAG, "%s", msg);
    reboot_soon();
    return ESP_OK;
}

/* ── POST /boot?slot=… , /erase?part=… , /reboot , /poison?on=… ───────── */

static esp_err_t boot_post_handler(httpd_req_t *req)
{
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

static esp_err_t erase_post_handler(httpd_req_t *req)
{
    char qry[48], val[16] = "nvs";
    if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) == ESP_OK)
        httpd_query_key_value(qry, "part", val, sizeof(val));
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                        ESP_PARTITION_SUBTYPE_ANY, val);
    if (!p) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no such data partition"); return ESP_FAIL; }
    esp_err_t err = esp_partition_erase_range(p, 0, p->size);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "ok: erased %s\n", p->label);
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "ok: rebooting\n");
    reboot_soon();
    return ESP_OK;
}

/* Arms the Phase A recovery tests. ?on=1 late crash (gate 3, boot counter),
 * ?on=2 early crash (gate 2, rollback), ?on=0 clears. */
static esp_err_t poison_post_handler(httpd_req_t *req)
{
    char qry[32], val[8] = "1";
    if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) == ESP_OK)
        httpd_query_key_value(qry, "on", val, sizeof(val));
    uint8_t on = (uint8_t)atoi(val);
    if (on > POISON_EARLY) on = 0;
    nvs_set_u8_commit(NVS_KEY_POISON, on);
    const char *msg = (on == POISON_LATE)  ? "ok: poison=late (crash after validate)\n"
                    : (on == POISON_EARLY) ? "ok: poison=early (crash before validate)\n"
                                           : "ok: poison cleared\n";
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

static esp_err_t root_handler(httpd_req_t *req)
{
    static const char page[] =
        "<!DOCTYPE html><meta charset=utf-8><title>elotto updater</title>"
        "<style>body{font-family:sans-serif;background:#0a2e0a;color:#eee;padding:24px}"
        "code{background:#00000055;padding:2px 6px;border-radius:4px}</style>"
        "<h2>elotto network updater</h2>"
        "<p>Push firmware with:</p>"
        "<pre><code>curl --data-binary @build/app.bin http://&lt;ip&gt;/update</code></pre>"
        "<ul>"
        "<li><a style='color:#90ee90' href='/info'>/info</a> — role, slots, versions, boot-fail count</li>"
        "<li><code>POST /update?slot=next|0|1</code> — write an image, set boot, reboot</li>"
        "<li><code>POST /boot?slot=factory|0|1</code> — choose what boots</li>"
        "<li><code>POST /erase?part=nvs</code> — erase a data partition</li>"
        "<li><code>POST /reboot</code></li>"
        "<li><code>POST /poison?on=1|2|0</code> — test hooks: 1 = crash after"
        " validating (boot-counter path), 2 = crash before (rollback path)</li>"
        "</ul>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, page);
    return ESP_OK;
}

static void start_webserver(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size        = 8192;
    cfg.max_uri_handlers  = 10;
    cfg.recv_wait_timeout = 20;   /* a multi-hundred-KB upload must not time out */
    cfg.send_wait_timeout = 20;
    cfg.lru_purge_enable  = true;

    httpd_handle_t srv = NULL;
    ESP_ERROR_CHECK(httpd_start(&srv, &cfg));
    static const httpd_uri_t uris[] = {
        {"/",       HTTP_GET,  root_handler,        NULL},
        {"/info",   HTTP_GET,  info_handler,        NULL},
        {"/update", HTTP_POST, update_post_handler, NULL},
        {"/boot",   HTTP_POST, boot_post_handler,   NULL},
        {"/erase",  HTTP_POST, erase_post_handler,  NULL},
        {"/reboot", HTTP_POST, reboot_post_handler, NULL},
        {"/poison", HTTP_POST, poison_post_handler, NULL},
    };
    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++)
        httpd_register_uri_handler(srv, &uris[i]);
    ESP_LOGI(TAG, "updater HTTP server up");
}

/* ── app_main ─────────────────────────────────────────────────────────── */

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    const esp_partition_t *run = esp_ota_get_running_partition();
    s_from_ota = run && run->subtype != ESP_PARTITION_SUBTYPE_APP_FACTORY;
    ESP_LOGI(TAG, "running from %s (%s)", run ? run->label : "?",
             s_from_ota ? "OTA slot" : "factory / recovery");

    /* Gate 2 hook, before anything else: an image that dies here never reaches
     * validation, which is precisely the case rollback exists for. */
    if (s_from_ota && nvs_get_u8_or(NVS_KEY_POISON, 0) == POISON_EARLY) {
        nvs_set_u8_commit(NVS_KEY_POISON, 0);   /* one-shot — see poison_task() */
        ESP_LOGE(TAG, "poison(early) — aborting before validation; expect rollback");
        abort();
    }

    if (s_from_ota) {
        /* Failure case 3: an image marked valid can never be rolled back, so the
         * only way out of a crash loop is to count the boots ourselves. */
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

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_eth_events = xEventGroupCreate();
    ethernet_init();

    EventBits_t bits = xEventGroupWaitBits(s_eth_events, ETH_GOT_IP_BIT,
                                           pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
    if (!(bits & ETH_GOT_IP_BIT)) {
        /* Deliberately do NOT mark valid: an image that cannot be reached over
         * the network is exactly the image that must roll back. */
        ESP_LOGE(TAG, "no IP after 30 s — not validating this image");
    } else {
        start_webserver();
        if (s_from_ota) {
            esp_ota_img_states_t st;
            if (esp_ota_get_state_partition(run, &st) == ESP_OK &&
                st == ESP_OTA_IMG_PENDING_VERIFY) {
                /* The validation criterion is "can I still be updated?", not
                 * "is the app correct?". Ethernet is up and the server answers,
                 * so a new image can always be pushed — that is what must never
                 * be lost. Anything else is recoverable over the wire. */
                esp_ota_mark_app_valid_cancel_rollback();
                ESP_LOGI(TAG, "image marked valid (network reachable)");
            }
        }
        xTaskCreate(health_task, "health", 3072, NULL, 3, NULL);
    }

    if (s_from_ota && nvs_get_u8_or(NVS_KEY_POISON, 0) == POISON_LATE)
        xTaskCreate(poison_task, "poison", 3072, NULL, 3, NULL);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(TAG, "idle — heap %lu", (unsigned long)esp_get_free_heap_size());
    }
}
