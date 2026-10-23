#pragma once
// HTTPS client for Gemini REST calls. Responses (up to several MB of base64 media)
// are streamed into PSRAM and parsed with a PSRAM-backed ArduinoJson allocator so the
// internal heap stays free for TLS, Wi-Fi and the display.
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>
#include <functional>

struct PsBuffer {
  uint8_t *data=nullptr; size_t len=0,cap=0;
  PsBuffer()=default;
  PsBuffer(const PsBuffer&)=delete; PsBuffer &operator=(const PsBuffer&)=delete;
  ~PsBuffer(){release();}
  void release(){if(data)free(data);data=nullptr;len=cap=0;}
  bool reserve(size_t n){
    if(n<=cap)return true;
    size_t next=max(n,cap?cap+cap/2:(size_t)65536); // 1.5x keeps realloc peaks lower for multi-MB media.
    uint8_t *grown=(uint8_t*)ps_realloc(data,next+1);
    if(!grown)return false;
    data=grown;cap=next;return true;
  }
  bool append(const uint8_t *src,size_t n){
    if(!reserve(len+n))return false;
    memcpy(data+len,src,n);len+=n;data[len]=0;return true;
  }
  bool push(uint8_t c){
    if(len+1>cap&&!reserve(len+1))return false;
    data[len++]=c;return true;
  }
  // Moves o's storage into this buffer without copying.
  void take(PsBuffer &o){release();data=o.data;len=o.len;cap=o.cap;o.data=nullptr;o.len=o.cap=0;}
};

// Incremental base64 decoder; skips padding, whitespace and JSON escape backslashes.
struct Base64Decoder {
  uint32_t acc=0;int bits=0;bool overflow=false;
  void reset(){acc=0;bits=0;}
  void feed(uint8_t c,PsBuffer &out){
    int v=c>='A'&&c<='Z'?c-'A':c>='a'&&c<='z'?c-'a'+26:c>='0'&&c<='9'?c-'0'+52:c=='+'||c=='-'?62:c=='/'||c=='_'?63:-1;
    if(v<0)return;
    acc=(acc<<6)|v;bits+=6;
    if(bits>=8){bits-=8;if(!out.push((uint8_t)(acc>>bits)))overflow=true;}
  }
};

struct PsramJsonAllocator : ArduinoJson::Allocator {
  void *allocate(size_t n) override {return ps_malloc(n);}
  void deallocate(void *p) override {free(p);}
  void *reallocate(void *p,size_t n) override {return ps_realloc(p,n);}
};
PsramJsonAllocator psramJson;

typedef std::function<bool(const uint8_t*,size_t)> BodySink;

// Reads an HTTP response body (chunked, sized or close-delimited) and passes it to `sink`,
// which returns false to stop early. Sleeps while waiting for data: HTTPClient::writeToStream
// busy-waits, which starves IDLE on this core and trips the task watchdog on slow responses.
// Requires the "Transfer-Encoding" header to have been collected. Aborts on genCancel.
// Returns 200, or -104 cancelled, -105 total timeout, -106 chunk read failure, -107 idle timeout.
int readHttpBody(HTTPClient &http,const BodySink &sink,size_t &total,uint32_t idleMs=65000,uint32_t totalMs=240000) {
  total=0;
  NetworkClient *stream=http.getStreamPtr();
  if(!stream)return -106;
  bool chunked=http.header("Transfer-Encoding").equalsIgnoreCase("chunked");
  int64_t remaining=http.getSize(); // -1 when unknown (chunked or close-delimited)
  uint8_t buf[2048];
  size_t chunkLeft=0;
  uint32_t started=millis(),lastData=millis();
  // Reads one chunk-size line of the chunked transfer encoding.
  auto readLine=[&](String &line)->bool{
    line="";uint32_t t=millis();
    while(millis()-t<idleMs&&!genCancel){
      if(stream->available()){
        char c=stream->read();
        if(c=='\n'){line.trim();return true;}
        if(line.length()<32)line+=c;
      }else if(!stream->connected())return false;
      else delay(2);
    }
    return false;
  };
  while(remaining!=0){
    if(genCancel)return -104;
    if(millis()-started>totalMs)return -105;
    if(chunked&&chunkLeft==0){
      String line;
      if(!readLine(line))return genCancel?-104:-106;
      if(line.isEmpty())continue; // CRLF that ends the previous chunk.
      chunkLeft=strtoul(line.c_str(),nullptr,16);
      if(chunkLeft==0)break; // Last chunk.
      continue;
    }
    int avail=stream->available();
    if(avail<=0){
      if(!stream->connected())break; // Close-delimited end of stream.
      if(millis()-lastData>idleMs)return -107;
      delay(2);continue;
    }
    size_t want=min((size_t)avail,sizeof(buf));
    if(chunked)want=min(want,chunkLeft);
    if(remaining>0)want=min(want,(size_t)remaining);
    int n=stream->read(buf,want);
    if(n<=0){
      if(millis()-lastData>idleMs)return -107;
      delay(1);continue;
    }
    lastData=millis();total+=n;
    if(chunked)chunkLeft-=n;
    if(remaining>0)remaining-=n;
    if(!sink(buf,n))break;
  }
  return 200;
}

// Collects a body into `out`, up to `limit` bytes. Returns 200, -102 if too large, or a
// negative readHttpBody code.
int readHttpBodyTo(HTTPClient &http,PsBuffer &out,size_t limit,uint32_t idleMs=65000) {
  bool overflow=false;size_t total=0;
  int r=readHttpBody(http,[&](const uint8_t *p,size_t n){
    if(out.len+n>limit||!out.append(p,n)){overflow=true;return false;}
    return true;
  },total,idleMs);
  return overflow?-102:r;
}

constexpr size_t GEMINI_MAX_RESPONSE=6*1024*1024;

// Returns the HTTP status (or a negative HTTPClient error). Never logs the key or body.
int geminiHttp(const char *method,const String &path,const String *body,PsBuffer &out,uint16_t timeoutMs=65000) {
  out.release();
  if(WiFi.status()!=WL_CONNECTED)return -100;
  NetworkClientSecure client;client.setCACert(GOOGLE_ROOTS);client.setHandshakeTimeout(15);
  HTTPClient http;http.setConnectTimeout(10000);http.setTimeout(timeoutMs);
  http.setReuse(false);
  if(!http.begin(client,"https://generativelanguage.googleapis.com"+path))return -101;
  http.addHeader("x-goog-api-key",apiKey);
  http.addHeader("Accept","application/json");
  if(body)http.addHeader("Content-Type","application/json");
  const char *keys[]={"Transfer-Encoding"};http.collectHeaders(keys,1);
  int code=body?http.sendRequest(method,*body):http.sendRequest(method);
  if(code>0){
    int r=readHttpBodyTo(http,out,GEMINI_MAX_RESPONSE,timeoutMs);
    if(r==-102)Serial.println("GEMINI_HTTP: response too large");
    if(r<0)code=r;
  }
  http.end();
  Serial.printf("GEMINI_HTTP: %s %s -> %d (%u bytes)\n",method,path.c_str(),code,(unsigned)out.len);
  return code;
}

// Logs the API error code, status and a truncated message (API error messages never
// contain the key). Accepts plain JSON, a JSON array, or an SSE-wrapped error body.
void logApiError(const PsBuffer &body) {
  if(!body.len){Serial.println("GEMINI_API_ERROR: empty body");return;}
  const char *start=(const char*)memchr(body.data,'{',body.len);
  if(!start){Serial.printf("GEMINI_API_ERROR: non-JSON body (%u bytes)\n",(unsigned)body.len);return;}
  JsonDocument doc(&psramJson);
  if(deserializeJson(doc,start,body.len-(start-(const char*)body.data))){
    Serial.printf("GEMINI_API_ERROR: unparsed body (%u bytes)\n",(unsigned)body.len);return;
  }
  JsonVariant err=doc["error"];
  String message=err["message"] | "";
  if(message.length()>240)message=message.substring(0,240)+"...";
  Serial.printf("GEMINI_API_ERROR: code=%s status=%s message=%s\n",err["code"].as<String>().c_str(),
                err["status"] | "",message.c_str());
}

int geminiJson(const char *method,const String &path,const String *body,JsonDocument &doc,uint16_t timeoutMs=65000) {
  PsBuffer out;
  int code=geminiHttp(method,path,body,out,timeoutMs);
  if(code<200||code>=300){logApiError(out);return code;}
  if(out.len&&deserializeJson(doc,(const char*)out.data,out.len)){Serial.println("GEMINI_HTTP: bad JSON");return -103;}
  return code;
}

bool base64ToPs(const char *encoded,PsBuffer &out) {
  out.release();
  if(!encoded)return false;
  size_t n=strlen(encoded),olen=0;
  if(!out.reserve(n/4*3+4))return false;
  if(mbedtls_base64_decode(out.data,out.cap,&olen,(const uint8_t*)encoded,n)!=0){out.release();return false;}
  out.len=olen;return true;
}

// Finds the string value of `"data"` after `"<parentKey>"` in a raw JSON body and decodes it
// in place (output never overtakes input), then moves the storage into `out`. `rest` receives
// the JSON with that value emptied (small, safe to parse). Avoids holding multi-MB base64
// twice in PSRAM, which made a full deserializeJson of a voice preview run out of memory.
bool takeBase64Field(PsBuffer &raw,const char *parentKey,PsBuffer &out,String &rest) {
  out.release();rest="";
  if(!raw.data||!raw.len)return false;
  String parent=String("\"")+parentKey+"\"";
  char *p=strstr((char*)raw.data,parent.c_str());
  if(!p||!(p=strstr(p,"\"data\"")))return false;
  p+=6;
  while(*p==' '||*p=='\n'||*p=='\r'||*p=='\t')p++;
  if(*p++!=':')return false;
  while(*p==' '||*p=='\n'||*p=='\r'||*p=='\t')p++;
  if(*p++!='"')return false;
  char *end=strchr(p,'"');
  if(!end)return false;
  rest.reserve((p-(char*)raw.data)+strlen(end)+1);
  rest.concat((const char*)raw.data,p-(char*)raw.data);
  rest.concat(end);
  uint8_t *dst=raw.data;uint32_t acc=0;int bits=0;size_t w=0;
  for(const uint8_t *s=(const uint8_t*)p;s<(const uint8_t*)end;s++){
    uint8_t c=*s;
    int v=c>='A'&&c<='Z'?c-'A':c>='a'&&c<='z'?c-'a'+26:c>='0'&&c<='9'?c-'0'+52:c=='+'||c=='-'?62:c=='/'||c=='_'?63:-1;
    if(v<0)continue; // Padding, whitespace, JSON escape backslashes.
    acc=(acc<<6)|v;bits+=6;
    if(bits>=8){bits-=8;dst[w++]=(uint8_t)(acc>>bits);}
  }
  if(!w)return false;
  raw.len=w;out.take(raw);
  return true;
}

// ---------- Streaming (server-sent events) ----------

// Receives each `data:` payload parsed as JSON plus the decoded bytes of its base64
// "data" field (empty if none; the handler may take() it). Return false to stop.
typedef std::function<bool(JsonDocument&,PsBuffer&)> SseHandler;

// Incremental SSE parser. JSON text of each event is buffered, except the value of any
// "data" string key: those base64 bytes are decoded on the fly into `blob` and the JSON
// keeps an empty string, so a multi-MB image never exists as text or a JSON node.
class SseParser {
public:
  SseHandler handler;
  bool done=false,aborted=false,overflow=false;
  uint32_t events=0;
  void feed(const uint8_t *p,size_t n){for(size_t i=0;i<n&&!done;i++)put(p[i]);}
  void finish(){if(!done)dispatch();}
private:
  PsBuffer data,blob;
  Base64Decoder b64;
  char prefix[5];uint8_t prefixLen=0,mode=0; // mode: 0 = field name, 1 = data, 2 = skip
  bool skipSpace=false,inBlob=false;
  uint8_t keyMatch=0,keyPhase=0; // Matches `"data"`, optional spaces, ':', spaces, '"'.
  // JSON string tracking: values longer than MAX_STRING are cut (escape-aware) so fields we
  // never read, like multi-MB thought signatures, cannot exhaust memory.
  static constexpr size_t MAX_STRING=16384;
  bool inStr=false,esc=false,dropping=false;uint8_t uLeft=0;size_t strLen=0;
  void push(uint8_t c){if(!data.push(c)){overflow=true;done=true;}}
  void matchKey(uint8_t c){
    static const char KEY[]="\"data\"";
    if(keyPhase==0){
      keyMatch=c==KEY[keyMatch]?keyMatch+1:(c=='"'?1:0);
      if(keyMatch==6){keyPhase=1;keyMatch=0;}
    }else if(c==' '){
    }else if(keyPhase==1&&c==':')keyPhase=2;
    else if(keyPhase==2&&c=='"'){inBlob=true;b64.reset();keyPhase=0;}
    else {keyPhase=0;keyMatch=c=='"'?1:0;}
  }
  void put(uint8_t c){
    if(c=='\n'){
      if(mode==1){if(!inBlob&&data.len&&data.data[data.len-1]=='\r')data.len--;}
      else if(mode==0&&(prefixLen==0||(prefixLen==1&&prefix[0]=='\r')))dispatch(); // Blank line ends an event.
      prefixLen=0;mode=0;return;
    }
    if(mode==2)return;
    if(mode==1){
      if(skipSpace){skipSpace=false;if(c==' ')return;}
      if(inBlob){
        if(c=='"'){inBlob=false;inStr=false;push('"');}
        else {b64.feed(c,blob);if(b64.overflow){overflow=true;done=true;}}
        return;
      }
      if(inStr){
        if(dropping){
          if(esc)esc=false;
          else if(c=='\\')esc=true;
          else if(c=='"'){inStr=dropping=false;push('"');matchKey(c);}
          return;
        }
        if(uLeft){uLeft--;push(c);return;}
        if(esc){esc=false;if(c=='u')uLeft=4;push(c);return;}
        if(c=='"'){inStr=false;push(c);matchKey(c);return;}
        if(strLen>=MAX_STRING){dropping=true;esc=c=='\\';return;}
        if(c=='\\')esc=true;
        push(c);strLen++;matchKey(c);
        return;
      }
      if(c=='"'){inStr=true;strLen=0;}
      push(c);matchKey(c);
      return;
    }
    prefix[prefixLen++]=c;
    if(prefixLen==5){
      if(memcmp(prefix,"data:",5))mode=2;
      else {mode=1;skipSpace=true;if(data.len&&!inBlob)push('\n');}
    }
  }
  void dispatch(){
    if(!data.len){blob.len=0;return;}
    if(data.len==6&&!memcmp(data.data,"[DONE]",6)){done=true;data.len=0;return;}
    events++;
    {
      JsonDocument doc(&psramJson);
      DeserializationError err=deserializeJson(doc,(const char*)data.data,data.len);
      if(err)Serial.printf("GEMINI_SSE: bad event JSON (%s, %u bytes)\n",err.c_str(),(unsigned)data.len);
      else if(!handler(doc,blob)){aborted=true;done=true;}
    }
    data.len=0;blob.len=0;inBlob=false;keyMatch=keyPhase=0; // Keep capacity for the next event.
    inStr=esc=dropping=false;uLeft=0;strLen=0;
  }
};

// POSTs `body` with streaming enabled and feeds each event to `handler` as it arrives.
// Aborts on genCancel (BOOT). Returns the HTTP status, or a negative transport/abort code.
int geminiStream(const String &path,const String &body,SseHandler handler,uint32_t idleMs=65000,uint32_t totalMs=240000) {
  if(WiFi.status()!=WL_CONNECTED)return -100;
  NetworkClientSecure client;client.setCACert(GOOGLE_ROOTS);client.setHandshakeTimeout(15);
  HTTPClient http;http.setConnectTimeout(10000);http.setTimeout((uint16_t)min(idleMs,(uint32_t)65000));
  http.setReuse(false);
  if(!http.begin(client,"https://generativelanguage.googleapis.com"+path))return -101;
  http.addHeader("x-goog-api-key",apiKey);
  http.addHeader("Accept","text/event-stream");
  http.addHeader("Content-Type","application/json");
  const char *keys[]={"Transfer-Encoding"};http.collectHeaders(keys,1);
  uint32_t t0=millis();
  Serial.printf("GEMINI_SSE: connecting (heap=%u)\n",ESP.getFreeHeap());
  int code=http.POST(body);
  Serial.printf("GEMINI_SSE: headers -> %d after %lu ms\n",code,(unsigned long)(millis()-t0));
  if(code!=200){
    PsBuffer err;
    if(code>0)readHttpBodyTo(http,err,65536,15000);
    else {
      char reason[96]={};client.lastError(reason,sizeof(reason));
      IPAddress ip;int dns=WiFi.hostByName("generativelanguage.googleapis.com",ip);
      Serial.printf("GEMINI_SSE: transport error: %s (dns=%d %s, heap=%u)\n",reason,dns,ip.toString().c_str(),ESP.getFreeHeap());
    }
    http.end();
    Serial.printf("GEMINI_SSE: POST %s -> %d\n",path.c_str(),code);
    logApiError(err);
    return code;
  }
  SseParser parser;parser.handler=handler;
  size_t total=0;
  int result=readHttpBody(http,[&](const uint8_t *p,size_t n){parser.feed(p,n);return !parser.done;},
                          total,idleMs,totalMs);
  if(result==200)parser.finish();
  if(parser.overflow)result=-102;
  http.end();
  Serial.printf("GEMINI_SSE: POST %s -> %d (%u bytes, %u events%s)\n",path.c_str(),result,(unsigned)total,
                (unsigned)parser.events,parser.aborted?", stopped":"");
  return result;
}
