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
#include "esp_random.h"
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
"<h1>&#9752; E-Lotto <a href='https://grokipedia.com/page/Global_Consciousness_Project'"
" target='_blank' style='color:inherit;text-decoration:none;border-bottom:1px dashed #90ee90'>GCP</a></h1>"
"<div id='subtitle'>ESP32-P4 &bull; Hardware TRNG &bull; GCP-Analyse</div>"
"<div id='slaveBadge' style='display:none;text-align:center;color:#a0e8ff;"
"font-size:.88em;margin:-18px 0 12px'>&#128279; Dual-ESP aktiv &bull; SNR &times;&radic;2</div>"
"<div class='card'>"
"<div id='runsRow' style='display:grid;grid-template-columns:1fr 1fr;gap:8px 14px;justify-items:center;margin-bottom:10px'>"
"<div style='grid-column:span 2;display:flex;align-items:center;gap:6px'>"
"<label style='color:#f0c040;font-size:.9em'>Baseline-L&auml;ufe:</label>"
"<input id='numBaseline' type='number' value='100' min='10' max='5000' step='50'"
" style='width:70px;padding:5px 8px;border-radius:6px;border:1px solid #a08030;"
"background:#0a2e0a;color:#fff;font-size:1em;text-align:center'>"
"</div>"
"<button class='btn btn-euro' style='width:100%' onclick='doStart(0)'>&#127808; Euro-Lotto</button>"
"<button class='btn btn-649' style='width:100%' onclick='doStart(1)'>&#127808; 6 aus 49</button>"
"</div>"
"<div style='text-align:center;margin-bottom:6px'>"
"<span id='runsErr' style='color:#ff6b6b;font-size:.9em'></span>"
"</div>"
"<div id='startBtns' style='display:none'></div>"
"<button class='btn btn-abort' id='btnAbort' onclick='doAbort()' style='display:none;margin:0 auto'>"
"&#9632; Abbrechen</button>"
"<div id='progArea' style='display:none'>"
"<div id='calArea'>"
"<div style='color:#f0c040;font-size:.88em;margin-bottom:4px'>&#128295; Kalibrierung"
"<span id='calCheck'></span></div>"
"<div class='prog-wrap' style='height:18px'>"
"<div id='pfCal' style='background:linear-gradient(90deg,#a08030,#f0c040);"
"height:100%;border-radius:20px;width:0%;transition:width .5s'></div></div>"
"<div style='color:#f0c040;font-size:.9em;text-align:center;margin-top:4px'>"
"<span id='sCalDone'>0</span> / <span id='sCalTotal'>100</span> Läufe</div>"
"</div>"
"<div id='scoreArea' style='margin-top:14px'>"
"<div style='color:#6ab0e8;font-size:.88em;margin-bottom:4px'>&#127919; Zahlenbewertung"
"<span id='scoreCheck'></span></div>"
"<div class='prog-wrap' style='height:18px'>"
"<div id='pfScore' style='background:linear-gradient(90deg,#206090,#6ab0e8);"
"height:100%;border-radius:20px;width:0%;transition:width .5s'></div></div>"
"<div style='color:#6ab0e8;font-size:.9em;text-align:center;margin-top:4px'>"
"<span id='sScoreDone'>0</span> / <span id='sScoreTotal'>-</span> Zahlen</div>"
"</div>"
"<div id='loadArea' style='display:none;text-align:center;margin-top:12px'>"
"<input type='file' id='csvFiles' accept='.csv' multiple style='display:none' onchange='loadCsvFiles(this.files)'>"
"<button class='btn' onclick=\"document.getElementById('csvFiles').click()\" style='background:#2e5f8c;color:#fff;font-size:.9em;padding:9px 22px'>&#128194; Fr&uuml;here CSV laden</button>"
"<div id='loadStatus' style='color:#a0e8ff;font-size:.85em;margin-top:5px;min-height:1.2em'></div>"
"</div>"
"<div id='measArea' style='display:none;margin-top:14px'>"
"<div style='color:#90ee90;font-size:.88em;margin-bottom:4px'>&#128202; Messung"
"<span id='measCheck'></span></div>"
"<div class='prog-wrap'><div class='prog-fill' id='pf'></div></div>"
"<div class='stats'>"
"<div class='stat'><div class='sv' id='sDone'>0</div><div class='sl'>Läufe</div></div>"
"<div class='stat'><div class='sv' id='sPct'>0%</div><div class='sl'>Fortschritt</div></div>"
"<div class='stat'><div class='sv' id='sTime'>0 Min</div><div class='sl'>Zeit</div></div>"
"<div class='stat'><div class='sv' id='sEta'>-</div><div class='sl'>Noch ca.</div></div>"
"</div>"
"</div>"
"</div>"
"<div id='msg'></div>"
"</div>"
"<div class='card' id='resCard' style='display:none'>"
"<h3 id='resTitle' style='color:#90ee90;margin-bottom:14px'></h3>"
"<table><thead id='resHead'></thead>"
"<tbody id='resBody'></tbody></table>"
"<div style='text-align:center;margin-top:14px'>"
"<button id='btnSave' class='btn' onclick='doSave()' style='display:none;background:#2e7d32;color:#fff;padding:10px 28px'>&#128190; CSV speichern</button>"
"</div>"
"</div>"
"</div>"
"<script>"
"var timer=null,curMode=0,loadedData=[],lastData=null;"
"function fmt(ms){"
"var m=Math.floor(ms/60000),h=Math.floor(m/60);m=m%60;"
"return h>0?h+':'+('0'+m).slice(-2)+' Std':m+' Min';}"
"function fmtEta(ms){"
"if(ms<90000)return Math.ceil(ms/1000)+' Sek';"
"return fmt(ms);}"
"function setMode(mode){"
"document.getElementById('subtitle').textContent="
"mode===0?'Eurojackpot • 5 aus 50 + 2 Eurozahlen':'6 aus 49 Lotto';}"
"window.onload=function(){"
"fetch('/status').then(function(r){return r.json();}).then(function(d){"
"if(d.state==='running'){"
"curMode=d.mode==='euro'?0:1;setMode(curMode);"
"document.getElementById('runsRow').style.display='none';"
"document.getElementById('startBtns').style.display='none';"
"document.getElementById('btnAbort').style.display='block';"
"document.getElementById('progArea').style.display='block';"
"document.getElementById('sCalTotal').textContent=d.baseline_total;"
"document.getElementById('sScoreTotal').textContent=d.scoring_total||0;"
"if(d.scoring_total>0){"
"var sp=Math.round(d.scoring_done*100/d.scoring_total);"
"document.getElementById('pfScore').style.width=sp+'%';"
"document.getElementById('sScoreDone').textContent=d.scoring_done||0;"
"if(d.scoring_done>=d.scoring_total){"
"document.getElementById('scoreCheck').innerHTML=\" <span style='color:#90ee90;font-size:1.1em'>&#10004;</span>\";"
"document.getElementById('loadArea').style.display='';}"
"if(d.phase==='measuring')document.getElementById('measArea').style.display='';"
"if(timer)clearInterval(timer);timer=setInterval(poll,1000);"
"}}else if(d.state==='done'||d.state==='aborted'){"
"curMode=d.mode==='euro'?0:1;setMode(curMode);"
"document.getElementById('msg').textContent="
"d.state==='done'?'✅ Fertig! ('+fmt(d.elapsed_ms)+')'"
":'⚠️ Abgebrochen nach '+d.completed+' Läufen — bisherige Ergebnisse:';"
"showResults(d);"
"}}).catch(function(){});};"
"function doStart(mode){"
"curMode=mode;"
"var base=parseInt(document.getElementById('numBaseline').value)||100;"
"document.getElementById('runsErr').textContent='';"
"document.getElementById('sCalTotal').textContent=base;"
"document.getElementById('pfScore').style.width='0%';"
"document.getElementById('sScoreDone').textContent='0';"
"document.getElementById('scoreCheck').innerHTML='';"
"document.getElementById('measArea').style.display='none';"
"fetch('/start?mode='+mode+'&baseline='+base,{method:'POST'});"
"document.getElementById('runsRow').style.display='none';"
"document.getElementById('startBtns').style.display='none';"
"document.getElementById('btnAbort').style.display='block';"
"document.getElementById('progArea').style.display='block';"
"loadedData=[];lastData=null;"
"document.getElementById('loadArea').style.display='none';"
"document.getElementById('loadStatus').textContent='';"
"document.getElementById('btnSave').style.display='none';"
"document.getElementById('csvFiles').value='';"
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
"var stDone=d.state==='done'||d.state==='aborted';"
"var scorePct=d.scoring_total>0?Math.round(d.scoring_done*100/d.scoring_total):0;"
"document.getElementById('pfScore').style.width=scorePct+'%';"
"document.getElementById('sScoreDone').textContent=d.scoring_done||0;"
"document.getElementById('sScoreTotal').textContent=d.scoring_total||0;"
"if(d.scoring_total>0&&d.scoring_done>=d.scoring_total){"
"document.getElementById('scoreCheck').innerHTML=\" <span style='color:#90ee90;font-size:1.1em'>&#10004;</span>\";"
"document.getElementById('loadArea').style.display='';}"
"var calPct=d.baseline_total>0?Math.round(d.baseline_done*100/d.baseline_total):0;"
"document.getElementById('pfCal').style.width=calPct+'%';"
"document.getElementById('sCalDone').textContent=d.baseline_done;"
"document.getElementById('sCalTotal').textContent=d.baseline_total;"
"if(d.baseline_done>=d.baseline_total&&d.baseline_total>0)"
"document.getElementById('calCheck').innerHTML=\" <span style='color:#90ee90;font-size:1.1em'>&#10004;</span>\";"
"if(d.phase==='measuring'||stDone){"
"document.getElementById('measArea').style.display='';"
"var pct=d.total>0?Math.round(d.completed*100/d.total):0;"
"document.getElementById('pf').style.width=pct+'%';"
"document.getElementById('sDone').textContent=d.completed+'/'+d.total;"
"document.getElementById('sPct').textContent=pct+'%';"
"document.getElementById('sTime').textContent=fmt(d.elapsed_ms);"
"var prepDone=(d.baseline_done||0)+(d.scoring_done||0);"
"var totalDone=prepDone+d.completed;"
"if(d.slave)document.getElementById('slaveBadge').style.display='';"
"if(d.completed>0&&totalDone>0&&d.elapsed_ms>0){"
"var msPerRun=d.elapsed_ms/totalDone;"
"var eta=Math.round(msPerRun*(d.total-d.completed));"
"document.getElementById('sEta').textContent=fmtEta(eta);"
"}else document.getElementById('sEta').textContent='-';}"
"if(d.state==='done'||d.state==='aborted'){"
"clearInterval(timer);"
"document.getElementById('btnAbort').style.display='none';"
"document.getElementById('measCheck').innerHTML=\" <span style='color:#90ee90;font-size:1.1em'>&#10004;</span>\";"
"document.getElementById('runsRow').style.display='grid';"
"var done=d.state==='done';"
"document.getElementById('msg').textContent="
"done?'✅ Fertig! ('+fmt(d.elapsed_ms)+')'"
":'⚠️ Abgebrochen nach '+d.completed+' Läufen — bisherige Ergebnisse:';"
"showResults(d);"
"}"
"}).catch(function(){});"
"}"
"function showResults(d){"
"lastData=d;"
"var res=d.results,mode=d.mode;"
"if(!res||res.length===0)return;"
"if(loadedData.length>0){"
"var all=res.slice();"
"for(var i=0;i<loadedData.length;i++)"
"if(loadedData[i]._mode===mode)all.push(loadedData[i]);"
"all.sort(function(a,b){return b.z-a.z;});"
"res=all;}"
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
"if(d.freq_z2>0&&d.freq_nums){"
"var fn='',fe='',nc=isEuro?5:6;"
"for(var j=0;j<nc;j++)if(d.freq_nums[j])fn+='<span class=\"num\">'+d.freq_nums[j]+'</span>';"
"if(isEuro&&d.freq_euro)for(var j=0;j<2;j++)if(d.freq_euro[j])fe+='<span class=\"num euro\">'+d.freq_euro[j]+'</span>';"
"var sep='border-top:2px solid rgba(240,192,64,.4)';"
"tb.innerHTML+='<tr style=\"background:rgba(240,192,64,.08)\">'"
"+'<td colspan=\"4\" style=\"color:#f0c040;font-weight:700;'+sep+';padding-top:10px\">'"
"+'&#128197; Am h&auml;ufigsten ('+d.freq_z2+'&times; Z&gt;2):</td>'"
"+'<td style=\"'+sep+'\">'+fn+'</td>'"
"+(isEuro?'<td style=\"'+sep+'\">'+fe+'</td>':'')+'</tr>';"
"}"
"document.getElementById('btnSave').style.display='';"
"document.getElementById('resCard').style.display='block';"
"}"
"function doSave(){"
"if(!lastData)return;"
"var d=lastData,isEuro=d.mode==='euro',nc=isEuro?5:6;"
"var hdr=isEuro?'run,z_score,p_value,n1,n2,n3,n4,n5,e1,e2':'run,z_score,p_value,n1,n2,n3,n4,n5,n6';"
"var lines=['# mode='+d.mode+',date='+new Date().toISOString().slice(0,10),hdr];"
"for(var i=0;i<d.results.length;i++){"
"var r=d.results[i],cols=[r.run,r.z.toFixed(6),r.p];"
"for(var j=0;j<nc;j++)cols.push(r.nums[j]);"
"if(isEuro){cols.push(r.euro[0]);cols.push(r.euro[1]);}"
"lines.push(cols.join(','));}"
"var url=URL.createObjectURL(new Blob([lines.join('\\n')],{type:'text/csv'}));"
"var a=document.createElement('a');"
"a.href=url;"
"a.download='elotto_'+d.mode+'_'+new Date().toISOString().slice(0,10)+'.csv';"
"document.body.appendChild(a);a.click();"
"setTimeout(function(){URL.revokeObjectURL(url);document.body.removeChild(a);},100);"
"}"
"function loadCsvFiles(files){"
"if(!files||files.length===0)return;"
"var total=files.length,done=0;"
"loadedData=[];"
"for(var fi=0;fi<files.length;fi++){"
"(function(f){"
"var reader=new FileReader();"
"reader.onload=function(e){"
"parseCsvInto(e.target.result);"
"done++;"
"if(done===total){"
"document.getElementById('loadStatus').textContent="
"loadedData.length+' Läufe aus '+total+' Datei(en) geladen';"
"if(lastData)showResults(lastData);"
"}};"
"reader.readAsText(f);"
"})(files[fi]);}"
"document.getElementById('csvFiles').value='';"
"}"
"function parseCsvInto(text){"
"var lines=text.split('\\n');"
"var mm=text.match(/mode=(euro|649)/);"
"var m=mm?mm[1]:'euro';"
"for(var i=0;i<lines.length;i++){"
"var line=lines[i].trim();"
"if(!line||line[0]==='#'||line.slice(0,3)==='run')continue;"
"var p=line.split(',');"
"var need=m==='euro'?10:9;"
"if(p.length<need)continue;"
"var r={run:parseInt(p[0]),z:parseFloat(p[1]),p:p[2],nums:[],euro:[],_mode:m};"
"if(m==='euro'){for(var j=0;j<5;j++)r.nums.push(parseInt(p[3+j]));r.euro=[parseInt(p[8]),parseInt(p[9])];}"
"else{for(var j=0;j<6;j++)r.nums.push(parseInt(p[3+j]));r.euro=[];}"
"if(!isNaN(r.z)&&r.nums.length>0)loadedData.push(r);"
"}}"
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

    const char *phase_str =
        g_status.phase == PHASE_SCORING  ? "scoring"  :
        g_status.phase == PHASE_BASELINE ? "baseline" :
                                           "measuring";
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "{\"state\":\"%s\",\"mode\":\"%s\",\"phase\":\"%s\","
        "\"slave\":%s,"
        "\"scoring_done\":%d,\"scoring_total\":%d,"
        "\"baseline_done\":%d,\"baseline_total\":%d,\"baseline_mean\":%.4f,"
        "\"completed\":%d,\"total\":%d,\"elapsed_ms\":%lld,\"results\":[",
        state_str, mode_str, phase_str,
        g_status.slave_connected ? "true" : "false",
        g_status.scoring_done, g_status.scoring_total,
        g_status.baseline_done, g_status.baseline_total, g_status.baseline_mean,
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
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "],\"freq_z2\":%d,\"freq_nums\":[%d,%d,%d,%d,%d,%d],\"freq_euro\":[%d,%d]}",
        g_status.freq_z2_count,
        g_status.freq_nums[0], g_status.freq_nums[1], g_status.freq_nums[2],
        g_status.freq_nums[3], g_status.freq_nums[4], g_status.freq_nums[5],
        g_status.freq_euro[0], g_status.freq_euro[1]);

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
        char qry[48] = "";
        g_status.mode           = MODE_EUROJACKPOT;
        g_status.runs_total     = 0;   // wird in elotto_task aus Kombinatorik berechnet
        g_status.baseline_total = 100;
        if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) == ESP_OK) {
            char val[16] = "";
            if (httpd_query_key_value(qry, "mode", val, sizeof(val)) == ESP_OK)
                g_status.mode = (val[0] == '1') ? MODE_LOTTO_649 : MODE_EUROJACKPOT;
            if (httpd_query_key_value(qry, "baseline", val, sizeof(val)) == ESP_OK) {
                int b = atoi(val);
                if (b > 0 && b <= 5000) g_status.baseline_total = b;
            }
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

/* ── /diag GET – TRNG Register vs esp_random() Diagnose ──────────── */
#define DIAG_REG   0x501101A4UL
#define DIAG_N     100000   // Wörter pro Test
#define DIAG_SEGS  500      // Mini-Runs für Z-Score-Verteilung

static esp_err_t diag_handler(httpd_req_t *req)
{
    static char buf[2048];
    int pos = 0;

    // --- Test 1: Direktes TRNG-Register ---
    int64_t t0 = esp_timer_get_time();
    uint64_t ones_reg = 0;
    uint32_t last = 0, stuck_reg = 0;
    double   z_sum_reg = 0.0;

    for (int s = 0; s < DIAG_SEGS; s++) {
        int ones = 0;
        for (int w = 0; w < 7; w++) {
            uint32_t v = *((volatile uint32_t *)DIAG_REG);
            if (w > 0 && v == last) stuck_reg++;
            last = v;
            ones += (w < 6) ? __builtin_popcount(v)
                             : __builtin_popcount(v & 0xFF);
        }
        ones_reg += ones;
        z_sum_reg += (ones - 100.0) / 7.07106781;
    }
    int64_t dt_reg = (esp_timer_get_time() - t0) / 1000;

    double bias_reg  = (double)ones_reg / (DIAG_SEGS * 200.0);
    double z_std_reg = z_sum_reg / DIAG_SEGS;

    // --- Test 2: esp_random() ---
    t0 = esp_timer_get_time();
    uint64_t ones_esp = 0;
    uint32_t stuck_esp = 0; last = 0;
    double   z_sum_esp = 0.0;

    for (int s = 0; s < DIAG_SEGS; s++) {
        int ones = 0;
        for (int w = 0; w < 7; w++) {
            uint32_t v = esp_random();
            if (w > 0 && v == last) stuck_esp++;
            last = v;
            ones += (w < 6) ? __builtin_popcount(v)
                             : __builtin_popcount(v & 0xFF);
        }
        ones_esp += ones;
        z_sum_esp += (ones - 100.0) / 7.07106781;
    }
    int64_t dt_esp = (esp_timer_get_time() - t0) / 1000;

    double bias_esp  = (double)ones_esp / (DIAG_SEGS * 200.0);
    double z_std_esp = z_sum_esp / DIAG_SEGS;

    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "{"
        "\"reg_ms\":%lld,\"reg_bias\":%.6f,\"reg_stuck\":%lu,\"reg_z_mean\":%.4f,"
        "\"esp_ms\":%lld,\"esp_bias\":%.6f,\"esp_stuck\":%lu,\"esp_z_mean\":%.4f,"
        "\"speedup\":%.2f,\"segs\":%d"
        "}",
        (long long)dt_reg, bias_reg, (unsigned long)stuck_reg, z_std_reg,
        (long long)dt_esp, bias_esp, (unsigned long)stuck_esp, z_std_esp,
        dt_esp > 0 ? (double)dt_esp / dt_reg : 0.0,
        DIAG_SEGS);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
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
        {"/diag",   HTTP_GET,  diag_handler,   NULL},
    };
    for (int i = 0; i < 5; i++)
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
