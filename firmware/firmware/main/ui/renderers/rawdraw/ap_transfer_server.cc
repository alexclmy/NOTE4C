/**
 * @file ap_transfer_server.cc
 * @brief WiFi AP + HTTP Server implementation for image transfer
 */

#include "ap_transfer_server.h"
#include "dashboard_api.h"
#include "boards/zectrix-s3-epaper-4.2/config.h"
#include "common/device_config_service.h"
#include "common/photo_storage.h"
#include "common/power_policy.h"
#include "settings.h"
#include "wifi_manager.h"

#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <esp_err.h>
#include <esp_sleep.h>
#include <lwip/ip_addr.h>
#include <unistd.h>
#include <cJSON.h>

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <new>
#include <string>
#include <utility>

namespace rawdraw {

namespace {
static const char* kTag = "ApTransferServer";

// AP configuration
constexpr const char* kApSsid = "InkScreen-AP";
constexpr const char* kApPassword = "12345678";
constexpr const char* kApIp = "192.168.4.1";
constexpr const char* kGalleryNamespace = "gallery";
constexpr const char* kSlideshowIntervalKey = "slide_min";

// Screen dimensions
constexpr int kScreenWidth = 400;
constexpr int kScreenHeight = 300;
constexpr size_t kImage1bppSize = kScreenWidth * kScreenHeight / 8;
constexpr size_t kImage2bppSize = kScreenWidth * kScreenHeight * 2 / 8;

// Embedded HTML. Kept self-contained because the ESP-IDF HTTP server serves
// this page from flash while the device is in AP mode.
const char kUploadHtml[] = R"HTML(
<!DOCTYPE html><html><head><meta charset="UTF-8"><title>E-paper transfer</title>
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<style>
*{box-sizing:border-box}body{margin:0;background:#ece8dc;color:#171717;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;font-size:12px}.app{max-width:520px;margin:0 auto;padding:10px}.top{display:flex;align-items:center;justify-content:space-between;margin-bottom:8px}.brand{font-weight:800;font-size:16px}.pill{border:1px solid #111;background:#ffd900;border-radius:3px;padding:3px 6px;font-size:11px}.panel{background:#fff;border:2px solid #111;border-radius:6px;box-shadow:3px 3px 0 #111;margin-bottom:10px;padding:9px}.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}.muted{color:#555}.btn{border:2px solid #111;background:#ff3b30;color:#fff;border-radius:5px;padding:8px 10px;font-weight:800;font-size:12px;box-shadow:2px 2px 0 #111}.btn.secondary{background:#fff;color:#111}.btn.yellow{background:#ffd900;color:#111}.btn.danger{background:#111;color:#fff}.btn.icon{width:32px;height:32px;border-radius:50%;padding:0;font-size:18px;line-height:1}.btn:disabled{opacity:.45}.file{position:absolute;left:-9999px}.radio{display:inline-flex;gap:5px;align-items:center;border:1px solid #111;border-radius:4px;padding:5px 7px;background:#fafafa}.radio input{margin:0}.preview{width:100%;aspect-ratio:4/3;border:2px solid #111;background:#fff;image-rendering:pixelated;margin-top:8px}.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:8px}.card{border:2px solid #111;border-radius:5px;background:#fff;overflow:hidden;position:relative}.thumb{width:100%;aspect-ratio:4/3;background:#f8f8f8;display:block;image-rendering:pixelated}.meta{padding:6px}.title{font-weight:800;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.body{font-size:11px;color:#444;line-height:1.35;height:30px;overflow:hidden}.check{position:absolute;top:5px;left:5px;width:20px;height:20px}.tag{position:absolute;top:5px;right:5px;background:#ffd900;border:1px solid #111;border-radius:3px;padding:2px 4px;font-size:10px}.bar{display:flex;align-items:center;justify-content:space-between;gap:6px;margin:8px 0}.status{min-height:18px;color:#333}.note{border:2px solid #111;background:#fffbe6;border-radius:6px;padding:9px;margin-bottom:10px;box-shadow:3px 3px 0 #111}.note ul{margin:6px 0 0 18px;padding:0;line-height:1.55}.modal{position:fixed;inset:0;background:rgba(0,0,0,.45);display:none;align-items:center;justify-content:center;padding:12px}.modal.open{display:flex}.dialog{max-width:520px;width:100%;background:#fff;border:2px solid #111;border-radius:7px;box-shadow:4px 4px 0 #111;position:relative;padding:10px}.close{position:absolute;right:8px;top:8px;border:2px solid #111;background:#fff;border-radius:50%;width:28px;height:28px;font-weight:900}.big{width:100%;aspect-ratio:4/3;border:2px solid #111;image-rendering:pixelated}.empty{padding:18px;text-align:center;border:1px dashed #999;background:#fafafa}.split{display:grid;grid-template-columns:1fr;gap:8px}@media(min-width:460px){.split{grid-template-columns:190px 1fr}.grid{grid-template-columns:repeat(3,minmax(0,1fr))}}
</style></head><body><main class="app">
<div class="top"><div><div class="brand">E-paper transfer</div><div class="muted" id="endpoint">Reading service address...</div></div><div class="row"><button class="btn secondary" id="settingsBtn">Settings</button><button class="btn secondary icon" id="helpBtn">!</button><div class="pill">400x300</div></div></div>
<section class="note" id="helpPanel" style="display:none"><b>How this works</b><ul><li>Uploads 1 BP monochrome and 2 BP four-color images. Saved images appear in the device gallery.</li><li>With the slideshow off, the full-screen view stays on the current image. With it on, the device cycles through the gallery at the chosen interval.</li><li>While the LAN service is on, the device IP is shown at the top of this page, so a phone or a local server can manage the images.</li><li>Stop service only closes the local transfer page. Stop and save power also stops the service and Wi-Fi and enters deep sleep; press BOOT to wake.</li><li>For maximum battery life: pick the image, turn the slideshow off, then stop and save power. The panel keeps showing the last image.</li></ul></section>
<section class="panel split"><div><div class="title">Send an image</div><p class="muted">Choose an image, check the converted preview, then send it to the device.</p><div class="row"><label class="radio"><input name="fmt" type="radio" value="1bpp" checked>1 BP mono</label><label class="radio"><input name="fmt" type="radio" value="bwry2bpp">2 BP four-color</label></div><div class="row" style="margin-top:8px"><button class="btn yellow" id="pick">Choose image</button><button class="btn" id="send" disabled>Send</button></div><input class="file" id="file" type="file" accept="image/*"><div class="status" id="status">Nothing selected</div></div><div><canvas class="preview" id="preview" width="400" height="300"></canvas></div></section>
<section class="panel" id="settingsPanel" style="display:none"><div class="bar"><b>Gallery slideshow interval</b><span class="muted" id="settingsState"></span></div><div class="row"><label class="radio"><input name="slide" type="radio" value="0">Off</label><label class="radio"><input name="slide" type="radio" value="5">5min</label><label class="radio"><input name="slide" type="radio" value="10">10min</label><label class="radio"><input name="slide" type="radio" value="30">30min</label><button class="btn yellow" id="saveSettings">Save settings</button><button class="btn secondary" id="stopService">Stop service</button><button class="btn danger" id="sleepNow">Stop and save power</button></div></section>
<section class="panel"><div class="bar"><div><b>Device images</b> <span class="muted" id="count"></span></div><div class="row"><button class="btn secondary" id="reload">Reload</button><button class="btn danger" id="batch" disabled>Delete selected</button></div></div><div id="photos" class="grid"><div class="empty">Loading...</div></div></section>
</main>
<div class="modal" id="modal"><div class="dialog"><button class="close" id="close">×</button><canvas class="big" id="big" width="400" height="300"></canvas><div class="meta"><input id="mTitle" style="width:100%;padding:7px;border:1px solid #111;font-weight:800"><div class="row" style="margin-top:6px"><input id="mDate" placeholder="Date" style="flex:1;padding:7px;border:1px solid #111"><input id="mLocation" placeholder="Location" style="flex:1;padding:7px;border:1px solid #111"></div><textarea id="mBody" rows="3" style="width:100%;margin-top:6px;padding:7px;border:1px solid #111"></textarea><div class="muted" id="mMeta" style="margin-top:5px"></div><div class="row" style="margin-top:8px"><button class="btn yellow" id="mSave">Save details</button><button class="btn secondary" id="mUp">Move up</button><button class="btn secondary" id="mDown">Move down</button><button class="btn danger" id="mDelete">Delete this one</button></div></div></div></div>
<script>
const W=400,H=300,photosEl=document.getElementById('photos'),statusEl=document.getElementById('status'),pv=document.getElementById('preview'),fileEl=document.getElementById('file'),sendBtn=document.getElementById('send'),batchBtn=document.getElementById('batch'),countEl=document.getElementById('count'),settingsPanel=document.getElementById('settingsPanel'),settingsState=document.getElementById('settingsState'),endpointEl=document.getElementById('endpoint');let pending=null,pendingFmt='1bpp',items=[],selected=new Set(),active=null;
async function loadStatus(){try{const j=await (await fetch('/status')).json();endpointEl.textContent=`${j.mode==='lan'?'LAN':'InkScreen-AP'} / ${j.ip}`;document.title=`E-paper transfer ${j.ip}`}catch(e){endpointEl.textContent='Could not read the service address'}}
function fmt(){return document.querySelector('input[name=fmt]:checked').value}
function rgba(c){return c===0?[0,0,0]:c===1?[255,255,255]:c===2?[255,217,0]:[220,0,0]}
function draw1(buf,canvas){const ctx=canvas.getContext('2d'),img=ctx.createImageData(W,H),d=img.data;for(let p=0,i=0;p<W*H;p++,i+=4){const v=(buf[p>>3]&(1<<(7-(p&7))))?255:0;d[i]=d[i+1]=d[i+2]=v;d[i+3]=255}ctx.putImageData(img,0,0)}
function draw2(buf,canvas){const ctx=canvas.getContext('2d'),img=ctx.createImageData(W,H),d=img.data;for(let p=0,i=0;p<W*H;p++,i+=4){const b=buf[p>>2],c=(b>>(6-((p&3)*2)))&3,r=rgba(c);d[i]=r[0];d[i+1]=r[1];d[i+2]=r[2];d[i+3]=255}ctx.putImageData(img,0,0)}
function fitImage(file){return new Promise((res,rej)=>{const img=new Image();img.onload=()=>{const c=document.createElement('canvas');c.width=W;c.height=H;const x=c.getContext('2d',{willReadFrequently:true});x.fillStyle='#fff';x.fillRect(0,0,W,H);const s=Math.min(W/img.width,H/img.height),w=Math.round(img.width*s),h=Math.round(img.height*s);x.drawImage(img,(W-w)/2,(H-h)/2,w,h);URL.revokeObjectURL(img.src);res(x.getImageData(0,0,W,H).data)};img.onerror=rej;img.src=URL.createObjectURL(file)})}
async function convert(file){const data=await fitImage(file),mode=fmt();pendingFmt=mode;if(mode==='bwry2bpp'){const work=new Array(H);for(let y=0;y<H;y++){work[y]=new Array(W);for(let x=0;x<W;x++){const i=(y*W+x)*4;work[y][x]={r:data[i],g:data[i+1],b:data[i+2]}}}const pal=[[0,0,0],[255,255,255],[255,0,0],[255,255,0]];const out=new Uint8Array(30000);for(let y=0;y<H;y++)for(let x=0;x<W;x++){const old=work[y][x];let minD=1e9,cIdx=0,cRgb=pal[0];for(let k=0;k<4;k++){const d=0.299*(old.r-pal[k][0])**2+0.587*(old.g-pal[k][1])**2+0.114*(old.b-pal[k][2])**2;if(d<minD){minD=d;cIdx=k;cRgb=pal[k]}}const bwry=cIdx===0?0:cIdx===1?1:cIdx===2?3:2;out[(y*W+x)>>2]|=bwry<<(6-((y*W+x)&3)*2);const eR=old.r-cRgb[0],eG=old.g-cRgb[1],eB=old.b-cRgb[2];const dist=(dy,dx,f)=>{const ny=y+dy,nx=x+dx;if(ny>=0&&ny<H&&nx>=0&&nx<W){work[ny][nx].r+=eR*f;work[ny][nx].g+=eG*f;work[ny][nx].b+=eB*f}};dist(0,1,7/16);dist(1,-1,3/16);dist(1,0,5/16);dist(1,1,1/16)}draw2(out,pv);return out}const gray=new Int16Array(W*H);for(let p=0,i=0;p<gray.length;p++,i+=4)gray[p]=(data[i]*30+data[i+1]*59+data[i+2]*11)/100|0;const out=new Uint8Array(15000);for(let y=0;y<H;y++)for(let x=0;x<W;x++){const p=y*W+x,old=Math.max(0,Math.min(255,gray[p])),nw=old>128?255:0,err=old-nw;if(nw>128)out[p>>3]|=1<<(7-(p&7));if(x+1<W)gray[p+1]+=err*7/16;if(y+1<H){if(x>0)gray[p+W-1]+=err*3/16;gray[p+W]+=err*5/16;if(x+1<W)gray[p+W+1]+=err/16}}draw1(out,pv);return out}
document.getElementById('pick').onclick=()=>fileEl.click();fileEl.onchange=async()=>{const f=fileEl.files[0];if(!f)return;statusEl.textContent='Converting...';try{pending=await convert(f);sendBtn.disabled=false;statusEl.textContent=`Preview: ${pendingFmt==='bwry2bpp'?'2 BP four-color':'1 BP mono'}. Press Send when it looks right`}catch(e){statusEl.textContent='Image processing failed';sendBtn.disabled=true}};document.querySelectorAll('input[name=fmt]').forEach(r=>r.onchange=()=>{if(fileEl.files[0])fileEl.onchange()});
sendBtn.onclick=async()=>{if(!pending)return;sendBtn.disabled=true;statusEl.textContent='Uploading...';try{const r=await fetch('/upload?format='+encodeURIComponent(pendingFmt),{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:pending});const j=await r.json();statusEl.textContent=j.success?'Sent and saved':'Failed: '+(j.error||'unknown');await loadPhotos()}catch(e){statusEl.textContent='Network error'}sendBtn.disabled=false};
async function loadBin(p,c){const b=new Uint8Array(await (await fetch('/photo?id='+encodeURIComponent(p.id),{cache:'no-store'})).arrayBuffer());(p.format==='bwry2bpp'||p.size>15000?draw2:draw1)(b,c)}
function updateBatch(){batchBtn.disabled=selected.size===0}
async function loadPhotos(){selected.clear();updateBatch();try{const j=await (await fetch('/photos',{cache:'no-store'})).json();items=j.photos||[];countEl.textContent=`${items.length}`;photosEl.innerHTML=items.length?'':'<div class="empty">No images yet</div>';const thumbs=[];for(const p of items){const card=document.createElement('div');card.className='card';card.innerHTML=`<input class="check" type="checkbox"><span class="tag">${p.format==='bwry2bpp'?'2BP':'1BP'}</span><canvas class="thumb" width="400" height="300"></canvas><div class="meta"><div class="title">${p.title||p.id}</div><div class="body">${p.body||''}</div><div class="muted">${p.date||''} ${p.location||''}</div><div class="row" style="margin-top:5px"><button class="btn yellow show">Show</button><button class="btn secondary up">Move up</button><button class="btn secondary down">Move down</button></div></div>`;const c=card.querySelector('canvas'),ck=card.querySelector('input');ck.onclick=e=>{e.stopPropagation();ck.checked?selected.add(p.id):selected.delete(p.id);updateBatch()};card.querySelector('.show').onclick=e=>{e.stopPropagation();showPhoto(p.id)};card.querySelector('.up').onclick=e=>{e.stopPropagation();movePhoto(p.id,-1)};card.querySelector('.down').onclick=e=>{e.stopPropagation();movePhoto(p.id,1)};card.onclick=()=>openModal(p);photosEl.appendChild(card);thumbs.push([p,c])}for(const [p,c] of thumbs){await loadBin(p,c).catch(()=>{})}}catch(e){photosEl.innerHTML='<div class="empty">Could not load</div>'}}
function openModal(p){active=p;document.getElementById('modal').classList.add('open');document.getElementById('mTitle').value=p.title||'';document.getElementById('mDate').value=p.date||'';document.getElementById('mLocation').value=p.location||'';document.getElementById('mMeta').textContent=`${p.format==='bwry2bpp'?'2 BP four-color':'1 BP mono'} · ${p.width}x${p.height}`;document.getElementById('mBody').value=p.body||'';loadBin(p,document.getElementById('big')).catch(()=>{})}
document.getElementById('close').onclick=()=>document.getElementById('modal').classList.remove('open');document.getElementById('modal').onclick=e=>{if(e.target.id==='modal')document.getElementById('modal').classList.remove('open')};
async function delOne(id){return fetch('/photo?id='+encodeURIComponent(id),{method:'DELETE'}).then(r=>r.json())}
document.getElementById('mDelete').onclick=async()=>{if(!active)return;if(!confirm('Delete this image?'))return;await delOne(active.id);document.getElementById('modal').classList.remove('open');loadPhotos()};
async function movePhoto(id,delta){await fetch('/photos/move',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id,delta})});await loadPhotos()}
async function showPhoto(id){const j=await (await fetch('/photo/show',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id})})).json();statusEl.textContent=j.success?'Switched the device to this image':'Could not display'}
document.getElementById('mUp').onclick=()=>active&&movePhoto(active.id,-1);
document.getElementById('mDown').onclick=()=>active&&movePhoto(active.id,1);
document.getElementById('mSave').onclick=async()=>{if(!active)return;const body={id:active.id,title:document.getElementById('mTitle').value,date:document.getElementById('mDate').value,location:document.getElementById('mLocation').value,body:document.getElementById('mBody').value};const r=await fetch('/photo/meta',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});if((await r.json()).success){document.getElementById('modal').classList.remove('open');await loadPhotos()}};
document.getElementById('settingsBtn').onclick=()=>{settingsPanel.style.display=settingsPanel.style.display==='none'?'block':'none';loadSettings()};
document.getElementById('helpBtn').onclick=()=>{const p=document.getElementById('helpPanel');p.style.display=p.style.display==='none'?'block':'none'};
async function loadSettings(){try{const j=await (await fetch('/settings')).json();document.querySelectorAll('input[name=slide]').forEach(r=>r.checked=Number(r.value)===j.slideshow_interval);const slide=j.slideshow_interval?`Slideshow ${j.slideshow_interval}min`:'Slideshow off';const svc=j.service_running?`Service on ${j.url||''}`:'Service will stop';settingsState.textContent=`${slide} · ${svc}`}catch(e){settingsState.textContent='Could not load'}}
document.getElementById('saveSettings').onclick=async()=>{const v=Number(document.querySelector('input[name=slide]:checked')?.value||0);const j=await (await fetch('/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({slideshow_interval:v})})).json();settingsState.textContent=j.success?(v?`Saved ${v}min`:'Off'):'Save failed'};
document.getElementById('stopService').onclick=async()=>{if(!confirm('Stop the local transfer service?'))return;await fetch('/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({service_enabled:false})});settingsState.textContent='Service is stopping'};
document.getElementById('sleepNow').onclick=async()=>{if(!confirm('Stop the service and Wi-Fi, and enter power saving?'))return;await fetch('/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({service_enabled:false,wifi_enabled:false,sleep:true})});settingsState.textContent='Device is entering power saving'};
batchBtn.onclick=async()=>{const ids=[...selected];if(!ids.length)return;if(!confirm(`Delete ${ids.length} image(s)?`))return;for(const id of ids)await delOne(id);loadPhotos()};document.getElementById('reload').onclick=loadPhotos;loadStatus();loadSettings();loadPhotos();
</script></body></html>
)HTML";

/**
 * @brief Put the radio into the state power_policy says a server needs.
 *
 * The rule is host tested in common/power_policy.cc; this is the one place that
 * maps it onto the driver, so the AP path and the LAN path cannot drift apart
 * again. Logged rather than silent: the single serial capture that is supposed
 * to prove a LAN client can now reach port 80 has to show that this happened.
 */
void ApplyServingPowerSave(bool serving) {
    const wifi_ps_type_t ps =
        power::ServingWifiPowerSave(serving) == power::WifiPowerSave::kNone
            ? WIFI_PS_NONE
            : WIFI_PS_MIN_MODEM;
    const esp_err_t err = esp_wifi_set_ps(ps);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "esp_wifi_set_ps(%s) failed: %s",
                 serving ? "NONE" : "MIN_MODEM", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(kTag, "LAN serving: wifi power save %s",
             serving ? "off" : "restored to the driver default");
}

/**
 * @brief Proof that a TCP connection was accepted at all.
 *
 * The failure this exists to tell apart is the one where `httpd_start` reports
 * success, 21 routes register, and no client can complete a handshake: with no
 * `open fd=` line the problem is below HTTP — the radio, or the window being
 * over — and with one it is above it.
 */
esp_err_t HttpSessionOpen(httpd_handle_t hd, int sockfd) {
    (void)hd;
    ESP_LOGI(kTag, "http: open fd=%d", sockfd);
    return ESP_OK;
}

/**
 * @brief The other half of that proof.
 *
 * A custom close_fn *replaces* httpd's default, and the default is the only
 * thing that closes the socket (see httpd_sess_delete). Leaving out the close()
 * below would leak a descriptor per connection and wedge the server after
 * max_open_sockets. The descriptor may already be invalid here — this runs for
 * sessions the network stack tore down too — which close() reports and we
 * ignore, because there is nothing else to do about it.
 */
void HttpSessionClose(httpd_handle_t hd, int sockfd) {
    (void)hd;
    ESP_LOGI(kTag, "http: close fd=%d", sockfd);
    close(sockfd);
}

/**
 * @brief One line per answered request.
 *
 * Deliberately narrow about what it says. The method, the path and the status
 * are what a field report needs; the authentication header is reported as
 * *present or not* and never by value. No body, no token, no hub URL, no
 * Wi-Fi identifier is logged anywhere in this file.
 */
void LogHttpRequest(httpd_req_t* req, const char* status) {
    if (req == nullptr) return;
    const bool auth_hdr = httpd_req_get_hdr_value_len(req, "X-Auth-Token") > 0;
    ESP_LOGI(kTag, "http: %s %s -> %s (auth_hdr=%d)",
             http_method_str(static_cast<enum http_method>(req->method)),
             req->uri, status, auth_hdr ? 1 : 0);
}

cJSON* ReadJsonBody(httpd_req_t* req) {
    if (!req || req->content_len == 0 || req->content_len > 2048) return nullptr;
    char* buf = static_cast<char*>(calloc(1, req->content_len + 1));
    if (!buf) return nullptr;
    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret <= 0) {
            free(buf);
            return nullptr;
        }
        received += static_cast<size_t>(ret);
    }
    cJSON* root = cJSON_Parse(buf);
    free(buf);
    return root;
}

void CloseCurrentSession(httpd_req_t* req) {
    if (!req || !req->handle) return;
    const int sockfd = httpd_req_to_sockfd(req);
    if (sockfd < 0) return;
    esp_err_t err = httpd_sess_trigger_close(req->handle, sockfd);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(kTag, "httpd_sess_trigger_close(%d) failed: %s",
                 sockfd, esp_err_to_name(err));
    }
}

void SendJson(httpd_req_t* req, const char* json) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    // These legacy responders never set a status, so httpd sends 200.
    LogHttpRequest(req, "200 OK");
    CloseCurrentSession(req);
}

void CopyJsonString(cJSON* root, const char* key, char* out, size_t out_size) {
    if (!root || !key || !out || out_size == 0) return;
    cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring) {
        strlcpy(out, item->valuestring, out_size);
    }
}

struct DeferredControlRequest {
    ApTransferServer* server = nullptr;
    bool stop_wifi = false;
    bool enter_sleep = false;
};

/**
 * @brief Hand "sleep now" to the one owner of what sleep means.
 *
 * @return true when the request was accepted and this task must not sleep the
 *         device itself.
 *
 * This path used to call esp_deep_sleep_start() having armed ext0 and nothing
 * else, so a device put to sleep from the legacy /settings page never woke on
 * its own again: the hourly refresh the product promises simply stopped until
 * somebody found the device and pressed BOOT. The v2 route has always been
 * right — POST /api/v1/actions/sleep restores the base mode and arms both
 * sources — so the fix is to go through the same door rather than to grow a
 * second, differently-configured one here. There is deliberately no fallback
 * interval invented in this file.
 *
 * The idempotency key is unique per request: this is a fresh deliberate action
 * every time, not a retry of a network call, and it must never be answered out
 * of the replay ring.
 */
bool DelegateSleepToDeviceAction() {
    auto& cfg = devcfg::DeviceConfigService::GetInstance();
    char idem_key[40];
    snprintf(idem_key, sizeof(idem_key), "legacy-web-sleep-%llu",
             static_cast<unsigned long long>(esp_timer_get_time()));
    bool replay = false;
    const devcfg::ActionError err = cfg.RequestAction(
        devcfg::Action::kSleep,
        devcfg::ActionConfirmation(devcfg::Action::kSleep), idem_key, &replay);
    if (err != devcfg::ActionError::kNone) {
        ESP_LOGW(kTag, "Legacy web sleep could not be delegated: %s",
                 devcfg::ActionErrorName(err));
        return false;
    }
    ESP_LOGI(kTag, "Legacy web sleep delegated to the device sleep action");
    return true;
}

void DeferredControlTask(void* arg) {
    auto* request = static_cast<DeferredControlRequest*>(arg);
    if (!request) {
        vTaskDelete(nullptr);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(300));
    if (request->server) {
        request->server->Stop();
    }

    // Delegate before stopping the radio. The action the config service arms
    // does the whole sequence itself — close the local services, restore the
    // base power mode, stop Wi-Fi, arm the timer *and* the button — and doing
    // half of it here first would only make its log confusing.
    if (request->enter_sleep && DelegateSleepToDeviceAction()) {
        delete request;
        vTaskDelete(nullptr);
        return;
    }

    if (request->stop_wifi || request->enter_sleep) {
        ESP_LOGI(kTag, "Stopping WiFi after web control request");
        esp_wifi_disconnect();
        esp_wifi_stop();
    }
    if (request->enter_sleep) {
        // Only reachable when the config service has no runner wired up, which
        // on a booted device means something is badly wrong. Sleeping on the
        // button alone is still better than staying awake on a flat battery,
        // and it is said out loud rather than logged as a normal sleep.
        ESP_LOGE(kTag, "Entering deep sleep with the button as the only wake "
                       "source; the device will not refresh on its own");
        esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(BOOT_BUTTON_GPIO), 0);
        esp_deep_sleep_start();
    }
    delete request;
    vTaskDelete(nullptr);
}

void ScheduleDeferredControl(ApTransferServer* server, bool stop_wifi, bool enter_sleep) {
    auto* request = new (std::nothrow) DeferredControlRequest{};
    if (!request) {
        ESP_LOGE(kTag, "Failed to allocate deferred control request");
        return;
    }
    request->server = server;
    request->stop_wifi = stop_wifi;
    request->enter_sleep = enter_sleep;
    BaseType_t ok = xTaskCreate(&DeferredControlTask,
                                "ap_web_control",
                                4096,
                                request,
                                4,
                                nullptr);
    if (ok != pdPASS) {
        ESP_LOGE(kTag, "Failed to create deferred control task");
        delete request;
    }
}

}  // namespace

ApTransferServer::ApTransferServer() {
    ESP_LOGI(kTag, "ApTransferServer created");
}

ApTransferServer::~ApTransferServer() {
    Stop();
    ESP_LOGI(kTag, "ApTransferServer destroyed");
}

void ApTransferServer::Start() {
    if (running_ || starting_) {
        ESP_LOGW(kTag, "Server already running");
        return;
    }

    ESP_LOGI(kTag, "Starting AP Transfer Server async");
    mode_ = TransferMode::kAp;
    starting_ = true;
    BaseType_t ok = xTaskCreate(&ApTransferServer::StartTask,
                                "ap_transfer_start",
                                16384,
                                this,
                                5,
                                &start_task_);
    if (ok != pdPASS) {
        starting_ = false;
        start_task_ = nullptr;
        mode_ = TransferMode::kNone;
        ESP_LOGE(kTag, "Failed to create AP start task");
        NotifyState(kError, "Start task failed");
    }
}

bool ApTransferServer::StartLan(const std::string& ip_address) {
    if (running_ || starting_) {
        ESP_LOGW(kTag, "Server already running");
        return true;
    }
    if (ip_address.empty()) {
        ESP_LOGW(kTag, "LAN HTTP server start skipped: empty IP address");
        NotifyState(kError, "No WiFi IP");
        return false;
    }

    mode_ = TransferMode::kLan;
    ap_ip_ = ip_address;
    ESP_LOGI(kTag, "Starting LAN HTTP server at http://%s/", ap_ip_.c_str());
    // Before the listener exists, and for the same reason StartAccessPoint()
    // has always done it: a station in modem sleep is a station whose inbound
    // SYNs the access point buffers and often fails to deliver in time. This
    // path not doing what the AP path did is what made the LAN API
    // unreachable on a device that had associated and taken a lease.
    ApplyServingPowerSave(true);
    if (!StartHttpServer()) {
        running_ = false;
        mode_ = TransferMode::kNone;
        // Nothing is serving, and Stop() will not run for a mode that was never
        // entered, so hand the radio back here rather than leaving it listening
        // for a server that does not exist.
        ApplyServingPowerSave(false);
        NotifyState(kError, "HTTP start failed");
        return false;
    }
    running_ = true;
    NotifyState(kApStarted, ap_ip_);
    return true;
}

void ApTransferServer::StartTask(void* arg) {
    auto* self = static_cast<ApTransferServer*>(arg);
    if (!self) {
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(kTag, "AP start task running, stack watermark=%u",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    vTaskDelay(pdMS_TO_TICKS(100));

    if (!self->starting_) {
        ESP_LOGI(kTag, "AP start task cancelled before WiFi init");
        self->start_task_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    if (!self->StartAccessPoint()) {
        self->running_ = false;
        self->starting_ = false;
        self->start_task_ = nullptr;
        self->mode_ = TransferMode::kNone;
        self->NotifyState(kError, "AP start failed");
        vTaskDelete(nullptr);
        return;
    }
    if (!self->starting_) {
        ESP_LOGI(kTag, "AP start task cancelled after WiFi init");
        self->start_task_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    if (!self->StartHttpServer()) {
        self->running_ = false;
        self->starting_ = false;
        self->start_task_ = nullptr;
        self->mode_ = TransferMode::kNone;
        self->NotifyState(kError, "HTTP start failed");
        vTaskDelete(nullptr);
        return;
    }

    self->running_ = true;
    self->starting_ = false;
    self->start_task_ = nullptr;
    self->NotifyState(kApStarted, self->GetApIp());
    ESP_LOGI(kTag, "AP start task done, stack watermark=%u",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    vTaskDelete(nullptr);
}

void ApTransferServer::Stop() {
    if (!running_ && !starting_ && server_ == nullptr && ap_netif_ == nullptr) return;

    ESP_LOGI(kTag, "Stopping AP Transfer Server");
    const TransferMode old_mode = mode_;
    starting_ = false;
    start_task_ = nullptr;

    if (server_) {
        httpd_stop(server_);
        server_ = nullptr;
    }

    if (old_mode == TransferMode::kLan) {
        // A deliberate stop, said plainly: the discrimination matrix in the
        // lot-3 notes needs "the server was switched off" to be distinguishable
        // from "the server was never reachable".
        ESP_LOGI(kTag, "LAN HTTP server stopped");
        // Nothing is serving any more, so the radio goes back to the driver
        // default. The device may stay awake for a fifteen-minute interactive
        // window after this, and there is no reason to spend it listening.
        ApplyServingPowerSave(false);
    }

    if (old_mode == TransferMode::kAp) {
        ESP_LOGI(kTag, "Returning WiFi to STA mode");
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "esp_wifi_set_mode(STA) failed: %s", esp_err_to_name(err));
        }
        err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            ESP_LOGW(kTag, "esp_wifi_connect failed after AP stop: %s", esp_err_to_name(err));
        }

        WifiManager::GetInstance().ResumeStationAfterExternalAp();
    }

    if (ap_netif_) {
        esp_netif_destroy_default_wifi(ap_netif_);
        ap_netif_ = nullptr;
    }

    running_ = false;
    mode_ = TransferMode::kNone;
    NotifyState(kStopped, "Server stopped");

}

bool ApTransferServer::StartAccessPoint() {
    WifiManager::GetInstance().SuspendStationForExternalAp();

    // AP mode is entered from several states: normal STA, user-disabled WiFi,
    // and after long idle/sleep. Stop any stale WiFi activity first so the AP
    // beacon is backed by a fresh driver state instead of a half-suspended STA.
    esp_err_t err = esp_wifi_scan_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_STATE) {
        ESP_LOGW(kTag, "esp_wifi_scan_stop before AP failed: %s", esp_err_to_name(err));
    }
    err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(kTag, "esp_wifi_disconnect before AP failed: %s", esp_err_to_name(err));
    }
    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(kTag, "esp_wifi_stop before AP failed: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(120));

    if (ap_netif_) {
        esp_netif_destroy_default_wifi(ap_netif_);
        ap_netif_ = nullptr;
    }

    // Create AP netif
    if (!ap_netif_) {
        ap_netif_ = esp_netif_create_default_wifi_ap();
    }
    if (!ap_netif_) {
        ESP_LOGE(kTag, "Failed to create AP netif");
        return false;
    }

    // Configure IP: 192.168.4.1
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

    err = esp_netif_dhcps_stop(ap_netif_);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "esp_netif_dhcps_stop failed: %s", esp_err_to_name(err));
    }
    err = esp_netif_set_ip_info(ap_netif_, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "esp_netif_set_ip_info failed: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_netif_dhcps_start(ap_netif_);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "esp_netif_dhcps_start failed: %s", esp_err_to_name(err));
        return false;
    }

    // WiFi AP config
    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.ap.ssid, kApSsid);
    wifi_config.ap.ssid_len = strlen(kApSsid);
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.channel = 1;
    strcpy((char*)wifi_config.ap.password, kApPassword);
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_LOGI(kTag, "Setting WiFi AP mode");
    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "esp_wifi_set_mode(AP) failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(kTag, "Setting WiFi AP config");
    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "esp_wifi_set_config(AP) failed: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "esp_wifi_set_ps(NONE) failed: %s", esp_err_to_name(err));
    }
    esp_err_t start_err = esp_wifi_start();
    if (start_err != ESP_OK) {
        ESP_LOGE(kTag, "esp_wifi_start failed: %s", esp_err_to_name(start_err));
        return false;
    }

    // Keep the AP IP fixed and log the same address that the screen renders.
    // This avoids confusing users with any transient netif readback while Wi-Fi
    // mode is switching.
    ap_ip_ = kApIp;

    ESP_LOGI(kTag, "AP started: SSID=%s, IP=%s", kApSsid, ap_ip_.c_str());
    return true;
}

bool ApTransferServer::StartHttpServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // 11 legacy routes + 6 dashboard API v1 routes + 4 config API v2 routes,
    // with room to spare. httpd refuses a registration past this limit and the
    // v2 routes register last, so a budget that is too small would take the
    // config API out silently rather than loudly.
    config.max_uri_handlers = 26;
    // The default is 4096. The config API v2 handlers build their responses in
    // stack buffers rather than on the heap, because a route that answers with
    // the device's own state should not be able to fail on an allocation: the
    // PATCH handler holds a 1 KB request body and a 1 KB response at the same
    // time, and the voice hub route already held about the same. 6 KB leaves
    // room for that plus the TLS-free httpd frames above it, and costs 2 KB of
    // internal RAM once, for one task.
    config.stack_size = 6144;
    config.max_open_sockets = 4;
    config.recv_wait_timeout = 30;  // Large images take time
    config.send_wait_timeout = 10;
    config.lru_purge_enable = true;
    // Accept/close tracing. `httpd_start` succeeding only proves a bind and a
    // listen on the lwIP side; these two are what say whether a client ever got
    // through to them. HttpSessionClose owns the close() that the default
    // handler would otherwise do — see its comment.
    config.open_fn = HttpSessionOpen;
    config.close_fn = HttpSessionClose;

    esp_err_t err = httpd_start(&server_, &config);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to start HTTP server: %s", esp_err_to_name(err));
        server_ = nullptr;
        return false;
    }

    // Register handlers.
    //
    // A registration failure used to `return false` on the spot, leaving
    // server_ non-null and the listener accepting connections while
    // IsRunning() — which is `server_ != nullptr` — and everything downstream
    // of it reported that no server was running. The device then answered some
    // routes and 404'd the rest, and the config route said there was nothing to
    // answer with. Every failure below tears the server down; see AbandonStart.
    const httpd_uri_t routes[] = {
        {.uri = "/",            .method = HTTP_GET,    .handler = IndexHandler,     .user_ctx = this},
        {.uri = "/upload",      .method = HTTP_POST,   .handler = UploadHandler,    .user_ctx = this},
        {.uri = "/status",      .method = HTTP_GET,    .handler = StatusHandler,    .user_ctx = this},
        {.uri = "/settings",    .method = HTTP_GET,    .handler = SettingsHandler,  .user_ctx = this},
        {.uri = "/settings",    .method = HTTP_POST,   .handler = SettingsHandler,  .user_ctx = this},
        {.uri = "/photos",      .method = HTTP_GET,    .handler = PhotosHandler,    .user_ctx = this},
        {.uri = "/photo",       .method = HTTP_GET,    .handler = PhotoHandler,     .user_ctx = this},
        {.uri = "/photo",       .method = HTTP_DELETE, .handler = PhotoHandler,     .user_ctx = this},
        {.uri = "/photo/meta",  .method = HTTP_POST,   .handler = PhotoMetaHandler, .user_ctx = this},
        {.uri = "/photos/move", .method = HTTP_POST,   .handler = PhotoMoveHandler, .user_ctx = this},
        {.uri = "/photo/show",  .method = HTTP_POST,   .handler = PhotoShowHandler, .user_ctx = this},
    };

    // Stop and forget a server that is only half registered. A partly wired
    // listener is worse than none: it is unreachable-looking to the tower and
    // reachable to a browser at the same time.
    const auto AbandonStart = [this](const char* what) {
        ESP_LOGE(kTag, "Failed to register %s; stopping the HTTP server", what);
        httpd_stop(server_);
        server_ = nullptr;
        return false;
    };

    for (const httpd_uri_t& route : routes) {
        if (httpd_register_uri_handler(server_, &route) != ESP_OK) {
            return AbandonStart(route.uri);
        }
    }

    // Dashboard API v1 and config API v2. These share this server rather than
    // opening a second listening socket, and are the only authenticated routes
    // here. They register last, so a route budget that was too small would take
    // them out — which is why max_uri_handlers above has room to spare and why
    // this failure is loud.
    if (RegisterDashboardApi(server_) != ESP_OK) {
        return AbandonStart("the device API routes");
    }

    ESP_LOGI(kTag, "HTTP server started at http://%s/", ap_ip_.c_str());
    return true;
}

esp_err_t ApTransferServer::IndexHandler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t ret = httpd_resp_send(req, kUploadHtml, strlen(kUploadHtml));
    // The root page bypasses SendJson, and it is the first thing a person
    // curls when they are trying to find out whether the device answers.
    LogHttpRequest(req, ret == ESP_OK ? "200 OK" : "send failed");
    CloseCurrentSession(req);
    return ret;
}

esp_err_t ApTransferServer::UploadHandler(httpd_req_t* req) {
    // Dashboard lockdown: this legacy route accepts writes from anyone on the
    // LAN, so it is closed while the device is acting as a dashboard.
    if (LegacyWriteBlocked(req)) return ESP_OK;
    auto* self = static_cast<ApTransferServer*>(req->user_ctx);
    
    self->NotifyState(kReceivingImage, "Receiving image...");

    char query[96] = {};
    char format[16] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "format", format, sizeof(format));
    }
    const bool is_2bpp = strcmp(format, "bwry2bpp") == 0 || strcmp(format, "2bpp") == 0;
    const size_t expected_size = is_2bpp ? kImage2bppSize : kImage1bppSize;

    if (req->content_len != expected_size) {
        ESP_LOGW(kTag, "Invalid upload size: %u", static_cast<unsigned>(req->content_len));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Connection", "close");
        esp_err_t send_ret = httpd_resp_send(req, is_2bpp
            ? "{\"success\":false,\"error\":\"expected 400x300 2bpp four-color data\"}"
            : "{\"success\":false,\"error\":\"expected 400x300 1bpp data\"}",
            HTTPD_RESP_USE_STRLEN);
        CloseCurrentSession(req);
        return send_ret == ESP_OK ? ESP_FAIL : send_ret;
    }

    auto* buf = static_cast<uint8_t*>(malloc(expected_size));
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }

    size_t received = 0;
    while (received < expected_size) {
        int ret = httpd_req_recv(req, reinterpret_cast<char*>(buf + received),
                                 expected_size - received);
        if (ret <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Receive failed");
            return ESP_FAIL;
        }
        received += static_cast<size_t>(ret);
    }

    ESP_LOGI(kTag, "Received %u bytes", static_cast<unsigned>(received));
    self->NotifyState(kProcessingImage, "Processing...");

    PhotoInfo info = {};
    const uint32_t now = static_cast<uint32_t>(time(nullptr));
    const uint64_t ms = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    snprintf(info.id, sizeof(info.id), "ap%011llu",
             static_cast<unsigned long long>(ms % 100000000000ULL));
    snprintf(info.title, sizeof(info.title), is_2bpp ? "Wi-Fi four-color image" : "Wi-Fi black and white image");
    snprintf(info.location, sizeof(info.location), "WiFi AP");
    snprintf(info.body, sizeof(info.body), is_2bpp ? "Phone Wi-Fi transfer - 2 BP four-color" : "Phone Wi-Fi transfer - 1 BP monochrome");
    info.width = kScreenWidth;
    info.height = kScreenHeight;
    info.file_size = expected_size;
    info.timestamp = now > 0 ? now : static_cast<uint32_t>(ms / 1000);

    if (now > 0) {
        time_t t = now;
        struct tm tm_info;
        localtime_r(&t, &tm_info);
        strftime(info.date, sizeof(info.date), "%Y-%m-%d", &tm_info);
    }

    const bool saved = photo_save(&info, buf) == 0;
    free(buf);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    if (saved) {
        self->NotifyState(kImageSaved, info.id);
        if (self->image_received_callback_) {
            self->image_received_callback_(info.id);
        }
        if (self->photos_changed_callback_) {
            self->photos_changed_callback_();
        }
        std::string response = std::string("{\"success\":true,\"id\":\"") + info.id + "\"}";
        esp_err_t send_ret = httpd_resp_send(req, response.c_str(), static_cast<ssize_t>(response.size()));
        if (send_ret != ESP_OK) {
            ESP_LOGW(kTag, "Upload response send failed: %s", esp_err_to_name(send_ret));
            return send_ret;
        }
        CloseCurrentSession(req);
        // Return the device screen to the connection/instructions page after a
        // successful upload. Keeping the renderer in kComplete left the panel
        // showing the saved file id and made the AP flow look stuck before the
        // next upload.
        self->NotifyState(kApStarted, self->GetApIp());
        return ESP_OK;
    }

    self->NotifyState(kError, "Save failed");
    esp_err_t send_ret = httpd_resp_send(req, "{\"success\":false,\"error\":\"save failed\"}", HTTPD_RESP_USE_STRLEN);
    CloseCurrentSession(req);
    return send_ret == ESP_OK ? ESP_FAIL : send_ret;
}

esp_err_t ApTransferServer::StatusHandler(httpd_req_t* req) {
    auto* self = static_cast<ApTransferServer*>(req->user_ctx);
    const char* mode = "ap";
    const char* ip = kApIp;
    if (self != nullptr) {
        mode = self->mode_ == TransferMode::kLan ? "lan" : "ap";
        ip = self->ap_ip_.empty() ? kApIp : self->ap_ip_.c_str();
    }
    char response[128];
    snprintf(response, sizeof(response),
             "{\"status\":\"ready\",\"mode\":\"%s\",\"ip\":\"%s\",\"url\":\"http://%s/\"}",
             mode, ip, ip);
    SendJson(req, response);
    return ESP_OK;
}

esp_err_t ApTransferServer::SettingsHandler(httpd_req_t* req) {
    // Only the write half of this route is gated; reads stay open.
    if (req->method == HTTP_POST && LegacyWriteBlocked(req)) return ESP_OK;
    auto* self = static_cast<ApTransferServer*>(req->user_ctx);
    Settings nvs(kGalleryNamespace, req->method == HTTP_POST);
    int interval = nvs.GetInt(kSlideshowIntervalKey, 5);
    bool close_service = false;
    bool stop_wifi = false;
    bool enter_sleep = false;

    if (req->method == HTTP_POST) {
        cJSON* root = ReadJsonBody(req);
        if (!root) {
            SendJson(req, "{\"success\":false,\"error\":\"bad_json\"}");
            return ESP_FAIL;
        }
        cJSON* item = cJSON_GetObjectItemCaseSensitive(root, "slideshow_interval");
        if (cJSON_IsNumber(item)) {
            interval = item->valueint;
            if (interval != 0 && interval != 5 && interval != 10 && interval != 30) {
                interval = 5;
            }
            nvs.SetInt(kSlideshowIntervalKey, interval);
            if (self && self->settings_changed_callback_) {
                self->settings_changed_callback_(interval);
            }
            ESP_LOGI(kTag, "AP settings updated: slideshow_interval=%d", interval);
        }
        cJSON* service_item = cJSON_GetObjectItemCaseSensitive(root, "service_enabled");
        if (cJSON_IsBool(service_item) && !cJSON_IsTrue(service_item)) {
            close_service = true;
        }
        cJSON* wifi_item = cJSON_GetObjectItemCaseSensitive(root, "wifi_enabled");
        if (cJSON_IsBool(wifi_item) && !cJSON_IsTrue(wifi_item)) {
            stop_wifi = true;
        }
        cJSON* sleep_item = cJSON_GetObjectItemCaseSensitive(root, "sleep");
        if (cJSON_IsBool(sleep_item) && cJSON_IsTrue(sleep_item)) {
            close_service = true;
            stop_wifi = true;
            enter_sleep = true;
        }
        cJSON_Delete(root);
    }

    const char* mode = "ap";
    const char* ip = kApIp;
    if (self != nullptr) {
        mode = self->mode_ == TransferMode::kLan ? "lan" : "ap";
        ip = self->ap_ip_.empty() ? kApIp : self->ap_ip_.c_str();
    }
    char response[256];
    snprintf(response, sizeof(response),
             "{\"success\":true,\"slideshow_interval\":%d,\"service_running\":%s,"
             "\"mode\":\"%s\",\"ip\":\"%s\",\"url\":\"http://%s/\","
             "\"closing\":%s,\"sleep\":%s}",
             interval,
             self && self->IsRunning() ? "true" : "false",
             mode,
             ip,
             ip,
             close_service ? "true" : "false",
             enter_sleep ? "true" : "false");
    SendJson(req, response);
    if (close_service || stop_wifi || enter_sleep) {
        ESP_LOGI(kTag, "Web control requested: close_service=%d stop_wifi=%d sleep=%d",
                 close_service ? 1 : 0,
                 stop_wifi ? 1 : 0,
                 enter_sleep ? 1 : 0);
        ScheduleDeferredControl(self, stop_wifi, enter_sleep);
    }
    return ESP_OK;
}

esp_err_t ApTransferServer::PhotosHandler(httpd_req_t* req) {
    cJSON* root = cJSON_CreateObject();
    cJSON* photos = cJSON_CreateArray();
    if (!root || !photos) {
        if (root) cJSON_Delete(root);
        if (photos) cJSON_Delete(photos);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }
    cJSON_AddItemToObject(root, "photos", photos);

    const int count = photo_get_count();
    for (int i = 0; i < count && i < PHOTO_MAX_PHOTOS; ++i) {
        PhotoInfo info = {};
        if (photo_get_by_index(i, &info) != 0) continue;
        cJSON* item = cJSON_CreateObject();
        if (!item) continue;
        cJSON_AddStringToObject(item, "id", info.id);
        cJSON_AddStringToObject(item, "title", info.title);
        cJSON_AddStringToObject(item, "date", info.date);
        cJSON_AddStringToObject(item, "location", info.location);
        cJSON_AddStringToObject(item, "body", info.body);
        cJSON_AddNumberToObject(item, "width", info.width);
        cJSON_AddNumberToObject(item, "height", info.height);
        cJSON_AddNumberToObject(item, "size", info.file_size);
        cJSON_AddStringToObject(item, "format", info.file_size > kImage1bppSize ? "bwry2bpp" : "1bpp");
        cJSON_AddItemToArray(photos, item);
    }

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t ret = httpd_resp_sendstr(req, json);
    CloseCurrentSession(req);
    cJSON_free(json);
    return ret;
}

esp_err_t ApTransferServer::PhotoHandler(httpd_req_t* req) {
    // Only the write half of this route is gated; reads stay open.
    if (req->method == HTTP_DELETE && LegacyWriteBlocked(req)) return ESP_OK;
    char query[64] = {};
    char id[16] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "id", id, sizeof(id)) != ESP_OK ||
        id[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id");
        return ESP_FAIL;
    }

    if (req->method == HTTP_DELETE) {
        const bool deleted = photo_delete(id) == 0;
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Connection", "close");
        esp_err_t ret = httpd_resp_send(req, deleted ? "{\"success\":true}" : "{\"success\":false}",
                                        HTTPD_RESP_USE_STRLEN);
        CloseCurrentSession(req);
        if (ret != ESP_OK) return ret;
        return deleted ? ESP_OK : ESP_FAIL;
    }

    PhotoInfo info = {};
    bool found = false;
    const int count = photo_get_count();
    for (int i = 0; i < count && i < PHOTO_MAX_PHOTOS; ++i) {
        if (photo_get_by_index(i, &info) == 0 && strcmp(info.id, id) == 0) {
            found = true;
            break;
        }
    }
    if (!found || info.file_size == 0 || info.file_size > kImage2bppSize) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }

    auto* buf = static_cast<uint8_t*>(malloc(info.file_size));
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }
    const int bytes = photo_load(id, buf, info.file_size);
    if (bytes <= 0) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t ret = httpd_resp_send(req, reinterpret_cast<const char*>(buf), bytes);
    CloseCurrentSession(req);
    free(buf);
    return ret;
}

esp_err_t ApTransferServer::PhotoMetaHandler(httpd_req_t* req) {
    // Dashboard lockdown: this legacy route accepts writes from anyone on the
    // LAN, so it is closed while the device is acting as a dashboard.
    if (LegacyWriteBlocked(req)) return ESP_OK;
    cJSON* root = ReadJsonBody(req);
    if (!root) {
        SendJson(req, "{\"success\":false,\"error\":\"bad_json\"}");
        return ESP_FAIL;
    }

    char id[16] = {};
    CopyJsonString(root, "id", id, sizeof(id));
    PhotoInfo info = {};
    bool found = false;
    const int count = photo_get_count();
    for (int i = 0; i < count && i < PHOTO_MAX_PHOTOS; ++i) {
        if (photo_get_by_index(i, &info) == 0 && strcmp(info.id, id) == 0) {
            found = true;
            break;
        }
    }
    if (!found) {
        cJSON_Delete(root);
        SendJson(req, "{\"success\":false,\"error\":\"not_found\"}");
        return ESP_FAIL;
    }

    CopyJsonString(root, "title", info.title, sizeof(info.title));
    CopyJsonString(root, "date", info.date, sizeof(info.date));
    CopyJsonString(root, "location", info.location, sizeof(info.location));
    CopyJsonString(root, "body", info.body, sizeof(info.body));
    cJSON_Delete(root);

    auto* self = static_cast<ApTransferServer*>(req->user_ctx);
    const bool ok = photo_update_info(id, &info) == 0;
    if (ok && self && self->photos_changed_callback_) {
        self->photos_changed_callback_();
    }
    SendJson(req, ok ? "{\"success\":true}" : "{\"success\":false}");
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t ApTransferServer::PhotoMoveHandler(httpd_req_t* req) {
    // Dashboard lockdown: this legacy route accepts writes from anyone on the
    // LAN, so it is closed while the device is acting as a dashboard.
    if (LegacyWriteBlocked(req)) return ESP_OK;
    cJSON* root = ReadJsonBody(req);
    if (!root) {
        SendJson(req, "{\"success\":false,\"error\":\"bad_json\"}");
        return ESP_FAIL;
    }
    char id[16] = {};
    CopyJsonString(root, "id", id, sizeof(id));
    cJSON* delta_item = cJSON_GetObjectItemCaseSensitive(root, "delta");
    const int delta = cJSON_IsNumber(delta_item) ? delta_item->valueint : 0;
    cJSON_Delete(root);

    auto* self = static_cast<ApTransferServer*>(req->user_ctx);
    const bool ok = photo_move(id, delta) == 0;
    if (ok && self && self->photos_changed_callback_) {
        self->photos_changed_callback_();
    }
    SendJson(req, ok ? "{\"success\":true}" : "{\"success\":false}");
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t ApTransferServer::PhotoShowHandler(httpd_req_t* req) {
    // Dashboard lockdown: this legacy route accepts writes from anyone on the
    // LAN, so it is closed while the device is acting as a dashboard.
    if (LegacyWriteBlocked(req)) return ESP_OK;
    cJSON* root = ReadJsonBody(req);
    if (!root) {
        SendJson(req, "{\"success\":false,\"error\":\"bad_json\"}");
        return ESP_FAIL;
    }
    char id[16] = {};
    CopyJsonString(root, "id", id, sizeof(id));
    cJSON_Delete(root);

    auto* self = static_cast<ApTransferServer*>(req->user_ctx);
    const bool ok = self && self->show_photo_callback_ && self->show_photo_callback_(id);
    SendJson(req, ok ? "{\"success\":true}" : "{\"success\":false,\"error\":\"not_found\"}");
    return ok ? ESP_OK : ESP_FAIL;
}

void ApTransferServer::NotifyState(ServerState state, const std::string& message) {
    ESP_LOGI(kTag, "State: %d, message: %s", state, message.c_str());
    if (state_callback_) {
        state_callback_(state, message);
    }
}

void ApTransferServer::SetStateCallback(std::function<void(ServerState, const std::string&)> callback) {
    state_callback_ = callback;
}

void ApTransferServer::SetImageReceivedCallback(std::function<void(const char* photo_id)> callback) {
    image_received_callback_ = callback;
}

void ApTransferServer::SetSettingsChangedCallback(std::function<void(int)> callback) {
    settings_changed_callback_ = callback;
}

void ApTransferServer::SetPhotosChangedCallback(std::function<void()> callback) {
    photos_changed_callback_ = callback;
}

void ApTransferServer::SetShowPhotoCallback(std::function<bool(const std::string&)> callback) {
    show_photo_callback_ = std::move(callback);
}

}  // namespace rawdraw
