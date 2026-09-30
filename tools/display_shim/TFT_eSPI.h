#pragma once
#include <cstdint>
#include <vector>
#include <algorithm>
#define TFT_BLACK 0
#define PSRAM_ENABLE 1
#define ILI9341_VSCRDEF 0x33
#define ILI9341_VSCRSADD 0x37
inline int g_displayPushes=0, g_displayCommands=0;
inline int g_scrollTop=0, g_scrollSpan=320, g_scrollBottom=0, g_scrollOffset=0;
inline std::vector<uint16_t> g_displayPixels(320*240);
inline uint16_t swapColor(uint16_t c) { return (c>>8)|(c<<8); }
class TFT_eSPI {
  uint8_t command=0;
  std::vector<uint8_t> data;
public:
  void init() {}
  void setRotation(int) {}
  int width() const { return 320; }
  int height() const { return 240; }
  uint16_t color565(uint8_t r,uint8_t g,uint8_t b) { return ((r&248)<<8)|((g&252)<<3)|(b>>3); }
  void fillScreen(uint16_t c) { std::fill(g_displayPixels.begin(),g_displayPixels.end(),c); ++g_displayPushes; }
  void startWrite() {}
  void writecommand(uint8_t c) { command=c; data.clear(); ++g_displayCommands; }
  void writedata(uint8_t d) { data.push_back(d); }
  void endWrite() {
    if(command==ILI9341_VSCRDEF && data.size()==6) {
      g_scrollTop=data[0]*256+data[1]; g_scrollSpan=data[2]*256+data[3]; g_scrollBottom=data[4]*256+data[5];
    }
    if(command==ILI9341_VSCRSADD && data.size()==2) g_scrollOffset=data[0]*256+data[1];
  }
};
class TFT_eSprite {
  int w=0,h=0;
  std::vector<uint16_t> pixels;
public:
  explicit TFT_eSprite(TFT_eSPI*) {}
  void setColorDepth(int) {}
  void setAttribute(int,bool) {}
  void* createSprite(int x,int y) { w=x; h=y; pixels.resize(w*h); return pixels.data(); }
  void* getPointer() { return pixels.empty()?nullptr:pixels.data(); }
  void fillSprite(uint16_t c) { std::fill(pixels.begin(),pixels.end(),swapColor(c)); }
  void drawPixel(int x,int y,uint16_t c) { pixels[y*w+x]=swapColor(c); }
  void drawFastHLine(int x,int y,int n,uint16_t c) { for(int i=0;i<n;i++) drawPixel(x+i,y,c); }
  void setScrollRect(int,int,int,int,uint16_t) {}
  void scroll(int,int dy) {
    auto old=pixels;
    for(int y=0;y<h;y++) for(int x=0;x<w;x++)
      pixels[y*w+x]=(y-dy>=0 && y-dy<h)?old[(y-dy)*w+x]:0;
  }
  void pushSprite(int,int) { ++g_displayPushes; for(size_t i=0;i<pixels.size();i++) g_displayPixels[i]=swapColor(pixels[i]); }
};
