/* elotto network updater — the "OTA-Firmware" of docs/PLAN_NETWORK.md.
 *
 * Replaces the USB serial bootloader as the *workflow* for getting firmware
 * onto a node: Ethernet + HTTP + esp_ota, nothing else. No camera, no GCP, none
 * of the statistics tables — which is why it uses ~68 KB of RAM where the
 * application needs 421 KB static.
 *
 * ONE BINARY, TWO ROLES (deliberate):
 *   - in `factory`: the recovery updater. Never an OTA target, so no
 *     application image can destroy it. This is what makes "drop USB" safe.
 *   - in ota_0/ota_1: a known-good application that validates itself, so the
 *     update path can be proven end to end before a real app exists.
 *
 * All of the update and boot-safety logic lives in components/elotto_ota,
 * shared with the master and (from Phase C) the slave — one implementation of
 * the code that decides whether a node can still be reached.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_partition.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "elotto_ota.h"

static const char *TAG = "otafw";

/* Wiring identical to the master's (Waveshare ESP32-P4-ETH, IP101GRI over
 * RMII) — reused from elotto.c rather than the generic IDF example, because
 * this combination is already proven on this hardware. */
#define ETH_MDC_GPIO      31
#define ETH_MDIO_GPIO     52
#define ETH_PHY_RST_GPIO  51
#define ETH_PHY_ADDR       1
#define ETH_GOT_IP_BIT    BIT0

static EventGroupHandle_t s_eth_events;

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

/* ── GET /info — chip and partition inventory, the esptool-equivalent view ── */

static esp_err_t info_handler(httpd_req_t *req)
{
    char buf[1024];
    int  pos = 0;

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_ETH);
    uint32_t flash_sz = 0;
    esp_flash_get_size(NULL, &flash_sz);

    pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"role\":\"%s\",",
                    elotto_ota_running_from_slot() ? "app" : "factory");
    pos += elotto_ota_status_json(buf + pos, sizeof(buf) - pos);
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        ",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"flash_bytes\":%lu,"
        "\"heap_free\":%lu,\"heap_min\":%lu,\"partitions\":[",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], (unsigned long)flash_sz,
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)esp_get_minimum_free_heap_size());

    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY,
                                                     ESP_PARTITION_SUBTYPE_ANY, NULL);
    bool first = true;
    while (it) {
        const esp_partition_t *p = esp_partition_get(it);
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"label\":\"%s\",\"off\":\"0x%lx\",\"size\":\"0x%lx\"}",
            first ? "" : ",", p->label,
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

static esp_err_t root_handler(httpd_req_t *req)
{
    static const char page[] =
        "<!DOCTYPE html><meta charset=utf-8><title>elotto updater</title>"
        "<style>body{font-family:sans-serif;background:#0a2e0a;color:#eee;padding:24px}"
        "code{background:#00000055;padding:2px 6px;border-radius:4px}"
        "a{color:#90ee90}</style>"
        "<h2>elotto network updater</h2>"
        "<pre><code>curl http://&lt;ip&gt;/update --data-binary @build/app.bin</code></pre>"
        "<ul>"
        "<li><a href='/info'>/info</a> — chip, flash, partitions, running image</li>"
        "<li><a href='/otainfo'>/otainfo</a> — image version / slot / state</li>"
        "<li><code>POST /update?slot=next|0|1</code> — write, set boot, reboot</li>"
        "<li><code>POST /boot?slot=factory|0|1</code> — choose what boots</li>"
        "<li><code>POST /reboot</code></li>"
        "<li><code>POST /poison?on=1|2|0</code> — recovery tests: 1 = crash after"
        " validating (boot-counter path), 2 = crash before (rollback path)</li>"
        "</ul>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, page);
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size        = 8192;
    cfg.max_uri_handlers  = 10;
    cfg.recv_wait_timeout = 20;   /* a multi-hundred-KB upload must not time out */
    cfg.send_wait_timeout = 20;
    cfg.lru_purge_enable  = true;

    httpd_handle_t srv = NULL;
    ESP_ERROR_CHECK(httpd_start(&srv, &cfg));
    static const httpd_uri_t root = {"/",     HTTP_GET, root_handler, NULL};
    static const httpd_uri_t info = {"/info", HTTP_GET, info_handler, NULL};
    httpd_register_uri_handler(srv, &root);
    httpd_register_uri_handler(srv, &info);
    return srv;
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* First: crash-loop protection, before anything that can itself crash. */
    elotto_ota_boot_check();

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
        httpd_handle_t srv = start_webserver();
        elotto_ota_register(srv, NULL);   /* an updater is never "busy" */
        elotto_ota_mark_valid();
        ESP_LOGI(TAG, "updater ready");
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(TAG, "idle — heap %lu", (unsigned long)esp_get_free_heap_size());
    }
}
