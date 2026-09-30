// Exercises the real display implementation against an in-memory panel.
#include "../RX-105-Xi/grating.h"
#include <cstdio>
static int failures=0;
static void check(bool ok,const char *message) {
  printf("%s %s\n",ok?"ok":"FAIL",message); if(!ok) ++failures;
}
int main() {
  GratingHandler display(16);
  check(!display.startAnimation(160),"cannot animate before loading a pattern");
  display.setBacklight(true);
  check(display.drawGrating(45,45,1),"draw and upload hardware-scrolled pattern");
  int transfers=g_displayPushes;
  auto diagonal=g_displayPixels;
  check(display.drawGrating(45,45,1) && g_displayPushes==transfers+1,
        "identical draw requests still regenerate and transfer");
  check(g_displayPixels==diagonal,"repeat generation produces identical pixels");
  check(g_pinValues[16]==HIGH,"drawing leaves backlight under caller control");
  check(GratingHandler::supportsHardwareScroll(45) && !GratingHandler::supportsHardwareScroll(90),
        "capability query distinguishes scrolling modes without rejecting rendering");
  check(GratingHandler::supportsHardwareScroll(90.0105f) &&
        GratingHandler::supportsHardwareScroll(89.9895f) &&
        !GratingHandler::supportsHardwareScroll(90.009f) &&
        !GratingHandler::supportsHardwareScroll(-89.991f) &&
        !GratingHandler::supportsHardwareScroll(270),
        "scroll mode preserves the original 0.01-degree cutoff and angle wrapping");
  display.setBacklight(false);
  check(display.drawGrating(45,45,1),"draw fresh hardware pattern");
  check(display.startAnimation(160),"start loaded hardware pattern");
  transfers=g_displayPushes; int commands=g_displayCommands;
  g_arduinoMillis+=20; display.update();
  check(g_displayPushes==transfers && g_displayCommands>commands,"hardware motion sends only register commands");
  check(g_pinValues[16]==LOW,"upload and animation do not enable backlight");
  check(display.drawGrating(45,45,1) && !display.isAnimating(),"a fresh draw stops previous animation");
  display.stopAnimation();
  commands=g_displayCommands; g_arduinoMillis+=20;
  check(!display.update() && g_displayCommands==commands,"stopping animation prevents further updates");
  check(display.drawGrating(45,90,0.25f),"draw software pattern after hardware scroll");
  check(g_scrollOffset==0 && g_scrollTop==0 && g_scrollBottom==0 && g_scrollSpan==320,
        "software load clears previous hardware scroll offset and margins");
  auto original=g_displayPixels;
  display.startAnimation(160); transfers=g_displayPushes;
  g_arduinoMillis+=20; display.update();
  check(g_displayPushes>transfers && g_displayPixels!=original,"90-degree pattern animates in software");
  bool correctContrast=true;
  for(auto pixel:g_displayPixels) { int red=((pixel>>11)&31)*8; if(red<88 || red>160) correctContrast=false; }
  check(correctContrast,"software redraw uses the current contrast");
  auto moved=g_displayPixels;
  display.stopAnimation(); display.drawGrating(30,135,0.8f); display.startAnimation(-160);
  g_arduinoMillis+=20; display.update(); display.stopAnimation();
  display.drawGrating(45,90,0.25f);
  check(g_displayPixels==original,"fresh drawing restores original pixels after animation");
  display.startAnimation(160); g_arduinoMillis+=20; display.update();
  check(g_displayPixels==moved,"fresh drawing resets software phase and fractional scroll state");
  display.stopAnimation(); display.drawGrating(45,270,0.5f); display.startAnimation(160);
  original=g_displayPixels; g_arduinoMillis+=20; display.update();
  check(g_displayPixels!=original,"270-degree pattern animates");
  display.stopAnimation(); display.drawGrating(45,45,1);
  display.startAnimation(160); g_arduinoMillis+=10000;
  check(display.update(),"animation continues until the caller stops it");
  display.setBacklight(true); display.fillColor(12,80,200);
  uint16_t expected=((12&248)<<8)|((80&252)<<3)|(200>>3);
  bool filled=true; for(auto pixel:g_displayPixels) if(pixel!=expected) filled=false;
  check(filled && g_pinValues[16]==HIGH && !display.isAnimating(),"general color fill leaves backlight unchanged and stops animation");
  check(!display.startAnimation(160),"fill invalidates previously loaded grating");
  check(!display.drawGrating(0,45,1),"invalid period is rejected");
  check(!display.startAnimation(160),"failed drawing cannot animate old pixels");
  check(display.drawGrating(45,60,1),"drawing can recover after invalid input");
  puts(failures?"FAIL":"PASS"); return failures?1:0;
}
