#pragma once
// JPEG -> 200x200 1-bit e-paper bitmap: scaled JPEG decode to grayscale, box-filter
// resize with center crop, percentile contrast stretch, Floyd-Steinberg dithering.
#include <JPEGDEC.h>

struct GrayTarget { uint8_t *pixels; int width,height; };

int jpegGrayDraw(JPEGDRAW *draw) {
  auto *target=(GrayTarget*)draw->pUser;
  const uint8_t *src=(const uint8_t*)draw->pPixels;
  for(int y=0;y<draw->iHeight;y++){
    int ty=draw->y+y;if(ty<0||ty>=target->height)continue;
    for(int x=0;x<draw->iWidthUsed;x++){
      int tx=draw->x+x;if(tx<0||tx>=target->width)continue;
      target->pixels[ty*target->width+tx]=src[y*draw->iWidth+x];
    }
  }
  return 1;
}

bool jpegToBitmap(uint8_t *jpg,size_t len,uint8_t *bitmap) {
  JPEGDEC *jpeg=new JPEGDEC();
  if(!jpeg)return false;
  bool ok=false;
  uint8_t *gray=nullptr,*small=nullptr;int16_t *err=nullptr;
  do {
    if(!jpeg->openRAM(jpg,len,jpegGrayDraw)){Serial.printf("IMAGE: JPEG open failed %d\n",jpeg->getLastError());break;}
    int w=jpeg->getWidth(),h=jpeg->getHeight();
    int scale=w>=800?JPEG_SCALE_QUARTER:w>=400?JPEG_SCALE_HALF:0;
    int div=scale==JPEG_SCALE_QUARTER?4:scale==JPEG_SCALE_HALF?2:1;
    GrayTarget target={nullptr,(w+div-1)/div,(h+div-1)/div};
    gray=(uint8_t*)ps_calloc(target.width*target.height,1);
    if(!gray)break;
    target.pixels=gray;
    jpeg->setPixelType(EIGHT_BIT_GRAYSCALE);jpeg->setUserPointer(&target);
    if(!jpeg->decode(0,0,scale)){Serial.printf("IMAGE: JPEG decode failed %d\n",jpeg->getLastError());break;}
    Serial.printf("IMAGE: decoded %dx%d -> %dx%d gray\n",w,h,target.width,target.height);
    // Center-crop to square, then box-filter down to 200x200.
    int side=min(target.width,target.height),ox=(target.width-side)/2,oy=(target.height-side)/2;
    small=(uint8_t*)ps_malloc(IMAGE_SIZE*IMAGE_SIZE);
    if(!small)break;
    uint32_t histogram[256]={};
    for(int y=0;y<IMAGE_SIZE;y++){
      int y0=oy+y*side/IMAGE_SIZE,y1=max(y0+1,oy+(y+1)*side/IMAGE_SIZE);
      for(int x=0;x<IMAGE_SIZE;x++){
        int x0=ox+x*side/IMAGE_SIZE,x1=max(x0+1,ox+(x+1)*side/IMAGE_SIZE);
        uint32_t sum=0,count=0;
        for(int sy=y0;sy<y1;sy++)for(int sx=x0;sx<x1;sx++){sum+=gray[sy*target.width+sx];count++;}
        uint8_t v=sum/count;small[y*IMAGE_SIZE+x]=v;histogram[v]++;
      }
    }
    // Stretch the 1st..99th percentile to full range so dithering keeps detail.
    uint32_t total=IMAGE_SIZE*IMAGE_SIZE,acc=0;int lo=0,hi=255;
    for(int i=0;i<256;i++){acc+=histogram[i];if(acc>=total/100){lo=i;break;}}
    acc=0;for(int i=255;i>=0;i--){acc+=histogram[i];if(acc>=total/100){hi=i;break;}}
    if(hi-lo<16){lo=0;hi=255;}
    err=(int16_t*)calloc((IMAGE_SIZE+2)*2,sizeof(int16_t));
    if(!err)break;
    int16_t *cur=err+1,*next=err+IMAGE_SIZE+3;
    memset(bitmap,0,IMAGE_BYTES);
    for(int y=0;y<IMAGE_SIZE;y++){
      for(int x=0;x<IMAGE_SIZE;x++){
        int v=(small[y*IMAGE_SIZE+x]-lo)*255/(hi-lo);
        v=constrain(v,0,255)+cur[x];
        int out=v<128?0:255,e=v-out;
        if(!out)bitmap[(y*IMAGE_SIZE+x)>>3]|=0x80>>(x&7);
        cur[x+1]+=e*7/16;next[x-1]+=e*3/16;next[x]+=e*5/16;next[x+1]+=e/16;
      }
      int16_t *swap=cur;cur=next;next=swap;
      memset(next-1,0,(IMAGE_SIZE+2)*sizeof(int16_t));
    }
    ok=true;
  } while(false);
  jpeg->close();delete jpeg;
  free(gray);free(small);free(err);
  return ok;
}
