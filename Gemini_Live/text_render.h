#pragma once
// UTF-8 text for the 200x200 panel.
// Latin text keeps the built-in GFX font (CP437) so English looks exactly as before.
// Chinese (plus kana, Greek and Cyrillic, all part of GB2312) uses WenQuanYi and
// Devanagari uses GNU Unifont, both through U8g2_for_Adafruit_GFX.
// There is no shaping engine: Unifont draws each Devanagari vowel sign, nasal mark and
// virama positioned over a 16 px consonant cell, so they are overlaid on the preceding
// consonant. Conjuncts stay as separate letters with a visible virama.
#include <Adafruit_GFX.h>
#include <U8g2_for_Adafruit_GFX.h>

constexpr int TEXT_MAX_LINES=14;

// The built-in GFX font is CP437; map common UTF-8 (German, French, Spanish,
// typographic punctuation) so transcripts render instead of showing garbage.
// Returns nullptr when the code point has no CP437 equivalent.
const char *cp437Lookup(uint32_t cp) {
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
  return nullptr;
}
const char *cp437Glyph(uint32_t cp) {
  const char *out=cp437Lookup(cp);
  if(out)return out;
  return cp>=0x1F000?"":"?"; // Drop emoji, mark other unsupported glyphs.
}

// Decodes one code point; returns the bytes consumed. Malformed bytes yield cp=0.
int utf8Decode(const char *str,int len,uint32_t &cp) {
  const uint8_t *s=(const uint8_t*)str;uint8_t c=s[0];
  if(c<0x80){cp=c;return 1;}
  int extra=(c&0xE0)==0xC0?1:(c&0xF0)==0xE0?2:(c&0xF8)==0xF0?3:0;
  if(!extra||extra>=len){cp=0;return 1;}
  cp=c&(0x3F>>extra);
  for(int k=1;k<=extra;k++){if((s[k]&0xC0)!=0x80){cp=0;return 1;}cp=(cp<<6)|(s[k]&0x3F);}
  return extra+1;
}

bool isDevanagari(uint32_t cp) { return cp>=0x0900&&cp<=0x097F; }
bool isDevaConsonant(uint32_t cp) { return (cp>=0x0915&&cp<=0x0939)||(cp>=0x0958&&cp<=0x095F)||(cp>=0x0978&&cp<=0x097F); }
// Dependent vowel signs, nasal marks, nukta and virama: drawn over the previous letter.
bool isDevaMark(uint32_t cp) {
  return (cp>=0x0900&&cp<=0x0903)||(cp>=0x093A&&cp<=0x093C)||(cp>=0x093E&&cp<=0x094F)||
         (cp>=0x0951&&cp<=0x0957)||cp==0x0962||cp==0x0963;
}
// Ideographs, kana and fullwidth forms may break a line between any two characters.
bool isWideCp(uint32_t cp) { return (cp>=0x2E80&&cp<=0x9FFF)||(cp>=0xF900&&cp<=0xFAFF)||(cp>=0xFF00&&cp<=0xFFEF); }
// Closing punctuation never starts a line.
bool noBreakBefore(uint32_t cp) {
  static const uint16_t list[]={0x3001,0x3002,0xFF0C,0xFF0E,0xFF01,0xFF1F,0xFF1A,0xFF1B,0x300D,0x300F,0xFF09,0x3011,0x300B};
  for(uint16_t c:list)if(c==cp)return true;
  return false;
}
// Invisible format characters (ZWNJ/ZWJ in Hindi, variation selectors, BOM).
bool isIgnorable(uint32_t cp) {
  return cp==0||cp==0x200B||cp==0x200C||cp==0x200D||cp==0xFEFF||(cp>=0xFE00&&cp<=0xFE0F)||cp>=0x1F000;
}

enum GlyphSource:uint8_t{GS_NONE,GS_GFX,GS_WIDE,GS_DEVA};

class PanelText {
 public:
  U8G2_FOR_ADAFRUIT_GFX u8;
  uint8_t size=1; // GFX text size: 1 = 6x8 cells, 2 = 12x16 cells.

  void begin(Adafruit_GFX &gfx) { u8.begin(gfx);u8.setForegroundColor(0);u8.setFontMode(1);current=nullptr; }
  void setSize(uint8_t s) { size=s; }

  // Where a code point is drawn from; `gfx` receives the CP437 bytes for GS_GFX.
  GlyphSource source(uint32_t cp,const char *&gfx) {
    static char ascii[2];
    if(cp=='\t'||cp=='\r')cp=' ';
    if(cp>=0x20&&cp<0x80){ascii[0]=(char)cp;ascii[1]=0;gfx=ascii;return GS_GFX;}
    if(cp<0x20||isIgnorable(cp))return GS_NONE;
    if((gfx=cp437Lookup(cp)))return GS_GFX;
    if(isDevanagari(cp)&&has(devaFont(),cp))return GS_DEVA;
    if(cp<=0xFFFF&&has(wideFont(),cp))return GS_WIDE;
    gfx="?";return GS_GFX;
  }
  int advance(uint32_t cp) {
    const char *gfx;GlyphSource s=source(cp,gfx);
    if(s==GS_GFX)return strlen(gfx)*6*size;
    if(s==GS_NONE||(s==GS_DEVA&&isDevaMark(cp)))return 0;
    use(s==GS_DEVA?devaFont():wideFont());
    return u8g2_GetGlyphWidth(&u8.u8g2,cp);
  }
  // Draws at the shared baseline; returns the advance.
  int draw(int x,int baseline,uint32_t cp) {
    const char *gfx;GlyphSource s=source(cp,gfx);
    if(s==GS_NONE)return 0;
    if(s==GS_GFX){
      int top=baseline-7*size;
      for(const char *p=gfx;*p;p++,x+=6*size)u8.u8g2.gfx->drawChar(x,top,(uint8_t)*p,0,0,size);
      return strlen(gfx)*6*size;
    }
    use(s==GS_DEVA?devaFont():wideFont());
    return u8.drawGlyph(x,baseline,cp);
  }
  int width(const char *t,int len) {
    int w=0;
    for(int i=0;i<len;){uint32_t cp;i+=utf8Decode(t+i,len-i,cp);w+=advance(cp);}
    return w;
  }

  // Vertical metrics for every font the text needs, so mixed scripts share a baseline.
  void metrics(const char *t,int &ascent,int &descent) {
    ascent=7*size;descent=size;
    bool wide=false,deva=false;int len=strlen(t);
    for(int i=0;i<len;){
      uint32_t cp;i+=utf8Decode(t+i,len-i,cp);
      const char *gfx;GlyphSource s=source(cp,gfx);
      wide|=s==GS_WIDE;deva|=s==GS_DEVA;
    }
    auto grow=[&](const uint8_t *f){
      use(f);
      ascent=max(ascent,(int)(u8.u8g2.font_info.max_char_height+u8.u8g2.font_info.y_offset));
      descent=max(descent,(int)-u8.u8g2.font_info.y_offset);
    };
    if(wide)grow(wideFont());
    if(deva)grow(devaFont());
  }

  // Word-wraps to `maxWidth` px. Breaks at spaces, and between wide characters.
  // keepLast keeps the newest `cap` lines; otherwise the first `cap` lines.
  // Returns the total line count, which may exceed `cap`.
  int wrap(const char *t,int maxWidth,int16_t *starts,uint16_t *lens,int cap,bool keepLast) {
    int len=strlen(t),pos=0,total=0;
    auto add=[&](int s,int l){
      if(total<cap){starts[total]=s;lens[total]=l;}
      else if(keepLast){memmove(starts,starts+1,(cap-1)*sizeof(int16_t));memmove(lens,lens+1,(cap-1)*sizeof(uint16_t));starts[cap-1]=s;lens[cap-1]=l;}
      total++;
    };
    while(pos<len){
      int j=pos,w=0,breakEnd=-1,breakNext=-1;
      bool prevWide=false;
      while(j<len&&t[j]!='\n'){
        uint32_t cp;int n=utf8Decode(t+j,len-j,cp);
        int a=advance(cp);
        if(w+a>maxWidth&&j>pos)break;
        if(cp==' '){breakEnd=j;breakNext=j+1;}
        else if(j>pos&&(isWideCp(cp)||prevWide)&&!noBreakBefore(cp)){breakEnd=j;breakNext=j;}
        prevWide=isWideCp(cp);
        w+=a;j+=n;
      }
      int end,next;
      if(j>=len||t[j]=='\n'){end=j;next=j+1;}
      else if(t[j]==' '){end=j;next=j+1;}
      else if(breakEnd>pos){end=breakEnd;next=breakNext;}
      else {end=j;next=j;} // Hard-break words longer than a line.
      while(end>pos&&t[end-1]==' ')end--;
      add(pos,end-pos);pos=next;
      while(pos<len&&t[pos]==' ')pos++;
    }
    return total;
  }

  // Draws one line. Devanagari marks overlay the previous letter; the short-i sign
  // (drawn on the left of its cell) overlays the first letter of the conjunct cluster.
  int drawLine(int x,int baseline,const char *t,int len) {
    int letterX=x,clusterX=x;uint32_t prev=0;
    for(int i=0;i<len;){
      uint32_t cp;i+=utf8Decode(t+i,len-i,cp);
      if(isDevaMark(cp)){draw(cp==0x093F?clusterX:letterX,baseline,cp);prev=cp;continue;}
      if(!(isDevaConsonant(cp)&&prev==0x094D))clusterX=x;
      letterX=x;x+=draw(x,baseline,cp);prev=cp;
    }
    return x;
  }

 private:
  const uint8_t *current=nullptr;
  const uint8_t *wideFont() const { return size>1?u8g2_font_wqy16_t_gb2312:u8g2_font_wqy12_t_gb2312; }
  const uint8_t *devaFont() const { return u8g2_font_unifont_t_devanagari; }
  // setFont() resets the library to opaque mode, which would paint each glyph box black.
  void use(const uint8_t *f) { if(f!=current){u8.setFont(f);u8.setFontMode(1);current=f;} }
  bool has(const uint8_t *f,uint32_t cp) { use(f);return u8g2_IsGlyph(&u8.u8g2,cp); }
};
