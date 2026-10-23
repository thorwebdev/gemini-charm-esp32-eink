#pragma once
// Transcript -> Gemini Flash-Lite router -> image / speech / song / voice design.
// Runs in genTask on core 0 so the UI loop stays responsive and BOOT can cancel.
// Every model call streams (SSE): speech starts playing while it is still being
// synthesized, and BOOT aborts a request mid-stream. Voice design is a plain
// resource call (POST /v1beta/voices) with no streaming mode.
#include <AudioGeneratorMP3.h>
#include <AudioFileSourcePROGMEM.h>
#include <AudioOutput.h>
#include "image_dither.h"

const char *ROUTER_MODEL="gemini-3.5-flash-lite";
// Flash-Lite Image only offers 1K; the panel needs 200x200, so the JPEG is downscaled on device.
const char *IMAGE_MODEL="gemini-3.1-flash-lite-image";
const char *IMAGE_SIZE_OPT="1K";
const char *TTS_MODEL="gemini-3.8-flash-lite-tts";
// Flash-Lite TTS cannot design prompted voices; voices designed with Flash TTS work with any TTS model.
const char *VOICE_DESIGN_MODEL="gemini-3.8-flash-tts";
const char *SONG_MODEL="lyria-3-clip-preview";
const char *DEFAULT_VOICE="Kore";
constexpr uint32_t TTS_RATE=24000; // Streaming TTS default: headerless L16, 24 kHz mono.
constexpr uint32_t PREROLL_MS=600; // Buffered speech before playback starts (absorbs network jitter).

struct Route { String action,prompt,style,voiceName,gender,language; };

// Receives a step.delta and the decoded bytes of its base64 "data" (empty for text deltas).
typedef std::function<bool(JsonVariant,PsBuffer&)> DeltaHandler;

// Appends media bytes, taking the buffer over (no copy) when it is the first chunk.
bool collectMedia(PsBuffer &dst,PsBuffer &chunk) {
  if(!dst.len){dst.take(chunk);return true;}
  return dst.append(chunk.data,chunk.len);
}

// POST /v1beta/interactions with stream:true. `onDelta` receives each step.delta
// (outside thought steps); return false to stop. Returns 200 on a clean stream.
int interactionStream(JsonDocument &req,DeltaHandler onDelta,uint32_t idleMs=65000) {
  req["stream"]=true;
  String body;serializeJson(req,body);
  bool apiError=false,thoughtStep=false;
  int code=geminiStream("/v1beta/interactions",body,[&](JsonDocument &ev,PsBuffer &blob)->bool{
    const char *type=ev["event_type"] | "";
    if(!strcmp(type,"error")){
      Serial.printf("GEMINI_API_ERROR: stream code=%s\n",ev["error"]["code"].as<String>().c_str());
      apiError=true;return false;
    }
    if(!strcmp(type,"step.start")){thoughtStep=ev["step"]["type"]=="thought";return !genCancel;}
    if(!strcmp(type,"step.delta")&&!thoughtStep)return onDelta(ev["delta"],blob);
    return !genCancel;
  },idleMs);
  return apiError?-110:code;
}

// ---------- Playback helpers (feed the shared ring at the codec's current rate) ----------

// Linear-interpolating mono resampler; used only if the codec cannot switch to the source rate.
struct RingSink {
  uint32_t inRate=16000,outRate=16000;
  float pos=0,step=1;int16_t prev=0;
  int16_t buf[512];int count=0;
  void begin(uint32_t rate){
    flush();
    inRate=rate;
    if(rate!=codecRate&&!setCodecRate(rate))Serial.printf("PLAYBACK: codec stays at %u Hz, resampling\n",(unsigned)codecRate);
    outRate=codecRate;step=(float)inRate/outRate;pos=0;prev=0;
  }
  void emit(int16_t s){buf[count++]=s;if(count==512)flush();}
  void push(int16_t s){
    if(inRate==outRate){emit(s);return;}
    while(pos<=1.0f){emit((int16_t)(prev+(s-prev)*pos));pos+=step;}
    pos-=1.0f;prev=s;
  }
  void flush(){if(count)enqueueAudioWait(buf,count);count=0;}
};

void playPcm(const uint8_t *data,size_t bytes,uint32_t rate,int channels) {
  RingSink sink;sink.begin(rate);
  size_t frame=2*channels;
  for(size_t i=0;i+frame<=bytes&&!genCancel;i+=frame){
    int32_t mixed=0;
    for(int c=0;c<channels;c++){int16_t s;memcpy(&s,data+i+2*c,2);mixed+=s;}
    sink.push((int16_t)(mixed/channels));
  }
  sink.flush();
}

bool parseWav(const uint8_t *d,size_t n,uint32_t &rate,int &channels,const uint8_t *&pcm,size_t &pcmLen) {
  if(n<12||memcmp(d,"RIFF",4)||memcmp(d+8,"WAVE",4))return false;
  size_t p=12;int bits=0;rate=0;channels=0;
  while(p+8<=n){
    uint32_t size=d[p+4]|d[p+5]<<8|d[p+6]<<16|(uint32_t)d[p+7]<<24;
    const uint8_t *chunk=d+p+8;
    if(!memcmp(d+p,"fmt ",4)&&size>=16&&p+8+16<=n){
      channels=chunk[2]|chunk[3]<<8;rate=chunk[4]|chunk[5]<<8|chunk[6]<<16|(uint32_t)chunk[7]<<24;bits=chunk[14]|chunk[15]<<8;
    }else if(!memcmp(d+p,"data",4)){
      pcm=chunk;pcmLen=min((size_t)size,n-(p+8));
      return rate>0&&bits==16&&channels>0;
    }
    if(size>n)break;
    p+=8+size+(size&1);
  }
  return false;
}

uint32_t mimeRate(const char *mime,uint32_t fallback) {
  const char *r=mime?strstr(mime,"rate="):nullptr;
  return r?atoi(r+5):fallback;
}

// Plays WAV or raw L16 audio held in memory.
bool playAudioPart(const PsBuffer &audio,const char *mime) {
  if(mime&&strstr(mime,"wav")){
    uint32_t rate;int channels;const uint8_t *pcm;size_t len;
    if(!parseWav(audio.data,audio.len,rate,channels,pcm,len)){Serial.println("PLAYBACK: bad WAV");return false;}
    Serial.printf("PLAYBACK: wav %u Hz ch=%d bytes=%u\n",(unsigned)rate,channels,(unsigned)len);
    playPcm(pcm,len,rate,channels);return true;
  }
  uint32_t rate=mimeRate(mime,16000);
  Serial.printf("PLAYBACK: pcm %u Hz bytes=%u\n",(unsigned)rate,(unsigned)audio.len);
  playPcm(audio.data,audio.len,rate,1);return true;
}

class Mp3RingOutput : public AudioOutput {
public:
  RingSink sink;bool started=false;
  bool SetRate(int hz) override {
    if(!started||(uint32_t)hz!=sink.inRate){Serial.printf("PLAYBACK: mp3 %d Hz\n",hz);sink.begin(hz);started=true;}
    hertz=hz;return true;
  }
  bool SetChannels(int c) override {channels=c;return true;}
  bool begin() override {return true;}
  bool ConsumeSample(int16_t sample[2]) override {
    if(genCancel)return true;
    sink.push(channels==2?(int16_t)(((int32_t)sample[0]+sample[1])/2):sample[0]);
    return true;
  }
  bool stop() override {sink.flush();return true;}
};

bool playMp3(const PsBuffer &mp3data) {
  AudioFileSourcePROGMEM source(mp3data.data,mp3data.len);
  Mp3RingOutput output;
  AudioGeneratorMP3 mp3;
  if(!mp3.begin(&source,&output)){Serial.println("PLAYBACK: mp3 begin failed");return false;}
  uint32_t frames=0;
  while(mp3.isRunning()&&!genCancel){
    if(!mp3.loop()){mp3.stop();break;}
    if(++frames%64==0)vTaskDelay(1); // Let the idle task run while the ring fills.
  }
  if(mp3.isRunning())mp3.stop();
  output.sink.flush();
  return true;
}

// Waits for the ring to drain (or BOOT), then returns the codec to the 16 kHz mic rate.
void finishPlayback() {
  audioGate=false;
  waitPlaybackDone();
  if(genCancel)clearAudio();
  setCodecRate(16000);
}

// ---------- Router ----------

String routerInstruction() {
  String voice=activeVoiceName.length()?activeVoiceName:String("the default voice (Kore)");
  return String(
    "You route dictated requests on a tiny voice device with a 200x200 black-and-white e-paper screen and a speaker. "
    "The user text is a speech transcript. Choose exactly one action:\n"
    "- image: the user wants a picture, drawing, illustration, poster or to see something. prompt = a vivid image prompt for a bold, "
    "high-contrast black-and-white illustration with a simple, centered composition, thick lines and minimal fine detail.\n"
    "- song: the user wants music, a song, melody, jingle or tune. prompt = a music prompt keeping the requested genre, mood, "
    "instruments, language and any lyrics the user dictated.\n"
    "- voice: the user wants to create, design or change to a new voice persona. prompt = the user's own voice description, "
    "kept as close to their words as possible: you may only drop filler like 'design the voice of'. NEVER remove, generalize or "
    "translate the spoken language, dialect, regional origin or accent (e.g. keep 'speaking Taiwanese Mandarin' or 'Sligo Irish "
    "accent' word for word), and keep age, gender, emotion and delivery. If the language is implied by the origin, state it "
    "explicitly (e.g. 'speaking Taiwanese Mandarin'). Also set voice_name (max 24 characters), gender, and language_code = the "
    "BCP-47 code of the language the voice should SPEAK, with region (e.g. zh-TW, en-IE, ja-JP), not the transcript language.\n"
    "- voice_reset: the user wants the normal or default voice back. prompt = a short spoken confirmation in the user's language.\n"
    "- speech: anything else (questions, chat, requests). prompt = your spoken reply: helpful, natural, 1-3 short sentences, "
    "in the user's language, no markdown or lists. style = a short delivery style such as 'warm and upbeat'.\n"
    "The current speaking voice is ")+voice+".";
}

bool routePrompt(const String &prompt,Route &route) {
  JsonDocument req;
  req["systemInstruction"]["parts"][0]["text"]=routerInstruction();
  req["contents"][0]["role"]="user";
  req["contents"][0]["parts"][0]["text"]=prompt;
  auto config=req["generationConfig"];
  config["responseMimeType"]="application/json";
  auto schema=config["responseSchema"];
  schema["type"]="OBJECT";
  auto props=schema["properties"];
  props["action"]["type"]="STRING";
  for(const char *a:{"image","speech","song","voice","voice_reset"})props["action"]["enum"].add(a);
  for(const char *field:{"prompt","style","voice_name","language_code"})props[field]["type"]="STRING";
  props["gender"]["type"]="STRING";
  for(const char *g:{"male","female","neutral"})props["gender"]["enum"].add(g);
  schema["required"].add("action");schema["required"].add("prompt");
  String body;serializeJson(req,body);
  // The JSON answer streams in as text chunks; concatenate the non-thought parts.
  String text;bool apiError=false;
  int code=geminiStream(String("/v1beta/models/")+ROUTER_MODEL+":streamGenerateContent?alt=sse",body,[&](JsonDocument &ev,PsBuffer&)->bool{
    if(ev["error"].is<JsonObject>()){
      Serial.printf("GEMINI_API_ERROR: stream code=%d\n",ev["error"]["code"].as<int>());
      apiError=true;return false;
    }
    for(JsonVariant part:ev["candidates"][0]["content"]["parts"].as<JsonArray>())
      if(!(part["thought"] | false)&&part["text"].is<const char*>())text+=part["text"].as<const char*>();
    return !genCancel;
  },30000);
  if(code!=200||apiError||text.isEmpty())return false;
  JsonDocument out;
  if(deserializeJson(out,text))return false;
  route.action=out["action"] | "";route.prompt=out["prompt"] | "";route.style=out["style"] | "";
  route.voiceName=out["voice_name"] | "";route.gender=out["gender"] | "";route.language=out["language_code"] | "";
  return route.action.length()&&route.prompt.length();
}

// ---------- Outcomes ----------

void genFail(const String &message) {appState=APP_READY;status(message);}
void genCancelled() {clearAudio();setCodecRate(16000);appState=APP_READY;status("Cancelled");}
void failOrCancel(const char *message) {if(genCancel)genCancelled();else genFail(message);}
void showBusy(const char *caption,const String &detail) {showScreen(MODE_BUSY,caption,detail,"BOOT: cancel",false);}
const char *RESULT_FOOTER="BOOT: new | Side: again";

void doImage(const Route &route) {
  showBusy("Drawing image...",route.prompt);
  JsonDocument req;
  req["model"]=IMAGE_MODEL;req["input"]=route.prompt;
  auto format=req["response_format"];
  format["type"]="image";format["mime_type"]="image/jpeg";format["aspect_ratio"]="1:1";format["image_size"]=IMAGE_SIZE_OPT;
  PsBuffer jpg;bool bad=false;String mime;
  uint32_t started=millis();
  int code=interactionStream(req,[&](JsonVariant d,PsBuffer &bytes)->bool{
    if(d["type"]=="image"&&bytes.len){
      if(mime.isEmpty())mime=d["mime_type"] | "";
      if(!collectMedia(jpg,bytes))bad=true;
    }
    return !genCancel&&!bad;
  });
  if(genCancel){genCancelled();return;}
  if(code!=200&&!jpg.len){genFail("Image failed. Try again");return;}
  if(bad||!jpg.len){genFail("No image returned");return;}
  Serial.printf("IMAGE: mime=%s %u bytes magic=%02x%02x request=%ums\n",mime.c_str(),(unsigned)jpg.len,jpg.data[0],jpg.data[1],(unsigned)(millis()-started));
  static uint8_t decoded[IMAGE_BYTES];
  uint32_t decodeStart=millis();
  if(!jpegToBitmap(jpg.data,jpg.len,decoded)){genFail("Image decode failed");return;}
  Serial.printf("IMAGE: decode+dither=%ums\n",(unsigned)(millis()-decodeStart));
  memcpy(imageBitmap,decoded,IMAGE_BYTES);
  appState=APP_RESULT;
  showScreen(MODE_IMAGE,"",route.prompt,"",false);
}

// Streams L16 audio straight into the playback ring; playback starts after a short pre-roll.
void doSpeech(const String &text,const String &style) {
  showBusy("Recording voice...",text);
  JsonDocument req;
  req["model"]=TTS_MODEL;
  auto content=req["input"][0];
  content["type"]="user_input";
  content["content"][0]["type"]="text";content["content"][0]["text"]=text;
  if(style.length()){content["content"][0]["annotations"][0]["type"]="speech_metadata";content["content"][0]["annotations"][0]["style"]=style;}
  auto format=req["response_format"];
  format["type"]="audio";format["mime_type"]="audio/l16";
  req["generation_config"]["speech_config"][0]["voice"]=activeVoiceId.length()?activeVoiceId:String(DEFAULT_VOICE);
  RingSink sink;
  bool started=false,haveOdd=false;uint8_t odd=0;
  uint32_t rate=TTS_RATE;size_t samples=0;
  int code=interactionStream(req,[&](JsonVariant d,PsBuffer &chunk)->bool{
    if(d["type"]!="audio")return !genCancel;
    if(!started){
      const char *mime=d["mime_type"] | "";
      rate=mimeRate(mime,d["sample_rate"] | TTS_RATE);
      audioGate=true;sink.begin(rate);started=true;
      Serial.printf("PLAYBACK: streaming mime=%s rate=%u codec=%u\n",mime,(unsigned)rate,(unsigned)codecRate);
      appState=APP_PLAYING;
      showScreen(MODE_PANEL,"SPEECH",text,"BOOT: stop",true);
    }
    size_t i=0;
    if(haveOdd&&chunk.len){sink.push((int16_t)(odd|chunk.data[0]<<8));i=1;haveOdd=false;samples++;}
    for(;i+1<chunk.len;i+=2){sink.push((int16_t)(chunk.data[i]|chunk.data[i+1]<<8));samples++;}
    if(i<chunk.len){odd=chunk.data[i];haveOdd=true;}
    if(audioGate&&samples>=rate*PREROLL_MS/1000){sink.flush();audioGate=false;}
    return !genCancel;
  });
  sink.flush();audioGate=false;
  if(genCancel){genCancelled();return;}
  if(!started){genFail(code==200?"No audio returned":"Speech failed. Try again");return;}
  Serial.printf("SPEECH: %u samples, stream=%d\n",(unsigned)samples,code);
  finishPlayback();
  if(genCancel){genCancelled();return;}
  appState=APP_RESULT;
  showScreen(MODE_PANEL,"SPEECH",text,RESULT_FOOTER,true);
}

void doSong(const Route &route) {
  showBusy("Composing song...",route.prompt);
  JsonDocument req;
  req["model"]=SONG_MODEL;req["input"]=route.prompt;
  PsBuffer audio;String lyrics,mime;bool bad=false;
  int code=interactionStream(req,[&](JsonVariant d,PsBuffer &bytes)->bool{
    if(d["type"]=="audio"&&bytes.len){
      if(mime.isEmpty())mime=d["mime_type"] | "";
      if(!collectMedia(audio,bytes))bad=true;
    }else if(d["type"]=="text"){
      const char *t=d["text"];if(t)lyrics+=t;
    }
    return !genCancel&&!bad;
  },90000);
  if(genCancel){genCancelled();return;}
  if(code!=200&&!audio.len){genFail("Song failed. Try again");return;}
  if(bad||!audio.len){genFail("No song returned");return;}
  Serial.printf("SONG: mime=%s bytes=%u\n",mime.length()?mime.c_str():"?",(unsigned)audio.len);
  lyrics.trim();
  if(lyrics.isEmpty())lyrics=route.prompt;
  appState=APP_PLAYING;
  showScreen(MODE_PANEL,"SONG",lyrics,"BOOT: stop",true);
  bool ok=mime.indexOf("wav")>=0||mime.indexOf("l16")>=0?playAudioPart(audio,mime.c_str()):playMp3(audio);
  audio.release();
  if(!ok){setCodecRate(16000);genFail("Song playback failed");return;}
  finishPlayback();
  if(genCancel){genCancelled();return;}
  appState=APP_RESULT;
  showScreen(MODE_PANEL,"SONG",lyrics,RESULT_FOOTER,true);
}

void doVoiceDesign(const Route &route) {
  showBusy("Designing voice...",route.prompt);
  String name=route.voiceName.length()?route.voiceName.substring(0,24):String("Custom voice");
  JsonDocument req;
  req["store"]=true;
  auto voice=req["voice"];
  voice["model"]=VOICE_DESIGN_MODEL;voice["type"]="prompted";voice["display_name"]=name;
  if(route.gender=="male"||route.gender=="female")voice["gender"]=route.gender;
  voice["language_code"]=route.language.length()?route.language:String("en-US");
  voice["prompted"]["input"]=route.prompt;
  String body;serializeJson(req,body);
  PsBuffer sample;String id,mime;
  {
    PsBuffer raw;
    int code=geminiHttp("POST","/v1beta/voices",&body,raw);
    if(code!=200){logApiError(raw);failOrCancel("Voice design failed");return;}
    // The base64 WAV preview is MBs: decode it in place and parse only the rest.
    String rest;
    if(!takeBase64Field(raw,"sample_audio",sample,rest)){
      Serial.println("VOICE: no sample_audio in response");
      rest=String((const char*)raw.data);
    }
    raw.release();
    JsonDocument res(&psramJson);
    DeserializationError err=deserializeJson(res,rest);
    if(err){Serial.printf("VOICE: bad JSON (%s, %u bytes)\n",err.c_str(),rest.length());genFail("Voice design failed");return;}
    // The Voice may be returned directly, wrapped (`voice` / LRO `response`), or as an array.
    JsonVariant top=res.is<JsonArray>()?res[0].as<JsonVariant>():res.as<JsonVariant>();
    bool direct=top["id"].is<const char*>()||top["name"].is<const char*>();
    JsonVariant v=direct?top:top["voice"].is<JsonObject>()?top["voice"].as<JsonVariant>():
                  top["response"].is<JsonObject>()?top["response"].as<JsonVariant>():top;
    id=v["id"] | "";
    if(id.isEmpty())id=v["voice_id"] | "";
    if(id.isEmpty()){String full=v["name"] | "";id=full.substring(full.lastIndexOf('/')+1);}
    name=v["display_name"] | name.c_str();
    mime=v["sample_audio"]["mime_type"] | "audio/wav";
    Serial.printf("VOICE: response parsed, sample=%u bytes %s\n",(unsigned)sample.len,mime.c_str());
    if(id.isEmpty()){
      // Log only the response shape (key names and types), never values.
      String shape;
      if(res.is<JsonObject>())for(JsonPair kv:res.as<JsonObject>()){
        shape+=String(kv.key().c_str())+(kv.value().is<JsonObject>()?"{":kv.value().is<JsonArray>()?"[":"");
        if(kv.value().is<JsonObject>()){
          for(JsonPair in:kv.value().as<JsonObject>())shape+=String(in.key().c_str())+",";
          shape+="}";
        }
        shape+=" ";
      }else shape=res.is<JsonArray>()?"<array>":"<not an object>";
      Serial.printf("VOICE: no id in response; keys: %s\n",shape.c_str());
    }
  }
  if(id.isEmpty()){genFail("Voice design failed");return;}
  String oldId=activeVoiceId;
  setActiveVoice(id,name);
  Serial.printf("VOICE: created id=%s name=%s\n",id.c_str(),name.c_str());
  String panel=name+"\n\n"+route.prompt;
  if(sample.len&&!genCancel){
    appState=APP_PLAYING;
    showScreen(MODE_PANEL,"NEW VOICE",panel,"BOOT: stop",true);
    playAudioPart(sample,mime.c_str());
    finishPlayback();
  }
  sample.release();
  // Keep the project under the stored-voice limit: drop the voice this one replaces.
  if(oldId.length()&&oldId!=id){
    PsBuffer ignored;
    int code=geminiHttp("DELETE","/v1beta/voices/"+oldId,nullptr,ignored,15000);
    Serial.printf("VOICE: deleted previous voice -> %d\n",code);
  }
  if(genCancel)clearAudio();
  appState=APP_RESULT;
  showScreen(MODE_PANEL,"NEW VOICE",panel,RESULT_FOOTER,true);
}

void runGeneration(const String &prompt) {
  showBusy("Thinking...",prompt);
  Route route;
  if(!routePrompt(prompt,route)){failOrCancel("Router failed. Try again");return;}
  Serial.printf("ROUTER: action=%s prompt=%s\n",route.action.c_str(),route.prompt.c_str());
  if(route.action=="voice")Serial.printf("ROUTER: voice name=%s gender=%s language=%s\n",route.voiceName.c_str(),
                                         route.gender.c_str(),route.language.c_str());
  if(genCancel){genCancelled();return;}
  if(route.action=="image")doImage(route);
  else if(route.action=="song")doSong(route);
  else if(route.action=="voice")doVoiceDesign(route);
  else if(route.action=="voice_reset"){
    setActiveVoice("","");Serial.println("VOICE: reset to default");
    doSpeech(route.prompt,"");
  }
  else doSpeech(route.prompt,route.style);
}

void genTask(void*) {
  for(;;){
    ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
    String prompt=genPrompt;
    runGeneration(prompt);
    genBusy=false;
  }
}
