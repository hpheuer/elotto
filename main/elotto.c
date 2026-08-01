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
#include "nodes.h"      // slave_probe(), for the UI's node-discovery button
#include "camera.h"
#include "elotto_ota.h"

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
// Bonus numbers are stars in the result tables too, not only in the Focus
// panel — same reason (1-12 overlaps 1-50, so shape carries the distinction),
// and a draw that reads one way live should read the same way afterwards.
// Slightly wider than a main circle with smaller type: a star's readable area
// is its inner pentagon, so a two-digit bonus needs the extra room.
".num.euro{background:linear-gradient(160deg,#ffdc6a,#dda200);color:#241c00;"
"border-radius:0;width:44px;height:42px;font-size:.82em;padding-top:7px;"
"clip-path:polygon(50% 0%,61% 35%,98% 35%,68% 57%,79% 91%,50% 70%,21% 91%,"
"32% 57%,2% 35%,39% 35%)}"
// Focus panel (PLAN_4NODE Phase 5). Salience over legibility: the observer is
// not meant to decode six numbers in half a second, they are meant to be
// present while those numbers are on screen and the noise is sampled. So the
// type is large and high-contrast because it must REGISTER, not because it must
// be parsed — and no attempt is made to shrink it to fit.
".numBig{display:inline-flex;align-items:center;justify-content:center;"
"background:#2e7d32;border-radius:50%;width:72px;height:72px;margin:4px;"
"font-weight:700;font-size:1.9em;flex-shrink:0;"
"box-shadow:0 0 18px rgba(144,238,144,.35)}"
// Eurojackpot's 1-12 bonus numbers are STARS, not circles. In a draw the panel
// shows 5 main + 2 euro side by side, so shape (not just colour) is what tells
// them apart at a glance — and glancing is the whole interaction here.
// clip-path rather than a glyph or an image: the page must stay self-contained,
// and a clipped element still centres its text normally. box-shadow would be
// clipped away with the corners, so the glow moves to a drop-shadow filter.
".numBig.euro{background:linear-gradient(160deg,#ffdc6a,#dda200);color:#2b2000;"
// Smaller type than a main circle on purpose: the readable area of a star is
// its inner pentagon (~36 px across here), not its bounding box, so 1.9em would
// leave a two-digit number touching the points.
"border-radius:0;box-shadow:none;width:100px;height:95px;padding-top:10px;"
"font-size:1.5em;"
"clip-path:polygon(50% 0%,61% 35%,98% 35%,68% 57%,79% 91%,50% 70%,21% 91%,"
"32% 57%,2% 35%,39% 35%);"
"filter:drop-shadow(0 0 10px rgba(240,192,64,.5))}"
"#focusBox{min-height:104px;display:flex;align-items:center;justify-content:center;"
"flex-wrap:wrap;gap:2px}"
// One label column of a FIXED width, so every input/select starts at the same x.
// The rows used to be independently centred flex boxes, which left each control
// hanging off a label of a different length.
".frow{grid-column:span 2;display:grid;grid-template-columns:150px 1fr;"
"align-items:center;gap:8px}"
".frow>label,.frow>span:first-child{color:#f0c040;font-size:.9em;text-align:right}"
".fin{width:80px;padding:5px 8px;border-radius:6px;border:1px solid #a08030;"
"background:#0a2e0a;color:#fff;font-size:1em;text-align:center}"
// Pause/Continue differ in width, and .btns is centre-justified, so the pair
// used to shift sideways every time the label changed. A floor wide enough for
// the longer word pins both.
"#btnPause,#btnAbort{min-width:168px}"
".tblwrap{overflow-x:auto}"
// Phone layout: shrink the CHROME only. The Focus numbers (.numBig) are sized
// for salience by the deliberate decision documented above and are left alone —
// #focusBox already wraps, so a narrow screen costs rows, not legibility.
"@media(max-width:520px){"
"body{padding:12px}"
"h1{font-size:1.7em;margin:8px 0 4px}"
"#subtitle{font-size:.9em;margin-bottom:16px}"
".card{padding:14px;border-radius:11px}"
".btn{padding:11px 16px;font-size:.95em}"
"#btnPause,#btnAbort{min-width:126px}"
".frow{grid-template-columns:118px 1fr;gap:6px}"
".frow>label,.frow>span:first-child{font-size:.8em}"
".sv{font-size:1.3em}"
".sl{font-size:.68em}"
".stats{gap:6px}"
".stat{padding:7px 4px}"
"table{font-size:.8em}"
"th,td{padding:6px 3px}"
".num{width:26px;height:26px;font-size:.8em}"
".num.euro{width:38px;height:36px;font-size:.74em;padding-top:6px}"
"#msg{font-size:.95em}"
"}"
"</style></head><body>"
"<div class='wrap'>"
"<h1>&#9752; E-Lotto <a href='https://grokipedia.com/page/Global_Consciousness_Project'"
" target='_blank' style='color:inherit;text-decoration:none;border-bottom:1px dashed #90ee90'>GCP</a></h1>"
"<div id='subtitle'>ESP32-P4 &bull; Camera dark-frame entropy &bull; GCP Analysis</div>"
"<div id='slaveBadge' style='display:none;text-align:center;color:#a0e8ff;"
"font-size:.88em;margin:-18px 0 12px'></div>"
"<div class='card'>"
"<div id='runsRow' style='display:grid;grid-template-columns:1fr 1fr;gap:8px 14px;margin-bottom:10px'>"
"<div class='frow'>"
"<label for='numBaseline'>Baseline runs:</label>"
// Defaults per the Phase 5 timing budget, restated for the uniform 1000 ms
// window: the cycle is ~1.38 s (1027 ms lit + 350 ms blank), so a ~10 min
// measurement loop is ~430 runs, not the 850 the 500 ms draw allowed.
// `Runs = 0` (all) still works, and 430 of 5005 is still stride-sampled across
// the whole combination space rather than a lexicographic prefix.
"<input id='numBaseline' class='fin' type='number'"
" value='" EL_STR(BASELINE_DEFAULT) "' min='" EL_STR(BASELINE_MIN) "'"
" max='" EL_STR(BASELINE_MAX) "' step='" EL_STR(BASELINE_STEP) "'>"
"</div>"
"<div class='frow'>"
"<label for='numLoops'>Loops:</label>"
"<input id='numLoops' class='fin' type='number'"
" value='" EL_STR(LOOPS_DEFAULT) "' min='1' max='" EL_STR(LOOPS_MAX) "' step='1'>"
"</div>"
"<div class='frow'>"
"<label for='numRuns'>Runs (0=all):</label>"
"<input id='numRuns' class='fin' type='number'"
" value='" EL_STR(RUNS_DEFAULT) "' min='0' max='" EL_STR(NUM_RUNS) "' step='1'>"
"</div>"
"<div class='frow'>"
"<label for='selRank'>Ranking:</label>"
"<select id='selRank' style='padding:5px 8px;border-radius:6px;border:1px solid #a08030;"
"background:#0a2e0a;color:#fff;font-size:.92em;max-width:100%'>"
"<option value='2'>Most extreme Z (keeps the peak per combination)</option>"
"<option value='3'>Most extreme Z, no studentization (raw scale &#9888;)</option>"
"<option value='1'>Cumulative Z (Stouffer)</option>"
"<option value='0'>Peak Z (best single run, pool re-scored each loop)</option>"
"</select>"
"</div>"
// No entropy selector: photons or nothing. The on-chip TRNG was removed from
// the firmware entirely (sensor.h) because an option to produce an
// indistinguishable result from a digital source is a liability, not a feature.
"<div class='frow'>"
"<span>Entropy:</span>"
"<span style='font-size:.92em'>&#128247; OV5647 dark-frame photons"
"<span style='color:#8fae8f'> &middot; the only source</span></span>"
"</div>"
// An attended session is a different experiment from an unattended one, so the
// choice is explicit and the answer is recorded with the results. Unchecking it
// gives the matched control the comparison needs — same mode, runs, segment
// counts, source and loops, panel off.
// Empty first cell so the checkbox lines up with the inputs above it rather
// than with their labels.
"<div class='frow'>"
"<span></span>"
"<span style='display:flex;align-items:center;gap:6px'>"
"<input id='chkFocus' type='checkbox' checked style='width:16px;height:16px'>"
"<label for='chkFocus' style='color:#f0c040;font-size:.9em'>"
"&#127919; Focus display (attended session)</label>"
"</span>"
"</div>"
"<button class='btn btn-euro' style='width:100%' onclick='doStart(0)'>&#127808; Euro-Lotto</button>"
"<button class='btn btn-649' style='width:100%' onclick='doStart(1)'>&#127808; 6 of 49</button>"
"</div>"
"<div style='text-align:center;margin-bottom:6px'>"
"<span id='runsErr' style='color:#ff6b6b;font-size:.9em'></span>"
"</div>"
"<div id='startBtns' style='display:none'></div>"
// margin-bottom, not a margin on what follows: several different things can sit
// under these buttons (loop badge, progress, message) and each would otherwise
// need its own spacing rule. Set here, it applies to whichever one appears.
// JS only ever writes .style.display, so the margin survives show/hide.
"<div class='btns' id='runBtns' style='display:none;margin-bottom:16px'>"
// Pause is required, not a nicety: without it the only way to stop attending is
// to abort and throw the loop away, which guarantees that tired-observer data
// gets measured rather than skipped.
"<button class='btn' id='btnPause' onclick='doPause()' "
"style='background:#a08030;color:#fff'>&#9208; Pause</button>"
"<button class='btn btn-abort' id='btnAbort' onclick='doAbort()'>&#9632; Abort</button>"
"</div>"
"<div id='progArea' style='display:none'>"
/* Loop counter and, beside it, the ranking mode this session is actually
   running. The mode decides what every published Z MEANS — a high-water mark, a
   Stouffer sum, or a raw unstudentized value — and it is chosen once in a
   dropdown that is off-screen for the rest of the session. Without it on the
   progress line, the operator reads numbers whose rule they cannot see. It is
   fed from /status (the DEVICE's mode), never from the dropdown, so a session
   started from another tab or by curl still labels itself honestly. */
"<div style='display:flex;align-items:center;justify-content:center;gap:10px;"
"flex-wrap:wrap;margin-bottom:10px'>"
"<div id='loopBadge' style='display:none;color:#f0c040;"
"font-weight:700;font-size:1.05em'></div>"
"<div id='modeBadge' style='display:none;color:#9fc79f;font-size:.86em;"
"border:1px solid rgba(159,199,159,.35);border-radius:999px;padding:2px 10px'></div>"
"</div>"
"<div id='calArea'>"
// Labelled "Baseline", NOT "Calibration". This bar tracks the baseline runs;
// the camera exposure sweep is a different phase entirely (and says so in
// #msg). Calling both "calibration" in one interface was ambiguous. Element ids
// keep their old cal* names so nothing else has to change.
"<div style='color:#f0c040;font-size:.88em;margin-bottom:4px' "
"title='Reference runs with the display off. Feeds the cross-loop drift "
"regression (drift_slope/drift_t). The ranking is unaffected either way: "
"studentize() removes any constant offset exactly. This is NOT the camera "
"exposure calibration.'><span id='calTitle'>&#128207; Baseline &#8212; drift reference</span>"
"<span id='calCheck'></span></div>"
"<div class='prog-wrap' style='height:18px'>"
"<div id='pfCal' style='background:linear-gradient(90deg,#a08030,#f0c040);"
"height:100%;border-radius:20px;width:0%;transition:width .5s'></div></div>"
"<div style='color:#f0c040;font-size:.9em;text-align:center;margin-top:4px'>"
"<span id='calCount'><span id='sCalDone'>0</span> / <span id='sCalTotal'>50</span> Runs</span></div>"
"</div>"
"<div id='scoreArea' style='margin-top:14px'>"
"<div style='color:#6ab0e8;font-size:.88em;margin-bottom:4px'>&#127919; Number scoring"
"<span id='scoreCheck'></span></div>"
"<div class='prog-wrap' style='height:18px'>"
"<div id='pfScore' style='background:linear-gradient(90deg,#206090,#6ab0e8);"
"height:100%;border-radius:20px;width:0%;transition:width .5s'></div></div>"
"<div style='color:#6ab0e8;font-size:.9em;text-align:center;margin-top:4px'>"
"<span id='sScoreDone'>0</span> / <span id='sScoreTotal'>-</span> Runs "
"(<span id='sScoreReps'>-</span>&times; per number, random order)</div>"
"</div>"
"<div id='measArea' style='display:none;margin-top:14px'>"
"<div style='color:#90ee90;font-size:.88em;margin-bottom:4px'>&#128202; Measurement"
"<span id='measCheck'></span></div>"
"<div class='prog-wrap'><div class='prog-fill' id='pf'></div></div>"
"<div class='stats'>"
"<div class='stat'><div class='sv' id='sDone'>0</div><div class='sl'>Runs</div></div>"
"<div class='stat'><div class='sv' id='sPct'>0%</div><div class='sl'>Progress</div></div>"
"<div class='stat'><div class='sv' id='sTime'>0 min</div><div class='sl'>Time</div></div>"
"<div class='stat'><div class='sv' id='sEta'>-</div><div class='sl'>ETA</div></div>"
"</div>"
"</div>"
"</div>"
"<div id='msg'></div>"
"</div>"
/* Pool confirmation modal. Fixed overlay rather than a card in the flow: the
   session is BLOCKED while this is up, so it must read as a stop, not as one
   more panel to scroll past. */
"<div id='poolOv' style='display:none;position:fixed;inset:0;z-index:50;"
"background:rgba(0,0,0,.72);align-items:center;justify-content:center;padding:16px'>"
"<div style='background:#0f3d0f;border:1px solid rgba(144,238,144,.35);"
"border-radius:14px;max-width:560px;width:100%;max-height:92vh;overflow-y:auto;"
"padding:20px;box-shadow:0 8px 40px rgba(0,0,0,.6)'>"
"<h3 style='color:#f0c040;margin-bottom:4px'>Selected Numbers</h3>"
"<div id='poolHint' style='color:#90ee90;font-size:.86em;margin-bottom:12px'></div>"
"<div id='poolMainWrap'><div style='color:#f0c040;font-size:.82em;margin-bottom:5px'>"
"Main numbers</div><div id='poolMain' style='display:flex;flex-wrap:wrap;gap:7px'></div></div>"
"<div id='poolEuroWrap' style='margin-top:14px'>"
"<div style='color:#f0c040;font-size:.82em;margin-bottom:5px'>Bonus numbers</div>"
"<div id='poolEuro' style='display:flex;flex-wrap:wrap;gap:7px'></div></div>"
"<div id='poolCount' style='margin-top:14px;font-size:.9em;color:#fff'></div>"
/* Placed inside the same overlay block below; see readyOv. */
"<div class='btns' style='margin-top:16px'>"
"<button class='btn btn-euro' id='poolOk' onclick='poolSend(\"ok\")'>OK</button>"
"<button class='btn' id='poolMore' onclick='poolSend(\"more\")' "
"style='background:#2e7d9e;color:#fff'>Select more</button>"
"<button class='btn btn-abort' id='poolCancel' onclick='poolSend(\"cancel\")'>Cancel</button>"
"</div></div></div>"
/* The observer gate. Deliberately sparse — one line and one big button. This is
   the moment the operator is meant to settle and concentrate, so the screen
   should give them nothing else to read. */
"<div id='readyOv' style='display:none;position:fixed;inset:0;z-index:60;"
"background:rgba(0,0,0,.82);align-items:center;justify-content:center;padding:16px'>"
"<div style='background:#0f3d0f;border:1px solid rgba(144,238,144,.35);"
"border-radius:14px;max-width:460px;width:100%;padding:28px;text-align:center;"
"box-shadow:0 8px 40px rgba(0,0,0,.6)'>"
"<div style='color:#90ee90;font-size:1.05em;margin-bottom:6px'>System ready</div>"
"<div style='color:#cfe8cf;font-size:.9em;margin-bottom:22px;line-height:1.5'>"
"Cameras calibrated and baseline recorded.<br>"
"Take your time. Press Start when you are settled and ready to attend."
"</div>"
/* The reminder rides ON the button rather than beside it: it is the last thing
   read before the first target appears, and the instruction it carries — attend
   to the number and nothing else — IS the experimental condition this session is
   tagged with. The second line is deliberately part of the same click target. */
"<button class='btn btn-euro' style='font-size:1.35em;padding:14px 34px;"
"line-height:1.3' onclick='readyGo()'>&#9654; Start"
"<div style='font-size:.52em;font-weight:400;opacity:.9;margin-top:7px'>"
"Focus and concentrate on the numbers only</div></button>"
"<div style='color:#a8c8a8;font-size:.78em;margin-top:14px'>"
"The first number appears one second after you press Start."
"</div>"
"</div></div>"
"<div class='card' id='focusCard' style='display:none;text-align:center'>"
"<div style='color:#f0c040;font-size:.88em;margin-bottom:8px;text-align:left'>"
"&#127919; Focus:</div>"
"<div id='focusBox'></div>"
"<div id='focusInfo' style='color:#a0c0a0;font-size:.78em;margin-top:10px'></div>"
"</div>"
"<div class='card' id='resCard' style='display:none'>"
"<h3 id='resTitle' style='color:#6ab0e8;margin-bottom:4px'></h3>"
"<div id='sigLine' style='color:#a0c0a0;font-size:.82em;margin-bottom:4px'></div>"
"<div style='color:#a0c0d0;font-size:.82em;margin-bottom:12px'>High-Z combinations"
" chosen to overlap as little as possible &mdash; spread across more numbers to cover"
" more potential draws.</div>"
"<table><thead id='resHead'></thead>"
"<tbody id='resBody'></tbody></table>"
"<div style='text-align:center;margin-top:14px'>"
"<button id='btnSave' class='btn' onclick='doSave()' style='display:none;background:#2e7d32;color:#fff;padding:10px 28px'>&#128190; Save CSV</button>"
"</div>"
"</div>"
"<div class='card' id='resCardCoverLow' style='display:none'>"
"<h3 id='resTitleCoverLow' style='color:#e8a0a0;margin-bottom:4px'></h3>"
"<div style='color:#d0b0b0;font-size:.82em;margin-bottom:12px'>Low-Z combinations"
" chosen to overlap as little as possible &mdash; the spread-out negative-deviation"
" picks.</div>"
"<table><thead id='resHeadCoverLow'></thead>"
"<tbody id='resBodyCoverLow'></tbody></table>"
"</div>"
"</div>"
"<script>"
"var timer=null,curMode=0,lastData=null,lastDisplayed=null,calShown=false;"
"var ftimer=null,lastSeq=-1,winSeen=0,winMissed=0,paused=false,pausePendUntil=0;"
// Set by the /status poll; read by pollFocus at 10 Hz. True while the rig is
// calibrating, running its baseline, or parked at the observer gate.
"var preparing=false;"
"function fmt(ms){"
"var m=Math.floor(ms/60000),h=Math.floor(m/60);m=m%60;"
"return h>0?h+':'+('0'+m).slice(-2)+' h':m+' min';}"
"function fmtEta(ms){"
"if(ms<90000)return Math.ceil(ms/1000)+' s';"
"return fmt(ms);}"
// Passes per number are derived from scoring_total, not hardcoded, so the label
// cannot drift away from sensor.c the way it did before. It reads 1 now that
// scoring is a single random sweep, and would still read true if full extra
// passes were ever added.
"function setScoreTotal(d){"
"document.getElementById('sScoreTotal').textContent=d.scoring_total||0;"
"var nn=(d.mode==='euro')?62:49;"
"document.getElementById('sScoreReps').textContent="
"d.scoring_total>0?Math.round(d.scoring_total/nn):'-';"
"}"
"function setMode(mode){"
"document.getElementById('subtitle').textContent="
"mode===0?'Eurojackpot • 5 of 50 + 2 bonus numbers':'6 of 49 Lotto';}"
"window.onload=function(){"
"fetch('/status').then(function(r){return r.json();}).then(function(d){"
"if(d.state==='running'){"
"curMode=d.mode==='euro'?0:1;setMode(curMode);"
"document.getElementById('runsRow').style.display='none';"
"document.getElementById('startBtns').style.display='none';"
"document.getElementById('runBtns').style.display='flex';"
"document.getElementById('progArea').style.display='block';"
"if(d.focus)startFocus();"
"applyPaused(d.paused);"
// A page loaded (or reloaded) while the device is already waiting must show the
// prompt too — the session is blocked until somebody answers it.
"if(d.phase==='poolconfirm'&&!poolShown&&d.pool_main)poolShow(d);"
// A page loaded or reloaded while the device is parked at the observer gate
// must show it too — the session is blocked until somebody presses Start.
"if(d.phase==='ready')readyShow();"
"updateLoopBadge(d.loop_current||1,d.loops_total||1);"
"updateModeBadge(d.rank);"
"document.getElementById('sCalTotal').textContent=d.baseline_total;"
"setScoreTotal(d);"
"if(d.scoring_total>0){"
"var sp=Math.round(d.scoring_done*100/d.scoring_total);"
"document.getElementById('pfScore').style.width=sp+'%';"
"document.getElementById('sScoreDone').textContent=d.scoring_done||0;"
"if(d.scoring_done>=d.scoring_total)"
"document.getElementById('scoreCheck').innerHTML=\" <span style='color:#90ee90;font-size:1.1em'>&#10004;</span>\";"
"if(d.phase==='measuring')document.getElementById('measArea').style.display='';"
"if(d.cover&&d.cover.length)showResults(d);"
"if(timer)clearInterval(timer);timer=setInterval(poll,1000);"
"}}else if(d.state==='done'||d.state==='aborted'){"
"curMode=d.mode==='euro'?0:1;setMode(curMode);"
"document.getElementById('msg').textContent="
"d.state==='done'?'✅ Done! ('+fmt(d.elapsed_ms)+')'"
":'⚠️ Aborted after '+d.completed+' runs — partial results:';"
"showResults(d);"
"}"
"showSlaveBadge(d);"
"}).catch(function(){});};"
// The badge names the array size and the SNR that follows from it. Nodes are
// discovered by broadcast, so "connected" alone says neither how many answered
// nor how many are still contributing after a drop.
"function showSlaveBadge(d){"
"var b=document.getElementById('slaveBadge');"
"var n=d.nodes_total||1,ok=d.nodes_ok||n;"
"if(n<2){b.style.display='none';return;}"
"b.style.display='';"
"var gain=Math.sqrt(ok).toFixed(ok===Math.round(Math.sqrt(ok))*Math.round(Math.sqrt(ok))?0:2);"
"b.innerHTML='\\uD83D\\uDD17 '+n+'-node array \\u00b7 SNR \\u00d7'+gain"
"+(ok<n?' \\u26a0 '+(n-ok)+' dropped, running on '+ok:'');"
"}"
"function updateLoopBadge(cur,total){"
"var b=document.getElementById('loopBadge');"
"if(total>1){b.style.display='';b.textContent='\\uD83D\\uDD01 Loop '+cur+' / '+total;}"
"else b.style.display='none';"
"}"
/* Shown independently of the loop badge, which hides itself on a single-loop
   session — the ranking rule matters just as much there. RAW is called out in
   its own colour: its Z is not N(0,1) and the p beside it is uncalibrated. */
"var RANKNAME={'max':'\\uD83C\\uDFAF Most extreme Z',"
"'maxraw':'\\u26a0 Most extreme Z \\u2014 RAW, not studentized',"
"'cum':'\\u03a3 Cumulative Z (Stouffer)',"
"'peak':'\\u2191 Peak Z (single run)'};"
"function updateModeBadge(rank){"
"var b=document.getElementById('modeBadge');"
"if(!rank||!RANKNAME[rank]){b.style.display='none';return;}"
"b.style.display='';"
"b.textContent=RANKNAME[rank];"
"var raw=(rank==='maxraw');"
"b.style.color=raw?'#f0a040':'#9fc79f';"
"b.style.borderColor=raw?'rgba(240,160,64,.55)':'rgba(159,199,159,.35)';"
"}"
"function doStart(mode){"
"curMode=mode;"
"var base=parseInt(document.getElementById('numBaseline').value)||" EL_STR(BASELINE_DEFAULT) ";"
"var loops=parseInt(document.getElementById('numLoops').value)||" EL_STR(LOOPS_DEFAULT) ";"
"if(loops<1)loops=1;if(loops>" EL_STR(LOOPS_MAX) ")loops=" EL_STR(LOOPS_MAX) ";"
"var runs=parseInt(document.getElementById('numRuns').value)||0;"
"if(runs<0)runs=0;if(runs>" EL_STR(NUM_RUNS) ")runs=" EL_STR(NUM_RUNS) ";"
"var rank=document.getElementById('selRank').value;"
"var foc=document.getElementById('chkFocus').checked?1:0;"
"document.getElementById('runsErr').textContent='';"
"document.getElementById('sCalTotal').textContent=base;"
"document.getElementById('pfScore').style.width='0%';"
"document.getElementById('sScoreDone').textContent='0';"
"document.getElementById('measArea').style.display='none';"
// A refused start must not leave the UI pretending a session began with the
// settings just typed in — that is how ignored parameters stay invisible.
// confirm=1 unconditionally from the UI: a human pressed this button, so there
// is by definition someone to answer the pool prompt. curl never sends it.
"fetch('/start?mode='+mode+'&baseline='+base+'&loops='+loops+'&runs='+runs+'&rank='+rank"
"+'&focus='+foc+'&confirm=1',{method:'POST'})"
".then(function(r){if(!r.ok){"
"document.getElementById('runsErr').textContent="
"'\\u26a0 a session is already running \\u2014 abort it first';"
"document.getElementById('runsRow').style.display='grid';"
"document.getElementById('runBtns').style.display='none';"
"document.getElementById('progArea').style.display='none';"
"if(timer)clearInterval(timer);stopFocus();}}).catch(function(){});"
"document.getElementById('runsRow').style.display='none';"
"document.getElementById('startBtns').style.display='none';"
"document.getElementById('runBtns').style.display='flex';"
"document.getElementById('progArea').style.display='block';"
// NOT `paused=false` first — setPauseBtn is idempotent now, so pre-assigning the
// state would make it skip the DOM update and a session started right after a
// paused one would show "Continue" while running. Clear the latch, let the
// setter see the real transition.
"pausePendUntil=0;setPauseBtn(false);"
"if(foc)startFocus();else stopFocus();"
"lastData=null;lastDisplayed=null;"
"document.getElementById('btnSave').style.display='none';"
"document.getElementById('resCard').style.display='none';"
"document.getElementById('resCardCoverLow').style.display='none';"
"document.getElementById('msg').textContent='';"
"updateLoopBadge(1,loops);"
/* Optimistic label from the dropdown so the badge is up before the first
   /status arrives; the very next poll overwrites it with what the DEVICE
   reports, which is the authority if the two ever disagree. */
"updateModeBadge({'0':'peak','1':'cum','2':'max','3':'maxraw'}[rank]);"
"setMode(mode);"
"if(timer)clearInterval(timer);"
"timer=setInterval(poll,1000);"
"}"
"function doAbort(){"
"fetch('/abort',{method:'POST'});"
"document.getElementById('msg').textContent='Aborting...';"
"}"
/* IDEMPOTENT ON PURPOSE. pollFocus() runs at 10 Hz and used to call this every
   tick, rewriting innerHTML ten times a second and repainting the button
   continuously — visible as a flicker even when nothing had changed. Bail out
   when the state already matches and the DOM is touched only on a real change. */
/* ── Pool confirmation ──────────────────────────────────────────────
   The device is parked at PHASE_POOL_CONFIRM until one of these three answers
   arrives, so the modal is rendered once (poolShown latches) and then left
   alone: re-rendering on every /status poll would wipe the operator's
   checkboxes out from under them mid-decision. */
"var poolShown=false,poolNeedM=0,poolNeedE=0,poolFullM=0,poolFullE=0;"
"function poolChecked(id){"
"var out=[],b=document.querySelectorAll('#'+id+' input:checked');"
"for(var i=0;i<b.length;i++)out.push(parseInt(b[i].value,10));"
"return out;}"
/* Button states are the spec: OK needs enough numbers left to form one draw,
   "Select more" only means anything while a slot is free. */
"function poolSync(){"
"var m=poolChecked('poolMain').length,e=poolChecked('poolEuro').length;"
"var okM=m>=poolNeedM,okE=(poolNeedE===0)||(e>=poolNeedE);"
"document.getElementById('poolOk').disabled=!(okM&&okE);"
"document.getElementById('poolMore').disabled=(m>=poolFullM)&&(e>=poolFullE);"
"var cm=comb(m,poolNeedM),ce=poolNeedE?comb(e,poolNeedE):1;"
"var t=cm*ce;"
"document.getElementById('poolCount').innerHTML=(okM&&okE)"
"?'Keeps <b>'+m+'</b>'+(poolNeedE?' + <b>'+e+'</b> bonus':'')+"
"' \\u2192 <b>'+t+'</b> combination'+(t===1?'':'s')+' to measure'"
"+(t===1?' \\u2014 the same draw every run, which is the most sensitive way to use it.':'')"
":'<span style=\"color:#ff6b6b\">Too few numbers for a single draw \\u2014 need at least '"
"+poolNeedM+(poolNeedE?' + '+poolNeedE+' bonus':'')+'.</span>';"
"}"
"function comb(n,k){if(k>n||k<0)return 0;var r=1;"
"for(var i=0;i<k;i++)r=r*(n-i)/(i+1);return Math.round(r);}"
"function poolChip(o,euro){"
"return \"<label style='display:inline-flex;align-items:center;gap:5px;\"+"
"\"background:rgba(0,0,0,.3);border-radius:8px;padding:5px 9px;cursor:pointer'>\"+"
"\"<input type='checkbox' checked value='\"+o.n+\"' onchange='poolSync()' \"+"
"\"style='width:15px;height:15px'>\"+"
"\"<b style='color:\"+(euro?'#ffdc6a':'#fff')+\"'>\"+o.n+\"</b>\"+"
"\"<span style='color:#8fae8f;font-size:.78em'>z \"+o.z.toFixed(2)+\"</span></label>\";}"
"function poolShow(d){"
"poolNeedM=d.pool_need_main;poolNeedE=d.pool_need_euro;"
"poolFullM=d.pool_main.length;poolFullE=d.pool_euro.length;"
"var h='';for(var i=0;i<d.pool_main.length;i++)h+=poolChip(d.pool_main[i],false);"
"document.getElementById('poolMain').innerHTML=h;"
"h='';for(var i=0;i<d.pool_euro.length;i++)h+=poolChip(d.pool_euro[i],true);"
"document.getElementById('poolEuro').innerHTML=h;"
"document.getElementById('poolEuroWrap').style.display=d.pool_euro.length?'block':'none';"
"document.getElementById('poolHint').textContent="
"'Scoring chose these. Uncheck any you do not want, then OK to measure them '"
"+'\\u2014 or Select more to re-score the rest and refill the free slots.';"
"document.getElementById('poolOv').style.display='flex';"
"poolShown=true;poolSync();"
"}"
"function poolHide(){"
"document.getElementById('poolOv').style.display='none';poolShown=false;}"
/* ── The observer gate ────────────────────────────────────────────────
   Hidden immediately on click: the device needs a moment to pick the answer up,
   and a lingering button invites a second press on a session that has moved on. */
"function readyShow(){document.getElementById('readyOv').style.display='flex';}"
"function readyHide(){document.getElementById('readyOv').style.display='none';}"
"function readyGo(){"
"readyHide();"
/* Blank the panel at the click, without waiting for the 1 Hz /status poll to
   notice the phase change. The device holds one second before the first target
   precisely so the press and the first onset do not overlap; leaving "Preparing
   the system" on screen for most of that second would spend the settle time on
   text instead of on the empty field the number is about to appear in. */
"preparing=false;"
"document.getElementById('focusBox').innerHTML='';"
"document.getElementById('msg').textContent='Starting number scoring\\u2026';"
"fetch('/ready',{method:'POST'}).then(function(r){"
"if(!r.ok)document.getElementById('msg').textContent='Start rejected by device.';});"
"}"
"function poolSend(act){"
"var m=poolChecked('poolMain'),e=poolChecked('poolEuro');"
"var q='/pool?act='+act+'&main='+m.join(',')+'&euro='+e.join(',');"
// Hide immediately: the device needs a moment to pick the answer up, and a
// modal that lingers invites a second click on a session that has moved on.
"poolHide();"
"if(act==='cancel'){document.getElementById('msg').textContent='Cancelled.';}"
"else{document.getElementById('msg').textContent="
"act==='more'?'Re-scoring the remaining numbers...':'Pool confirmed \\u2014 measuring...';}"
"fetch(q,{method:'POST'}).then(function(r){"
"if(!r.ok)document.getElementById('msg').textContent='Pool rejected by device.';});"
"}"
"function setPauseBtn(p){"
"p=!!p;"
"if(p===paused)return;"
"paused=p;"
"var b=document.getElementById('btnPause');"
"b.innerHTML=paused?'\\u25b6 Continue':'\\u23f8 Pause';"
"b.style.background=paused?'#4a9e4a':'#a08030';"
"}"
/* The second half of the flicker was a race, not a repaint. doPause() updates
   the button immediately (so the click feels responsive) but /pause only takes
   effect BETWEEN runs — up to ~1.4 s away. Meanwhile the 10 Hz poller kept
   reporting the device's old state and flipping the button straight back, so a
   single click produced ~10 alternations of Pause/Continue before the device
   caught up. Device state is therefore ignored while a request is outstanding,
   until it agrees with what was asked. The deadline is a self-heal: if the POST
   is lost, the UI re-syncs to the device rather than lying forever. */
"function applyPaused(p){"
"p=!!p;"
"if(Date.now()<pausePendUntil){if(p!==paused)return;pausePendUntil=0;}"
"setPauseBtn(p);"
"}"
// Pause is device-side, so this only asks; /status and /focus report the truth.
"function doPause(){"
"var want=paused?0:1;"
"pausePendUntil=Date.now()+6000;"
"fetch('/pause?on='+want,{method:'POST'});"
"setPauseBtn(want);"
"}"
/* Focus polling at 10 Hz. A 50-100 ms offset is not a problem — at a 500 ms
   window that is still 80-90% overlap, and conscious noticing is itself smeared
   over ~100-300 ms, so tighter sync would be precision the experiment cannot
   use. What DOES matter is a window missed entirely: the observer then attends
   to combination N while N+1's bits are collected, and per-combination z feeds
   the Stouffer accumulation, so the effect gets credited to an unrelated
   combination. `seq` is monotonic, so a jump of more than 1 is exactly that
   failure — counted here, which is a more honest diagnostic than a jitter
   histogram because it detects corruption rather than blur. */
"function startFocus(){"
"lastSeq=-1;winSeen=0;winMissed=0;"
"document.getElementById('focusCard').style.display='block';"
"if(ftimer)clearInterval(ftimer);"
"ftimer=setInterval(pollFocus,100);"
"}"
/* Hides the whole card, not just its contents. Clearing focusBox alone left an
   empty card on screen after Abort/Done, which reads as "a window is still
   coming" — exactly the wrong signal from a panel whose only job is to say what
   is being measured RIGHT NOW. Nothing is being measured, so nothing is shown. */
"function stopFocus(){"
"if(ftimer)clearInterval(ftimer);ftimer=null;"
"document.getElementById('focusBox').innerHTML='';"
"document.getElementById('focusCard').style.display='none';"
"}"
"function pollFocus(){"
"fetch('/focus').then(function(r){return r.json();}).then(function(f){"
"var box=document.getElementById('focusBox');"
"if(f.p){"
// Unmistakable, so "no numbers on screen" never has to be read as "maybe I
// missed one".
"box.innerHTML=\"<span style='color:#f0c040;font-size:1.6em;font-weight:700'>"
"\\u23f8 PAUSED</span>\";"
"lastSeq=f.seq;applyPaused(true);return;}"
"applyPaused(false);"
/* While the instrument is preparing itself there is no target to attend to, but
   an empty panel for two minutes reads as a crash. Say what is happening
   instead. `preparing` is set by the 1 Hz /status poll — /focus alone does not
   know which phase the session is in. */
"if(!f.on){box.innerHTML=preparing"
"?\"<span style='color:#90ee90;font-size:1.15em'>Preparing the system"
"<br><span style='color:#8fae8f;font-size:.85em'>please wait\\u2026</span></span>\""
":'';return;}"
"if(f.seq===lastSeq)return;"
"if(lastSeq>=0&&f.seq>lastSeq+1)winMissed+=f.seq-lastSeq-1;"
"lastSeq=f.seq;winSeen++;"
"var h='';"
"for(var i=0;i<f.n.length;i++)h+='<span class=\"numBig\">'+f.n[i]+'</span>';"
"for(var i=0;i<f.e.length;i++)h+='<span class=\"numBig euro\">'+f.e[i]+'</span>';"
"box.innerHTML=h;"
"}).catch(function(){});"
"}"
"function poll(){"
"fetch('/status').then(function(r){return r.json();}).then(function(d){"
"updateLoopBadge(d.loop_current||1,d.loops_total||1);"
"updateModeBadge(d.rank);"
"updateFocusInfo(d);"
// Raise the prompt as soon as the device parks on it, and take it down again
// the moment it moves on (an answer from another browser tab, or the timeout).
"if(d.phase==='poolconfirm'){if(!poolShown&&d.pool_main)poolShow(d);}"
"else if(poolShown)poolHide();"
// The observer gate: raise it while the device is parked, take it down the
// moment it moves on (another tab answered, or the session was aborted).
"if(d.state==='running'&&d.phase==='ready')readyShow();else readyHide();"
"preparing=(d.state==='running'&&(d.phase==='calibrating'||d.phase==='baseline'"
"||d.phase==='ready'));"
// Calibration is the first thing a loop does and nothing is on screen for it —
// the panel stays hidden, since nothing is being attended to while the sensor is
// tuned. Say so, or a 30 s pause at the top of every loop looks like a stall.
// Only on the transition, so it cannot wipe 'Aborting...'.
"var calNow=(d.phase==='calibrating');"
"if(calNow!==calShown){calShown=calNow;"
"document.getElementById('msg').textContent=calNow"
"?'\\uD83D\\uDD27 Calibrating cameras \\u2014 exposure sweep, '"
"+Math.round((d.cal_budget_ms||0)/1000)+' s':'';}"
"var stDone=d.state==='done'||d.state==='aborted';"
"var scorePct=d.scoring_total>0?Math.round(d.scoring_done*100/d.scoring_total):0;"
"document.getElementById('pfScore').style.width=scorePct+'%';"
"document.getElementById('sScoreDone').textContent=d.scoring_done||0;"
"setScoreTotal(d);"
"if(d.scoring_total>0&&d.scoring_done>=d.scoring_total)"
"document.getElementById('scoreCheck').innerHTML=\" <span style='color:#90ee90;font-size:1.1em'>&#10004;</span>\";"
"else document.getElementById('scoreCheck').innerHTML='';"
/* The baseline bar doubles as the calibration bar. Reusing it rather than adding
   a second one is deliberate: the two never run at once, and the operator needs
   ONE place to look to see that something is happening. Without this a ~25 s
   silent gap at the head of every loop reads as a crash.
   Progress is estimated against the LAST sweep's measured duration, not the
   budget: the budget is a cap (30 s) while a sweep actually takes ~24 s, so a
   budget-based bar would visibly stall at 80%. Falls back to the budget on the
   first loop, when there is no previous sweep to estimate from. */
"if(d.phase==='calibrating'){"
"var est=(d.cal_ms>0?d.cal_ms:(d.cal_budget_ms||" EL_STR(CAL_BUDGET_DEFAULT_MS) "));"
"var el=d.cal_elapsed_ms||0;"
"var left=Math.max(0,Math.round((est-el)/1000));"
"document.getElementById('calTitle').innerHTML='\\uD83D\\uDD27 Camera calibration \\u2014 exposure sweep';"
"document.getElementById('pfCal').style.width="
"Math.max(0,Math.min(100,Math.round(el*100/est)))+'%';"
"document.getElementById('calCount').textContent="
"left>0?left+' s left':'finishing\\u2026';"
"}else{"
"document.getElementById('calTitle').innerHTML='&#128207; Baseline &#8212; drift reference';"
"var calPct=d.baseline_total>0?Math.round(d.baseline_done*100/d.baseline_total):0;"
"document.getElementById('pfCal').style.width=calPct+'%';"
"document.getElementById('calCount').innerHTML='<span id=\"sCalDone\">'+(d.baseline_done||0)"
"+'</span> / <span id=\"sCalTotal\">'+(d.baseline_total||0)+'</span> Runs';"
"}"
"if(d.baseline_done>=d.baseline_total&&d.baseline_total>0)"
"document.getElementById('calCheck').innerHTML=\" <span style='color:#90ee90;font-size:1.1em'>&#10004;</span>\";"
"else document.getElementById('calCheck').innerHTML='';"
"if(d.phase==='measuring'||stDone){"
"document.getElementById('measArea').style.display='';"
"if(!stDone)document.getElementById('measCheck').innerHTML='';"
"var pct=d.total>0?Math.round(d.completed*100/d.total):0;"
"document.getElementById('pf').style.width=pct+'%';"
"document.getElementById('sDone').textContent=d.completed+'/'+d.total;"
"document.getElementById('sPct').textContent=pct+'%';"
"document.getElementById('sTime').textContent=fmt(d.elapsed_ms);"
"var perLoop=(d.baseline_total||0)+(d.scoring_total||0)+(d.total||0);"
"var lc=d.loop_current||1,lt=d.loops_total||1;"
"var gDone=(lc-1)*perLoop+(d.baseline_done||0)+(d.scoring_done||0)+d.completed;"
"var gTotal=lt*perLoop;"
"showSlaveBadge(d);"
"if(gDone>0&&d.elapsed_ms>0&&gTotal>gDone){"
"var msPer=d.elapsed_ms/gDone;"
"var eta=Math.round(msPer*(gTotal-gDone));"
"document.getElementById('sEta').textContent=fmtEta(eta);"
"}else document.getElementById('sEta').textContent='-';"
"}else if((d.loop_current||1)>1){"
"document.getElementById('measArea').style.display='';"
"document.getElementById('pf').style.width='100%';"
"document.getElementById('sDone').textContent=d.total+'/'+d.total;"
"document.getElementById('sPct').textContent='100%';"
"document.getElementById('sTime').textContent=fmt(d.elapsed_ms);"
"document.getElementById('measCheck').innerHTML=\" <span style='color:#90ee90;font-size:1.1em'>&#10004;</span>\";"
"}"
"if(d.state==='running'&&d.cover&&d.cover.length)showResults(d);"
"if(d.state==='done'||d.state==='aborted'){"
"clearInterval(timer);stopFocus();poolHide();"
"document.getElementById('runBtns').style.display='none';"
"document.getElementById('measCheck').innerHTML=\" <span style='color:#90ee90;font-size:1.1em'>&#10004;</span>\";"
"document.getElementById('runsRow').style.display='grid';"
"var done=d.state==='done';"
"document.getElementById('msg').textContent="
"done?'✅ Done! ('+fmt(d.elapsed_ms)+')'"
":'⚠️ Aborted after '+d.completed+' runs — partial results:';"
"showResults(d);"
"}"
"}).catch(function(){});"
"}"
/* The Phase 5 gate in one line: the measured lit window (must be within +-10%
   of 1000 ms scoring / 500 ms draw), the measured natural inter-run gap (which
   says whether the ~200 ms blanking was free or had to be paid for), and the
   count of windows the UI missed entirely (must stay 0). */
"function updateFocusInfo(d){"
/* Also hidden whenever nothing is being measured. This runs on every /status
   poll, so without the state test it re-showed the card immediately after
   stopFocus() hid it — including on a page load against an already-finished
   session, where the panel would reappear with no run behind it. */
"if(!d.focus||d.state!=='running'){"
"document.getElementById('focusCard').style.display='none';return;}"
"document.getElementById('focusCard').style.display='block';"
"var s='window '+(d.focus_win_ms||0).toFixed(0)+' ms \\u00b7 gap '"
"+(d.focus_gap_ms||0).toFixed(0)+' ms \\u00b7 windows '+winSeen"
"+' \\u00b7 missed '+winMissed+(winMissed?' \\u26a0':' \\u2713');"
"if(d.paused_ms>0)s+=' \\u00b7 paused '+fmt(d.paused_ms)+' (excluded)';"
"document.getElementById('focusInfo').textContent=s;"
"}"
"function showResults(d){"
"lastData=d;"
"var res=d.cover,isEuro=d.mode==='euro';"
"var sl=document.getElementById('sigLine');"
"if(d.best_z!==undefined&&d.comparisons>0){"
"var rk=d.rank==='cum'?'cumulative Z (Stouffer)':"
"(d.rank==='max'?'most extreme Z per combination':"
// Flagged wherever the number is shown, not only in the dropdown: a raw-scale Z
// is not N(0,1), so the corrected p beside it is uncalibrated and a reader who
// missed the mode setting would take it at face value.
"(d.rank==='maxraw'?'most extreme Z, RAW \\u2014 not studentized, p uncalibrated':'peak Z'));"
"var pc=d.p_corr<0.001?d.p_corr.toExponential(1):d.p_corr.toFixed(3);"
"var sig=d.p_corr<0.05?'significant':'consistent with chance';"
"sl.innerHTML='Ranking: '+rk+' \\u00b7 most extreme |Z| = '+d.best_z.toFixed(2)"
"+' \\u00b7 corrected p = '+pc+' over '+d.comparisons+' comparisons ('+sig+')';"
"}else sl.innerHTML='';"
"var s2='';"
"if(d.loop_sigma>0)s2+='per-run \\u03c3 = '+d.loop_sigma.toFixed(3);"
// Which condition produced these numbers. An attended session is not equivalent
// to an unattended one, so the two must never be pooled later.
"s2+=(s2?' \\u00b7 ':'')+(d.focus?'\\uD83C\\uDFAF attended (focus)':'unattended (control)');"
// Carried into the results line because the Focus card — where these normally
// live — is hidden once the session ends. `missed` is a gate, not a curiosity:
// a skipped window credits an effect to the wrong combination, so it has to
// survive the panel it was displayed in.
"if(d.focus&&winSeen>0)s2+=' \\u00b7 windows '+winSeen+' \\u00b7 missed '+winMissed"
"+(winMissed?' \\u26a0':' \\u2713');"
"if(d.paused_ms>0)s2+=' \\u00b7 paused '+fmt(d.paused_ms)+' (excluded from elapsed)';"
// The WORST pair, not an average: the sqrt(n) gain fails if ANY pair
// correlates, so five clean pairs must not dilute one bad one.
"if(d.pair_n>1){"
"var rz=Math.abs(d.pair_r)*Math.sqrt(d.pair_n);"
"s2+=(s2?' \\u00b7 ':'')+'worst pair r = '+d.pair_r.toFixed(3)"
"+' (n'+d.pair_i+'\\u2013n'+d.pair_j+' of '+d.pair_count+' pairs)'"
"+(rz>" EL_STR(PAIR_FLAG_T) "?' \\u26a0 correlated':' ok');}"
"if(s2)sl.innerHTML+=(sl.innerHTML?'<br>':'')+s2;"
// Per-node row: source, own sigma, camera rate, stalls and missed replies. The
// combined z averages exactly these differences away, so a node that quietly
// degraded is invisible anywhere else.
"if(d.nodes&&d.nodes.length){"
"var s3='<table style=\"width:100%;font-size:.82em;margin-top:6px\">'"
"+'<tr style=\"color:#90ee90\"><th align=left>node</th>'"
"+'<th align=left>\\u03c3</th><th align=left>Mbit/s</th><th align=left>exp</th>'"
"+'<th align=left>stalls</th>'"
"+'<th align=left>lost</th></tr>';"
"for(var i=0;i<d.nodes.length;i++){var N=d.nodes[i];"
// The master reports ip:"self" (it has no idea what address the client used to
// reach it), so take the address this page was served from -- which IS the
// master's, by construction. Keeps the list symmetric: every row names a host.
"var ipTxt=(N.ip&&N.ip!=='self')?N.ip:location.hostname;"
"var nm=(i===0?'master':'slave'+i)+(ipTxt?' '+ipTxt:'');"
// A camera fault is named, not merely reflected in a shrunken node count: the
// operator has to know WHICH node died and that it was rebooted.
"var st=N.cam_fault?' \\u26a0 CAMERA FAULT \\u2013 rebooted'+(N.reboots>1?' x'+N.reboots:'')"
":(N.ok?'':' \\u26a0 dropped');"
// Each node calibrates its own camera and they WILL differ — different physical
// sensors, which is why one measured cleaner than the other at identical
// settings. So the setting is shown per node, with the fold state that goes with
// it; '!' marks a node that certified nothing and kept its previous setting.
"var ex='\\u2013';"
"if(N.cam_exp>0)ex=N.cam_exp+(N.cam_fold?'\\u2295':'')+(N.cam_cal?'':'!');"
"s3+='<tr style=\"opacity:'+(N.ok?1:.55)+'\"><td>'+nm+st+'</td>'"
"+'<td>'+(N.sigma>0?N.sigma.toFixed(3):'\\u2013')+'</td>'"
"+'<td>'+(N.cam_mbit>0?N.cam_mbit.toFixed(2):'\\u2013')+'</td>'"
"+'<td title=\"exposure chosen by this loop\\u2019s calibration\">'+ex+'</td>'"
"+'<td>'+(N.cam_stalls>0?'\\u26a0 '+N.cam_stalls:'0')+'</td>'"
"+'<td>'+(N.lost>0?'\\u26a0 '+N.lost:'0')+'</td></tr>';}"
"s3+='</table>';"
// The fault string is the primary channel: there is no substitute source, so a
// camera failure ends that node's participation and the operator must see why.
"if(d.fault)s3='<div style=\"color:#ff9c6e;font-weight:600;margin:4px 0\">"
"\\u26a0 '+d.fault+'</div>'+s3;"
"sl.innerHTML+=(sl.innerHTML?'<br>':'')+s3;}"
// Transport health (PLAN_NETWORK Phase C). UDP can drop where the UART could
// not, so the gate is a counted "zero lost triggers", not an impression that it
// felt reliable. Stale replies are answers that arrived after we stopped
// waiting — dropped by sequence number, never folded into a z.
"if(d.nodes_total>1){"
"var nl=d.net_lost||0;"
"var s5='link: UDP \\u00b7 lost triggers '+nl+(nl?' \\u26a0':' \\u2713')"
"+' \\u00b7 resends '+(d.net_retries||0)+' \\u00b7 stale replies '+(d.net_stale||0);"
"sl.innerHTML+='<br>'+s5;}"
// Cross-loop drift. studentize() removes each loop's own offset exactly, so a
// constant bias is harmless — a trend across loops is not, and only a long
// session gives it room to show. |t| > 3 means the slope is real, not noise.
"if(d.loops_done>1){"
"var sgn=d.drift_slope>=0?'+':'';"
// The slope needs >= 3 loops before it has a standard error at all
// 6 loops, not 3: at 3 the regression has one degree of freedom and |t|>3 fires
// on noise (measured: t=+10.30 at loop 3, -0.20 by loop 10). See DRIFT_MIN_LOOPS.
"var d4=d.loops_done<6?'':(' \\u00b7 drift '+sgn+d.drift_slope.toFixed(4)+' z/loop (t = '"
"+d.drift_t.toFixed(1)+(Math.abs(d.drift_t)>" EL_STR(DRIFT_FLAG_T) "?' \\u26a0 drifting)':' ok)'));"
"var s4='offset '+d.off_first.toFixed(3)+' \\u2192 '+d.off_last.toFixed(3)+' z/run'+d4"
"+' \\u00b7 \\u03c3 '+d.sigma_lo.toFixed(3)+'\\u2013'+d.sigma_hi.toFixed(3)"
"+' over '+d.loops_done+' loops'"
"+\" \\u00b7 <a href='/loops' target='_blank' style='color:#90ee90'>table</a>\";"
"sl.innerHTML+='<br>'+s4;}"
"document.getElementById('resCard').style.display='block';"
"if(!res||res.length===0){"
"document.getElementById('resTitle').innerHTML='\\uD83E\\uDDE9 Coverage';"
"document.getElementById('resHead').innerHTML='';"
"document.getElementById('resBody').innerHTML="
"'<tr><td style=\"color:#d0b0b0;padding:10px\">Coverage needs a locked-pool ranking mode "
"(<b>Most extreme Z</b> or <b>Cumulative Z</b>).</td></tr>';"
"document.getElementById('btnSave').style.display='none';"
"document.getElementById('resCardCoverLow').style.display='none';"
"return;}"
"lastDisplayed=res;"
"document.getElementById('resTitle').innerHTML="
"'\\uD83E\\uDDE9 Coverage-'+res.length+(isEuro?' Eurojackpot':' 6-of-49')+' (highest Z, diversified)';"
"renderRunTable('resHead','resBody',res,isEuro);"
"if(d.freq_z2>0&&d.freq_nums){"
"var tb=document.getElementById('resBody');"
"var fn='',fe='',nc=isEuro?5:6;"
"for(var j=0;j<nc;j++)if(d.freq_nums[j])fn+='<span class=\"num\">'+d.freq_nums[j]+'</span>';"
"if(isEuro&&d.freq_euro)for(var j=0;j<2;j++)if(d.freq_euro[j])fe+='<span class=\"num euro\">'+d.freq_euro[j]+'</span>';"
"var sep='border-top:2px solid rgba(240,192,64,.4)';"
"tb.innerHTML+='<tr style=\"background:rgba(240,192,64,.08)\">'"
"+'<td colspan=\"4\" style=\"color:#f0c040;font-weight:700;'+sep+';padding-top:10px\">'"
"+'&#128197; Most frequent ('+d.freq_z2+'&times; Z&gt;2):</td>'"
"+'<td style=\"'+sep+'\">'+fn+'</td>'"
"+(isEuro?'<td style=\"'+sep+'\">'+fe+'</td>':'')+'</tr>';"
"}"
"document.getElementById('btnSave').style.display='';"
"showCoverLow(d);"
"}"
"function renderRunTable(headId,bodyId,res,isEuro){"
"document.getElementById(headId).innerHTML="
"'<tr><th>#</th><th>Run</th><th>Z-Score</th><th>p-Value</th><th>Numbers</th>'"
"+(isEuro?'<th>Bonus</th>':'')+'</tr>';"
"var tb=document.getElementById(bodyId);tb.innerHTML='';"
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
"}"
"function showCoverLow(d){"
"var res=d.cover_low,isEuro=d.mode==='euro';"
"if(!res||res.length===0){document.getElementById('resCardCoverLow').style.display='none';return;}"
"document.getElementById('resTitleCoverLow').innerHTML="
"'\\uD83E\\uDDE9 Coverage-'+res.length+(isEuro?' Eurojackpot':' 6-of-49')+' (lowest, diversified)';"
"renderRunTable('resHeadCoverLow','resBodyCoverLow',res,isEuro);"
"document.getElementById('resCardCoverLow').style.display='block';"
"}"
"function doSave(){"
"if(!lastData||!lastDisplayed)return;"
"var d=lastData,isEuro=d.mode==='euro',nc=isEuro?5:6;"
"var hdr=isEuro?'run,z_score,p_value,n1,n2,n3,n4,n5,e1,e2':'run,z_score,p_value,n1,n2,n3,n4,n5,n6';"
"function rows(arr){var o=[];for(var i=0;i<arr.length;i++){"
"var r=arr[i],cols=[r.run,r.z.toFixed(6),r.p];"
"for(var j=0;j<nc;j++)cols.push(r.nums[j]);"
"if(isEuro){cols.push(r.euro[0]);cols.push(r.euro[1]);}"
"o.push(cols.join(','));}return o;}"
// The condition travels with the data: attended and unattended sessions are
// different experiments and must not be pooled after the fact.
"var lines=['# mode='+d.mode+',date='+new Date().toISOString().slice(0,10)"
"+',focus='+(d.focus?'on':'off')+',paused_ms='+(d.paused_ms||0),"
"'# coverage-'+(lastDisplayed?lastDisplayed.length:0)+' (highest z, diversified)',hdr];"
"lines=lines.concat(rows(lastDisplayed));"
"if(d.cover_low&&d.cover_low.length){"
"lines.push('# coverage-'+d.cover_low.length+' (lowest z, diversified)');"
"lines.push(hdr);"
"lines=lines.concat(rows(d.cover_low));}"
"var url=URL.createObjectURL(new Blob([lines.join('\\n')],{type:'text/csv'}));"
"var a=document.createElement('a');"
"a.href=url;"
"a.download='elotto_'+d.mode+'_'+new Date().toISOString().slice(0,10)+'.csv';"
"document.body.appendChild(a);a.click();"
"setTimeout(function(){URL.revokeObjectURL(url);document.body.removeChild(a);},100);"
"}"
"</script></body></html>";

/* Serialize one RunResult as a JSON object; returns chars written. */
static int emit_run(char *buf, int cap, const RunResult *r, bool euro)
{
    if (euro)
        return snprintf(buf, cap,
            "{\"run\":%d,\"z\":%.4f,\"p\":\"%s\","
            "\"nums\":[%d,%d,%d,%d,%d],\"euro\":[%d,%d]}",
            r->index, r->z_score, r->p_value,
            r->nums[0], r->nums[1], r->nums[2], r->nums[3], r->nums[4],
            r->euro[0], r->euro[1]);
    return snprintf(buf, cap,
        "{\"run\":%d,\"z\":%.4f,\"p\":\"%s\","
        "\"nums\":[%d,%d,%d,%d,%d,%d],\"euro\":[]}",
        r->index, r->z_score, r->p_value,
        r->nums[0], r->nums[1], r->nums[2],
        r->nums[3], r->nums[4], r->nums[5]);
}

/* ── /status JSON ─────────────────────────────────────────────────── */
static esp_err_t status_handler(httpd_req_t *req)
{
    static char buf[6144];
    int  pos = 0;
    const char *state_str =
        g_status.state == ELOTTO_RUNNING ? "running" :
        g_status.state == ELOTTO_DONE    ? "done"    :
        g_status.state == ELOTTO_ABORTED ? "aborted" : "idle";
    const char *mode_str =
        g_status.mode == MODE_EUROJACKPOT ? "euro" : "649";

    const char *phase_str =
        g_status.phase == PHASE_SCORING      ? "scoring"     :
        g_status.phase == PHASE_BASELINE     ? "baseline"    :
        g_status.phase == PHASE_CALIBRATE    ? "calibrating" :
        g_status.phase == PHASE_POOL_CONFIRM ? "poolconfirm" :
        g_status.phase == PHASE_READY        ? "ready"       :
                                               "measuring";
    const char *rank_str =
        g_status.rank_mode == RANK_CUMULATIVE  ? "cum"    :
        g_status.rank_mode == RANK_EXTREME     ? "max"    :
        g_status.rank_mode == RANK_EXTREME_RAW ? "maxraw" : "peak";
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "{\"state\":\"%s\",\"mode\":\"%s\",\"phase\":\"%s\","
        "\"slave\":%s,\"rank\":\"%s\","
        "\"src\":\"camera\",\"src_stalled\":%s,\"fault\":\"%s\","
        "\"best_z\":%.4f,\"p_corr\":%.6g,\"comparisons\":%d,"
        "\"loop_sigma\":%.4f,\"pair_r\":%.4f,\"pair_n\":%d,"
        "\"pair_i\":%d,\"pair_j\":%d,\"pair_count\":%d,"
        "\"nodes_total\":%d,\"nodes_ok\":%d,"
        "\"net_retries\":%lu,\"net_lost\":%lu,\"net_stale\":%lu,"
        "\"focus\":%s,\"paused\":%s,\"paused_ms\":%lld,"
        "\"focus_win_ms\":%.1f,\"focus_gap_ms\":%.1f,"
        "\"cal_budget_ms\":%d,\"cal_ms\":%d,\"cal_elapsed_ms\":%d,"
        "\"cal_interval_ms\":%d,\"cal_did_sweep\":%d,"
        "\"loops_done\":%d,\"drift_slope\":%.5f,\"drift_t\":%.2f,"
        "\"off_first\":%.4f,\"off_last\":%.4f,"
        "\"sigma_lo\":%.4f,\"sigma_hi\":%.4f,"
        "\"loop_current\":%d,\"loops_total\":%d,"
        "\"scoring_done\":%d,\"scoring_total\":%d,"
        "\"baseline_done\":%d,\"baseline_total\":%d,\"baseline_mean\":%.4f,"
        "\"completed\":%d,\"total\":%d,\"elapsed_ms\":%lld,"
        "\"pool_confirm\":%d,\"pool_auto\":%d,"
        "\"pool_need_main\":%d,\"pool_need_euro\":%d,",
        state_str, mode_str, phase_str,
        g_status.slave_connected ? "true" : "false", rank_str,
        g_status.noise_stalled ? "true" : "false", g_status.fault,
        g_status.best_z, g_status.p_corrected, g_status.comparisons,
        g_status.loop_sigma, g_status.pair_r_max, g_status.pair_n,
        g_status.pair_r_i, g_status.pair_r_j, g_status.pair_count,
        g_status.node_count, g_status.node_ok,
        (unsigned long)g_status.net_retries, (unsigned long)g_status.net_lost,
        (unsigned long)g_status.net_stale,
        g_status.focus_mode ? "true" : "false",
        g_status.paused ? "true" : "false", (long long)g_status.paused_ms,
        g_status.focus_win_ms, g_status.focus_gap_ms,
        g_status.cal_budget_ms, g_status.cal_ms,
        /* Live sweep progress, 0 when none is in flight. cal_ms is only written
         * when a sweep ENDS, so it cannot drive a bar while one runs. */
        g_status.cal_start_us
            ? (int)((esp_timer_get_time() - g_status.cal_start_us) / 1000) : 0,
        g_status.cal_interval_ms, g_status.cal_did_sweep ? 1 : 0,
        g_status.loops_done, g_status.drift_slope, g_status.drift_t,
        g_status.off_first, g_status.off_last,
        g_status.sigma_lo, g_status.sigma_hi,
        g_status.loop_current, g_status.loops_total,
        g_status.scoring_done, g_status.scoring_total,
        g_status.baseline_done, g_status.baseline_total, g_status.baseline_mean,
        g_status.runs_completed, g_status.runs_total,
        (long long)g_status.elapsed_ms,
        g_status.pool_confirm, g_status.pool_auto,
        g_status.pool_need_main, g_status.pool_need_euro);

    /* The proposed pool, and only while it is actually being asked about: it is
     * ~150 bytes and /status is polled once a second for the whole session, so
     * there is no reason to carry it through the other 99 % of the run.
     *
     * RUNNING is part of the test, not decoration: `phase` keeps its last value
     * after a session ends, so a cancelled confirmation leaves the device
     * reading poolconfirm/aborted indefinitely. Both UI call sites gate on
     * `pool_main` being present, so withholding it here is what stops the modal
     * being raised over a session that is already gone. */
    if (g_status.phase == PHASE_POOL_CONFIRM &&
        g_status.state == ELOTTO_RUNNING) {
        pos += snprintf(buf+pos, sizeof(buf)-pos, "\"pool_main\":[");
        for (int i = 0; i < g_status.pool_n_main; i++)
            pos += snprintf(buf+pos, sizeof(buf)-pos, "%s{\"n\":%d,\"z\":%.2f}",
                            i ? "," : "", g_status.pool_main[i],
                            (double)g_status.pool_main_z[i]);
        pos += snprintf(buf+pos, sizeof(buf)-pos, "],\"pool_euro\":[");
        for (int i = 0; i < g_status.pool_n_euro; i++)
            pos += snprintf(buf+pos, sizeof(buf)-pos, "%s{\"n\":%d,\"z\":%.2f}",
                            i ? "," : "", g_status.pool_euro[i],
                            (double)g_status.pool_euro_z[i]);
        pos += snprintf(buf+pos, sizeof(buf)-pos, "],");
    }

    /* Firmware identity: version, build time, elf sha256, running slot, OTA
     * state. Without it an update that answered "ok" cannot be distinguished
     * from one that silently rolled back. */
    pos += elotto_ota_status_json(buf + pos, sizeof(buf) - pos);
    pos += snprintf(buf + pos, sizeof(buf) - pos, ",");

    /* Per-node health (PLAN_NETWORK Phase D). A node that quietly degraded —
     * lost its camera, started missing replies, or drifted off σ = 1 — has to be
     * visible individually; the combined z averages exactly that away.
     * Index 0 is the master. There is no per-node "src" any more: one source
     * exists, so a node either produced camera bits or it faulted and says so.
     */
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\"nodes\":[");
    for (int i = 0; i < g_status.node_count && i < MAX_NODES; i++) {
        const NodeStatus *N = &g_status.nodes[i];
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"id\":%d,\"ip\":\"%s\",\"ok\":%s,\"sigma\":%.4f,"
            "\"lost\":%lu,\"cam_mbit\":%.3f,\"cam_stalls\":%lu,"
            "\"cam_fault\":%d,\"reboots\":%lu,"
            "\"cam_exp\":%lu,\"cam_gain\":%d,\"cam_fold\":%d,\"cam_cal\":%d,"
            "\"cam_bias\":%.6f,\"cam_cal_mbit\":%.3f}",
            i ? "," : "", i, i ? N->ip : "self", N->ok ? "true" : "false",
            N->sigma,
            (unsigned long)N->lost, N->cam_mbit, (unsigned long)N->cam_stalls,
            (int)N->cam_fault, (unsigned long)N->reboots,
            (unsigned long)N->cam_exp, (int)N->cam_gain, (int)N->cam_fold,
            (int)N->cam_cal_ok, N->cam_bias, N->cam_cal_mbit);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "],");

    /* Every pair, not just the worst. The array is wired as PLAN_NETWORK's
     * Risk 1 control (master on isolated power, slaves on one PoE rail), so
     * WHICH pairs correlate is the question a maximum cannot answer. */
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\"pairs\":[");
    bool first_pair = true;
    for (int i = 0; i < g_status.node_count; i++)
        for (int j = i + 1; j < g_status.node_count; j++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s{\"i\":%d,\"j\":%d,\"r\":%.4f}",
                first_pair ? "" : ",", i, j, g_status.pair_r[i][j]);
            first_pair = false;
        }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "],");

    // Cumulative top-N is published continuously, so intermediate results
    // (after each loop) are shown too — not only when the whole job is done.
    bool euro = (g_status.mode == MODE_EUROJACKPOT);

    // Only the diversified coverage sets are shown; raw top/bottom rankings
    // (g_status.top[]/low[]) are still computed internally for the significance
    // line but no longer serialized.
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "\"freq_z2\":%d,\"freq_nums\":[%d,%d,%d,%d,%d,%d],\"freq_euro\":[%d,%d],\"cover\":[",
        g_status.freq_z2_count,
        g_status.freq_nums[0], g_status.freq_nums[1], g_status.freq_nums[2],
        g_status.freq_nums[3], g_status.freq_nums[4], g_status.freq_nums[5],
        g_status.freq_euro[0], g_status.freq_euro[1]);

    int cshow = g_status.cover_count < COVER_SHOW ? g_status.cover_count : COVER_SHOW;
    if (cshow < 0) cshow = 0;
    for (int i = 0; i < cshow; i++) {
        if (i) pos += snprintf(buf+pos, sizeof(buf)-pos, ",");
        pos += emit_run(buf+pos, sizeof(buf)-pos, &g_status.cover[i], euro);
    }
    pos += snprintf(buf+pos, sizeof(buf)-pos, "],\"cover_low\":[");

    int clshow = g_status.cover_low_count < COVER_SHOW ? g_status.cover_low_count : COVER_SHOW;
    if (clshow < 0) clshow = 0;
    for (int i = 0; i < clshow; i++) {
        if (i) pos += snprintf(buf+pos, sizeof(buf)-pos, ",");
        pos += emit_run(buf+pos, sizeof(buf)-pos, &g_status.cover_low[i], euro);
    }
    pos += snprintf(buf+pos, sizeof(buf)-pos, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ── /loops GET – per-loop health table (PLAN_4NODE Phase 3) ──────────
 * The long-run drift record: one row per completed loop with the raw
 * (pre-studentize) per-run offsets and σ per node, plus camera health at that
 * moment. Chunked — the table outgrows any sane single buffer.
 * `raw_m` = base + mean_m is the offset the master's source actually produced;
 * that series is what drift_slope/drift_t are regressed on. */
static esp_err_t loops_handler(httpd_req_t *req)
{
    char buf[288];
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    int n = g_status.loop_hist ? g_status.loop_hist_n : 0;
    if (n > LOOP_HIST) n = LOOP_HIST;
    int len = snprintf(buf, sizeof(buf),
        "{\"loops_done\":%d,\"stored\":%d,\"cap\":%d,"
        "\"drift_slope\":%.5f,\"drift_t\":%.2f,"
        "\"sigma_lo\":%.4f,\"sigma_hi\":%.4f,\"loops\":[",
        g_status.loops_done, n, LOOP_HIST,
        g_status.drift_slope, g_status.drift_t,
        g_status.sigma_lo, g_status.sigma_hi);
    httpd_resp_send_chunk(req, buf, len);

    for (int i = 0; i < n; i++) {
        const LoopStat *L = &g_status.loop_hist[i];
        int nn = L->nodes ? L->nodes : 1;
        if (nn > MAX_NODES) nn = MAX_NODES;
        len = snprintf(buf, sizeof(buf),
            "%s{\"loop\":%d,\"t_s\":%lu,\"base\":%.4f,\"raw_m\":%.4f,"
            "\"mean\":%.4f,\"sigma\":%.4f,\"cal_ms\":%d,"
            "\"win_ms\":%.1f,\"gap_ms\":%.1f,\"nodes\":%d,\"n\":[",
            i ? "," : "", i + 1, (unsigned long)L->t_s,
            L->base, L->base + L->mean_n[0], L->mean, L->sigma, (int)L->cal_ms,
            L->win_ms, L->gap_ms, nn);
        httpd_resp_send_chunk(req, buf, len);
        for (int k = 0; k < nn; k++) {
            // cam_exp/gain/fold are the operating point this loop was MEASURED
            // AT, not a diagnostic: per-loop re-tuning is only defensible
            // because the setting travels with the loop it produced (§1.5.2).
            len = snprintf(buf, sizeof(buf),
                "%s{\"mean\":%.4f,\"sigma\":%.4f,\"cam_mbit\":%.3f,\"cam_stalls\":%lu,"
                "\"cam_exp\":%lu,\"cam_gain\":%d,\"cam_fold\":%d,\"cam_cal\":%d,"
                "\"cam_bias\":%.6f}",
                k ? "," : "", L->mean_n[k], L->sig_n[k], L->cam_mbit[k],
                (unsigned long)L->cam_stalls[k], (unsigned long)L->cam_exp[k],
                (int)L->cam_gain[k], (int)L->cam_fold[k], (int)L->cam_cal_ok[k],
                L->cam_bias[k]);
            httpd_resp_send_chunk(req, buf, len);
        }
        httpd_resp_send_chunk(req, "]}", 2);
    }
    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── /calibrate GET – the master's last camera sweep (PLAN.md Task 1) ──
 * The WHOLE per-candidate table, not just the winner. The Task 1 gate is
 * "a full sweep produces a monotonic, explainable bias-vs-exposure curve rather
 * than noise — if bias does not respond to exposure, the premise is wrong and
 * the task stops there", and one chosen point cannot answer that. Each row
 * carries the gate bitmask it failed, so a sweep that certified nothing says
 * which property was missing instead of only "no".
 *
 * Read-only: the sweep runs inside a session, at the start of every loop, so
 * this reports the real code path rather than a separate manual one that could
 * quietly diverge from it. `POST /start?runs=1&loops=1&baseline=1` gives a
 * curve in about a minute.
 *
 * Step 0 is always the setting that was in force when the sweep began. Against
 * the same exposure appearing later in the ladder it is the camera_stats_reset()
 * proof the gate asks for — two windows at the same setting must agree within
 * sampling error — and from loop 2 on it measures drift at a fixed operating
 * point, which is what tells a genuinely moving optimum from a noisy sweep. */
static esp_err_t calibrate_handler(httpd_req_t *req)
{
    /* Body lives in the camera component now: the slaves serve the same path,
     * and a per-node optical fault is diagnosed by comparing ladders, which is
     * only meaningful if every node emits the same shape. */
    return camera_cal_send_json(req, elotto_last_calibration());
}

/* ── /focus GET – the current target (PLAN_4NODE Phase 5) ─────────────
 * Deliberately NOT part of /status. That response is ~2.5 KB and polled at
 * 1 Hz: far too fat and far too slow to track a 500 ms window. This one is
 * ~60 bytes and polled at 10 Hz (~600 B/s, five samples per window), which is
 * finer than the 100–300 ms smear of conscious noticing itself — tight sync
 * would be precision the experiment cannot use.
 *
 * `seq` is what tells the UI a NEW window started, including when two
 * consecutive draws happen to look similar. A gap in it means a window was
 * missed entirely, which is not blur but mislabeling — the observer attends to
 * combination N while N+1's bits are collected — so the UI counts those.
 *
 * `on` is 0 during the inter-run gap, when no bits are being collected. */
static esp_err_t focus_handler(httpd_req_t *req)
{
    /* Read the state twice around the copy: the measurement task bumps `seq`
     * last, so an unchanged seq proves the numbers belong to it. Cheaper than
     * putting a lock on the run loop for a diagnostic read. */
    FocusState f;
    for (int try = 0; try < 3; try++) {
        uint32_t s0 = g_status.focus.seq;
        f = g_status.focus;
        if (g_status.focus.seq == s0) break;
    }

    char buf[160];
    int  pos = snprintf(buf, sizeof(buf),
        "{\"seq\":%lu,\"on\":%d,\"p\":%d,\"k\":%d,\"n\":[",
        (unsigned long)f.seq, f.active ? 1 : 0, g_status.paused ? 1 : 0, f.kind);
    for (int i = 0; i < f.n && i < 6; i++)
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s%d", i ? "," : "", f.nums[i]);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"e\":[");
    for (int i = 0; i < f.ne && i < 2; i++)
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s%d", i ? "," : "", f.euro[i]);
    snprintf(buf + pos, sizeof(buf) - pos, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ── /pause POST ?on=1|0 ──────────────────────────────────────────────
 * Not abort: the state stays `running`, nothing is published, and the
 * permutation index and Σz accumulation continue where they left off. The flag
 * is only *read* between runs (pause_gate() in sensor.c), so the run in flight
 * always finishes and is kept — bits sampled while nobody was watching must
 * never end up inside a run labelled as attended.
 *
 * Device-side, like the loop itself: closing the browser does not resume it. */
static esp_err_t pause_handler(httpd_req_t *req)
{
    bool on = true;
    char qry[32] = "", val[8] = "";
    if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) == ESP_OK &&
        httpd_query_key_value(qry, "on", val, sizeof(val)) == ESP_OK)
        on = (val[0] == '1');
    g_status.paused = on;
    httpd_resp_sendstr(req, on ? "paused" : "running");
    return ESP_OK;
}

/* ── /start POST ──────────────────────────────────────────────────── */
/* Refuses with 409 while a session runs, rather than answering "ok" and doing
 * nothing. The silent version was a trap: the caller got a success reply and a
 * session still carrying the PREVIOUS run's parameters, so a /start whose
 * loops= or runs= were quietly ignored looked identical to one that worked.
 * Same contract as /update, which has refused mid-measurement since Phase B. */
static esp_err_t start_handler(httpd_req_t *req)
{
    if (g_status.state == ELOTTO_RUNNING) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "session already running -- abort it first");
        return ESP_OK;
    }
    {
        // read mode from query string (?mode=0 or ?mode=1). Sized for the full
        // set the UI sends — a truncated query silently drops trailing keys.
        char qry[128] = "";
        g_status.mode           = MODE_EUROJACKPOT;
        g_status.runs_total     = 0;   // computed in elotto_task from combinatorics
        // One definition, shared with the form field (sensor.h), so a
        // curl-started session and a browser-started one are the same
        // experiment. ?baseline= still overrides either way.
        g_status.baseline_total = BASELINE_DEFAULT;
        g_status.loops_total    = 1;
        g_status.runs_limit     = 0;   // 0 = measure all combinations
        // EXTREME is the default since 2026-07-30 (user decision): a combination
        // keeps the most extreme z it has ever produced, and a milder later loop
        // does not erase it. See ElottoRank in sensor.h for what each mode means
        // and why the significance line, not the raw Z, is the thing to read.
        g_status.rank_mode      = RANK_EXTREME;
        // No ?src= any more: the camera is the only source this firmware has
        // (sensor.h). A session that cannot run on photons does not run.
        /* Per-loop camera calibration (PLAN.md Task 1). **10 s since 2026-07-30
         * (user decision)**, down from 30 s.
         *
         * The budget is a CAP that is divided evenly over the 9 candidates, so
         * this is not a 3× saving on a fixed measurement — it is 3× less DATA
         * per rung: ~1.1 s per candidate instead of ~3.3 s. Each rung's bias and
         * σ are therefore estimated from about a third of the bits, and the gate
         * decisions get correspondingly noisier (the per-candidate bias SE was
         * ~1.7e-4 at the old budget). Rungs near a gate boundary will flip
         * between sweeps more often than they did — which is exactly the failure
         * §1.17's σ-margin rule was added to contain.
         * ?cal=0 turns it off — the matched control a calibrated session has to
         * be compared against, and the only way to measure what the sweep costs.
         * ?cal=<ms> restores any other budget without a reflash. */
        g_status.cal_budget_ms  = CAL_BUDGET_DEFAULT_MS;
        // Minimum wall time between sweeps. 5 min sits well under a full ~10 min
        // loop, so a normal session still calibrates every loop exactly as
        // §1.5.1 measured; it only bites when loops are SHORT (a Runs cap, or a
        // 6-of-49 pool), where a ~24 s sweep per loop would otherwise dominate
        // the session and re-tune a camera that has not had time to drift.
        g_status.cal_interval_ms = CAL_INTERVAL_DEFAULT_MS;
        // Absent ?focus= means UNATTENDED. A session started by curl or a
        // script has no observer by definition, so the control condition is
        // what a missing flag has to mean; the UI always sends it explicitly.
        // This flag is the session's record of which condition produced its
        // numbers — attended and unattended runs must never be pooled later.
        g_status.focus_mode     = false;
        g_status.pool_confirm   = 0;
        g_status.pool_auto      = 0;
        if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) == ESP_OK) {
            char val[16] = "";
            if (httpd_query_key_value(qry, "mode", val, sizeof(val)) == ESP_OK)
                g_status.mode = (val[0] == '1') ? MODE_LOTTO_649 : MODE_EUROJACKPOT;
            if (httpd_query_key_value(qry, "baseline", val, sizeof(val)) == ESP_OK) {
                int b = atoi(val);
                if (b > 0 && b <= BASELINE_MAX) g_status.baseline_total = b;
            }
            if (httpd_query_key_value(qry, "loops", val, sizeof(val)) == ESP_OK) {
                int l = atoi(val);
                if (l > 0 && l <= LOOPS_MAX) g_status.loops_total = l;
            }
            if (httpd_query_key_value(qry, "runs", val, sizeof(val)) == ESP_OK) {
                int r = atoi(val);
                if (r > 0 && r <= NUM_RUNS) g_status.runs_limit = r;
            }
            // ?rank= 0 peak | 1 cumulative (Stouffer) | 2 extreme (default).
            // The 0/1 meanings are unchanged so older scripts and the ?cal=0
            // control still select what they always did; only the ABSENT default
            // moved, and an unrecognised value keeps the default rather than
            // silently falling into a mode nobody asked for.
            if (httpd_query_key_value(qry, "rank", val, sizeof(val)) == ESP_OK) {
                if      (val[0] == '0') g_status.rank_mode = RANK_PEAK;
                else if (val[0] == '1') g_status.rank_mode = RANK_CUMULATIVE;
                else if (val[0] == '2') g_status.rank_mode = RANK_EXTREME;
                else if (val[0] == '3') g_status.rank_mode = RANK_EXTREME_RAW;
            }
            // ?focus=1 -> attended session: the Focus panel is live and the
            // session is tagged. Run lengths do NOT depend on this — a matched
            // no-focus control must be identical in every other respect.
            if (httpd_query_key_value(qry, "focus", val, sizeof(val)) == ESP_OK)
                g_status.focus_mode = (val[0] == '1');
            // ?cal=<ms> -> sweep budget per loop; 0 = do not calibrate.
            if (httpd_query_key_value(qry, "cal", val, sizeof(val)) == ESP_OK) {
                int c = atoi(val);
                if (c >= 0 && c <= CAL_BUDGET_MAX_MS) g_status.cal_budget_ms = c;
            }
            // ?calint=<ms> -> minimum time between sweeps; 0 = sweep at the top
            // of every loop (the pre-2026-07-29 behaviour, and what a comparison
            // against §1.14/§1.15 has to be run with).
            if (httpd_query_key_value(qry, "calint", val, sizeof(val)) == ESP_OK) {
                int c = atoi(val);
                if (c >= 0 && c <= CAL_INTERVAL_MAX_MS) g_status.cal_interval_ms = c;
            }
            // ?confirm=1 -> stop after scoring and let the operator edit the
            // pool. Opt-in, and deliberately NOT tied to ?focus: the device
            // would otherwise block forever on any curl-started or scheduled
            // run, including the ?cal=0 control. The web UI always sends it.
            if (httpd_query_key_value(qry, "confirm", val, sizeof(val)) == ESP_OK)
                g_status.pool_confirm = (val[0] == '1');
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

/* ── /pool POST — the operator's answer to the pool proposal ─────────
 *
 * `POST /pool?act=ok|more|cancel&main=3,7,12&euro=2,9`
 *
 * `main`/`euro` are the numbers still CHECKED. For `ok` they become the pool;
 * for `more` they are kept and omitted from a fresh scoring pass that refills
 * the rest. Omitting the lists entirely means "unchanged", which is what the
 * timeout path uses.
 *
 * 409 when no session is waiting, so a stale browser tab cannot inject a pool
 * into a session that has already moved on. */
static int parse_num_list(const char *s, uint8_t *out, int max_n, int max_val)
{
    int n = 0;
    while (*s && n < max_n) {
        while (*s == ',' || *s == ' ') s++;
        if (!*s) break;
        int v = 0, digits = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); digits++; }
        if (!digits) { while (*s && *s != ',') s++; continue; }
        if (v >= 1 && v <= max_val) {
            bool dup = false;               // a duplicate would corrupt the
            for (int i = 0; i < n; i++)     // combination enumeration
                if (out[i] == (uint8_t)v) { dup = true; break; }
            if (!dup) out[n++] = (uint8_t)v;
        }
    }
    return n;
}

/* POST /ready — the operator is settled and wants scoring to begin.
 *
 * Separate from /pool because it answers a different question at a different
 * point: /pool edits what will be measured, /ready says when to start measuring
 * it. 409 if nothing is waiting, so a stale tab cannot un-pause a session that
 * has already moved on. */
/* POST /probe — re-run slave discovery.
 *
 * Discovery is otherwise a one-shot broadcast at boot, so a slave that was
 * rebooting at that moment never appears until the next session starts. That
 * is exactly what happens after flashing all four nodes together, and it makes
 * the health page report a node as absent when it is fine.
 *
 * Refused while a session runs: nodes_discover() rebuilds the whole node table,
 * and doing that mid-session would renumber the array underneath a measurement
 * in flight. */
static esp_err_t probe_handler(httpd_req_t *req)
{
    if (g_status.state == ELOTTO_RUNNING) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "cannot re-scan while a session is running");
        return ESP_OK;
    }
    slave_probe();
    char msg[64];
    snprintf(msg, sizeof(msg), "ok: %d node(s)", g_status.node_count);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

static esp_err_t ready_handler(httpd_req_t *req)
{
    if (g_status.state != ELOTTO_RUNNING || g_status.phase != PHASE_READY) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "no session waiting to start");
        return ESP_OK;
    }
    elotto_ready_go();
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

static esp_err_t pool_handler(httpd_req_t *req)
{
    char qry[512] = "";
    char val[256] = "";
    PoolAction act = POOL_ACCEPT;
    uint8_t m[POOL_MAIN_49], e[POOL_EURO_12];
    int nm = 0, ne = 0;
    int max_main = (g_status.mode == MODE_EUROJACKPOT) ? 50 : 49;

    if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) == ESP_OK) {
        if (httpd_query_key_value(qry, "act", val, sizeof(val)) == ESP_OK) {
            if      (!strcmp(val, "more"))   act = POOL_MORE;
            else if (!strcmp(val, "cancel")) act = POOL_CANCEL;
        }
        if (httpd_query_key_value(qry, "main", val, sizeof(val)) == ESP_OK)
            nm = parse_num_list(val, m, POOL_MAIN_49, max_main);
        if (httpd_query_key_value(qry, "euro", val, sizeof(val)) == ESP_OK)
            ne = parse_num_list(val, e, POOL_EURO_12, 12);
    }

    /* Refuse a selection too small to build a draw from. The UI disables OK in
     * that state, but the endpoint is public and a session that accepted an
     * empty pool would compute comb(0,5) and measure nothing at all. */
    if (act == POOL_ACCEPT && nm > 0 && nm < g_status.pool_need_main) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "too few main numbers for one draw");
        return ESP_OK;
    }
    if (act == POOL_ACCEPT && g_status.pool_need_euro &&
        ne > 0 && ne < g_status.pool_need_euro) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "too few bonus numbers for one draw");
        return ESP_OK;
    }

    if (!elotto_pool_reply(act, m, nm, e, ne)) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "no session waiting for a pool");
        return ESP_OK;
    }
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

/* ── /diag GET – the camera, which is the only source there is ──────
 *
 * This used to A/B the on-chip TRNG register against esp_random() and report
 * the camera alongside them. Both TRNG tests are gone with the TRNG itself
 * (sensor.h): keeping a diagnostic that measures a source the firmware cannot
 * use would only invite comparisons against a number this instrument is not
 * allowed to produce. */
/* GET /diag — the four-camera health page.
 *
 * One row per node, live. Built because per-node optical faults are the thing
 * this rig actually suffers from — a dimming LED, a sensor dispersing more than
 * its neighbours — and until now answering "how are the cameras right now?"
 * meant four separate curl calls and reading JSON by eye.
 *
 * The browser fetches each node directly (every node serves its own stats with
 * an Access-Control-Allow-Origin header) rather than having the master proxy
 * them. Two reasons: the master's UDP 'D' command is only safe between runs, so
 * a proxy could not refresh during a session; and a node that has crashed
 * should show as unreachable on its own row instead of silently reporting a
 * stale value the master cached.
 *
 * Deliberately narrow: the parameters that are tuned (exposure, gain), the ones
 * the calibration gates test (bias, sigma, autocorr, mean_px), throughput, and
 * stall count. Everything else that used to be in the JSON — frame_pairs,
 * drops, waits — is diagnostics for a different question and is still one click
 * away at /diagjson. */
static const char DIAG_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>elotto - camera health</title><style>"
"body{background:#0b1a0b;color:#cfe8cf;font-family:system-ui,sans-serif;margin:0;padding:16px}"
"h1{color:#90ee90;font-size:1.1em;margin:0 0 2px}"
"p.sub{color:#8fae8f;font-size:.82em;margin:0 0 14px}"
"table{border-collapse:collapse;width:100%;font-size:.86em}"
"th{color:#f0c040;text-align:right;padding:7px 9px;border-bottom:1px solid #2c4a2c;font-weight:600}"
"th.l,td.l{text-align:left}"
"td{text-align:right;padding:7px 9px;border-bottom:1px solid #1b301b;font-variant-numeric:tabular-nums}"
"tr.off td{color:#7a5a5a}"
"a{color:#6ab0e8}"
".ok{color:#90ee90}.warn{color:#f0c040}.bad{color:#ff6b6b;font-weight:700}"
".n{color:#fff;font-weight:600}"
/* Exposure controls. Buttons only, no text field, and that is deliberate: the
   table is rebuilt from scratch every 2 s, so anything holding operator state
   between ticks (a half-typed number, a caret position) would be wiped
   mid-keystroke. A button carries no state to lose. */
"button.e{background:#1b301b;color:#cfe8cf;border:1px solid #2c4a2c;border-radius:5px;"
"font:inherit;font-size:.95em;padding:2px 8px;margin:0 1px;cursor:pointer}"
"button.e:hover{background:#2c4a2c;color:#fff}"
"button.e:disabled{opacity:.35;cursor:default}"
"td.set{white-space:nowrap}"
".ip{color:#8fae8f;font-size:.84em;font-weight:400;font-variant-numeric:tabular-nums}"
"</style></head><body>"
"<h1>&#128247; Camera health</h1>"
"<p class='sub'>Live, every 2 s. Colour is against the calibration gates: "
"bias 1e-3, |&sigma;-1| 0.05, autocorr 0.01, mean_px 100. "
"<a href='/'>&#8592; back</a> &middot; <a href='/diagjson'>master JSON</a> &middot; "
"<a href='#' onclick='rescan();return false'>re-scan nodes</a></p>"
"<p class='sub'>&#9881; <b>Set exp</b> halves or doubles that node's exposure and "
"resets its statistics, so mean_px answers within a second or two &mdash; for "
"tuning the physical light. Refused while a session runs, and the next "
"calibration sweep overwrites it.</p>"
"<table><thead><tr>"
"<th class='l'>Node</th><th>Exp</th><th class='l'>set exp</th><th>Gain</th><th>mean_px</th>"
"<th>bias&minus;0.5</th><th>&sigma;</th><th>zero_diff</th><th>autocorr</th>"
"<th>Mbit/s</th><th>stalls</th>"
"</tr></thead><tbody id='rows'><tr><td class='l' colspan='11'>loading&hellip;</td></tr>"
"</tbody></table>"
"<p class='sub' id='msg' style='margin-top:12px;color:#cfe8cf;min-height:1.2em'></p>"
"<p class='sub' id='foot' style='margin-top:2px'></p>"
"<script>"
"var nodes=null;"
/* The ONE place in the firmware that maps an address to a node name, and it is
   deliberately cosmetic: nothing addresses a node by it, nothing in the combine
   sees it, and it is never written to /loops. The array still finds its nodes by
   UDP broadcast with no IP table anywhere, which is why a wrong or changed
   address here costs a label and nothing else.
   It exists because the operator has to walk to a physical box and change a lamp,
   and "192.168.178.145" is not what that box is called. The addresses are static
   DHCP leases keyed on MAC, so the mapping is stable in this rig.
   An unknown address is shown as-is rather than guessed at: the slave INDEX is
   discovery order (reply arrival), not identity, so naming a row from its
   position in the array would silently mislabel nodes the day two of them race. */
"var NAMES={'192.168.178.100':'master','192.168.178.103':'slave0',"
"'192.168.178.145':'slave1','192.168.178.155':'slave2'};"
"function nameOf(ip){return NAMES[ip]||'unnamed';}"
/* Name over address, both always visible: the name is what the operator thinks
   in, the address is what they need to curl or to find the node in the router. */
"function cell(nm,ip){return \"<td class='l n'>\"+nm+\"<div class='ip'>\"+ip+\"</div></td>\";}"
/* Node list comes from /status, which is populated by the discovery broadcast
   at boot -- so the page works even when no session has ever run. */
"function cls(v,warn,bad){return Math.abs(v)>=bad?'bad':(Math.abs(v)>=warn?'warn':'ok');}"
"function f(v,n){return (v===undefined||v===null)?'-':Number(v).toFixed(n);}"
/* The endpoint lives on the node itself, exactly like the reads above it: the
   master proxying it would need the UDP link, which is only safe between runs,
   and a node that has crashed must fail on its own row rather than through the
   master. `base` is '' for the master (same origin) and 'http://<ip>' for a
   slave, which is the one thing that differs between them. */
/* Reports into its own line, not #foot: tick() rewrites #foot every 2 s, so the
   answer to a click would vanish before it was read. */
"function setExp(base,ip,v){"
"var f=document.getElementById('msg'),who=nameOf(ip)+' ('+ip+')';"
"f.textContent='setting '+who+' to exposure '+v+'\\u2026';"
"fetch(base+'/expose?exp='+v,{method:'POST'}).then(function(r){"
"return r.json().then(function(j){return {s:r.status,j:j};});}).then(function(x){"
"if(x.j&&x.j.ok)f.textContent=who+': exposure now '+x.j.exposure+' (gain '+x.j.gain+')';"
"else f.textContent='\\u26a0 '+who+': '+((x.j&&x.j.err)?x.j.err:'refused ('+x.s+')');"
"tick();"
"}).catch(function(){f.textContent='\\u26a0 '+who+': no answer to /expose';});"
"}"
"function row(name,ip,d,base){"
"if(!d)return \"<tr class='off'>\"+cell(name,ip)+"
"\"<td class='l' colspan='10'>no answer</td></tr>\";"
"var c=d.cam||{};"
"var b=(c.bias===undefined?0:c.bias-0.5);"
"var s=(c.sigma===undefined?1:c.sigma);"
"var ac=0;if(c.autocorr&&c.autocorr.length){for(var i=0;i<c.autocorr.length;i++)"
"ac=Math.max(ac,Math.abs(c.autocorr[i]));}"
"var st=c.stalls||0;"
/* Powers of two, because that is the ladder calibration itself sweeps
   (4..512) — stepping in the same units keeps a hand-set rung comparable with a
   swept one. Clamped to the register's 20 bits at the top and to 1 at the
   bottom, where 0 would stop integration altogether. */
"var e=c.exposure,ctl='-';"
"if(e!==undefined){"
"var dn=Math.max(1,Math.floor(e/2)),up=Math.min(1048575,e*2);"
"ctl=\"<button class='e' onclick=\\\"setExp('\"+base+\"','\"+ip+\"',\"+dn+\")\\\" \"+"
"(e<=1?'disabled':'')+\">&minus;</button>\"+"
"\"<button class='e' onclick=\\\"setExp('\"+base+\"','\"+ip+\"',\"+up+\")\\\" \"+"
"(e>=1048575?'disabled':'')+\">+</button>\";"
"}"
"return \"<tr>\"+cell(name,ip)+"
"\"<td>\"+(c.exposure===undefined?'-':c.exposure)+\"</td>\"+"
"\"<td class='l set'>\"+ctl+\"</td>\"+"
"\"<td>\"+(c.gain===undefined?'-':c.gain)+\"</td>\"+"
"\"<td class='\"+(c.mean_pixel>=100?'bad':'')+\"'>\"+f(c.mean_pixel,1)+\"</td>\"+"
"\"<td class='\"+cls(b,5e-4,1e-3)+\"'>\"+(b>=0?'+':'')+b.toExponential(2)+\"</td>\"+"
"\"<td class='\"+cls(s-1,0.025,0.05)+\"'>\"+f(s,4)+\"</td>\"+"
"\"<td>\"+f(c.zero_diff,4)+\"</td>\"+"
"\"<td class='\"+cls(ac,0.005,0.01)+\"'>\"+f(ac,4)+\"</td>\"+"
"\"<td>\"+f(c.mbit_s,3)+\"</td>\"+"
"\"<td class='\"+(st>0?'bad':'ok')+\"'>\"+st+\"</td></tr>\";"
"}"
"function get(u){return fetch(u).then(function(r){return r.json();}).catch(function(){return null;});}"
/* Discovery is a one-shot broadcast at boot, so a slave that was rebooting then
   is missing from the list until the next session starts -- which is exactly
   what happens after flashing all four together. Re-scan rebuilds it. Clearing
   the cache is the point: without it the page would keep the stale list. */
"function rescan(){"
"document.getElementById('foot').textContent='re-scanning\\u2026';"
"fetch('/probe',{method:'POST'}).then(function(r){return r.text();}).then(function(t){"
"nodes=null;tick();"
"}).catch(function(){document.getElementById('foot').textContent='re-scan failed';});"
"}"
"function tick(){"
"var p=nodes?Promise.resolve(nodes):get('/status').then(function(d){"
"nodes=(d&&d.nodes)?d.nodes:[];return nodes;});"
"p.then(function(ns){"
"var jobs=[get('/diagjson')];"
"for(var i=1;i<ns.length;i++)jobs.push(get('http://'+ns[i].ip+'/diag'));"
"Promise.all(jobs).then(function(res){"
"var h='';"
/* The master's own row has no address in /status — it is 'self' there — so it
   takes the one the browser is already talking to. */
"h+=row(nameOf(location.hostname)+' (self)',location.hostname,res[0],'');"
"for(var i=1;i<ns.length;i++)"
"h+=row(nameOf(ns[i].ip),ns[i].ip,res[i],'http://'+ns[i].ip);"
"document.getElementById('rows').innerHTML=h;"
"document.getElementById('foot').textContent="
"'updated '+new Date().toLocaleTimeString()+' \\u00b7 '+ns.length+' node(s) known';"
"});});}"
"tick();setInterval(tick,2000);"
"</script></body></html>";

static esp_err_t diag_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, DIAG_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* GET /diagjson — this node's own camera statistics, machine-readable.
 *
 * Was /diag until 2026-07-28. /diag is now the human-facing page that renders
 * all four nodes; this is the raw source behind the master's row, and each
 * slave serves the same shape at its own /diag. */
static esp_err_t diagjson_handler(httpd_req_t *req)
{
    static char buf[3072];
    int pos = 0;

    camera_stats_t cam;
    camera_get_stats(&cam);
    uint32_t exp_now = 0, gain_now = 0;
    camera_get_exposure(&exp_now, &gain_now);

    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "{"
        "\"src\":\"camera-only\","
        "\"cam\":{"
        "\"ready\":%s,\"frame_pairs\":%llu,\"bits\":%llu,\"stuck_frames\":%lu,"
        "\"bias\":%.6f,\"sigma\":%.4f,\"sigma_n\":%d,"
        "\"autocorr\":[%.4f,%.4f,%.4f,%.4f],"
        "\"mean_pixel\":%.2f,\"mbit_s\":%.3f,\"zero_diff\":%.4f,"
        "\"drops\":%lu,\"waits\":%lu,\"stalls\":%lu,"
        "\"exposure\":%lu,\"gain\":%lu,\"fold\":%s"
        "}"
        "}",
        cam.ready ? "true" : "false",
        (unsigned long long)cam.frame_pairs, (unsigned long long)cam.bits_extracted,
        (unsigned long)cam.stuck_frame_count,
        cam.bias, cam.sigma, cam.sigma_samples,
        cam.autocorr_lag[0], cam.autocorr_lag[1], cam.autocorr_lag[2], cam.autocorr_lag[3],
        cam.mean_pixel_level, cam.mbit_per_sec, cam.zero_diff_frac,
        (unsigned long)cam.ring_drops, (unsigned long)cam.consumer_waits,
        (unsigned long)cam.stalls,
        (unsigned long)exp_now, (unsigned long)gain_now,
        camera_get_xor_fold() ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* POST /expose?exp=<lines>[&gain=<g>] — set THIS node's operating point by hand.
 *
 * The logic is in the camera component so every node answers identically; see
 * camera.h. Refused while a session runs: `session_running()` is the same
 * predicate /update uses, and re-tuning the sensor mid-session would change the
 * instrument under a Σz that is already accumulating.
 *
 * ⚠ Not sticky. The next calibration sweep re-derives the setting and overwrites
 * whatever was set here — which is correct (the sweep is the thing that decides
 * an operating point on evidence), but it means this is a tool for tuning the
 * LIGHT against a live mean_px, not a way to pin a node to a rung. */
static esp_err_t expose_handler(httpd_req_t *req)
{
    return camera_expose_handle(req, g_status.state == ELOTTO_RUNNING);
}

/* ── GET / ────────────────────────────────────────────────────────── */
static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, HTML);
    return ESP_OK;
}

/* ── Webserver ────────────────────────────────────────────────────── */
/* An update must never destroy a measurement: flashing mid-session silently
 * discards however many hours it had accumulated. /update answers 409 while
 * this returns true. The USB path had no such guard — only discipline and a
 * note in CLAUDE.md to check /status first. */
static bool session_running(void) { return g_status.state == ELOTTO_RUNNING; }

static void start_webserver(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* 12 here + 5 registered by elotto_ota = 17. Keep headroom: registration
     * past this limit fails, and the return value is not checked at either
     * call site, so an endpoint would simply 404 with nothing logged. Adding
     * /ready and /diagjson took it from 14 to 17 and silently over the old
     * cap of 16 — count them when adding one. */
    cfg.max_uri_handlers  = 24;
    cfg.stack_size        = 8192;
    cfg.recv_wait_timeout = 20;   /* /update streams a ~700 KB body */
    cfg.send_wait_timeout = 20;
    cfg.lru_purge_enable  = true;
    httpd_handle_t srv = NULL;
    ESP_ERROR_CHECK(httpd_start(&srv, &cfg));
    static const httpd_uri_t uris[] = {
        {"/",       HTTP_GET,  root_handler,   NULL},
        {"/status", HTTP_GET,  status_handler, NULL},
        {"/start",  HTTP_POST, start_handler,  NULL},
        {"/abort",  HTTP_POST, abort_handler,  NULL},
        {"/diag",     HTTP_GET, diag_handler,     NULL},
        {"/diagjson", HTTP_GET, diagjson_handler, NULL},
        {"/loops",  HTTP_GET,  loops_handler,  NULL},
        {"/focus",  HTTP_GET,  focus_handler,  NULL},
        {"/pause",  HTTP_POST, pause_handler,  NULL},
        {"/calibrate", HTTP_GET, calibrate_handler, NULL},
        {"/pool", HTTP_POST, pool_handler, NULL},
        {"/ready", HTTP_POST, ready_handler, NULL},
        {"/probe", HTTP_POST, probe_handler, NULL},
        {"/expose", HTTP_POST, expose_handler, NULL},
    };
    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++)
        httpd_register_uri_handler(srv, &uris[i]);

    /* /update, /boot, /reboot, /poison, /otainfo — the same shared code the
     * factory updater runs (docs/PLAN_NETWORK.md). */
    elotto_ota_register(srv, session_running);

    /* Only now is this image proven reachable, which is the criterion that
     * matters: an app that answers here can always be replaced over the wire,
     * however broken its measurement logic. Marking valid any earlier would
     * hand rollback's protection away for nothing. */
    elotto_ota_mark_valid();

    ESP_LOGI(TAG, "Webserver running");
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
    if (bits & ETH_GOT_IP_BIT) {
        start_webserver();
        /* The slave now lives on the same Ethernet as the browser, so the probe
         * cannot run before there is an IP the way the UART one did. It follows
         * the webserver rather than preceding it: mark-valid must not wait on a
         * peer that may be offline. */
        slave_probe();
    } else {
        ESP_LOGE(TAG, "No Ethernet after 30s");
    }
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

    /* Before anything that can itself crash: counts boots that never reached a
     * healthy uptime and falls back to the factory updater. This is the only
     * escape from a *validated* image that crash-loops — rollback is disarmed
     * once an image is marked valid, so the bootloader would relaunch it
     * forever and the reset button would not help. */
    elotto_ota_boot_check();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    eth_event_group = xEventGroupCreate();
    ethernet_init();
    xTaskCreate(webserver_task, "ws_task", 8192, NULL, 5, NULL);

    // Camera bring-up. Non-fatal HERE on purpose: Ethernet, the webserver and
    // OTA must come up regardless, or a node with a dead camera could not be
    // diagnosed, rebooted or reflashed over the wire. But it is the only source
    // there is, so a session that finds no camera faults immediately — see
    // camera_source_begin() in sensor.c.
    esp_err_t cam_ret = camera_init();
    if (cam_ret != ESP_OK) {
        ESP_LOGE(TAG, "camera_init: %s -- THIS NODE CANNOT MEASURE",
                 esp_err_to_name(cam_ret));
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(TAG, "Heap: %lu", (unsigned long)esp_get_free_heap_size());
    }
}
