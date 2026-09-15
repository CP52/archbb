#pragma once
#include <Arduino.h>
class Preferences { public:
  bool begin(const char*, bool readOnly = false); void end();
  bool clear(); bool remove(const char*);
  size_t putBytes(const char*, const void*, size_t);
  size_t getBytes(const char*, void*, size_t);
  size_t getBytesLength(const char*);
  size_t putUInt(const char*, uint32_t); uint32_t getUInt(const char*, uint32_t d = 0);
  // F20b: la taratura viaggia su chiavi scalari fuori dal blob (§TARATURA).
  // Aggiunte qui perche' il banco le ha segnalate mancanti al primo uso — che
  // e' esattamente il servizio che deve rendere: uno stub incompleto e' un
  // cono d'ombra, e si chiude quando lo si incontra.
  size_t putUChar(const char*, uint8_t);  uint8_t getUChar(const char*, uint8_t d = 0);
  size_t putFloat(const char*, float);    float   getFloat(const char*, float d = 0.0f);
  bool isKey(const char*);
};
