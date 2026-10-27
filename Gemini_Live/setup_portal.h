#pragma once
// Phone-based onboarding. The device opens its own WPA2 hotspot and a captive portal;
// the e-paper shows QR #1 (join the hotspot) and, once a phone has joined, QR #2 (setup URL).
// The page saves Wi-Fi + Gemini API key to NVS (same keys as the serial `configure`
// command) and the device restarts into normal mode.
// Included from Gemini_Live.ino after the screen, Wi-Fi and session globals are declared.
#include <WebServer.h>
#include <DNSServer.h>
#include <algorithm>

constexpr uint32_t SETUP_HOLD_MS=10000;       // BOOT hold that enters setup while running.
constexpr uint32_t SETUP_IDLE_MS=10*60*1000;  // Configured devices leave an unused setup.
const char *SETUP_URL="http://192.168.4.1/";

String apSsid,apPass,scanJson="[]";
WebServer *portal=nullptr;
DNSServer *captiveDns=nullptr;
uint8_t setupStep=0;
bool setupScanning=false;
uint32_t setupActivity=0,setupRestartAt=0,setupStepCheck=0;

// Self-contained page: the phone has no internet while on the hotspot, so no web fonts or CDNs.
static const char SETUP_PAGE[] PROGMEM = R"PAGE(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="color-scheme" content="dark"><meta name="theme-color" content="#0b0d12">
<title>Gemini device setup</title>
<meta name="description" content="Connect your Gemini device to Wi-Fi and add your Gemini API key.">
<style>
:root{--bg:#0b0d12;--card:rgba(22,26,36,.82);--line:#262c3a;--text:#e8eaf0;--mute:#9aa3b5;--accent:#8ab4f8;--err:#f28b82;--ok:#81c995;
--grad:linear-gradient(115deg,#4285f4 0%,#9b72cb 50%,#d96570 100%)}
*{box-sizing:border-box}
html,body{margin:0}
body{min-height:100vh;background:radial-gradient(120% 55% at 50% -12%,#232a52 0,#141833 35%,var(--bg) 70%) fixed;color:var(--text);
font:16px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;-webkit-font-smoothing:antialiased}
main{max-width:440px;margin:0 auto;padding:30px 18px calc(36px + env(safe-area-inset-bottom))}
header{text-align:center;margin-bottom:22px}
.spark{width:52px;height:52px;filter:drop-shadow(0 4px 18px rgba(155,114,203,.55));animation:pulse 3.2s ease-in-out infinite}
@keyframes pulse{50%{transform:scale(1.08) rotate(8deg)}}
h1{font-size:27px;letter-spacing:-.3px;margin:10px 0 4px;background:var(--grad);-webkit-background-clip:text;background-clip:text;color:transparent}
header p{margin:0;color:var(--mute);font-size:14px}
.card{background:var(--card);border:1px solid var(--line);border-radius:20px;padding:18px;margin-bottom:14px;
-webkit-backdrop-filter:blur(12px);backdrop-filter:blur(12px);animation:rise .45s ease both}
.card:nth-child(2){animation-delay:.06s}.card:nth-child(3){animation-delay:.12s}
@keyframes rise{from{opacity:0;transform:translateY(10px)}}
.step{display:flex;align-items:center;gap:10px;font-weight:600;margin-bottom:12px}
.step span:nth-child(2){flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.num{flex:none;width:24px;height:24px;border-radius:50%;background:var(--grad);display:grid;place-items:center;font-size:13px;color:#fff}
ul{list-style:none;margin:0;padding:0;max-height:264px;overflow:auto;border-radius:14px;border:1px solid var(--line);background:#0f121a}
li button{all:unset;box-sizing:border-box;display:flex;align-items:center;gap:10px;width:100%;padding:13px 14px;border-bottom:1px solid var(--line);cursor:pointer;transition:background .15s}
li:last-child button{border-bottom:0}
li button:hover{background:#181d2a}
li button[aria-pressed=true]{background:linear-gradient(90deg,rgba(66,133,244,.2),rgba(155,114,203,.1));box-shadow:inset 3px 0 0 var(--accent)}
li button:focus-visible{outline:2px solid var(--accent);outline-offset:-2px}
.name{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.other .name{color:var(--accent)}
.bars{display:flex;gap:2px;align-items:flex-end;height:14px}
.bars i{width:3px;background:#3a4256;border-radius:1px}.bars i.on{background:var(--accent)}
.lock{width:13px;height:13px;color:var(--mute)}
.empty{padding:14px;color:var(--mute);font-size:14px}
.field{position:relative;margin-top:10px}
input{width:100%;padding:14px 48px 14px 14px;border-radius:14px;border:1px solid var(--line);background:#0f121a;color:var(--text);font-size:16px;outline:none;transition:border-color .15s,box-shadow .15s}
input::placeholder{color:#6b7385}
input:focus{border-color:var(--accent);box-shadow:0 0 0 3px rgba(138,180,248,.18)}
.eye{position:absolute;right:6px;top:50%;transform:translateY(-50%);background:none;border:0;color:var(--mute);padding:8px;cursor:pointer;font-size:13px}
.link{background:none;border:0;color:var(--accent);font:inherit;font-size:14px;cursor:pointer;padding:4px 2px}
.link:disabled{color:var(--mute)}
.hint{color:var(--mute);font-size:13px;margin:10px 2px 0}
a{color:var(--accent);text-decoration:none;font-weight:600}
.cta{width:100%;padding:16px;border:0;border-radius:16px;background:var(--grad);background-size:180% 100%;color:#fff;font-size:17px;font-weight:600;cursor:pointer;
box-shadow:0 8px 24px rgba(110,100,220,.35);transition:background-position .5s,transform .1s,opacity .2s;display:flex;justify-content:center;align-items:center;gap:10px}
.cta:hover{background-position:100% 0}.cta:active{transform:scale(.98)}.cta:disabled{opacity:.65;cursor:default}
.spin{width:16px;height:16px;border:2px solid rgba(255,255,255,.4);border-top-color:#fff;border-radius:50%;animation:r .8s linear infinite}
@keyframes r{to{transform:rotate(360deg)}}
.err{color:var(--err);font-size:14px;min-height:20px;margin:4px 2px 12px}
.done{text-align:center;padding:34px 18px}
.done h2{margin:16px 0 6px;font-size:22px}.done p{color:var(--mute);margin:6px 0}
.check{width:76px;height:76px}
.check circle{stroke:url(#g);stroke-dasharray:226;stroke-dashoffset:226;animation:draw .7s ease forwards}
.check path{stroke:#fff;stroke-dasharray:50;stroke-dashoffset:50;animation:draw .4s .55s ease forwards}
@keyframes draw{to{stroke-dashoffset:0}}
footer{text-align:center;color:#6b7385;font-size:12px;margin-top:18px}
.hidden{display:none!important}
</style></head><body><main>
<header>
<svg class="spark" viewBox="0 0 48 48" aria-hidden="true"><defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#4285f4"/><stop offset=".5" stop-color="#9b72cb"/><stop offset="1" stop-color="#d96570"/></linearGradient></defs>
<path fill="url(#g)" d="M24 2c1.6 11.4 10.6 20.4 22 22-11.4 1.6-20.4 10.6-22 22C22.4 34.6 13.4 25.6 2 24 13.4 22.4 22.4 13.4 24 2z"/></svg>
<h1>Set up your device</h1>
<p id="device">Connect to Wi-Fi and add your Gemini key</p>
</header>
<form id="form" novalidate>
<section class="card">
<div class="step"><span class="num">1</span><span>Choose Wi-Fi</span><button type="button" class="link" id="rescan">Rescan</button></div>
<ul id="nets"><li class="empty">Looking for networks…</li></ul>
<div class="field hidden" id="manual"><input id="ssid" placeholder="Network name" autocomplete="off" autocapitalize="off" spellcheck="false" maxlength="32"></div>
<p class="hint">Use a 2.4 GHz network.</p>
</section>
<section class="card" id="pwCard">
<div class="step"><span class="num">2</span><span id="pwTitle">Wi-Fi password</span></div>
<div class="field"><input id="pw" type="password" placeholder="Password" autocomplete="off" autocapitalize="off" spellcheck="false" maxlength="63"><button type="button" class="eye" data-for="pw">Show</button></div>
</section>
<section class="card">
<div class="step"><span class="num" id="keyNum">3</span><span>Gemini API key</span></div>
<div class="field"><input id="key" type="password" placeholder="Paste your API key" autocomplete="off" autocapitalize="off" spellcheck="false" maxlength="200"><button type="button" class="eye" data-for="key">Show</button></div>
<p class="hint"><a href="https://aistudio.google.com/apikey" target="_blank" rel="noopener">Get a free key in Google AI Studio ↗</a><br>
While your phone is on the device's Wi-Fi this link can't load. Switch to your usual network, copy the key, then come back here.</p>
</section>
<p class="err" id="err" role="alert"></p>
<button class="cta" id="save" type="submit">Save &amp; connect</button>
</form>
<section class="card done hidden" id="done">
<svg class="check" viewBox="0 0 80 80" fill="none" stroke-width="5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="40" cy="40" r="36"/><path d="M25 41l10 10 20-21"/></svg>
<h2>All set!</h2>
<p>Your device is restarting and joining <b id="doneSsid"></b>.</p>
<p>You can reconnect your phone to your usual Wi-Fi now. Press BOOT on the device to start transcribing.</p>
</section>
<footer>Your settings are stored only on the device.</footer>
</main>
<script>
const $=s=>document.querySelector(s);
let nets=[],sel=null,manual=false,info={};
const LOCK='<svg class="lock" viewBox="0 0 24 24" fill="currentColor"><path d="M17 9h-1V7a4 4 0 0 0-8 0v2H7a2 2 0 0 0-2 2v9a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2v-9a2 2 0 0 0-2-2zm-7-2a2 2 0 1 1 4 0v2h-4V7z"/></svg>';
function bars(r){const n=r>-55?4:r>-67?3:r>-78?2:1;return '<span class="bars">'+[5,8,11,14].map((h,i)=>'<i style="height:'+h+'px" class="'+(i<n?'on':'')+'"></i>').join('')+'</span>'}
function item(label,pressed,html,cls,click){const li=document.createElement('li'),b=document.createElement('button');b.type='button';if(cls)li.className=cls;
b.setAttribute('aria-pressed',pressed);b.innerHTML='<span class="name"></span>'+html;b.querySelector('.name').textContent=label;b.onclick=click;li.appendChild(b);return li}
function update(){
const ul=$('#nets');ul.innerHTML='';
nets.forEach(n=>ul.appendChild(item(n.ssid,!manual&&!!sel&&sel.ssid===n.ssid,(n.secure?LOCK:'')+bars(n.rssi),'',()=>{sel=n;manual=false;update();if(n.secure)$('#pw').focus()})));
ul.appendChild(item('Other network…',manual,'','other',()=>{manual=true;sel=null;update();$('#ssid').focus()}));
$('#manual').classList.toggle('hidden',!manual);
const open=!manual&&sel&&!sel.secure;$('#pwCard').classList.toggle('hidden',!!open);$('#keyNum').textContent=open?'2':'3';
$('#pwTitle').textContent=sel?'Password for “'+sel.ssid+'”':'Wi-Fi password';
}
async function getJson(u){const r=await fetch(u,{cache:'no-store'});return r.json()}
async function scan(refresh){const b=$('#rescan');b.disabled=true;b.textContent='Scanning…';
try{let r=await getJson('/scan'+(refresh?'?refresh=1':''));
while(r.scanning){await new Promise(s=>setTimeout(s,1200));r=await getJson('/scan')}
nets=r.networks||[];
if(sel)sel=nets.find(n=>n.ssid===sel.ssid)||sel;
else if(!manual&&info.ssid)sel=nets.find(n=>n.ssid===info.ssid)||null;
update()}catch(e){}
b.disabled=false;b.textContent='Rescan'}
document.querySelectorAll('.eye').forEach(b=>b.onclick=()=>{const i=$('#'+b.dataset.for),s=i.type==='password';i.type=s?'text':'password';b.textContent=s?'Hide':'Show'});
$('#rescan').onclick=()=>scan(true);
$('#form').onsubmit=async e=>{e.preventDefault();const err=$('#err');err.textContent='';
const ssid=manual?$('#ssid').value.trim():(sel?sel.ssid:'');
if(!ssid){err.textContent='Choose your Wi-Fi network.';return}
const pw=$('#pwCard').classList.contains('hidden')?'':$('#pw').value;
if(pw&&(pw.length<8||pw.length>63)){err.textContent='Wi-Fi passwords are 8–63 characters.';return}
const key=$('#key').value.trim();
if(!key&&!info.hasKey){err.textContent='Paste your Gemini API key.';return}
const btn=$('#save');btn.disabled=true;btn.innerHTML='<span class="spin"></span>Saving…';
try{const r=await fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({ssid,password:pw,key}).toString()});
const j=await r.json();if(!j.ok)throw new Error(j.error||'Could not save. Try again.');
$('#doneSsid').textContent=ssid;$('#form').classList.add('hidden');$('#done').classList.remove('hidden');
}catch(x){err.textContent=x instanceof TypeError?'Lost the connection. Make sure your phone is still on the device Wi-Fi.':x.message;
btn.disabled=false;btn.textContent='Save & connect'}};
getJson('/info').then(i=>{info=i;if(i.device)$('#device').textContent='Connected to '+i.device;
if(i.hasKey)$('#key').placeholder='Leave empty to keep current key'}).catch(()=>{}).finally(()=>scan(false));
update();
</script></body></html>)PAGE";

// Characters with meaning in the WIFI: QR format must be backslash-escaped.
String qrEscape(const String &s) {
  String out;
  for(size_t i=0;i<s.length();i++){char c=s[i];if(strchr("\\;,:\"",c))out+='\\';out+=c;}
  return out;
}
// QR screen: caption = step title, body = QR payload, status line = hint under the QR.
void showQr(const char *caption,const String &payload,const String &hint,const char *footer) {
  if(poweringOff)return;
  xSemaphoreTake(screenLock,portMAX_DELAY);
  screenMode=MODE_QR;screenCaption=caption;screenBody=payload;screenFooter=footer;screenFilled=false;
  lastScreen=hint;
  pushScreenLocked();
  xSemaphoreGive(screenLock);
}
void showSetupStep(uint8_t step) {
  setupStep=step;
  const char *footer=configured?"BOOT: cancel setup":"Hold PWR 3s: power off";
  Serial.printf("SETUP_STEP: %u\n",step);
  if(step==1)showQr("Step 1/2: Scan to connect","WIFI:T:WPA;S:"+qrEscape(apSsid)+";P:"+qrEscape(apPass)+";;",apSsid+"  pw "+apPass,footer);
  else showQr("Step 2/2: Open setup page",SETUP_URL,"or visit 192.168.4.1",footer);
}

// Nearby networks for the page: named, deduplicated, strongest first.
void buildScanJson(int n) {
  static int order[64];
  int count=0;
  for(int i=0;i<n&&count<64;i++)if(WiFi.SSID(i).length())order[count++]=i;
  std::sort(order,order+count,[](int a,int b){return WiFi.RSSI(a)>WiFi.RSSI(b);});
  JsonDocument doc;JsonArray list=doc.to<JsonArray>();
  for(int k=0;k<count&&list.size()<25;k++){
    int i=order[k];String name=WiFi.SSID(i);bool seen=false;
    for(JsonObject o: list)if(name==o["ssid"].as<const char*>()){seen=true;break;}
    if(seen)continue;
    JsonObject o=list.add<JsonObject>();
    o["ssid"]=name;o["rssi"]=WiFi.RSSI(i);o["secure"]=WiFi.encryptionType(i)!=WIFI_AUTH_OPEN;
  }
  scanJson="";serializeJson(doc,scanJson);
  WiFi.scanDelete();
  Serial.printf("SETUP_SCAN: %u networks\n",(unsigned)list.size());
}

void redirectToPortal() {
  portal->sendHeader("Location",SETUP_URL,true);
  portal->send(302,"text/plain","");
}
void handleSetupPage() {
  setupActivity=millis();
  portal->sendHeader("Cache-Control","no-store");
  portal->send_P(200,"text/html",SETUP_PAGE);
}
void handleSetupInfo() {
  setupActivity=millis();
  JsonDocument doc;
  doc["device"]=apSsid;doc["configured"]=configured;doc["hasKey"]=apiKey.length()>=20;
  if(configured)doc["ssid"]=ssid; // Network name only; secrets are never sent back.
  String out;serializeJson(doc,out);
  portal->sendHeader("Cache-Control","no-store");
  portal->send(200,"application/json",out);
}
void handleSetupScan() {
  setupActivity=millis();
  if(portal->arg("refresh")=="1"&&!setupScanning&&WiFi.scanComplete()!=WIFI_SCAN_RUNNING){
    setupScanning=WiFi.scanNetworks(true)==WIFI_SCAN_RUNNING;
  }
  portal->sendHeader("Cache-Control","no-store");
  portal->send(200,"application/json",String("{\"scanning\":")+(setupScanning?"true":"false")+",\"networks\":"+scanJson+"}");
}
void setupFail(const char *message) {
  Serial.println("SETUP_INVALID");
  JsonDocument doc;doc["ok"]=false;doc["error"]=message;
  String out;serializeJson(doc,out);
  portal->send(400,"application/json",out);
}
void handleSetupSave() {
  setupActivity=millis();
  if(setupRestartAt){portal->send(200,"application/json","{\"ok\":true}");return;}
  String newSsid=portal->arg("ssid"),newPass=portal->arg("password"),newKey=portal->arg("key");
  newSsid.trim();newKey.trim();
  bool keepKey=newKey.isEmpty()&&apiKey.length()>=20;
  if(newSsid.isEmpty()||newSsid.length()>32){setupFail("Choose a Wi-Fi network (up to 32 characters).");return;}
  if(newPass.length()&&(newPass.length()<8||newPass.length()>63)){setupFail("Wi-Fi passwords are 8-63 characters.");return;}
  if(!keepKey){
    bool valid=newKey.length()>=20&&newKey.length()<=200;
    for(size_t i=0;valid&&i<newKey.length();i++){char c=newKey[i];valid=isalnum((unsigned char)c)||c=='-'||c=='_';}
    if(!valid){setupFail("That doesn't look like a Gemini API key. Copy it again from AI Studio.");return;}
  }
  ssid=newSsid;password=newPass;if(!keepKey)apiKey=newKey;
  prefs.putString("ssid",ssid);prefs.putString("password",password);prefs.putString("key",apiKey);
  selectedHotspot=false;prefs.putBool("use_hotspot",false);
  configured=true;
  Serial.println("SETUP_SAVED");
  portal->send(200,"application/json","{\"ok\":true}");
  status("Saved. Restarting");
  setupRestartAt=millis()+3000; // Let the response reach the phone and the panel redraw.
}

void enterSetupMode() {
  if(setupMode)return;
  Serial.println("SETUP_MODE: start");
  if(wanted){wanted=live=false;ws.disconnect();}
  appState=APP_READY;
  setupMode=true;setupRestartAt=0;setupStep=0;
  status("Starting setup...");
  if(esp_sntp_enabled())esp_sntp_stop();
  netPrimed=false;
  // Stop the station from rejoining saved Wi-Fi: that would move the hotspot's channel.
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false,true);
  WiFi.mode(WIFI_STA);delay(100);
  // Scan before the hotspot exists so the first page load already has the list.
  int n=WiFi.scanNetworks();
  if(n>=0)buildScanJson(n);else scanJson="[]";
  uint8_t mac[6];WiFi.macAddress(mac);
  char name[16];snprintf(name,sizeof(name),"Gemini-%02X%02X",mac[4],mac[5]);apSsid=name;
  const char *alphabet="23456789abcdefghjkmnpqrstuvwxyz"; // No 0/o/1/l/i look-alikes.
  apPass="";for(int i=0;i<8;i++)apPass+=alphabet[esp_random()%strlen(alphabet)];
  WiFi.mode(WIFI_AP_STA);
  if(!WiFi.softAP(apSsid.c_str(),apPass.c_str(),1,0,4)){Serial.println("SETUP_MODE: hotspot failed");status("Setup failed. Restart");return;}
  delay(100);
  if(!captiveDns)captiveDns=new DNSServer();
  captiveDns->setErrorReplyCode(DNSReplyCode::NoError);
  captiveDns->start(53,"*",WiFi.softAPIP());
  if(!portal){
    portal=new WebServer(80);
    portal->on("/",HTTP_GET,handleSetupPage);
    portal->on("/info",HTTP_GET,handleSetupInfo);
    portal->on("/scan",HTTP_GET,handleSetupScan);
    portal->on("/save",HTTP_POST,handleSetupSave);
    // Connectivity probes (generate_204, hotspot-detect.html, connecttest.txt, ...) land
    // here; redirecting them makes phones open the captive-portal sheet.
    portal->onNotFound(redirectToPortal);
  }
  portal->begin();
  setupActivity=setupStepCheck=millis();
  showSetupStep(1);
}
// BOOT in setup mode: back to the saved settings (only when there are some).
void cancelSetupMode() {
  if(!configured||setupRestartAt)return;
  buttonTone(440);
  Serial.println("SETUP_MODE: cancelled");Serial.flush();
  delay(150);ESP.restart();
}
void setupLoop() {
  captiveDns->processNextRequest();
  portal->handleClient();
  if(setupScanning){
    int n=WiFi.scanComplete();
    if(n>=0){buildScanJson(n);setupScanning=false;}
    else if(n==WIFI_SCAN_FAILED)setupScanning=false;
  }
  if(setupRestartAt){
    if((int32_t)(millis()-setupRestartAt)>=0){Serial.println("SETUP_MODE: restart");Serial.flush();ESP.restart();}
    return;
  }
  if(millis()-setupStepCheck>=500){
    setupStepCheck=millis();
    int stations=WiFi.softAPgetStationNum();
    if(stations>0)setupActivity=millis();
    uint8_t step=stations>0?2:1;
    if(step!=setupStep)showSetupStep(step);
  }
  if(configured&&millis()-setupActivity>=SETUP_IDLE_MS){Serial.println("SETUP_MODE: timeout");Serial.flush();ESP.restart();}
}
