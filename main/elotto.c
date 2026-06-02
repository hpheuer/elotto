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
#include "nvs_flash.h"
#include "esp_timer.h"
#include "sensor.h"

static const char *TAG = "ELOTTO";

#define ETH_MDC_GPIO      31
#define ETH_MDIO_GPIO     52
#define ETH_PHY_RST_GPIO  51
#define ETH_PHY_ADDR       1
#define ETH_GOT_IP_BIT    BIT0

static EventGroupHandle_t eth_event_group;

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(eth_event_group, ETH_GOT_IP_BIT);
    }
}

/* ── HTML ─────────────────────────────────────────────────────────── */
static const char HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<title>E-Lotto GCP</title>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:sans-serif;"
"background:linear-gradient(160deg,#0a2e0a 0%,#1a5c1a 45%,#0a2e0a 100%);"
"min-height:100vh;color:#fff;padding:20px}"
".wrap{max-width:760px;margin:0 auto}"
"h1{text-align:center;font-size:2.5em;margin:16px 0 4px;text-shadow:0 2px 8px rgba(0,0,0,.5)}"
"#subtitle{text-align:center;color:#90ee90;font-size:1.1em;margin-bottom:28px;min-height:1.4em}"
".card{background:rgba(0,0,0,.38);border-radius:14px;padding:24px;margin-bottom:18px;"
"border:1px solid rgba(144,238,144,.2);box-shadow:0 4px 20px rgba(0,0,0,.4)}"
".btns{display:flex;gap:14px;justify-content:center;flex-wrap:wrap}"
".btn{border:none;padding:14px 34px;border-radius:9px;font-size:1.05em;font-weight:700;"
"cursor:pointer;transition:.2s;letter-spacing:.3px}"
".btn-euro{background:#4a9e4a;color:#fff}.btn-euro:hover{background:#3d8c3d}"
".btn-649{background:#2e7d9e;color:#fff}.btn-649:hover{background:#256a87}"
".btn-abort{background:#c0392b;color:#fff}.btn-abort:hover{background:#a93226}"
".prog-wrap{background:rgba(255,255,255,.15);border-radius:20px;height:26px;"
"margin:18px 0 10px;overflow:hidden}"
".prog-fill{background:linear-gradient(90deg,#4a9e4a,#90ee90);height:100%;"
"border-radius:20px;width:0%;transition:width .6s}"
".stats{display:flex;gap:12px;margin-top:8px}"
".stat{flex:1;text-align:center;background:rgba(0,0,0,.25);border-radius:8px;padding:10px}"
".sv{font-size:1.8em;font-weight:700;color:#90ee90}"
".sl{font-size:.78em;color:#aaa;margin-top:3px}"
"#msg{text-align:center;margin-top:12px;font-size:1.05em;font-weight:700;min-height:22px}"
"table{width:100%;border-collapse:collapse;font-size:.92em}"
"th{color:#90ee90;padding:9px 6px;border-bottom:2px solid rgba(144,238,144,.3);text-align:left}"
"td{padding:7px 6px;border-bottom:1px solid rgba(255,255,255,.08);vertical-align:middle}"
".num{display:inline-flex;align-items:center;justify-content:center;"
"background:#2e7d32;border-radius:50%;width:30px;height:30px;margin:2px;"
"font-weight:700;font-size:.88em;flex-shrink:0}"
".euro{background:#7b6e00}"
"</style></head><body>"
"<div class='wrap'>"
"<h1>&#9752; E-Lotto GCP</h1>"
"<div id='subtitle'>ESP32-P4 &bull; Hardware TRNG &bull; GCP-Analyse</div>"
"<div class='card'>"
"<div class='btns' id='startBtns'>"
"<button class='btn btn-euro' onclick='doStart(0)'>&#127808; Euro-Lotto</button>"
"<button class='btn btn-649'  onclick='doStart(1)'>&#127808; 6 aus 49</button>"
"</div>"
"<button class='btn btn-abort' id='btnAbort' onclick='doAbort()' style='display:none;margin:0 auto'>"
"&#9632; Abbrechen</button>"
"<div id='progArea' style='display:none'>"
"<div class='prog-wrap'><div class='prog-fill' id='pf'></div></div>"
"<div class='stats'>"
"<div class='stat'><div class='sv' id='sDone'>0</div><div class='sl'>Läufe</div></div>"
"<div class='stat'><div class='sv' id='sPct'>0%</div><div class='sl'>Fortschritt</div></div>"
"<div class='stat'><div class='sv' id='sTime'>0:00</div><div class='sl'>Zeit</div></div>"
"<div class='stat'><div class='sv' id='sEta'>-:--</div><div class='sl'>Noch ca.</div></div>"
"</div>"
"</div>"
"<div id='msg'></div>"
"</div>"
"<div class='card' id='resCard' style='display:none'>"
"<h3 id='resTitle' style='color:#90ee90;margin-bottom:14px'></h3>"
"<table><thead id='resHead'></thead>"
"<tbody id='resBody'></tbody></table>"
"</div>"
"</div>"
"<script>"
"var timer=null,curMode=0;"
"function fmt(ms){var s=Math.floor(ms/1000);return Math.floor(s/60)+':'+('0'+s%60).slice(-2);}"
"function setMode(mode){"
"document.getElementById('subtitle').textContent="
"mode===0?'Eurojackpot • 5 aus 50 + 2 Eurozahlen':'6 aus 49 Lotto';}"
"window.onload=function(){"
"fetch('/status').then(function(r){return r.json();}).then(function(d){"
"if(d.state==='running'){"
"curMode=d.mode==='euro'?0:1;setMode(curMode);"
"document.getElementById('startBtns').style.display='none';"
"document.getElementById('btnAbort').style.display='block';"
"document.getElementById('progArea').style.display='block';"
"if(timer)clearInterval(timer);timer=setInterval(poll,1000);"
"}else if(d.state==='done'||d.state==='aborted'){"
"curMode=d.mode==='euro'?0:1;setMode(curMode);"
"document.getElementById('msg').textContent="
"d.state==='done'?'✅ Fertig! ('+fmt(d.elapsed_ms)+')'"
":'⚠️ Abgebrochen nach '+d.completed+' Läufen';"
"showResults(d.results,d.mode);"
"}}).catch(function(){});};"
"function doStart(mode){"
"curMode=mode;"
"fetch('/start?mode='+mode,{method:'POST'});"
"document.getElementById('startBtns').style.display='none';"
"document.getElementById('btnAbort').style.display='block';"
"document.getElementById('progArea').style.display='block';"
"document.getElementById('resCard').style.display='none';"
"document.getElementById('msg').textContent='';"
"setMode(mode);"
"if(timer)clearInterval(timer);"
"timer=setInterval(poll,1000);"
"}"
"function doAbort(){"
"fetch('/abort',{method:'POST'});"
"document.getElementById('msg').textContent='Abbruch läuft...';"
"}"
"function poll(){"
"fetch('/status').then(function(r){return r.json();}).then(function(d){"
"var pct=d.total>0?Math.round(d.completed*100/d.total):0;"
"document.getElementById('pf').style.width=pct+'%';"
"document.getElementById('sDone').textContent=d.completed+'/'+d.total;"
"document.getElementById('sPct').textContent=pct+'%';"
"document.getElementById('sTime').textContent=fmt(d.elapsed_ms);"
"if(d.completed>0&&d.elapsed_ms>0){"
"var eta=Math.round(d.elapsed_ms/d.completed*(d.total-d.completed));"
"document.getElementById('sEta').textContent=fmt(eta);"
"}else document.getElementById('sEta').textContent='-:--';"
"if(d.state==='done'||d.state==='aborted'){"
"clearInterval(timer);"
"document.getElementById('btnAbort').style.display='none';"
"document.getElementById('startBtns').style.display='flex';"
"var done=d.state==='done';"
"document.getElementById('msg').textContent="
"done?'✅ Fertig! ('+fmt(d.elapsed_ms)+')'"
":'⚠️ Abgebrochen nach '+d.completed+' Läufen';"
"showResults(d.results,d.mode);"
"}"
"}).catch(function(){});"
"}"
"function showResults(res,mode){"
"if(!res||res.length===0)return;"
"var isEuro=mode==='euro';"
"document.getElementById('resTitle').innerHTML="
"'☘️ Top-'+res.length+(isEuro?' Eurojackpot-Läufe':' 6-aus-49-Läufe');"
"document.getElementById('resHead').innerHTML="
"'<tr><th>#</th><th>Lauf</th><th>Z-Score</th><th>p-Wert</th><th>Zahlen</th>'"
"+(isEuro?'<th>Eurozahlen</th>':'')+'</tr>';"
"var tb=document.getElementById('resBody');tb.innerHTML='';"
"for(var i=0;i<res.length;i++){"
"var r=res[i],nums='';"
"for(var j=0;j<r.nums.length;j++)"
"nums+='<span class=\"num\">'+r.nums[j]+'</span>';"
"var estr='';"
"if(isEuro&&r.euro&&r.euro.length)"
"for(var j=0;j<r.euro.length;j++)"
"estr+='<span class=\"num euro\">'+r.euro[j]+'</span>';"
"tb.innerHTML+='<tr><td>'+(i+1)+'</td><td>'+r.run+'</td>"
"<td>'+r.z.toFixed(4)+'</td><td>'+r.p+'</td><td>'+nums+'</td>"
"'+(isEuro?'<td>'+estr+'</td>':'')+'</tr>';"
"}"
"document.getElementById('resCard').style.display='block';"
"}"
"</script></body></html>";

/* ── /status JSON ─────────────────────────────────────────────────── */
static esp_err_t status_handler(httpd_req_t *req)
{
    static char buf[4096];
    int  pos = 0;
    const char *state_str =
        g_status.state == ELOTTO_RUNNING ? "running" :
        g_status.state == ELOTTO_DONE    ? "done"    :
        g_status.state == ELOTTO_ABORTED ? "aborted" : "idle";
    const char *mode_str =
        g_status.mode == MODE_EUROJACKPOT ? "euro" : "649";

    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "{\"state\":\"%s\",\"mode\":\"%s\","
        "\"completed\":%d,\"total\":%d,\"elapsed_ms\":%lld,\"results\":[",
        state_str, mode_str,
        g_status.runs_completed, g_status.runs_total,
        (long long)g_status.elapsed_ms);

    int show = 0;
    if (g_status.state == ELOTTO_DONE || g_status.state == ELOTTO_ABORTED) {
        show = g_status.runs_completed < TOP_N ? g_status.runs_completed : TOP_N;
    }
    bool euro = (g_status.mode == MODE_EUROJACKPOT);

    for (int i = 0; i < show; i++) {
        RunResult *r = &g_status.results[i];
        if (i) pos += snprintf(buf+pos, sizeof(buf)-pos, ",");
        if (euro) {
            pos += snprintf(buf+pos, sizeof(buf)-pos,
                "{\"run\":%d,\"z\":%.4f,\"p\":\"%s\","
                "\"nums\":[%d,%d,%d,%d,%d],\"euro\":[%d,%d]}",
                r->index, r->z_score, r->p_value,
                r->nums[0], r->nums[1], r->nums[2], r->nums[3], r->nums[4],
                r->euro[0], r->euro[1]);
        } else {
            pos += snprintf(buf+pos, sizeof(buf)-pos,
                "{\"run\":%d,\"z\":%.4f,\"p\":\"%s\","
                "\"nums\":[%d,%d,%d,%d,%d,%d],\"euro\":[]}",
                r->index, r->z_score, r->p_value,
                r->nums[0], r->nums[1], r->nums[2],
                r->nums[3], r->nums[4], r->nums[5]);
        }
    }
    pos += snprintf(buf+pos, sizeof(buf)-pos, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ── /start POST ──────────────────────────────────────────────────── */
static esp_err_t start_handler(httpd_req_t *req)
{
    if (g_status.state != ELOTTO_RUNNING) {
        // mode aus Query-String lesen (?mode=0 oder ?mode=1)
        char qry[32] = "";
        if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) == ESP_OK) {
            char val[8] = "";
            httpd_query_key_value(qry, "mode", val, sizeof(val));
            g_status.mode = (val[0] == '1') ? MODE_LOTTO_649 : MODE_EUROJACKPOT;
        } else {
            g_status.mode = MODE_EUROJACKPOT;
        }
        xTaskCreate(elotto_task, "elotto", 8192, NULL, 5, NULL);
    }
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

/* ── /abort POST ──────────────────────────────────────────────────── */
static esp_err_t abort_handler(httpd_req_t *req)
{
    g_status.abort_requested = true;
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

/* ── GET / ────────────────────────────────────────────────────────── */
static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, HTML);
    return ESP_OK;
}

/* ── Webserver ────────────────────────────────────────────────────── */
static void start_webserver(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.stack_size = 8192;
    httpd_handle_t srv = NULL;
    ESP_ERROR_CHECK(httpd_start(&srv, &cfg));
    static const httpd_uri_t uris[] = {
        {"/",       HTTP_GET,  root_handler,   NULL},
        {"/status", HTTP_GET,  status_handler, NULL},
        {"/start",  HTTP_POST, start_handler,  NULL},
        {"/abort",  HTTP_POST, abort_handler,  NULL},
    };
    for (int i = 0; i < 4; i++)
        httpd_register_uri_handler(srv, &uris[i]);
    ESP_LOGI(TAG, "Webserver läuft");
}

/* ── Ethernet ─────────────────────────────────────────────────────── */
static void ethernet_init(void)
{
    esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&ncfg);

    eth_mac_config_t        mac_cfg  = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.smi_gpio.mdc_num   = ETH_MDC_GPIO;
    emac_cfg.smi_gpio.mdio_num  = ETH_MDIO_GPIO;
    emac_cfg.interface          = EMAC_DATA_INTERFACE_RMII;
    emac_cfg.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr           = ETH_PHY_ADDR;
    phy_cfg.reset_gpio_num     = ETH_PHY_RST_GPIO;
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_cfg);

    esp_eth_config_t  eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t  eth_hdl = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &eth_hdl));
    esp_netif_attach(netif, esp_eth_new_netif_glue(eth_hdl));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip_event, NULL, NULL));
    esp_eth_start(eth_hdl);
}

/* ── Webserver Task ───────────────────────────────────────────────── */
static void webserver_task(void *arg)
{
    EventBits_t bits = xEventGroupWaitBits(eth_event_group, ETH_GOT_IP_BIT,
                                           pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
    if (bits & ETH_GOT_IP_BIT) start_webserver();
    else ESP_LOGE(TAG, "Kein Ethernet nach 30s");
    vTaskDelete(NULL);
}

/* ── app_main ─────────────────────────────────────────────────────── */
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    eth_event_group = xEventGroupCreate();
    ethernet_init();
    xTaskCreate(webserver_task, "ws_task", 8192, NULL, 5, NULL);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(TAG, "Heap: %lu", (unsigned long)esp_get_free_heap_size());
    }
}
