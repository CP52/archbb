// ============================================================================
//  ArchBB 1.83 — rtc_clock.cpp · FASE 6 (orologio RTC PCF85063)
//  Cesare Pagura · Padova/Noale IT · 21 luglio 2026
// ----------------------------------------------------------------------------
//  Vedi rtc_clock.h. Punto chiave: bus I2C GIA' attivo (no Wire.begin), e
//  validita' basata sul flag OS del chip + sanita' dell'anno.
// ============================================================================
#include "rtc_clock.h"
#include <sys/time.h>   // F24b.1: settimeofday per allineare l'orologio di sistema
#include <time.h>
#include "imu.h"        // i2cLock/i2cUnlock (mutex del bus I2C condiviso)
#include <Wire.h>
#include <SensorPCF85063.hpp>

volatile bool g_rtc_ok = false;

static SensorPCF85063 s_rtc;

// Indirizzo e registro secondi (bit7 = flag OS). L'indirizzo 0x51 e' il valore
// reale del PCF85063 (la costante di SensorLib non e' nel nostro scope).
static constexpr uint8_t RTC_ADDR = 0x51;
static constexpr uint8_t PCF85063_SEC_REG = 0x04;

// Legge il flag OS (Oscillator Stop) direttamente dal registro secondi.
// true = l'oscillatore si e' fermato -> ora inaffidabile (mai impostata o persa).
static bool readOscStop() {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(PCF85063_SEC_REG);
  if (Wire.endTransmission(false) != 0) return true;   // errore bus -> inaffidabile
  Wire.requestFrom((int)RTC_ADDR, 1);
  if (!Wire.available()) return true;
  return (Wire.read() & 0x80) != 0;
}

bool rtc_init() {
  if (g_rtc_ok) return true;
  // begin sul bus GIA' attivo (touchInit ha fatto Wire.begin).
  if (!s_rtc.begin(Wire, IMU_I2C_SDA, IMU_I2C_SCL)) {
    DBG("RTC: PCF85063 non risponde");
    g_rtc_ok = false;
    return false;
  }
  g_rtc_ok = true;
  DBG("RTC: PCF85063 ok (valido=%d)", rtc_is_valid());
  return true;
}

bool rtc_is_valid() {
  if (!g_rtc_ok) return false;
  i2cLock();
  bool valid = false;
  do {
    // 1) flag OS: se il chip si e' fermato, l'ora non e' affidabile.
    if (readOscStop()) break;
    // 2) sanita' dell'anno: anche con OS pulito, un anno assurdo = non impostato.
    RTC_DateTime dt = s_rtc.getDateTime();
    int y = dt.getYear(); if (y < 100) y += 2000;
    if (y < 2024 || y > 2099) break;
    valid = true;
  } while (0);
  i2cUnlock();
  return valid;
}

RtcTime rtc_now() {
  RtcTime t{};
  t.valid = false;
  if (!g_rtc_ok) return t;
  i2cLock();
  if (rtc_is_valid()) {               // ricorsivo: rtc_is_valid riprende il lock
    RTC_DateTime dt = s_rtc.getDateTime();
    int y = dt.getYear(); if (y < 100) y += 2000;
    t.year   = (uint16_t)y;
    t.month  = dt.getMonth();
    t.day    = dt.getDay();
    t.hour   = dt.getHour();
    t.minute = dt.getMinute();
    t.second = dt.getSecond();
    t.valid  = true;
  }
  i2cUnlock();
  return t;
}

bool rtc_set(uint16_t year, uint8_t month, uint8_t day,
             uint8_t hour, uint8_t minute, uint8_t second) {
  if (!g_rtc_ok) return false;
  i2cLock();
  s_rtc.setDateTime(RTC_DateTime(year, month, day, hour, minute, second));
  i2cUnlock();
  DBG("RTC: impostato a %04u-%02u-%02u %02u:%02u:%02u",
      year, month, day, hour, minute, second);
  // Verifica: dopo il set il flag OS deve essere pulito e l'anno plausibile.
  return rtc_is_valid();
}

void rtc_iso_string(char* buf, size_t bufSize) {
  RtcTime t = rtc_now();
  if (!t.valid) { snprintf(buf, bufSize, "non impostata"); return; }
  snprintf(buf, bufSize, "%04u-%02u-%02uT%02u:%02u:%02u",
           t.year, t.month, t.day, t.hour, t.minute, t.second);
}

void rtc_compact_string(char* buf, size_t bufSize) {
  RtcTime t = rtc_now();
  if (!t.valid) { if (bufSize) buf[0] = '\0'; return; }
  snprintf(buf, bufSize, "%04u%02u%02u_%02u%02u",
           t.year, t.month, t.day, t.hour, t.minute);
}


// ----------------------------------------------------------------------------
//  FASE 24b.1 — rtc_sync_system_clock
// ----------------------------------------------------------------------------
//  mktime() interpreta la struct come ora LOCALE. Qui non impostiamo nessun
//  fuso (TZ resta UTC) e l'RTC porta l'ora di casa: il risultato e' che
//  l'orologio di sistema porta la stessa ora dell'RTC, che e' esattamente cio'
//  che vogliamo. FAT memorizza ora locale senza fuso, quindi qualunque
//  conversione qui produrrebbe file datati a un'ora diversa da quella nel nome
//  della cartella — due orologi nella stessa card, la cosa peggiore.
bool rtc_sync_system_clock() {
  RtcTime t = rtc_now();
  if (!t.valid) return false;

  struct tm tmv = {};
  tmv.tm_year = (int)t.year - 1900;
  tmv.tm_mon  = (int)t.month - 1;
  tmv.tm_mday = (int)t.day;
  tmv.tm_hour = (int)t.hour;
  tmv.tm_min  = (int)t.minute;
  tmv.tm_sec  = (int)t.second;
  tmv.tm_isdst = 0;

  time_t epoch = mktime(&tmv);
  if (epoch <= 0) return false;

  struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
  if (settimeofday(&tv, nullptr) != 0) return false;
  return true;
}
