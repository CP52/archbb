// ============================================================================
//  ArchBB 1.83 — touch.h · FASE 2 (bring-up touch CST816)
//  Cesare Pagura · Padova/Noale IT · 11 luglio 2026
// ----------------------------------------------------------------------------
//  Interfaccia del modulo touch. Il CST816 (0x15) e' un touch capacitivo con
//  gesture integrate. VINCOLO HARDWARE FONDAMENTALE: risponde all'I2C SOLO dopo
//  un evento touch. Non risponde in init, non fa polling continuo affidabile.
//  Dopo un tocco asserisce l'INT (attivo basso) e risponde alle letture I2C per
//  un breve periodo. Da qui la scelta POLLING-GATED (vedi touch.cpp).
//
//  Registri (riuso logica 1.69, verificati su CST816D/T):
//    Lettura a partire da reg 0x01, 6+ byte:
//      [0x01] gesture id
//      [0x02] finger num (bit0..3) — 0=su, 1=giu'
//      [0x03] X high (bit0..3) + event flag (bit6..7)
//      [0x04] X low
//      [0x05] Y high (bit0..3)
//      [0x06] Y low
//  NB: coordinate a 12 bit (4 bit high + 8 bit low).
// ----------------------------------------------------------------------------
#pragma once
#include <Arduino.h>
#include "config.h"   // TOUCH_CAL_* : la correzione vive nel driver (vedi touch.cpp)

// ----------------------------------------------------------------------------
//  Gesture riconosciute dal CST816 (id nel registro 0x01).
// ----------------------------------------------------------------------------
enum class TouchGesture : uint8_t {
  NONE        = 0x00,
  SLIDE_DOWN  = 0x01,
  SLIDE_UP    = 0x02,
  SLIDE_LEFT  = 0x03,
  SLIDE_RIGHT = 0x04,
  SINGLE_TAP  = 0x05,
  DOUBLE_TAP  = 0x0B,
  LONG_PRESS  = 0x0C,
  UNKNOWN     = 0xFF
};

// Nome leggibile della gesture (per debug a schermo/seriale).
const char* touchGestureName(TouchGesture g);

// ----------------------------------------------------------------------------
//  Dato di un campione touch.
// ----------------------------------------------------------------------------
struct TouchData {
  bool          valid;       // true se c'e' un evento touch in questo campione
  uint16_t      x;           // coordinata X (0..239 attesa)
  uint16_t      y;           // coordinata Y (0..283 attesa)
  uint8_t       fingers;     // numero dita (0 = rilascio)
  TouchGesture  gesture;     // gesture riconosciuta
  uint8_t       rawGid;      // gesture id grezzo (per diagnostica)
  // Coordinate GREZZE, come le riporta il CST816 PRIMA di ogni conversione.
  // Le usa SOLO la calibrazione (touch_cal), che deve misurare cosa dice il chip
  // e non cosa il firmware ha gia' corretto. Nessun handler deve leggerle: per
  // tutto il resto esistono x,y (gia' corrette, unico punto di verita').
  uint16_t      rawX;
  uint16_t      rawY;
  bool          coordsFresh; // true se x,y vengono da questo frame; false se
                             // sono l'ultima posizione valida ereditata (caso
                             // del frame di rilascio, dove il CST816 azzera le
                             // coordinate ma segnala la gesture). Per lo scoring
                             // e' il flag che dice "fidati di x,y anche sul tap".
};

// ----------------------------------------------------------------------------
//  API del modulo
// ----------------------------------------------------------------------------

// Inizializza I2C, reset hardware del CST816, pin INT in input.
// Ritorna true se il bus I2C e' pronto (NB: il chip NON risponde finche' non
// viene toccato, quindi non si puo' "probare" in init in modo affidabile).
bool touchInit();

// Legge un campione touch, MA solo se l'INT segnala un evento fresco.
// Se non c'e' evento, ritorna un TouchData con valid=false senza toccare l'I2C.
// E' il cuore della strategia polling-gated: rispetta il vincolo del chip e
// non spreca transazioni I2C sul bus condiviso.
TouchData touchRead();

// True se il pin INT e' attualmente attivo (evento touch disponibile).
bool touchPending();
