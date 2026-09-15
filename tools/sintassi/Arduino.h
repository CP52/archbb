#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
typedef uint8_t byte;
#define F(x) x
#define PROGMEM
#define degrees(r) ((r)*57.2957795131)
#define radians(d) ((d)*0.01745329252)
unsigned long millis(); unsigned long micros();
void delay(unsigned long); void delayMicroseconds(unsigned int);
void pinMode(int,int); void digitalWrite(int,int); int digitalRead(int);
int analogRead(int); void analogSetPinAttenuation(int,int);
#define OUTPUT 1
#define INPUT 0
#define INPUT_PULLUP 2
#define HIGH 1
#define LOW 0
struct String {
  String(){} String(const char*){} String(int){} String(float,int){} String(char){}
  const char* c_str() const { return ""; }
  unsigned length() const { return 0; }
  char operator[](int) const { return 0; }
  bool startsWith(const char*) const { return false; }
  bool endsWith(const char*) const { return false; }
  int indexOf(char) const { return -1; }
  int indexOf(const char*) const { return -1; }
  String substring(int) const { return String(); }
  String substring(int,int) const { return String(); }
  void trim() {}
  void toUpperCase() {}
  int toInt() const { return 0; }
  float toFloat() const { return 0; }
  bool equals(const char*) const { return false; }
  bool operator==(const char*) const { return false; }
  String operator+(const char*) const { return String(); }
};
struct SerialT {
  void begin(unsigned long); void setTxTimeoutMs(int);
  operator bool() const;
  int availableForWrite();
  int available(); int read();
  void print(const char*); void print(int); void println(); void println(const char*); void println(int);
  int printf(const char*,...);
  void flush();
};
extern SerialT Serial;
struct TwoWire { bool begin(int,int,uint32_t); void beginTransmission(uint8_t);
  size_t write(uint8_t); uint8_t endTransmission(); uint8_t requestFrom(uint8_t,size_t);
  int available(); int read(); void setClock(uint32_t); };
extern TwoWire Wire;

// PSRAM (esp32-hal)
bool psramFound(); void* ps_malloc(size_t); void* ps_calloc(size_t,size_t);
uint32_t ESP_getFreeHeap();

// --- FASE 24a: LEDC e frequenza di CPU -------------------------------------
// Il banco dichiara ENTRAMBE le API LEDC perche' display.cpp le contiene
// entrambe dietro una guardia di versione: se lo stub ne avesse una sola, il
// ramo non compilato resterebbe fuori dal controllo proprio mentre e' quello
// che verra' usato sul dispositivo.
bool ledcAttach(uint8_t pin, uint32_t freq, uint8_t resolution);
bool ledcWrite(uint8_t pin, uint32_t duty);
bool ledcSetup(uint8_t ch, uint32_t freq, uint8_t resolution);
void ledcAttachPin(uint8_t pin, uint8_t ch);
bool setCpuFrequencyMhz(uint32_t mhz);
uint32_t getCpuFrequencyMhz();

long map(long,long,long,long,long);
template<class T> T constrain(T v, T lo, T hi) { return v<lo?lo:(v>hi?hi:v); }
