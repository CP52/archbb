#pragma once
#include <cstdint>
// Colori base di TFT_eSPI (i nomi che il progetto usa direttamente).
#define TFT_BLACK 0x0000
#define TFT_WHITE 0xFFFF
#define TFT_RED 0xF800
#define TFT_GREEN 0x07E0
#define TFT_BLUE 0x001F
#define TFT_CYAN 0x07FF
#define TFT_YELLOW 0xFFE0
#define TFT_ORANGE 0xFD20
#define TFT_DARKGREY 0x7BEF
#define TFT_LIGHTGREY 0xD69A
#define TFT_MAGENTA 0xF81F
// Pin del pannello: nel mondo vero arrivano da User_Setup / build_flags.
// FASE 24a: il pin del backlight NON e' piu' di TFT_eSPI (v. platformio.ini).
#ifndef ARCHBB_BL_PIN
#define ARCHBB_BL_PIN 40
#endif
#define TC_DATUM 1
#define TL_DATUM 2
#define MC_DATUM 3
#define TR_DATUM 4
#define BC_DATUM 5
#define ML_DATUM 6
#define MR_DATUM 7
#define BL_DATUM 8
#define BR_DATUM 9
#define CC_DATUM 10
#define TC_DATUM_ 11
class TFT_eSPI { public:
  TFT_eSPI(){} void init(); void setRotation(uint8_t);
  void fillScreen(uint32_t); void fillRect(int,int,int,int,uint32_t);
  void drawRect(int,int,int,int,uint32_t); void drawLine(int,int,int,int,uint32_t);
  void fillRoundRect(int,int,int,int,int,uint32_t);
  void drawRoundRect(int,int,int,int,int,uint32_t);
  void fillTriangle(int,int,int,int,int,int,uint32_t);
  void drawFastHLine(int,int,int,uint32_t); void drawFastVLine(int,int,int,uint32_t);
  void drawPixel(int,int,uint32_t);
  void setFreeFont(const void*); void setCursor(int,int);
  int textWidth(const char*,uint8_t f=1); int fontHeight(uint8_t f=1);
  int print(const char*); int println(const char*);
  void drawCircle(int,int,int,uint32_t); void fillCircle(int,int,int,uint32_t);
  void setTextDatum(uint8_t); void setTextColor(uint32_t,uint32_t);
  void setTextColor(uint32_t); void setTextSize(uint8_t);
  int drawString(const char*,int,int,uint8_t); int drawString(const char*,int,int);
  void setSwapBytes(bool); void pushImage(int,int,int,int,const uint16_t*);
  int width(); int height();
  uint16_t color565(uint8_t,uint8_t,uint8_t);
  void writecommand(uint8_t);   // FASE 24a: SLPIN/SLPOUT del pannello
};
