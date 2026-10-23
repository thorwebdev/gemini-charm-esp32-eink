#include <Arduino.h>
#include <HWCDC.h>
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "esp_sntp.h"
#include "esp_netif.h"
#include <math.h>
#include <WiFi.h>
#include "src/websockets/WebSocketsClient.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <mbedtls/base64.h>
#include "src/codec_board/codec_board.h"
#include "src/codec_board/codec_init.h"
#include "epaper_driver_bsp.h"
#include "google_roots.h"
#include "screen_msg.h"

// Live transcription: stream mic audio to the transcribe model with SMART formatting.
// Interim hypotheses render while speaking; the cleaned transcript replaces them once
// server-side VAD finalizes the segment.
// The side button then turns the collected transcript into an image, speech, a song or
// a new designed voice (see generate.h).
const char *TRANSCRIBE_MODEL="gemini-3.5-transcribe-live";
constexpr int SIDE_BUTTON_PIN=18;

Preferences prefs;
WebSocketsClient ws;
volatile uint8_t wifiFailureReason=0;
bool wifiDiagnosticScan=false;
String backupSsid,backupPassword;
bool selectedHotspot=false,bootLongHandled=false,wifiRestartPending=false;
bool netPrimed=false; // DNS primed and SNTP started for the current Wi-Fi connection.
uint32_t bootPressed=0;
String ssid, password, apiKey, serialLine, fragments;
esp_codec_dev_handle_t mic, speaker;
QueueHandle_t screenQueue, micQueue, toneQueue;
void buttonTone(int frequency) { if(toneQueue)xQueueSend(toneQueue,&frequency,0); }
constexpr float MIC_GAIN_DB=18.0f;
constexpr int PLAY_VOLUME=100;
struct MicFrame { int16_t samples[512]; };
bool audioReady=false,wanted=false,live=false,configured=false;
bool lastButton=HIGH,stableButton=HIGH;
bool lastSide=HIGH,stableSide=HIGH;
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
String interimText,smartText; // Raw UTF-8; converted to CP437 when pushed to the screen.
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
String screenCaption,screenBody,screenFooter; // Already converted to CP437.
bool screenFilled=false;
uint8_t imageBitmap[IMAGE_BYTES];
String activeVoiceId,activeVoiceName;

// Generation worker.
TaskHandle_t genHandle=nullptr;
String genPrompt;
volatile bool genCancel=false,genBusy=false;
const char *LISTEN_FOOTER="BOOT: stop | Side: create";
const char *PROMPT_FOOTER="Side: create | BOOT: redo";


void sampleBattery(){
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
}

bool lastUsbConnected=false;

// The built-in GFX font is CP437; map common UTF-8 (German, French, Spanish,
// typographic punctuation) so transcripts render instead of showing garbage.
const char *cp437Glyph(uint32_t cp) {
  struct Glyph{uint16_t cp;const char *out;};
  static const Glyph glyphs[]={
    {0xE4,"\x84"},{0xF6,"\x94"},{0xFC,"\x81"},{0xC4,"\x8E"},{0xD6,"\x99"},{0xDC,"\x9A"},{0xDF,"\xE1"},
    {0xE9,"\x82"},{0xE8,"\x8A"},{0xEA,"\x88"},{0xEB,"\x89"},{0xE0,"\x85"},{0xE2,"\x83"},{0xE7,"\x87"},
    {0xC7,"\x80"},{0xC9,"\x90"},{0xEF,"\x8B"},{0xEE,"\x8C"},{0xEC,"\x8D"},{0xF4,"\x93"},{0xF2,"\x95"},
    {0xFB,"\x96"},{0xF9,"\x97"},{0xFF,"\x98"},{0xE1,"\xA0"},{0xED,"\xA1"},{0xF3,"\xA2"},{0xFA,"\xA3"},
    {0xF1,"\xA4"},{0xD1,"\xA5"},{0xBF,"\xA8"},{0xA1,"\xAD"},{0xB0,"\xF8"},{0xE5,"\x86"},{0xC5,"\x8F"},
    {0xE6,"\x91"},{0xC6,"\x92"},{0xA3,"\x9C"},{0xA0," "},{0x20AC,"EUR"},{0x2018,"'"},{0x2019,"'"},
    {0x201C,"\""},{0x201D,"\""},{0x201E,"\""},{0x2013,"-"},{0x2014,"-"},{0x2022,"-"},{0x2026,"..."},
  };
  for(auto &g:glyphs)if(g.cp==cp)return g.out;
  return cp>=0x1F000?"":"?"; // Drop emoji, mark other unsupported glyphs.
}
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
  xSemaphoreTake(screenLock,portMAX_DELAY);
  screenMode=mode;screenCaption=toDisplayText(caption);screenBody=toDisplayText(body.c_str());
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
// Home screen with a status line. An image result stays up (photo frame) through sleep
// and Wi-Fi reconnects; any other status replaces it.
void status(const String &s) {
  xSemaphoreTake(screenLock,portMAX_DELAY);
  bool keepImage=screenMode==MODE_IMAGE&&(s=="Light sleep"||isReadyText(s)||isConnectingText(s));
  if(s==lastScreen&&(screenMode==MODE_HOME||keepImage)){xSemaphoreGive(screenLock);return;}
  lastScreen=s;
  Serial.println("STATUS: "+s);
  if(keepImage){
    if(s=="Light sleep")pushScreenLocked(); // Screen task only parks the panel.
    xSemaphoreGive(screenLock);return;
  }
  screenMode=MODE_HOME;screenFilled=false;screenBody="";
  screenCaption=activeVoiceName.length()?toDisplayText(("Voice: "+activeVoiceName).c_str()):String();
  bool connected=WiFi.status()==WL_CONNECTED&&!isConnectingText(s);
  screenFooter=s=="Light sleep"?"Click BOOT to wake up":
               (isReadyText(s)||(connected&&configured&&!wanted))?"BOOT: start transcribing":"";
  pushScreenLocked();
  xSemaphoreGive(screenLock);
}
void setActiveVoice(const String &id,const String &name) {
  xSemaphoreTake(screenLock,portMAX_DELAY);
  activeVoiceId=id;activeVoiceName=name;
  xSemaphoreGive(screenLock);
  prefs.putString("voice_id",id);prefs.putString("voice_name",name);
}

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
// Word-wraps text and keeps only the last `cap` lines so the newest words stay visible.
// Returns the total line count, which may exceed `cap`.
int wrapText(const char *t,int cols,int16_t *starts,uint8_t *lens,int cap) {
  int len=strlen(t),pos=0,total=0;
  auto add=[&](int s,int l){
    if(total<cap){starts[total]=s;lens[total]=l;}
    else {memmove(starts,starts+1,(cap-1)*sizeof(int16_t));memmove(lens,lens+1,cap-1);starts[cap-1]=s;lens[cap-1]=l;}
    total++;
  };
  while(pos<len){
    int j=pos,lastSpace=-1;
    while(j<len&&t[j]!='\n'&&j-pos<cols){if(t[j]==' ')lastSpace=j;j++;}
    int end,next;
    if(j>=len||t[j]=='\n'){end=j;next=j+1;}
    else if(t[j]==' '){end=j;next=j+1;}
    else if(lastSpace>pos){end=lastSpace;next=lastSpace+1;}
    else {end=j;next=j;} // Hard-break words longer than a line.
    add(pos,end-pos);pos=next;
    while(pos<len&&t[pos]==' ')pos++;
  }
  return total;
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
  static int16_t starts[14];static uint8_t lens[14];
  const char *t=text;while(*t==' '||*t=='\n')t++;
  int size=2,maxLines=8,lineHeight=17;
  int total=wrapText(t,16,starts,lens,maxLines);
  if(total>maxLines){size=1;maxLines=14;lineHeight=10;total=wrapText(t,32,starts,lens,maxLines);}
  c.setTextSize(size);
  for(int i=0;i<min(total,maxLines);i++){
    c.setCursor(4,50+i*lineHeight);
    for(int k=0;k<lens[i];k++)c.write((uint8_t)t[starts[i]+k]);
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
    bool sleeping=strcmp(text,"Light sleep")==0;
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
      if(selectedHotspot)iconHotspot(canvas);else iconWifi(canvas);
      if(!connected)canvas.drawLine(8,30,32,5,0);
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
          statusLines(canvas,msg.text,168);
          break;
        default:
          iconHome(canvas,sleeping);
          centeredText(canvas,sleeping?"Light sleep":"Transcribe",146,2);
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

// ---------- Audio ----------

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
}
void waitPlaybackDone() {
  while(audioReady&&!genCancel&&(audioUsed>0||playing))vTaskDelay(pdMS_TO_TICKS(10));
}

#include "gemini_rest.h"
#include "generate.h"

// ---------- Transcription session ----------

void showTranscript() {
  bool smart=transcriptKind==TRANSCRIPT_SMART;
  const String &text=transcriptKind==TRANSCRIPT_INTERIM?interimText:transcriptKind==TRANSCRIPT_SMART?smartText:String();
  showScreen(MODE_LISTEN,smart?"SMART TRANSCRIPT":"LIVE ...",text,LISTEN_FOOTER,smart);
}
void stopSession(const String &message) {
  lastActivity=millis();wanted=live=false;ws.disconnect();
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
    stopSession("API error. Check key / model");return;
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
      if(wanted){wanted=false;if(appState==APP_TRANSCRIBING)appState=APP_READY;status("Disconnected. Click BOOT");}break;
    case WStype_ERROR:stopSession("Connection error");break;
    default:break;
  }
}
void startSession() {
  lastActivity=millis();
  if(genBusy)return;
  if(!configured){status("Enter setup on Mac");return;}
  if(WiFi.status()!=WL_CONNECTED){status("Wi-Fi unavailable");return;}
  if(!audioReady){status("Audio unavailable");return;}
  if(time(nullptr)<1700000000){status("Waiting for clock");return;}
  wanted=true;live=false;connectStarted=millis();appState=APP_TRANSCRIBING;
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
  WiFi.mode(WIFI_STA);WiFi.begin((selectedHotspot?backupSsid:ssid).c_str(),(selectedHotspot?backupPassword:password).c_str());
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
  if(backupSsid.isEmpty()){status("Save hotspot on Mac first");return;}
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
      configured=true;Serial.println("CONFIG_SAVED");connectWifi();
    }else if(serialLine.length()<2048)serialLine+=c;
    else serialLine="";
  }
}
void setup() {
  // Waveshare ESP32-S3-ePaper-1.54 V2: keep battery power latched on.
  // Assert before USB logging and peripheral startup so PWR can be released.
  pinMode(17,OUTPUT);
  digitalWrite(17,HIGH);
  analogReadResolution(12);analogSetPinAttenuation(4,ADC_11db);
  sampleBattery();lastBatterySample=millis();lastUsbConnected=HWCDC::isPlugged();
  Serial.begin(115200);delay(1500);pinMode(0,INPUT_PULLUP);pinMode(SIDE_BUTTON_PIN,INPUT_PULLUP);
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
  xTaskCreatePinnedToCore(genTask,"gen",20480,nullptr,1,&genHandle,0);
  ws.onEvent(socketEvent);
  ssid=prefs.getString("ssid");password=prefs.getString("password");apiKey=prefs.getString("key");
  backupSsid=prefs.getString("backup_ssid");backupPassword=prefs.getString("backup_password");
  selectedHotspot=prefs.getBool("use_hotspot",false)&&!backupSsid.isEmpty();
  configured=!ssid.isEmpty()&&!apiKey.isEmpty();
  if(configured)connectWifi();else status("Enter setup on Mac");
  lastActivity=millis();
  Serial.printf("GEMINI_FIRMWARE_READY: transcribe-create-v1 model=%s voice=%s idle=120s mic_gain=18 wake=BOOT\n",
                TRANSCRIBE_MODEL,activeVoiceName.length()?activeVoiceName.c_str():DEFAULT_VOICE);
}
// The audio worker acknowledges only between complete I2S blocks.
// RAM and task state are retained; the wake press is consumed before normal BOOT handling.
void enterIdleSleep() {
  if(!sleepScreenReady)return;
  if(audioReady&&!holdAudio()){lastActivity=millis();return;}
  audioHold=true;
  if(audioReady){esp_codec_dev_close(mic);esp_codec_dev_close(speaker);xQueueReset(micQueue);xQueueReset(toneQueue);}
  if(esp_sntp_enabled())esp_sntp_stop();
  WiFi.disconnect(false,false);WiFi.mode(WIFI_OFF);
  while(xSemaphoreTake(sleepScreenReady,0)==pdTRUE){}
  status("Light sleep");
  bool screenReady=xSemaphoreTake(sleepScreenReady,pdMS_TO_TICKS(15000))==pdTRUE;
  esp_err_t result=ESP_FAIL;
  if(screenReady&&digitalRead(0)==HIGH){
    result=gpio_wakeup_enable(GPIO_NUM_0,GPIO_INTR_LOW_LEVEL);
    if(result==ESP_OK)result=esp_sleep_enable_gpio_wakeup();
    if(result==ESP_OK){
      gpio_hold_en(GPIO_NUM_17); // Preserve the battery power latch.
      Serial.println("LIGHT_SLEEP_ENTER: BOOT wakes device");Serial.flush();
      result=esp_light_sleep_start();
      gpio_hold_dis(GPIO_NUM_17);digitalWrite(17,HIGH);
    }
    gpio_wakeup_disable(GPIO_NUM_0);esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
  }
  if(audioReady){
    codecRate=16000;
    audioReady=openAudio(16000);
    if(!audioReady)Serial.println("ERROR: audio resume failed");
  }
  Serial.printf("LIGHT_SLEEP_EXIT: result=%d wake=%d\n",result,esp_sleep_get_wakeup_cause());
  // Keep the worker parked on failure to reopen audio rather than spin on closed handles.
  if(!audioReady&&audioPaused){Serial.println("ERROR: restarting to recover audio");ESP.restart();}
  audioHold=false;
  while(digitalRead(0)==LOW)delay(10);
  lastButton=stableButton=HIGH;buttonChanged=millis();bootLongHandled=false;wifiRestartPending=false;
  lastSide=stableSide=digitalRead(SIDE_BUTTON_PIN);sideChanged=millis();
  lastActivity=millis();lastBatterySample=millis();
  if(configured)connectWifi();else status("Enter setup on Mac");
}

void onBootShort() {
  if(appState==APP_GENERATING||appState==APP_PLAYING){buttonTone(440);cancelGeneration();return;}
  if(genBusy)return; // Cancel already requested; genTask is wrapping up.
  if(wanted){buttonTone(440);endTranscription();}
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

  bool button=digitalRead(0);
  if(button!=lastButton){lastActivity=millis();lastButton=button;buttonChanged=millis();}
  if(button!=stableButton&&millis()-buttonChanged>40){stableButton=button;
    if(button==LOW){bootPressed=millis();bootLongHandled=false;}
    else if(bootLongHandled&&wifiRestartPending){delay(100);ESP.restart();}
    else if(!bootLongHandled)onBootShort();
  }
  if(stableButton==LOW&&!bootLongHandled&&!genBusy&&millis()-bootPressed>=3000){bootLongHandled=true;selectOtherWifi();}
  bool side=digitalRead(SIDE_BUTTON_PIN);
  if(side!=lastSide){lastActivity=millis();lastSide=side;sideChanged=millis();}
  if(side!=stableSide&&millis()-sideChanged>40){stableSide=side;if(side==LOW)onSidePress();}
  if(wanted)ws.loop();
  if(wanted&&!live&&millis()-connectStarted>20000)stopSession("Connection timeout");
  if(live&&millis()-sessionStarted>5*60*1000)endTranscription();
  if(configured&&!wanted&&!genBusy) {
    if(WiFi.status()==WL_CONNECTED&&(lastScreen=="Connecting home Wi-Fi"||lastScreen=="Connecting hotspot"))status(selectedHotspot?"Hotspot ready. BOOT":"Home Wi-Fi ready. BOOT");
    else if(WiFi.status()!=WL_CONNECTED&&millis()-wifiAttempt>30000){status(selectedHotspot?"Hotspot failed. Hold BOOT for home":"Home Wi-Fi failed. Check setup");WiFi.reconnect();wifiAttempt=millis();}
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
     digitalRead(0)==HIGH&&millis()-lastActivity>=IDLE_SLEEP_MS)enterIdleSleep();
  delay(1);
}
