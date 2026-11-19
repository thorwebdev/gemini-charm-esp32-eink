#include "board_config.h"
#include <Arduino.h>
#include <HWCDC.h>
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sntp.h"
#include "esp_netif.h"
#include <math.h>
#include <WiFi.h>
#include "src/websockets/WebSocketsClient.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <mbedtls/base64.h>
#if defined(BOARD_M5STICKS3)
#include <M5Unified.h>
#else
#include "src/codec_board/codec_board.h"
#include "src/codec_board/codec_init.h"
#include "epaper_driver_bsp.h"
#endif
#include "google_roots.h"
#include "screen_msg.h"
#include "src/qrcodegen/qrcodegen.h"
#include <U8g2_for_Adafruit_GFX.h>
#include "text_render.h"

// Live transcription: stream mic audio to the transcribe model with SMART formatting.
// Interim hypotheses render while speaking; the cleaned transcript replaces them once
// server-side VAD finalizes the segment.
// The side button then turns the collected transcript into an image, speech, a song or
// a new designed voice (see generate.h).
const char *TRANSCRIBE_MODEL="gemini-3.5-transcribe-live";

Preferences prefs;
WebSocketsClient ws;
volatile uint8_t wifiFailureReason=0;
bool wifiDiagnosticScan=false;
String backupSsid,backupPassword;
bool selectedHotspot=false,bootLongHandled=false,wifiRestartPending=false;
bool netPrimed=false; // DNS primed and SNTP started for the current Wi-Fi connection.
uint32_t bootPressed=0;
// Setup mode (setup_portal.h): device hotspot + captive portal, entered on first boot or a 10 s BOOT hold.
bool setupMode=false;
bool bootSetupHandled=false,bootLongStopped=false;
String ssid, password, apiKey, serialLine, fragments;
#if defined(BOARD_M5STICKS3)
volatile bool wantMicMode=false;
constexpr uint8_t STICK_SPEAKER_VOL=240; // 0..255 (M5.Speaker applies quadratic scaling).
constexpr uint8_t STICK_SPK_MAG=12;      // Midpoint gain: ~3.3x louder than mag=4, clean below 0 dBFS.
constexpr uint8_t STICK_DAC_VOL=0xBF;    // ES8311 reg 0x32: 0xBF = 0 dB (no digital clipping).
uint16_t *imageRgb565=nullptr;
#else
esp_codec_dev_handle_t mic, speaker;
#endif
QueueHandle_t screenQueue, micQueue, toneQueue;
void buttonTone(int frequency) { if(toneQueue)xQueueSend(toneQueue,&frequency,0); }
constexpr float MIC_GAIN_DB=18.0f;
constexpr int PLAY_VOLUME=100;
struct MicFrame { int16_t samples[512]; };
bool audioReady=false,wanted=false,live=false,configured=false;
bool lastButton=HIGH,stableButton=HIGH;
bool lastSide=HIGH,stableSide=HIGH;
// The side button is the board's PWR key: short press = create (on release), long hold = power off.
bool sideLongHandled=false;
uint32_t sidePressed=0;
constexpr uint32_t POWER_OFF_HOLD_MS=3000;
// Set once power-off starts so genTask cannot draw over the "Powered off" screen.
volatile bool poweringOff=false;
uint32_t buttonChanged=0,sideChanged=0,connectStarted=0,sessionStarted=0,wifiAttempt=0;
String lastScreen;
int batteryPercent=-1;
float batteryVoltage=0;
uint32_t lastBatterySample=0;
constexpr uint32_t IDLE_SLEEP_MS=120000;
uint32_t lastActivity=0;
// audioHold parks the audio worker between I2S blocks (light sleep, codec rate change).
volatile bool audioHold=false,audioPaused=false;
SemaphoreHandle_t sleepScreenReady=nullptr;
String interimText,smartText; // Raw UTF-8.
uint8_t transcriptKind=TRANSCRIPT_NONE;
bool smartSegmentOpen=false;
String sessionPrompt; // All smart segments of the current session, newline-joined.
constexpr size_t SESSION_PROMPT_CAP=4000;
volatile uint8_t appState=APP_READY;

// Playback ring (PSRAM, mono, at codecRate). genTask fills it, audioTask drains it.
constexpr size_t RING_SAMPLES=512*1024;
int16_t *audioRing=nullptr;
SemaphoreHandle_t audioMutex=nullptr;
size_t readPos=0,writePos=0;
volatile size_t audioUsed=0;
volatile bool playing=false;
volatile bool audioGate=false; // Holds playback while streamed speech builds its pre-roll.
volatile uint32_t codecRate=16000;

// Screen state shared by the loop and genTask; guarded by screenLock.
SemaphoreHandle_t screenLock=nullptr;
uint8_t screenMode=MODE_HOME;
String screenCaption,screenFooter; // Already converted to CP437.
String screenBody; // Raw UTF-8; PanelText draws it (QR mode: the QR payload).
bool screenFilled=false;
uint8_t imageBitmap[IMAGE_BYTES];
String activeVoiceId,activeVoiceName;

// Generation worker.
TaskHandle_t genHandle=nullptr;
String genPrompt;
volatile bool genCancel=false,genBusy=false;
const char *LISTEN_FOOTER=MAIN_BTN_NAME ": stop | " SIDE_BTN_NAME ": create";
const char *PROMPT_FOOTER=SIDE_BTN_NAME ": create | " MAIN_BTN_NAME ": redo";


void sampleBattery(){
#if defined(BOARD_M5STICKS3)
  int lvl=M5.Power.getBatteryLevel();
  int mv=M5.Power.getBatteryVoltage();
  batteryVoltage=mv>0?mv/1000.0f:0.0f;
  batteryPercent=(lvl>=0&&lvl<=100)?lvl:-1;
  Serial.printf("GEMINI_BATTERY: voltage=%.3f estimate=%d%%\n",batteryVoltage,batteryPercent);
#else
  // Waveshare's ADC example: ADC1 channel 3 (GPIO4), 2:1 divider.
  uint32_t total=0;for(int i=0;i<16;i++)total+=analogReadMilliVolts(4);
  float voltage=total/16.0f*0.002f;
  if(voltage<2.5f||voltage>4.5f){batteryPercent=-1;return;}
  batteryVoltage=batteryVoltage==0?voltage:batteryVoltage*0.75f+voltage*0.25f;
  // Approximate single-cell Li-ion curve; this board has no fuel gauge.
  const float volts[]={3.20f,3.50f,3.60f,3.70f,3.75f,3.80f,3.85f,3.90f,4.00f,4.10f,4.20f};
  const int percent[]={0,5,10,20,30,40,50,60,75,90,100};
  int estimate=100;
  if(batteryVoltage<=volts[0])estimate=0;
  else for(int i=1;i<11;i++)if(batteryVoltage<=volts[i]){estimate=lroundf(percent[i-1]+(batteryVoltage-volts[i-1])*(percent[i]-percent[i-1])/(volts[i]-volts[i-1]));break;}
  batteryPercent=constrain(estimate,0,100);
  Serial.printf("GEMINI_BATTERY: voltage=%.3f estimate=%d%%\n",batteryVoltage,batteryPercent);
#endif
}

bool lastUsbConnected=false;

// Captions and status lines use the built-in CP437 font; see cp437Glyph() in text_render.h.
String toDisplayText(const char *in) {
  String out;const uint8_t *s=(const uint8_t*)in;size_t n=strlen(in);out.reserve(n);
  for(size_t i=0;i<n;){
    uint8_t c=s[i];
    if(c<0x80){out+=(c=='\t'||c=='\r')?' ':(char)c;i++;continue;}
    int extra=(c&0xE0)==0xC0?1:(c&0xF0)==0xE0?2:(c&0xF8)==0xF0?3:0;
    if(!extra||i+extra>=n){i++;continue;}
    uint32_t cp=c&(0x3F>>extra);
    for(int k=1;k<=extra;k++)cp=(cp<<6)|(s[i+k]&0x3F);
    i+=extra+1;out+=cp437Glyph(cp);
  }
  return out;
}

// ---------- Screen state (call with screenLock held) ----------

void pushScreenLocked() {
  if(!screenQueue||lastScreen.isEmpty())return;
  static ScreenMsg msg;memset(&msg,0,sizeof(msg));
  lastScreen.toCharArray(msg.status,sizeof(msg.status));
  msg.mode=screenMode;msg.filledLabel=screenFilled;
  screenCaption.toCharArray(msg.caption,sizeof(msg.caption));
  screenFooter.toCharArray(msg.footer,sizeof(msg.footer));
  size_t start=screenBody.length()>TRANSCRIPT_CAP-1?screenBody.length()-(TRANSCRIPT_CAP-1):0;
  while(start<screenBody.length()&&((uint8_t)screenBody[start]&0xC0)==0x80)start++; // UTF-8 boundary
  strncpy(msg.text,screenBody.c_str()+start,TRANSCRIPT_CAP-1);
  xQueueOverwrite(screenQueue,&msg);
}
// Redraws the current screen (battery/USB changes). Images have no status bar to refresh.
void pushScreen() {
  if(!screenLock)return;
  xSemaphoreTake(screenLock,portMAX_DELAY);
  if(screenMode!=MODE_IMAGE)pushScreenLocked();
  xSemaphoreGive(screenLock);
}
// Body is raw UTF-8. Safe to call from the loop and genTask.
void showScreen(uint8_t mode,const char *caption,const String &body,const char *footer,bool filled) {
  if(poweringOff)return;
  xSemaphoreTake(screenLock,portMAX_DELAY);
  screenMode=mode;screenCaption=toDisplayText(caption);screenBody=body;
  screenFooter=footer;screenFilled=filled;
  pushScreenLocked();
  xSemaphoreGive(screenLock);
}
bool isReadyText(const String &s) {
  return s=="Hotspot ready. BOOT"||s=="Home Wi-Fi ready. BOOT"||s.indexOf("Stopped")>=0;
}
bool isConnectingText(const String &s) {
  return s.indexOf("Connecting hotspot")>=0||s.indexOf("Switching to hotspot")>=0||
         s.indexOf("Connecting home Wi-Fi")>=0||s.indexOf("Switching to home")>=0;
}
// Light sleep and power off both park the panel after drawing.
bool isParkedText(const String &s) { return s=="Light sleep"||s=="Powered off"; }
// Home screen with a status line. An image result stays up (photo frame) through sleep
// and Wi-Fi reconnects; any other status replaces it.
void status(const String &s) {
  if(poweringOff&&s!="Powered off")return;
  xSemaphoreTake(screenLock,portMAX_DELAY);
  bool keepImage=screenMode==MODE_IMAGE&&(isParkedText(s)||isReadyText(s)||isConnectingText(s));
  if(s==lastScreen&&(screenMode==MODE_HOME||keepImage)){xSemaphoreGive(screenLock);return;}
  lastScreen=s;
  Serial.println("STATUS: "+s);
  if(keepImage){
    if(isParkedText(s))pushScreenLocked(); // Screen task only parks the panel.
    xSemaphoreGive(screenLock);return;
  }
  screenMode=MODE_HOME;screenFilled=false;screenBody="";
  screenCaption=activeVoiceName.length()?toDisplayText(("Voice: "+activeVoiceName).c_str()):String();
  bool connected=WiFi.status()==WL_CONNECTED&&!isConnectingText(s);
  screenFooter=s=="Light sleep"?"Click " MAIN_BTN_NAME " to wake":
               s=="Powered off"?"Press PWR to turn on":
               (isReadyText(s)||(connected&&configured&&!wanted))?(MAIN_BTN_NAME ": start transcribing"):"";
  pushScreenLocked();
  xSemaphoreGive(screenLock);
}
void setActiveVoice(const String &id,const String &name) {
  xSemaphoreTake(screenLock,portMAX_DELAY);
  activeVoiceId=id;activeVoiceName=name;
  xSemaphoreGive(screenLock);
  prefs.putString("voice_id",id);prefs.putString("voice_name",name);
}

// Body text renderer (UTF-8: Latin, Chinese, Devanagari). Used only by screenTask.
PanelText panelText;

#if defined(BOARD_M5STICKS3)
// ---------- M5Stack StickS3 135x240 Color LCD Renderer ----------
constexpr uint16_t COL_BG     = 0x0842; // Deep midnight navy (#0B0D14)
constexpr uint16_t COL_FG     = 0xFFFF; // Pure white
constexpr uint16_t COL_MUTE   = 0x9CF3; // Cool gray
constexpr uint16_t COL_BLUE   = 0x653F; // Gemini blue
constexpr uint16_t COL_PURPLE = 0xA45F; // Gemini purple
constexpr uint16_t COL_GREEN  = 0x4EF0; // Emerald
constexpr uint16_t COL_RED    = 0xF32E; // Coral red
constexpr uint16_t COL_AMBER  = 0xFDE0; // Amber

void iconStroke16(GFXcanvas16 &c,int x1,int y1,int x2,int y2,uint16_t col,int width=4) {
  int steps=max(abs(x2-x1),abs(y2-y1));
  for(int i=0;i<=steps;i++)c.fillCircle(x1+(x2-x1)*i/max(steps,1),y1+(y2-y1)*i/max(steps,1),width/2,col);
}
void iconArc16(GFXcanvas16 &c,int x,int y,int radius,int first,int last,uint16_t col,int width=4) {
  for(int a=first;a<=last;a+=2){float t=a*PI/180.0f;c.fillCircle(x+roundf(radius*cosf(t)),y+roundf(radius*sinf(t)),width/2,col);}
}
void iconFace16(GFXcanvas16 &c) {
  const int cx=67,cy=96;
  c.fillCircle(cx,cy,28,COL_BLUE);c.fillCircle(cx,cy,23,COL_BG);
  for(int x: {cx-10,cx+10})iconArc16(c,x,cy-5,4,20,160,COL_FG,3);
  iconArc16(c,cx,cy+7,5,20,160,COL_FG,3);
  for(int r: {36,45,54}){
    uint16_t col=r==54?COL_PURPLE:COL_BLUE;
    iconArc16(c,cx,cy,r,155,205,col,r==54?5:4);
    iconArc16(c,cx,cy,r,335,385,col,r==54?5:4);
  }
}
void iconHome16(GFXcanvas16 &c,bool sleeping=false) {
  const int cx=67,cy=102;
  uint16_t wall=sleeping?COL_MUTE:COL_BLUE;
  uint16_t roof=sleeping?COL_MUTE:COL_PURPLE;
  c.fillRoundRect(cx-26,cy-18,52,42,7,wall);c.fillRoundRect(cx-21,cy-17,42,36,4,COL_BG);
  c.fillTriangle(cx-25,cy-17,cx,cy-43,cx+25,cy-17,COL_BG);
  c.fillRoundRect(cx+16,cy-38,6,18,2,roof);
  iconStroke16(c,cx-31,cy-12,cx,cy-44,roof,6);iconStroke16(c,cx,cy-44,cx+31,cy-12,roof,6);
  for(int x: {cx-10,cx+10})iconArc16(c,x,cy-6,4,20,160,COL_FG,3);
  if(sleeping){
    c.fillCircle(cx,cy+6,3,COL_MUTE);
    c.setTextColor(COL_AMBER);
    c.setTextSize(2);c.setCursor(cx+26,cy-28);c.print("z");
    c.setTextSize(2);c.setCursor(cx+38,cy-42);c.print("Z");
    c.setTextColor(COL_FG);
  }else{
    iconArc16(c,cx,cy+4,4,20,160,COL_FG,3);
    c.fillCircle(cx+31,cy-38,4,COL_RED);c.fillCircle(cx+38,cy-38,4,COL_RED);
    c.fillTriangle(cx+27,cy-38,cx+42,cy-38,cx+34,cy-28,COL_RED);
  }
  iconStroke16(c,cx-42,cy+25,cx+42,cy+25,COL_MUTE,3);
}
void iconSpark16(GFXcanvas16 &c) {
  auto star=[&](int x,int y,int r,uint16_t col){
    int w=max(2,r/4);
    c.fillTriangle(x-w,y,x+w,y,x,y-r,col);c.fillTriangle(x-w,y,x+w,y,x,y+r,col);
    c.fillTriangle(x,y-w,x,y+w,x-r,y,col);c.fillTriangle(x,y-w,x,y+w,x+r,y,col);
  };
  star(64,94,32,COL_BLUE);star(102,62,14,COL_PURPLE);star(34,124,10,COL_FG);
}
void iconWifi16(GFXcanvas16 &c,uint16_t col) {
  for(int r: {6,10,14})iconArc16(c,16,20,r,225,315,col,2);
  c.fillCircle(16,19,2,col);
}
void iconHotspot16(GFXcanvas16 &c,uint16_t col) {
  c.fillRoundRect(9,4,13,20,2,col);c.fillRect(11,7,9,13,COL_BG);
  c.fillCircle(15,21,1,COL_BG);
  c.drawFastVLine(24,10,7,col);c.drawFastVLine(27,8,11,col);
}
void centeredText16(GFXcanvas16 &c,const char *text,int y,int size,uint16_t col=COL_FG) {
  c.setTextColor(col);c.setTextSize(size);
  c.setCursor(max(2,(SCREEN_W-(int)strlen(text)*6*size)/2),y);c.print(text);
  c.setTextColor(COL_FG);
}
void drawQr16(GFXcanvas16 &c,const char *payload,int top,int avail) {
  static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(6)],tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
  if(!qrcodegen_encodeText(payload,tmp,qr,qrcodegen_Ecc_MEDIUM,1,6,qrcodegen_Mask_AUTO,true)){
    centeredText16(c,"QR error",top+avail/2,1,COL_RED);return;
  }
  int box=min(SCREEN_W-8,avail);
  int bx=(SCREEN_W-box)/2,by=top+(avail-box)/2;
  c.fillRoundRect(bx,by,box,box,6,0xFFFF);
  int n=qrcodegen_getSize(qr),scale=max(1,(box-8)/n),px=n*scale;
  int x0=bx+(box-px)/2,y0=by+(box-px)/2;
  for(int y=0;y<n;y++)for(int x=0;x<n;x++)
    if(qrcodegen_getModule(qr,x,y))c.fillRect(x0+x*scale,y0+y*scale,scale,scale,0x0000);
}
void drawPanel16(GFXcanvas16 &c,const char *label,bool filled,const char *text) {
  if(*label){
    int w=min(SCREEN_W-8,(int)strlen(label)*6+10),x=(SCREEN_W-w)/2;
    c.setTextSize(1);
    if(filled){c.fillRoundRect(x,27,w,13,6,COL_BLUE);c.setTextColor(COL_BG);}
    else {c.drawRoundRect(x,27,w,13,6,COL_BLUE);c.setTextColor(COL_BLUE);}
    c.setCursor(x+5,30);c.print(label);c.setTextColor(COL_FG);
  }
  static int16_t starts[TEXT_MAX_LINES];static uint16_t lens[TEXT_MAX_LINES];
  const char *t=text;while(*t==' '||*t=='\n')t++;
  panelText.begin(c,COL_FG);
  // Lines fill y=46..218 (172 px height, 127 px width).
  int ascent=0,descent=0,lineHeight=17,maxLines=8,total=0;
  for(int size=2;size>=1;size--){
    panelText.setSize(size);panelText.metrics(t,ascent,descent);
    lineHeight=max(size>1?17:11,ascent+descent+1);
    maxLines=min(TEXT_MAX_LINES,172/lineHeight);
    total=panelText.wrap(t,SCREEN_W-8,starts,lens,maxLines,true);
    if(total<=maxLines)break;
  }
  for(int i=0;i<min(total,maxLines);i++)panelText.drawLine(4,46+i*lineHeight+ascent,t+starts[i],lens[i]);
}
void bodyLines16(GFXcanvas16 &c,const char *text,int y) {
  int16_t starts[3];uint16_t lens[3];
  int ascent,descent;
  panelText.begin(c,COL_MUTE);panelText.setSize(1);panelText.metrics(text,ascent,descent);
  int lineHeight=max(11,ascent+descent+1),maxLines=lineHeight<=11?3:2;
  int total=panelText.wrap(text,SCREEN_W-8,starts,lens,maxLines,false);
  for(int i=0;i<min(total,maxLines);i++){
    int w=panelText.width(text+starts[i],lens[i]);
    panelText.drawLine(max(4,(SCREEN_W-w)/2),y+i*lineHeight+ascent,text+starts[i],lens[i]);
  }
}
void statusLines16(GFXcanvas16 &c,const char *text,int y) {
  int16_t starts[3];uint16_t lens[3];
  int ascent,descent;
  panelText.begin(c,COL_MUTE);panelText.setSize(1);panelText.metrics(text,ascent,descent);
  int lineHeight=max(11,ascent+descent+1);
  int total=panelText.wrap(text,SCREEN_W-8,starts,lens,3,false);
  for(int i=0;i<min(total,3);i++){
    int w=panelText.width(text+starts[i],lens[i]);
    panelText.drawLine(max(4,(SCREEN_W-w)/2),y+i*lineHeight+ascent,text+starts[i],lens[i]);
  }
}

void screenTask(void*) {
  GFXcanvas16 *canvas=new GFXcanvas16(SCREEN_W,SCREEN_H);
  bool panelAsleep=false;
  static ScreenMsg msg;
  for(;;) if(xQueueReceive(screenQueue,&msg,portMAX_DELAY)) {
    const char *text=msg.status;
    bool poweredOff=strcmp(text,"Powered off")==0;
    bool sleeping=poweredOff||strcmp(text,"Light sleep")==0;
    if(panelAsleep&&!sleeping){
      M5.Display.wakeup();M5.Display.setBrightness(200);panelAsleep=false;
    }
    if(!canvas)continue;
    if(msg.mode==MODE_IMAGE){
      if(sleeping){
        if(!panelAsleep){M5.Display.setBrightness(0);M5.Display.sleep();panelAsleep=true;}
        xSemaphoreGive(sleepScreenReady);continue;
      }
      if(imageRgb565){
        xSemaphoreTake(screenLock,portMAX_DELAY);
        memcpy(canvas->getBuffer(),imageRgb565,(size_t)SCREEN_W*SCREEN_H*sizeof(uint16_t));
        xSemaphoreGive(screenLock);
        M5.Display.pushImage(0,0,SCREEN_W,SCREEN_H,canvas->getBuffer());
      }
      continue;
    }
    canvas->fillScreen(COL_BG);canvas->setTextColor(COL_FG);
    canvas->setTextWrap(false);canvas->cp437(true);
    bool ready=msg.mode==MODE_HOME&&(strcmp(text,"Hotspot ready. BOOT")==0||strcmp(text,"Home Wi-Fi ready. BOOT")==0||strstr(text,"Stopped"));
    bool connectingHotspot=strstr(text,"Connecting hotspot")||strstr(text,"Switching to hotspot");
    bool connectingWifi=strstr(text,"Connecting home Wi-Fi")||strstr(text,"Switching to home");
    bool connected=!sleeping&&WiFi.status()==WL_CONNECTED&&!connectingHotspot&&!connectingWifi;
    if(msg.mode!=MODE_QR){
      uint16_t netCol=connected?COL_GREEN:(connectingHotspot||connectingWifi)?COL_AMBER:COL_RED;
      if(selectedHotspot)iconHotspot16(*canvas,netCol);else iconWifi16(*canvas,netCol);
      if(!connected&&!connectingHotspot&&!connectingWifi)canvas->drawLine(6,22,26,4,COL_RED);
    }
    switch(msg.mode){
      case MODE_LISTEN:
        if(msg.text[0])drawPanel16(*canvas,msg.caption,msg.filledLabel,msg.text);
        else {iconFace16(*canvas);centeredText16(*canvas,"Listening",160,2,COL_BLUE);centeredText16(*canvas,"Speak now...",184,1,COL_MUTE);}
        break;
      case MODE_PROMPT:case MODE_PANEL:
        drawPanel16(*canvas,msg.caption,msg.filledLabel,msg.text);break;
      case MODE_BUSY:
        iconSpark16(*canvas);
        centeredText16(*canvas,msg.caption,146,strlen(msg.caption)*12<=SCREEN_W-6?2:1,COL_PURPLE);
        bodyLines16(*canvas,msg.text,168);
        break;
      case MODE_QR:
        canvas->setTextColor(COL_BLUE);canvas->setTextSize(2);canvas->setCursor(6,6);canvas->print("Setup");
        centeredText16(*canvas,msg.caption,28,1,COL_FG);
        drawQr16(*canvas,msg.text,42,130);
        centeredText16(*canvas,text,180,1,COL_AMBER);
        break;
      default:
        iconHome16(*canvas,sleeping);
        centeredText16(*canvas,poweredOff?"Powered off":sleeping?"Light sleep":"Transcribe",150,2,sleeping?COL_MUTE:COL_FG);
        if(connectingHotspot||connectingWifi)centeredText16(*canvas,connectingHotspot?"Hotspot...":"Connecting...",174,1,COL_AMBER);
        else if(!sleeping&&!ready)statusLines16(*canvas,text,172);
        else if(ready&&msg.caption[0])centeredText16(*canvas,msg.caption,176,1,COL_PURPLE);
        break;
    }
    centeredText16(*canvas,msg.footer,226,1,COL_MUTE);
    // Battery indicator top-right
    const bool usbConnected=lastUsbConnected;
    uint16_t batCol=usbConnected?COL_GREEN:(batteryPercent>=0&&batteryPercent<=20)?COL_RED:COL_FG;
    canvas->drawRect(97,5,32,14,batCol);canvas->fillRect(129,9,2,6,batCol);
    canvas->setTextSize(1);canvas->setTextColor(batCol);
    String batteryLabel=batteryPercent<0?"--":String(batteryPercent)+"%";
    if(usbConnected){
      const int x=109,y=6;
      canvas->fillTriangle(x+4,y,x,y+6,x+4,y+6,COL_GREEN);
      canvas->fillTriangle(x+2,y+4,x+6,y+4,x+2,y+11,COL_GREEN);
      canvas->setCursor(113-batteryLabel.length()*3,21);
      canvas->print(batteryLabel);
    }else{
      canvas->setCursor(113-batteryLabel.length()*3,8);
      canvas->print(batteryLabel);
    }
    M5.Display.pushImage(0,0,SCREEN_W,SCREEN_H,canvas->getBuffer());
    if(sleeping){
      vTaskDelay(pdMS_TO_TICKS(350));
      M5.Display.setBrightness(0);M5.Display.sleep();panelAsleep=true;
      xSemaphoreGive(sleepScreenReady);
    }
  }
}
#else
// Monochrome vector artwork: round strokes stay crisp on the e-paper panel.
void iconStroke(GFXcanvas1 &c,int x1,int y1,int x2,int y2,int width=5) {
  int steps=max(abs(x2-x1),abs(y2-y1));
  for(int i=0;i<=steps;i++)c.fillCircle(x1+(x2-x1)*i/max(steps,1),y1+(y2-y1)*i/max(steps,1),width/2,0);
}
void iconArc(GFXcanvas1 &c,int x,int y,int radius,int first,int last,int width=5) {
  for(int a=first;a<=last;a+=2){float t=a*PI/180.0f;c.fillCircle(x+roundf(radius*cosf(t)),y+roundf(radius*sinf(t)),width/2,0);}
}
void iconFace(GFXcanvas1 &c) {
  c.fillCircle(100,91,40,0);c.fillCircle(100,91,34,1);
  for(int x: {85,115})iconArc(c,x,84,6,20,160,4);
  iconArc(c,100,100,6,20,160,4);
  for(int r: {49,60,72}){
    iconArc(c,100,91,r,155,205,r==72?7:5);
    iconArc(c,100,91,r,335,385,r==72?7:5);
  }
}
void iconHome(GFXcanvas1 &c,bool sleeping=false) {
  c.fillRoundRect(63,76,75,59,10,0);c.fillRoundRect(70,77,61,51,5,1);
  c.fillTriangle(65,77,100,41,136,77,1);
  c.fillRoundRect(123,49,8,26,2,0);
  iconStroke(c,56,83,100,39,9);iconStroke(c,100,39,144,83,9);
  for(int x: {85,115})iconArc(c,x,93,6,20,160,4);
  if(sleeping){
    c.fillCircle(100,108,4,0);
    c.setTextSize(2);c.setCursor(137,61);c.print("z");
    c.setTextSize(3);c.setCursor(155,41);c.print("z");
  }else iconArc(c,100,105,6,20,160,4);
  if(!sleeping){c.fillCircle(143,44,6,0);c.fillCircle(153,44,6,0);c.fillTriangle(137,44,159,44,148,59,0);}
  iconStroke(c,43,136,158,136,4);
  for(int x: {49,151}){
    iconStroke(c,x,136,x-2,113,3);
    c.fillCircle(x-7,114,4,0);c.fillCircle(x+4,122,4,0);c.fillCircle(x-7,127,4,0);
  }
}
// Gemini-style sparkles while a generation is running.
void iconSpark(GFXcanvas1 &c) {
  auto star=[&](int x,int y,int r){
    int w=max(2,r/4);
    c.fillTriangle(x-w,y,x+w,y,x,y-r,0);c.fillTriangle(x-w,y,x+w,y,x,y+r,0);
    c.fillTriangle(x,y-w,x,y+w,x-r,y,0);c.fillTriangle(x,y-w,x,y+w,x+r,y,0);
  };
  star(94,92,40);star(140,56,15);star(60,128,10);
}
void iconWifi(GFXcanvas1 &c) {
  for(int r: {7,12,17})iconArc(c,20,25,r,225,315,3);
  c.fillCircle(20,24,2,0);
}
// Phone silhouette distinguishes hotspot from the usual Wi-Fi symbol.
void iconHotspot(GFXcanvas1 &c) {
  c.fillRoundRect(12,5,15,24,3,0);c.fillRect(14,8,11,16,1);
  c.fillCircle(19,26,1,1);
  c.drawFastVLine(30,12,9,0);c.drawFastVLine(33,9,15,0);
}
void centeredText(GFXcanvas1 &c,const char *text,int y,int size) {
  c.setTextSize(size);c.setCursor(max(4,(200-(int)strlen(text)*6*size)/2),y);c.print(text);
}
// Centered QR code at the largest whole-pixel scale that fits `avail` px of height.
void drawQr(GFXcanvas1 &c,const char *payload,int top,int avail) {
  static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(6)],tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
  if(!qrcodegen_encodeText(payload,tmp,qr,qrcodegen_Ecc_MEDIUM,1,6,qrcodegen_Mask_AUTO,true)){
    centeredText(c,"QR error",top+avail/2,1);return;
  }
  int n=qrcodegen_getSize(qr),scale=max(1,avail/n),px=n*scale,x0=(200-px)/2,y0=top+(avail-px)/2;
  for(int y=0;y<n;y++)for(int x=0;x<n;x++)
    if(qrcodegen_getModule(qr,x,y))c.fillRect(x0+x*scale,y0+y*scale,scale,scale,0);
}
// Caption pill (filled = final result) plus large text while it fits, then small text
// showing the most recent lines.
void drawPanel(GFXcanvas1 &c,const char *label,bool filled,const char *text) {
  if(*label){
    int w=strlen(label)*6+12,x=(200-w)/2;
    c.setTextSize(1);
    if(filled){c.fillRoundRect(x,33,w,13,6,0);c.setTextColor(1);}
    else c.drawRoundRect(x,33,w,13,6,0);
    c.setCursor(x+6,36);c.print(label);c.setTextColor(0);
  }
  static int16_t starts[TEXT_MAX_LINES];static uint16_t lens[TEXT_MAX_LINES];
  const char *t=text;while(*t==' '||*t=='\n')t++;
  panelText.begin(c);
  // Lines fill y=50..190: 8 large or 14 small Latin lines; taller scripts get fewer.
  int ascent=0,descent=0,lineHeight=17,maxLines=8,total=0;
  for(int size=2;size>=1;size--){
    panelText.setSize(size);panelText.metrics(t,ascent,descent);
    lineHeight=max(size>1?17:10,ascent+descent+1);
    maxLines=min(TEXT_MAX_LINES,140/lineHeight);
    total=panelText.wrap(t,192,starts,lens,maxLines,true);
    if(total<=maxLines)break;
  }
  for(int i=0;i<min(total,maxLines);i++)panelText.drawLine(4,50+i*lineHeight+ascent,t+starts[i],lens[i]);
}
// The start of `text` in small type, centered from y: two lines, or one for tall scripts.
void bodyLines(GFXcanvas1 &c,const char *text,int y) {
  int16_t starts[2];uint16_t lens[2];
  int ascent,descent;
  panelText.begin(c);panelText.setSize(1);panelText.metrics(text,ascent,descent);
  int lineHeight=max(11,ascent+descent+1),maxLines=lineHeight<=11?2:1;
  int total=panelText.wrap(text,192,starts,lens,maxLines,false);
  for(int i=0;i<min(total,maxLines);i++){
    int w=panelText.width(text+starts[i],lens[i]);
    panelText.drawLine(max(4,(200-w)/2),y+i*lineHeight+ascent,text+starts[i],lens[i]);
  }
}
// Up to two centered lines of small text.
void statusLines(GFXcanvas1 &c,const char *text,int y) {
  char line[32]={};strncpy(line,text,31);centeredText(c,line,y,1);
  if(strlen(text)>31){strncpy(line,text+31,31);line[31]=0;centeredText(c,line,y+11,1);}
}

void screenTask(void*) {
  pinMode(6,OUTPUT); digitalWrite(6,LOW); delay(100);
  custom_lcd_spi_t cfg={};
  cfg.cs=11;cfg.dc=10;cfg.rst=9;cfg.busy=8;cfg.mosi=13;cfg.scl=12;
  cfg.spi_host=SPI2_HOST;cfg.buffer_len=5000;
  epaper_driver_display display(200,200,cfg);
  display.EPD_Init(); display.EPD_Clear();display.EPD_DisplayPartBaseImage();
  display.EPD_Init_Partial();
  bool panelAsleep=false;
  static ScreenMsg msg;
  for(;;) if(xQueueReceive(screenQueue,&msg,portMAX_DELAY)) {
    const char *text=msg.status;
    bool poweredOff=strcmp(text,"Powered off")==0;
    bool sleeping=poweredOff||strcmp(text,"Light sleep")==0;
    // Photo frame: leave the image on the panel and only power it down.
    if(sleeping&&msg.mode==MODE_IMAGE){
      if(!panelAsleep){display.EPD_Sleep();digitalWrite(6,HIGH);panelAsleep=true;}
      xSemaphoreGive(sleepScreenReady);continue;
    }
    if(panelAsleep){digitalWrite(6,LOW);vTaskDelay(pdMS_TO_TICKS(100));display.EPD_Init_Partial();panelAsleep=false;}
    GFXcanvas1 canvas(200,200);
    canvas.fillScreen(1);canvas.setTextColor(0);
    canvas.setTextWrap(false);canvas.cp437(true);
    if(msg.mode==MODE_IMAGE){
      for(int y=0;y<IMAGE_SIZE;y++)for(int x=0;x<IMAGE_SIZE;x++){
        int i=y*IMAGE_SIZE+x;
        if(imageBitmap[i>>3]&(0x80>>(i&7)))canvas.drawPixel(x,y,0);
      }
    }else{
      bool ready=msg.mode==MODE_HOME&&(strcmp(text,"Hotspot ready. BOOT")==0||strcmp(text,"Home Wi-Fi ready. BOOT")==0||strstr(text,"Stopped"));
      bool connectingHotspot=strstr(text,"Connecting hotspot")||strstr(text,"Switching to hotspot");
      bool connectingWifi=strstr(text,"Connecting home Wi-Fi")||strstr(text,"Switching to home");
      bool connected=!sleeping&&WiFi.status()==WL_CONNECTED&&!connectingHotspot&&!connectingWifi;
      if(msg.mode!=MODE_QR){
        if(selectedHotspot)iconHotspot(canvas);else iconWifi(canvas);
        if(!connected)canvas.drawLine(8,30,32,5,0);
      }
      canvas.setTextSize(1);
      if(msg.mode==MODE_HOME&&(connectingHotspot||connectingWifi)){
        canvas.setCursor(38,12);canvas.print(connectingHotspot?"Connecting hotspot":"Connecting Wi-Fi");
      }
      switch(msg.mode){
        case MODE_LISTEN:
          if(msg.text[0])drawPanel(canvas,msg.caption,msg.filledLabel,msg.text);
          else {iconFace(canvas);centeredText(canvas,"Listening",146,2);centeredText(canvas,"Speak to transcribe",172,1);}
          break;
        case MODE_PROMPT:case MODE_PANEL:
          drawPanel(canvas,msg.caption,msg.filledLabel,msg.text);break;
        case MODE_BUSY:
          iconSpark(canvas);
          centeredText(canvas,msg.caption,146,strlen(msg.caption)*12<=196?2:1);
          bodyLines(canvas,msg.text,168);
          break;
        case MODE_QR:
          // Battery stays top-right; the USB label can reach y=33, so the title starts below it.
          canvas.setTextSize(2);canvas.setCursor(8,8);canvas.print("Setup");
          centeredText(canvas,msg.caption,35,1);
          drawQr(canvas,msg.text,46,128);
          centeredText(canvas,text,177,1);
          break;
        default:
          iconHome(canvas,sleeping);
          centeredText(canvas,poweredOff?"Powered off":sleeping?"Light sleep":"Transcribe",146,2);
          // Keep errors and setup prompts visible; ready/connecting home stays clean.
          if(!sleeping&&!ready&&!connectingHotspot&&!connectingWifi)statusLines(canvas,text,166);
          else if(ready&&msg.caption[0])centeredText(canvas,msg.caption,168,1);
          break;
      }
      centeredText(canvas,msg.footer,190,1);
      const bool usbConnected=HWCDC::isPlugged();
      canvas.drawRect(159,6,35,16,0);canvas.fillRect(194,11,2,6,0);
      canvas.setTextSize(1);
      String batteryLabel=batteryPercent<0?"--":String(batteryPercent)+"%";
      canvas.setCursor(176-batteryLabel.length()*3,usbConnected?25:10);
      canvas.print(batteryLabel);
      if(usbConnected){
        const int x=173,y=8;
        canvas.fillTriangle(x+5,y,x,y+7,x+5,y+7,0);
        canvas.fillTriangle(x+2,y+5,x+7,y+5,x+2,y+12,0);
      }
    }
    display.EPD_Clear();
    for(int y=0;y<200;y++)for(int x=0;x<200;x++)
      if(!canvas.getPixel(x,y))display.EPD_DrawColorPixel(x,y,DRIVER_COLOR_BLACK);
    display.EPD_DisplayPart();
    if(sleeping){display.EPD_Sleep();digitalWrite(6,HIGH);panelAsleep=true;xSemaphoreGive(sleepScreenReady);}
  }
}
#endif

// ---------- Audio ----------

#if defined(BOARD_M5STICKS3)
// M5Stack StickS3 shares one ES8311 codec between mic and speaker (half-duplex).
// audioTask owns M5.Mic and M5.Speaker lifecycle and switches cleanly between them.
void stickSpeakerBegin() {
  M5.Speaker.begin();
  M5.Speaker.setVolume(STICK_SPEAKER_VOL);
  M5.Speaker.setChannelVolume(0,255); // Speech / music at full volume.
  M5.Speaker.setChannelVolume(1,96);  // Keep button beeps moderate.
  M5.In_I2C.writeRegister8(0x18,0x32,STICK_DAC_VOL,100000);
}
void audioTask(void*) {
  static MicFrame recBuf[2];
  static int16_t playBuf[3][512];
  int playIdx=0;
  bool inMicMode=false;
  stickSpeakerBegin();
  for(;;) {
    if(audioHold){
      if(inMicMode){M5.Mic.end();inMicMode=false;}
      else {M5.Speaker.stop();M5.Speaker.end();}
      playing=false;
      audioPaused=true;
      while(audioHold)vTaskDelay(pdMS_TO_TICKS(10));
      audioPaused=false;
      if(!wantMicMode)stickSpeakerBegin();
      continue;
    }
    if(wantMicMode&&!inMicMode){
      // Let any short start-session button tone finish before handing the codec to the mic.
      int requested;
      if(xQueueReceive(toneQueue,&requested,0)==pdTRUE){
        M5.Speaker.tone((float)requested,80,1,true);
        vTaskDelay(pdMS_TO_TICKS(95));
      }
      M5.Speaker.stop();M5.Speaker.end();
      playing=false;
      if(M5.Mic.begin()){
        inMicMode=true;
        M5.Mic.record(recBuf[0].samples,512,16000,false);
        M5.Mic.record(recBuf[1].samples,512,16000,false);
        int readyIdx=0;
        while(wantMicMode&&!audioHold){
          while(M5.Mic.isRecording()>=2&&wantMicMode&&!audioHold)vTaskDelay(1);
          if(!wantMicMode||audioHold)break;
          if(xQueueSend(micQueue,&recBuf[readyIdx],0)!=pdTRUE){
            MicFrame dropped;xQueueReceive(micQueue,&dropped,0);xQueueSend(micQueue,&recBuf[readyIdx],0);
          }
          M5.Mic.record(recBuf[readyIdx].samples,512,16000,false);
          readyIdx^=1;
        }
        M5.Mic.end();
        inMicMode=false;
      }else{
        Serial.println("ERROR: M5.Mic.begin failed");
        vTaskDelay(pdMS_TO_TICKS(50));
      }
      if(!audioHold)stickSpeakerBegin();
      continue;
    }
    // Speaker mode: tones and streaming PCM playback from audioRing.
    int requested;
    if(xQueueReceive(toneQueue,&requested,0)==pdTRUE){
      M5.Speaker.tone((float)requested,80,1,true);
    }
    if(audioUsed&&!audioGate){
      if(M5.Speaker.isPlaying(0)<2){
        xSemaphoreTake(audioMutex,portMAX_DELAY);
        size_t got=min((size_t)512,(size_t)audioUsed);
        for(size_t i=0;i<got;i++){playBuf[playIdx][i]=audioRing[readPos];if(++readPos==RING_SAMPLES)readPos=0;}
        audioUsed-=got;
        xSemaphoreGive(audioMutex);
        if(got){
          M5.Speaker.playRaw(playBuf[playIdx],got,codecRate,false,1,0);
          playIdx=(playIdx+1)%3;
        }
      }else{
        vTaskDelay(1);
      }
    }else{
      vTaskDelay(pdMS_TO_TICKS(5));
    }
    playing=(audioUsed>0&&!audioGate)||(M5.Speaker.isPlaying(0)>0);
  }
}
bool holdAudio() {
  audioHold=true;
  uint32_t started=millis();
  while(!audioPaused){
    if(millis()-started>2000){audioHold=false;return false;}
    delay(10);
  }
  return true;
}
bool setCodecRate(uint32_t hz) {
  if(!audioReady)return false;
  codecRate=hz; // M5.Speaker.playRaw resamples any source rate natively.
  return true;
}
#else
// Mic and speaker share one I2S clock: each captured block is paired with a playback
// block (generated audio from the ring, button tones, or silence) so the speaker never
// replays stale DMA data.
void audioTask(void*) {
  alignas(16) int16_t captured[1024],stereo[1024];
  int16_t mono[512];
  int toneFrequency=0,toneRemaining=0,tonePosition=0;
  for(;;) {
    if(audioHold){audioPaused=true;while(audioHold)vTaskDelay(pdMS_TO_TICKS(10));audioPaused=false;toneRemaining=0;}
    if(esp_codec_dev_read(mic,captured,sizeof(captured))!=0){vTaskDelay(1);continue;}
    MicFrame frame;for(int i=0;i<512;i++)frame.samples[i]=captured[2*i];
    // Drop stale input rather than delay transcription behind network stalls.
    if(xQueueSend(micQueue,&frame,0)!=pdTRUE){MicFrame dropped;xQueueReceive(micQueue,&dropped,0);xQueueSend(micQueue,&frame,0);}
    size_t got=0;
    if(audioUsed&&!audioGate){
      xSemaphoreTake(audioMutex,portMAX_DELAY);
      got=min((size_t)512,(size_t)audioUsed);
      for(size_t i=0;i<got;i++){mono[i]=audioRing[readPos];if(++readPos==RING_SAMPLES)readPos=0;}
      audioUsed-=got;
      xSemaphoreGive(audioMutex);
    }
    for(size_t i=got;i<512;i++)mono[i]=0;
    playing=got>0;
    uint32_t rate=codecRate;
    int requested;
    if(xQueueReceive(toneQueue,&requested,0)==pdTRUE){toneFrequency=requested;toneRemaining=rate*80/1000;tonePosition=0;}
    for(int i=0;i<512;i++) {
      int32_t sample=mono[i];
      if(toneRemaining>0){
        float envelope=min(1.0f,min(tonePosition/128.0f,toneRemaining/128.0f));
        sample+=(int32_t)(2400*envelope*sinf(2.0f*PI*toneFrequency*tonePosition/(float)rate));
        tonePosition++;toneRemaining--;
      }
      stereo[2*i]=stereo[2*i+1]=(int16_t)constrain(sample,-32768,32767);
    }
    if(esp_codec_dev_write(speaker,stereo,sizeof(stereo))!=0)Serial.println("ERROR: speaker write");
  }
}

bool openAudio(uint32_t rate) {
  esp_codec_dev_sample_info_t fmt={};fmt.sample_rate=rate;fmt.channel=2;fmt.bits_per_sample=16;
  int out=esp_codec_dev_open(speaker,&fmt),in=esp_codec_dev_open(mic,&fmt);
  Serial.printf("AUDIO OPEN: rate=%u speaker=%d mic=%d\n",(unsigned)rate,out,in);
  if(out!=0||in!=0)return false;
  esp_codec_dev_set_out_vol(speaker,PLAY_VOLUME);esp_codec_dev_set_in_gain(mic,MIC_GAIN_DB);
  return true;
}
// Parks the worker until it acknowledges between I2S blocks.
bool holdAudio() {
  audioHold=true;
  uint32_t started=millis();
  while(!audioPaused){
    if(millis()-started>2000){audioHold=false;return false;}
    delay(10);
  }
  return true;
}
// Reopens mic + speaker at `hz` so generated audio plays at its native rate.
// Falls back to 16 kHz (callers then resample). Only called from genTask.
bool setCodecRate(uint32_t hz) {
  if(!audioReady)return false;
  if(hz==codecRate)return true;
  if(!holdAudio())return false;
  esp_codec_dev_close(mic);esp_codec_dev_close(speaker);
  bool ok=openAudio(hz);
  if(ok)codecRate=hz;
  else {
    esp_codec_dev_close(mic);esp_codec_dev_close(speaker);
    codecRate=16000;
    if(!openAudio(16000)){audioReady=false;Serial.println("ERROR: audio reopen failed");return false;} // Worker stays parked.
  }
  xQueueReset(micQueue);
  audioHold=false;
  return ok;
}
#endif
// Blocks until all samples are queued; gives up on cancel.
void enqueueAudioWait(const int16_t *samples,size_t count) {
  while(count&&!genCancel&&audioReady){
    xSemaphoreTake(audioMutex,portMAX_DELAY);
    size_t chunk=min(count,RING_SAMPLES-audioUsed);
    for(size_t i=0;i<chunk;i++){audioRing[writePos]=samples[i];if(++writePos==RING_SAMPLES)writePos=0;}
    audioUsed+=chunk;
    xSemaphoreGive(audioMutex);
    samples+=chunk;count-=chunk;
    if(count)vTaskDelay(pdMS_TO_TICKS(10));
  }
}
void clearAudio() {
  if(!audioMutex)return;
  xSemaphoreTake(audioMutex,portMAX_DELAY);
  readPos=writePos=0;audioUsed=0;audioGate=false;
  xSemaphoreGive(audioMutex);
#if defined(BOARD_M5STICKS3)
  if(!wantMicMode)M5.Speaker.stop(0);
#endif
}
void waitPlaybackDone() {
  while(audioReady&&!genCancel&&(audioUsed>0||playing))vTaskDelay(pdMS_TO_TICKS(10));
}

#include "gemini_rest.h"
#include "generate.h"
#include "setup_portal.h"

// ---------- Transcription session ----------

void showTranscript() {
  bool smart=transcriptKind==TRANSCRIPT_SMART;
  const String &text=transcriptKind==TRANSCRIPT_INTERIM?interimText:transcriptKind==TRANSCRIPT_SMART?smartText:String();
  showScreen(MODE_LISTEN,smart?"SMART TRANSCRIPT":"LIVE ...",text,LISTEN_FOOTER,smart);
}
void stopSession(const String &message) {
  lastActivity=millis();wanted=live=false;ws.disconnect();
#if defined(BOARD_M5STICKS3)
  wantMicMode=false;
#endif
  if(appState==APP_TRANSCRIBING)appState=APP_READY;
  status(message);
}
void appendPrompt(const char *text,bool newSegment) {
  if(newSegment&&sessionPrompt.length())sessionPrompt+="\n";
  sessionPrompt+=text;
  if(sessionPrompt.length()>SESSION_PROMPT_CAP)sessionPrompt.remove(0,sessionPrompt.length()-SESSION_PROMPT_CAP);
}
// Stops listening and shows the collected prompt (side button then creates).
void endTranscription() {
  lastActivity=millis();wanted=live=false;ws.disconnect();
#if defined(BOARD_M5STICKS3)
  wantMicMode=false;
#endif
  // Speech that VAD has not finalized yet still belongs to the prompt.
  if(transcriptKind==TRANSCRIPT_INTERIM&&interimText.length())appendPrompt(interimText.c_str(),true);
  interimText="";transcriptKind=TRANSCRIPT_NONE;
  sessionPrompt.trim();
  if(sessionPrompt.isEmpty()){appState=APP_READY;status(selectedHotspot?"Hotspot ready. BOOT":"Home Wi-Fi ready. BOOT");return;}
  appState=APP_PROMPT;
  Serial.printf("PROMPT: %u chars\n",(unsigned)sessionPrompt.length());
  showScreen(MODE_PROMPT,"PROMPT",sessionPrompt,PROMPT_FOOTER,false);
}
void sendJson(JsonDocument &doc) {String data;serializeJson(doc,data);ws.sendTXT(data);}

void serverMessage(const uint8_t *data,size_t len) {
  JsonDocument doc;
  if(deserializeJson(doc,data,len)){stopSession("Response error");return;}
  if(doc["error"].is<JsonObject>()){
    // Do not print raw server messages, URLs, or credentials.
    Serial.printf("Gemini API error code: %d\n",doc["error"]["code"].as<int>());
    stopSession("API error. Hold " MAIN_BTN_NAME " 10s");return;
  }
  if(doc["setupComplete"].is<JsonObject>()) {
    live=true;sessionStarted=millis();lastScreen="Listening";Serial.println("STATUS: Listening");
    showTranscript();return;
  }
  auto content=doc["serverContent"];
  bool changed=false;
  // Interim hypotheses replace each other while the speaker is talking.
  const char *interim=content["interimInputTranscription"]["text"];
  if(interim&&*interim){
    Serial.print("TRANSCRIPT_INTERIM: ");Serial.println(interim);
    interimText=interim;transcriptKind=TRANSCRIPT_INTERIM;smartSegmentOpen=false;changed=true;
  }
  // Finalized SMART text arrives after VAD end-of-speech; consecutive chunks form one segment.
  const char *finalText=content["inputTranscription"]["text"];
  if(finalText&&*finalText){
    Serial.print("TRANSCRIPT_SMART: ");Serial.println(finalText);
    appendPrompt(finalText,!smartSegmentOpen);
    if(!smartSegmentOpen)smartText="";
    smartText+=finalText;
    if(smartText.length()>1200)smartText.remove(0,smartText.length()-1200);
    interimText="";transcriptKind=TRANSCRIPT_SMART;smartSegmentOpen=true;changed=true;
  }
  if(changed){lastActivity=millis();showTranscript();}
  if(doc["goAway"].is<JsonObject>())endTranscription();
}
void socketEvent(WStype_t type,uint8_t *data,size_t len) {
  switch(type) {
    case WStype_CONNECTED: {
      // Dedicated speech-to-text pipeline with server-side automatic VAD.
      JsonDocument doc;auto setup=doc["setup"].to<JsonObject>();
      setup["model"]=String("models/")+TRANSCRIBE_MODEL;
      setup["generationConfig"]["responseModalities"][0]="TEXT";
      setup["inputAudioTranscription"]["mode"]="SMART";
      setup["inputAudioTranscription"]["languageCodes"].to<JsonArray>(); // Auto language detection.
      sendJson(doc);status("Starting Gemini");break;
    }
    case WStype_TEXT:case WStype_BIN:serverMessage(data,len);break;
    case WStype_FRAGMENT_TEXT_START:case WStype_FRAGMENT_BIN_START:
      fragments="";fragments.concat((const char*)data,len);break;
    case WStype_FRAGMENT:case WStype_FRAGMENT_FIN:
      if(fragments.length()+len>64*1024){stopSession("Response too large");break;}
      fragments.concat((const char*)data,len);
      if(type==WStype_FRAGMENT_FIN){serverMessage((const uint8_t*)fragments.c_str(),fragments.length());fragments="";}break;
    case WStype_DISCONNECTED:
      live=false;
#if defined(BOARD_M5STICKS3)
      wantMicMode=false;
#endif
      if(wanted){wanted=false;if(appState==APP_TRANSCRIBING)appState=APP_READY;status("Disconnected. Click " MAIN_BTN_NAME);}break;
    case WStype_ERROR:stopSession("Connection error");break;
    default:break;
  }
}
void startSession() {
  lastActivity=millis();
  if(genBusy)return;
  if(!configured){status("Hold " MAIN_BTN_NAME " 10s for setup");return;}
  if(WiFi.status()!=WL_CONNECTED){status("Wi-Fi unavailable");return;}
  if(!audioReady){status("Audio unavailable");return;}
  if(time(nullptr)<1700000000){status("Waiting for clock");return;}
  wanted=true;live=false;connectStarted=millis();appState=APP_TRANSCRIBING;
#if defined(BOARD_M5STICKS3)
  wantMicMode=true;
#endif
  interimText=smartText=sessionPrompt="";transcriptKind=TRANSCRIPT_NONE;smartSegmentOpen=false;
  xQueueReset(micQueue);
  String path="/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key="+apiKey;
  ws.beginSslWithCA("generativelanguage.googleapis.com",443,path.c_str(),GOOGLE_ROOTS,"");
  ws.setReconnectInterval(0);status("Connecting Gemini");
}
// Hands the session prompt to genTask (router -> image / speech / song / voice).
void startGeneration() {
  lastActivity=millis();
  if(genBusy||!genHandle)return;
  if(sessionPrompt.isEmpty()){status("Say something first");return;}
  if(WiFi.status()!=WL_CONNECTED){status("Wi-Fi unavailable");return;}
  if(time(nullptr)<1700000000){status("Waiting for clock");return;}
  Serial.printf("GENERATE: %u chars\n",(unsigned)sessionPrompt.length());
  genPrompt=sessionPrompt;genCancel=false;genBusy=true;appState=APP_GENERATING;
  xTaskNotifyGive(genHandle);
}
// BOOT during generation/playback: stop audio now; genTask notices at its next check.
void cancelGeneration() {
  if(genCancel)return;
  genCancel=true;clearAudio();
  Serial.println("GENERATE: cancel requested");
  if(appState==APP_GENERATING)showScreen(MODE_BUSY,"Cancelling...","Waiting for Gemini","",false);
}
void connectWifi() {
  // SNTP must not have a DNS query pending when the first lookup clears the DNS cache (see primeNetwork).
  if(esp_sntp_enabled())esp_sntp_stop();
  netPrimed=false;
  WiFi.mode(WIFI_STA);
  // Modem sleep delays downlink packets to the next beacon; with the core's 5.7 KB TCP window
  // that caps media downloads. Idle light sleep turns Wi-Fi off anyway.
  WiFi.setSleep(false);
  WiFi.begin((selectedHotspot?backupSsid:ssid).c_str(),(selectedHotspot?backupPassword:password).c_str());
  wifiAttempt=millis();status(selectedHotspot?"Connecting hotspot":"Connecting home Wi-Fi");
}
// arduino-esp32 3.3.0: the first hostByName() after the IP changes calls dns_clear_cache()
// without the lwIP core lock; a pending SNTP lookup then asserts in udp_new. Do that first
// lookup here, before SNTP starts, so later lookups (genTask, WebSocket) never clear the cache.
// Some networks hand out a DNS server that never answers; fall back to public resolvers.
void useFallbackDns() {
  esp_netif_t *netif=WiFi.STA.netif();
  if(!netif)return;
  esp_netif_dns_info_t dns={};dns.ip.type=ESP_IPADDR_TYPE_V4;
  dns.ip.u_addr.ip4.addr=ESP_IP4TOADDR(8,8,8,8);esp_netif_set_dns_info(netif,ESP_NETIF_DNS_MAIN,&dns);
  dns.ip.u_addr.ip4.addr=ESP_IP4TOADDR(1,1,1,1);esp_netif_set_dns_info(netif,ESP_NETIF_DNS_BACKUP,&dns);
  Serial.println("NET: network DNS not answering, using 8.8.8.8 / 1.1.1.1");
}
void primeNetwork() {
  static uint8_t attempts=0;static uint32_t lastTry=0;
  if(attempts&&millis()-lastTry<1500)return;
  lastTry=millis();attempts++;
  IPAddress ip;
  int ok=WiFi.hostByName("generativelanguage.googleapis.com",ip);
  Serial.printf("NET: DNS primed=%d try=%u dns=%s\n",ok==1,attempts,WiFi.dnsIP().toString().c_str());
  if(ok!=1&&attempts==2)useFallbackDns();
  if(ok!=1&&attempts<6)return; // DNS can lag the IP by a moment; SNTP waits for it.
  attempts=0;
  configTime(0,0,"time.google.com","pool.ntp.org");
  netPrimed=true;
}
void selectOtherWifi() {
  if(wanted){stopSession("Stopped. Hold BOOT to switch Wi-Fi");return;}
  if(backupSsid.isEmpty()){
    // Released after the 3 s hint without a backup saved: put the normal screen back.
    if(WiFi.status()==WL_CONNECTED)status(selectedHotspot?"Hotspot ready. BOOT":"Home Wi-Fi ready. BOOT");
    else status("No backup Wi-Fi saved");
    return;
  }
  selectedHotspot=!selectedHotspot;prefs.putBool("use_hotspot",selectedHotspot);
  status(selectedHotspot?"Switching to hotspot. Restarting":"Switching to home. Restarting");
  Serial.printf("WIFI SELECTED: %s\n",selectedHotspot?"hotspot":"primary");
  wifiRestartPending=true;
}
void readSetup() {
  while(Serial.available()) {
    char c=Serial.read();
    if(c=='\n') {
      JsonDocument doc;auto err=deserializeJson(doc,serialLine);serialLine="";
      if(err)continue;
      // Wipes Wi-Fi + key so the next boot starts phone setup (backup Wi-Fi and voice are kept).
      if(doc["command"]=="forget_setup") {
        prefs.remove("ssid");prefs.remove("password");prefs.remove("key");prefs.putBool("use_hotspot",false);
        Serial.println("SETUP_FORGOTTEN");Serial.flush();delay(100);ESP.restart();
      }
      // While the setup portal runs, only the configuration commands apply.
      if(setupMode&&doc["command"]!="configure"&&doc["command"]!="configure_backup_wifi")continue;
      if(doc["command"]=="start_session") {lastActivity=millis();if(!wanted)startSession();continue;}
      if(doc["command"]=="stop_session") {if(wanted)endTranscription();continue;}
      if(doc["command"]=="generate") {
        // Test hook: skip dictation and run the create flow on the given text.
        String text=doc["text"] | "";
        if(!wanted&&!genBusy&&text.length()){sessionPrompt=text.substring(0,SESSION_PROMPT_CAP);startGeneration();}
        else Serial.println("GENERATE: busy or empty");
        continue;
      }
      if(doc["command"]=="select_primary_wifi") {
        stopSession("Returning to home Wi-Fi");prefs.putBool("use_hotspot",false);Serial.println("PRIMARY_WIFI_SELECTED");delay(100);ESP.restart();
      }
      if(doc["command"]=="wifi_diagnostics") {
        Serial.printf("WIFI DIAG: status=%d reason=%u selected=%s hotspot_saved=%d\n",(int)WiFi.status(),wifiFailureReason,selectedHotspot?"hotspot":"primary",!backupSsid.isEmpty());
        Serial.print("PRIMARY WIFI: ");Serial.println(ssid);
        Serial.print("WIFI TARGET: ");Serial.println(selectedHotspot?backupSsid:ssid);
        if(!wanted&&!wifiDiagnosticScan){WiFi.scanNetworks(true);wifiDiagnosticScan=true;}
        continue;
      }
      if(doc["command"]=="configure_backup_wifi") {
        String name=doc["ssid"] | "",pass=doc["password"] | "";
        if(name.isEmpty()||name.length()>32||pass.length()<8||pass.length()>63){Serial.println("BACKUP_WIFI_INVALID");continue;}
        backupSsid=name;backupPassword=pass;prefs.putString("backup_ssid",name);prefs.putString("backup_password",pass);
        Serial.println("BACKUP_WIFI_SAVED");continue;
      }
      if(doc["command"]!="configure")continue;
      String newSsid=doc["ssid"].as<String>(),newKey=doc["key"].as<String>();
      if(newSsid.isEmpty()||newKey.length()<20){Serial.println("CONFIG_INVALID");continue;}
      stopSession("Saving setup");
      ssid=newSsid;password=doc["password"].as<String>();apiKey=newKey;
      prefs.putString("ssid",ssid);prefs.putString("password",password);prefs.putString("key",apiKey);
      selectedHotspot=false;prefs.putBool("use_hotspot",false);
      configured=true;Serial.println("CONFIG_SAVED");
      if(setupMode){Serial.flush();delay(100);ESP.restart();} // Leave the portal via a clean boot.
      connectWifi();
    }else if(serialLine.length()<2048)serialLine+=c;
    else serialLine="";
  }
}
void setup() {
#if defined(BOARD_M5STICKS3)
  auto cfg=M5.config();
  cfg.internal_spk=true;
  cfg.internal_mic=true;
  M5.begin(cfg);
  M5.Display.setRotation(0);
  M5.Display.setBrightness(200);
  M5.Display.setSwapBytes(true);
  M5.Display.fillScreen(0x0842);
  M5.Speaker.end();
  auto spkCfg=M5.Speaker.config();
  spkCfg.magnification=STICK_SPK_MAG;
  M5.Speaker.config(spkCfg);
  rtc_gpio_deinit((gpio_num_t)MAIN_BUTTON_PIN);
  rtc_gpio_deinit((gpio_num_t)SIDE_BUTTON_PIN);
  sampleBattery();lastBatterySample=millis();lastUsbConnected=HWCDC::isPlugged();
  Serial.begin(115200);delay(1500);
  pinMode(MAIN_BUTTON_PIN,INPUT_PULLUP);pinMode(SIDE_BUTTON_PIN,INPUT_PULLUP);
  WiFi.onEvent([](WiFiEvent_t,WiFiEventInfo_t info){wifiFailureReason=info.wifi_sta_disconnected.reason;},ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  prefs.begin("gemini",false);
  activeVoiceId=prefs.getString("voice_id");activeVoiceName=prefs.getString("voice_name");
  sleepScreenReady=xSemaphoreCreateBinary();
  screenLock=xSemaphoreCreateMutex();audioMutex=xSemaphoreCreateMutex();
  audioRing=(int16_t*)ps_malloc(RING_SAMPLES*sizeof(int16_t));
  imageRgb565=(uint16_t*)ps_calloc((size_t)SCREEN_W*SCREEN_H,sizeof(uint16_t));
  screenQueue=xQueueCreate(1,sizeof(ScreenMsg));toneQueue=xQueueCreate(4,sizeof(int));
  micQueue=xQueueCreate(4,sizeof(MicFrame));
  xTaskCreatePinnedToCore(screenTask,"screen",8192,nullptr,1,nullptr,1);
  audioReady=micQueue&&toneQueue&&audioRing&&M5.Mic.isEnabled()&&M5.Speaker.isEnabled();
  Serial.printf("AUDIO INIT (StickS3): mic=%d spk=%d free RAM=%u PSRAM=%u\n",
                M5.Mic.isEnabled(),M5.Speaker.isEnabled(),ESP.getFreeHeap(),ESP.getFreePsram());
  if(audioReady)xTaskCreatePinnedToCore(audioTask,"audio",12288,nullptr,4,nullptr,1);
#else
  // Waveshare ESP32-S3-ePaper-1.54 V2: keep battery power latched on.
  // Assert before USB logging and peripheral startup so PWR can be released.
  // A USB power-off (deep sleep) leaves GPIO17 held low and GPIO18 as an RTC pin; undo both.
  gpio_hold_dis(GPIO_NUM_17);gpio_deep_sleep_hold_dis();
  pinMode(17,OUTPUT);
  digitalWrite(17,HIGH);
  rtc_gpio_deinit(GPIO_NUM_18);
  analogReadResolution(12);analogSetPinAttenuation(4,ADC_11db);
  sampleBattery();lastBatterySample=millis();lastUsbConnected=HWCDC::isPlugged();
  Serial.begin(115200);delay(1500);pinMode(MAIN_BUTTON_PIN,INPUT_PULLUP);pinMode(SIDE_BUTTON_PIN,INPUT_PULLUP);
  WiFi.onEvent([](WiFiEvent_t,WiFiEventInfo_t info){wifiFailureReason=info.wifi_sta_disconnected.reason;},ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  prefs.begin("gemini",false);
  activeVoiceId=prefs.getString("voice_id");activeVoiceName=prefs.getString("voice_name");
  sleepScreenReady=xSemaphoreCreateBinary();
  screenLock=xSemaphoreCreateMutex();audioMutex=xSemaphoreCreateMutex();
  audioRing=(int16_t*)ps_malloc(RING_SAMPLES*sizeof(int16_t));
  screenQueue=xQueueCreate(1,sizeof(ScreenMsg));toneQueue=xQueueCreate(4,sizeof(int));
  micQueue=xQueueCreate(4,sizeof(MicFrame));
  xTaskCreatePinnedToCore(screenTask,"screen",8192,nullptr,1,nullptr,1);
  pinMode(42,OUTPUT);digitalWrite(42,LOW);delay(100);
  set_codec_board_type("S3_ePaper_1_54");
  codec_init_cfg_t cfg={CODEC_I2S_MODE_STD,CODEC_I2S_MODE_STD,false,false};
  int codecResult=init_codec(&cfg);
  Serial.printf("AUDIO INIT: codec=%d free RAM=%u PSRAM=%u\n",codecResult,ESP.getFreeHeap(),ESP.getFreePsram());
  if(codecResult==0) {
    mic=get_record_handle();speaker=get_playback_handle();
    audioReady=micQueue&&toneQueue&&audioRing&&openAudio(16000);
    if(audioReady)xTaskCreatePinnedToCore(audioTask,"audio",12288,nullptr,4,nullptr,1);
  }
#endif
  xTaskCreatePinnedToCore(genTask,"gen",20480,nullptr,1,&genHandle,0);
  ws.onEvent(socketEvent);
  ssid=prefs.getString("ssid");password=prefs.getString("password");apiKey=prefs.getString("key");
  backupSsid=prefs.getString("backup_ssid");backupPassword=prefs.getString("backup_password");
  selectedHotspot=prefs.getBool("use_hotspot",false)&&!backupSsid.isEmpty();
  configured=!ssid.isEmpty()&&!apiKey.isEmpty();
  if(configured)connectWifi();else enterSetupMode();
  lastActivity=millis();
  // PWR is usually still held from powering on: treat it as already handled so its
  // release does not trigger "create" and holding on does not power straight back off.
  lastSide=stableSide=digitalRead(SIDE_BUTTON_PIN);sideChanged=millis();sideLongHandled=stableSide==LOW;
  Serial.printf("GEMINI_FIRMWARE_READY: transcribe-create-v2 board=%s setup=qr model=%s voice=%s idle=120s\n",
#if defined(BOARD_M5STICKS3)
                "M5StickS3",
#else
                "Waveshare-ePaper-1.54",
#endif
                TRANSCRIBE_MODEL,activeVoiceName.length()?activeVoiceName.c_str():DEFAULT_VOICE);
}
// The audio worker acknowledges only between complete I2S blocks.
// RAM and task state are retained; the wake press is consumed before normal button handling.
void enterIdleSleep() {
  if(!sleepScreenReady)return;
  if(audioReady&&!holdAudio()){lastActivity=millis();return;}
  audioHold=true;
#if defined(BOARD_M5STICKS3)
  if(audioReady){xQueueReset(micQueue);xQueueReset(toneQueue);}
#else
  if(audioReady){esp_codec_dev_close(mic);esp_codec_dev_close(speaker);xQueueReset(micQueue);xQueueReset(toneQueue);}
#endif
  if(esp_sntp_enabled())esp_sntp_stop();
  WiFi.disconnect(false,false);WiFi.mode(WIFI_OFF);
  while(xSemaphoreTake(sleepScreenReady,0)==pdTRUE){}
  status("Light sleep");
  bool screenReady=xSemaphoreTake(sleepScreenReady,pdMS_TO_TICKS(15000))==pdTRUE;
  esp_err_t result=ESP_FAIL;
  if(screenReady&&digitalRead(MAIN_BUTTON_PIN)==HIGH){
    result=gpio_wakeup_enable((gpio_num_t)MAIN_BUTTON_PIN,GPIO_INTR_LOW_LEVEL);
    if(result==ESP_OK)result=esp_sleep_enable_gpio_wakeup();
    if(result==ESP_OK){
#if !defined(BOARD_M5STICKS3)
      gpio_hold_en(GPIO_NUM_17); // Preserve the battery power latch.
#endif
      Serial.println("LIGHT_SLEEP_ENTER: " MAIN_BTN_NAME " wakes device");Serial.flush();
      result=esp_light_sleep_start();
#if !defined(BOARD_M5STICKS3)
      gpio_hold_dis(GPIO_NUM_17);digitalWrite(17,HIGH);
#endif
    }
    gpio_wakeup_disable((gpio_num_t)MAIN_BUTTON_PIN);esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
  }
#if !defined(BOARD_M5STICKS3)
  if(audioReady){
    codecRate=16000;
    audioReady=openAudio(16000);
    if(!audioReady)Serial.println("ERROR: audio resume failed");
  }
#endif
  Serial.printf("LIGHT_SLEEP_EXIT: result=%d wake=%d\n",result,esp_sleep_get_wakeup_cause());
  // Keep the worker parked on failure to reopen audio rather than spin on closed handles.
  if(!audioReady&&audioPaused){Serial.println("ERROR: restarting to recover audio");ESP.restart();}
  audioHold=false;
  while(digitalRead(MAIN_BUTTON_PIN)==LOW)delay(10);
  lastButton=stableButton=HIGH;buttonChanged=millis();bootLongHandled=bootSetupHandled=false;wifiRestartPending=false;
  lastSide=stableSide=digitalRead(SIDE_BUTTON_PIN);sideChanged=millis();sideLongHandled=stableSide==LOW;
  lastActivity=millis();lastBatterySample=millis();
  if(configured)connectWifi();else enterSetupMode();
}

void onBootShort() {
  if(appState==APP_GENERATING||appState==APP_PLAYING){buttonTone(440);cancelGeneration();return;}
  if(genBusy)return; // Cancel already requested; genTask is wrapping up.
  if(wanted){endTranscription();buttonTone(440);}
  else {buttonTone(880);startSession();}
}
// Side button: "create" from the current (or just-finished) transcript.
void onSidePress() {
  lastActivity=millis();
  if(genBusy)return;
  if(wanted){
    endTranscription();
    if(sessionPrompt.isEmpty()){status("Say something first");return;}
  }
  buttonTone(1320);
  startGeneration();
}
// Long side-button hold: power off.
void powerOff() {
  Serial.println("POWER_OFF: requested");
#if defined(BOARD_M5STICKS3)
  wantMicMode=false;
#endif
  buttonTone(330);delay(200);
  if(genBusy)cancelGeneration();
  poweringOff=true;
  if(wanted){wanted=live=false;ws.disconnect();}
  if(esp_sntp_enabled())esp_sntp_stop();
  WiFi.disconnect(false,false);WiFi.mode(WIFI_OFF);
  while(xSemaphoreTake(sleepScreenReady,0)==pdTRUE){}
  status("Powered off");
  xSemaphoreTake(sleepScreenReady,pdMS_TO_TICKS(15000));
#if defined(BOARD_M5STICKS3)
  if(audioReady)holdAudio();
  while(digitalRead(SIDE_BUTTON_PIN)==LOW)delay(10);
  delay(200);
  M5.Power.powerOff();
  delay(500);
  // Still running (e.g. USB powered): fall back to deep sleep until BtnA or BtnB.
  Serial.println("POWER_OFF: on USB, deep sleep until " SIDE_BTN_NAME);Serial.flush();
  rtc_gpio_pullup_en((gpio_num_t)SIDE_BUTTON_PIN);rtc_gpio_pulldown_dis((gpio_num_t)SIDE_BUTTON_PIN);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)SIDE_BUTTON_PIN,0);
  esp_deep_sleep_start();
#else
  digitalWrite(17,LOW);
  while(digitalRead(SIDE_BUTTON_PIN)==LOW)delay(10);
  delay(500);
  // Still running, so USB is powering the board.
  Serial.println("POWER_OFF: on USB, deep sleep until PWR");Serial.flush();
  gpio_hold_en(GPIO_NUM_17);gpio_deep_sleep_hold_en();
  rtc_gpio_pullup_en(GPIO_NUM_18);rtc_gpio_pulldown_dis(GPIO_NUM_18);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_18,0);
  esp_deep_sleep_start();
#endif
}

void loop() {
  bool batteryRefresh=false;
  if(millis()-lastBatterySample>=30000){
    lastBatterySample=millis();int previous=batteryPercent;sampleBattery();
    batteryRefresh=batteryPercent!=previous;
  }
  bool usbConnected=HWCDC::isPlugged();
  if(usbConnected!=lastUsbConnected){lastUsbConnected=usbConnected;batteryRefresh=true;}
  if(batteryRefresh)pushScreen();
  if(!netPrimed&&WiFi.status()==WL_CONNECTED)primeNetwork();
  readSetup();
  if(wifiDiagnosticScan){int n=WiFi.scanComplete();if(n>=0){bool exact=false;
    for(int i=0;i<n;i++)if(WiFi.SSID(i)==backupSsid){exact=true;Serial.printf("HOTSPOT VISIBLE: channel=%d signal=%d\n",WiFi.channel(i),WiFi.RSSI(i));}
    Serial.printf("HOTSPOT SCAN: exact_match=%d\n",exact);WiFi.scanDelete();wifiDiagnosticScan=false;
  }else if(n==WIFI_SCAN_FAILED){Serial.println("HOTSPOT SCAN: failed");wifiDiagnosticScan=false;}}

  bool button=digitalRead(MAIN_BUTTON_PIN);
  if(button!=lastButton){lastActivity=millis();lastButton=button;buttonChanged=millis();}
  if(button!=stableButton&&millis()-buttonChanged>40){stableButton=button;
    if(button==LOW){bootPressed=millis();bootLongHandled=bootSetupHandled=bootLongStopped=false;}
    else if(bootSetupHandled){} // Release after the 10 s hold that opened setup.
    else if(setupMode){if(!bootLongHandled)cancelSetupMode();}
    else if(bootLongHandled){
      // Released between 3 s and 10 s: switch primary/backup Wi-Fi.
      if(!bootLongStopped){selectOtherWifi();if(wifiRestartPending){delay(100);ESP.restart();}}
    }
    else onBootShort();
  }
  if(stableButton==LOW&&!setupMode&&!genBusy){
    uint32_t held=millis()-bootPressed;
    if(held>=SETUP_HOLD_MS&&!bootSetupHandled){bootSetupHandled=bootLongHandled=true;buttonTone(1320);enterSetupMode();}
    else if(held>=3000&&!bootLongHandled){
      bootLongHandled=true;
      if(wanted){bootLongStopped=true;stopSession("Stopped. Hold " MAIN_BTN_NAME " 10s for setup");}
      else status(backupSsid.length()?"Release: switch Wi-Fi. Hold: setup":"Keep holding for setup");
    }
  }
  bool side=digitalRead(SIDE_BUTTON_PIN);
  if(side!=lastSide){lastActivity=millis();lastSide=side;sideChanged=millis();}
  if(side!=stableSide&&millis()-sideChanged>40){stableSide=side;
    if(side==LOW){sidePressed=millis();sideLongHandled=false;}
    else if(!sideLongHandled&&!setupMode)onSidePress(); // Fire on release so a long hold never creates.
  }
  if(stableSide==LOW&&!sideLongHandled&&millis()-sidePressed>=POWER_OFF_HOLD_MS){sideLongHandled=true;powerOff();}
  // Setup portal owns Wi-Fi: no sessions, reconnects or idle sleep until it restarts.
  if(setupMode){setupLoop();delay(1);return;}
  if(wanted)ws.loop();
  if(wanted&&!live&&millis()-connectStarted>20000)stopSession("Connection timeout");
  if(live&&millis()-sessionStarted>5*60*1000)endTranscription();
  if(configured&&!wanted&&!genBusy) {
    if(WiFi.status()==WL_CONNECTED&&(lastScreen=="Connecting home Wi-Fi"||lastScreen=="Connecting hotspot"))status(selectedHotspot?"Hotspot ready. BOOT":"Home Wi-Fi ready. BOOT");
    else if(WiFi.status()!=WL_CONNECTED&&millis()-wifiAttempt>30000){status(selectedHotspot?"Hotspot failed. Hold " MAIN_BTN_NAME " for home":"Wi-Fi failed. Hold " MAIN_BTN_NAME " 10s for setup");WiFi.reconnect();wifiAttempt=millis();}
  }
  MicFrame frame;
  if(audioReady&&xQueueReceive(micQueue,&frame,0)==pdTRUE&&live) {
    char encoded[1372];size_t out=0;
    mbedtls_base64_encode((uint8_t*)encoded,sizeof(encoded)-1,&out,(uint8_t*)frame.samples,sizeof(frame.samples));encoded[out]=0;
    JsonDocument doc;doc["realtimeInput"]["audio"]["mimeType"]="audio/pcm;rate=16000";
    doc["realtimeInput"]["audio"]["data"]=encoded;sendJson(doc);
  }
  if(wanted||genBusy||wifiDiagnosticScan||!configured||WiFi.status()!=WL_CONNECTED)lastActivity=millis();
  if(!wanted&&!genBusy&&!wifiDiagnosticScan&&configured&&WiFi.status()==WL_CONNECTED&&
     digitalRead(MAIN_BUTTON_PIN)==HIGH&&millis()-lastActivity>=IDLE_SLEEP_MS)enterIdleSleep();
  delay(1);
}
