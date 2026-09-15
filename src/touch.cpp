// ============================================================================
//  ArchBB 1.83 — touch.cpp · FASE 2 (bring-up touch CST816) · v2
//  Cesare Pagura · Padova/Noale IT · 11 luglio 2026
// ----------------------------------------------------------------------------
//  CORREZIONE v2 — "il tap dava posizioni fantasma" ("i dati comandano"):
//  In v1 avevo aggiunto una logica "last-valid" per rimediare allo 0,0 sul
//  frame di rilascio. I dati di campo l'hanno smentita: ereditava posizioni che
//  non c'entravano col tocco (DOWN 135,64 -> UP 53,193). ERRORE mio. Rimossa.
//
//  Ho allineato il driver a InfiniTime (riferimento maturo per il CST816):
//   1) CONFIGURO il chip in init (registri IrqCtl 0xFA e MotionMask 0xEC). v1
//      NON configurava nulla: il chip restava nel suo default gesture-heavy che
//      produceva eventi con coordinate inaffidabili. QUESTO era il buco vero.
//   2) Leggo 7 byte a partire dal registro 0x00 (come InfiniTime), non 6 da 0x01.
//   3) Niente posizioni "ereditate": uso solo il dato del frame corrente. Il
//      frame di rilascio ha semplicemente fingers=0 e non viene usato per la
//      posizione.
//
//  Registri di configurazione (Waveshare CST816S register declaration):
//    0xFA IrqCtl: [7]EnTest [6]EnTouch [5]EnChange [4]EnMotion [0]OnceWLP
//    0xEC MotionMask: abilita/maschera le gesture di movimento
//  Config scelta = 0x60 su 0xFA: EnTouch|EnChange (pulse su tocco e su cambio
//  stato), SENZA EnMotion -> l'INT segnala il touch, non solo le gesture, e le
//  coordinate restano pulite. MotionMask=0x00: nessuna gesture continua.
// ============================================================================
#include "touch.h"
#include "imu.h"        // i2cLock/i2cUnlock (mutex del bus I2C condiviso)
#include <Wire.h>

// Il CST816 usa INT attivo BASSO: LOW = evento disponibile.
static constexpr uint8_t TP_INT_ACTIVE = LOW;

// Registri di configurazione.
static constexpr uint8_t REG_MOTION_MASK = 0xEC;
static constexpr uint8_t REG_IRQ_CTL      = 0xFA;

// ----------------------------------------------------------------------------
//  Nome leggibile della gesture.
// ----------------------------------------------------------------------------
const char* touchGestureName(TouchGesture g) {
  switch (g) {
    case TouchGesture::NONE:        return "-";
    case TouchGesture::SLIDE_DOWN:  return "GIU";
    case TouchGesture::SLIDE_UP:    return "SU";
    case TouchGesture::SLIDE_LEFT:  return "SX";
    case TouchGesture::SLIDE_RIGHT: return "DX";
    case TouchGesture::SINGLE_TAP:  return "TAP";
    case TouchGesture::DOUBLE_TAP:  return "2TAP";
    case TouchGesture::LONG_PRESS:  return "LONG";
    default:                        return "?";
  }
}

static TouchGesture mapGesture(uint8_t gid) {
  switch (gid) {
    case 0x00: return TouchGesture::NONE;
    case 0x01: return TouchGesture::SLIDE_DOWN;
    case 0x02: return TouchGesture::SLIDE_UP;
    case 0x03: return TouchGesture::SLIDE_LEFT;
    case 0x04: return TouchGesture::SLIDE_RIGHT;
    case 0x05: return TouchGesture::SINGLE_TAP;
    case 0x0B: return TouchGesture::DOUBLE_TAP;
    case 0x0C: return TouchGesture::LONG_PRESS;
    default:   return TouchGesture::UNKNOWN;
  }
}

// Scrive un byte in un registro del CST816.
static bool writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission((uint8_t)ARCHBB_TP_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

// ----------------------------------------------------------------------------
//  Reset hardware del CST816.
// ----------------------------------------------------------------------------
static void touchHardReset() {
  pinMode(ARCHBB_TP_RST, OUTPUT);
  digitalWrite(ARCHBB_TP_RST, LOW);
  delay(10);
  digitalWrite(ARCHBB_TP_RST, HIGH);
  delay(50);   // il chip completa il boot interno
}

// ----------------------------------------------------------------------------
//  Configura il chip. VA FATTO SUBITO DOPO IL RESET, mentre il chip risponde.
// ----------------------------------------------------------------------------
//  NB: il CST816 risponde all'I2C solo dopo un evento O subito dopo il reset,
//  per una breve finestra. Configuriamo qui approfittando di quella finestra.
static void touchConfigure() {
  // MotionMask = 0x00: disabilita le gesture di movimento continue che
  // sporcavano le coordinate. Le gesture "discrete" (tap, swipe) restano
  // leggibili dal registro GestureID, ma non generano stream spuri.
  writeReg(REG_MOTION_MASK, 0x00);

  // IrqCtl = 0x60: EnTouch (bit6) + EnChange (bit5). L'INT pulsa sul tocco e
  // sul cambio di stato, NON solo sulle gesture. Cosi' il polling-gated vede
  // ogni tocco con coordinate valide, non gli eventi-gesture con coord sporche.
  writeReg(REG_IRQ_CTL, 0x60);
}

// ----------------------------------------------------------------------------
//  Init.
// ----------------------------------------------------------------------------
bool touchInit() {
  pinMode(ARCHBB_TP_INT, INPUT_PULLUP);

  touchHardReset();

  Wire.begin(ARCHBB_I2C_SDA, ARCHBB_I2C_SCL);
  Wire.setClock(400000);

  // FIX HANG: crea il mutex del BUS I2C condiviso PRIMA che partano i task
  // (imu_task/trigger_task). Da qui in poi ogni accesso Wire (touch, IMU,
  // batteria, RTC) passa per i2cLock()/i2cUnlock(), che serializzano il bus ed
  // eliminano le collisioni che causavano l'hang. Lo creiamo qui perche'
  // touchInit e' il primo a inizializzare il bus, nel setup mono-thread.
  if (!g_i2c_mutex) g_i2c_mutex = xSemaphoreCreateRecursiveMutex();

  // Configuro subito, nella finestra post-reset in cui il chip risponde.
  touchConfigure();

  return true;
}

// ----------------------------------------------------------------------------
//  touchPending — c'e' un evento fresco?
// ----------------------------------------------------------------------------
bool touchPending() {
  return digitalRead(ARCHBB_TP_INT) == TP_INT_ACTIVE;
}

// ----------------------------------------------------------------------------
//  touchRead — lettura gated dall'INT (7 byte da reg 0x00, stile InfiniTime).
// ----------------------------------------------------------------------------
TouchData touchRead() {
  TouchData td = {false, 0, 0, 0, TouchGesture::NONE, 0x00, false};

  // GATE: nessun evento -> niente I2C.
  if (!touchPending()) {
    return td;   // valid = false
  }

  // Leggo 7 byte a partire dal registro 0x00 (come InfiniTime). TUTTA la
  // transazione I2C sta dentro il mutex del bus: cosi' imu_task non puo'
  // interromperla a meta' (era la causa dell'hang). buf[] e got vengono riempiti
  // sotto lock; l'interpretazione dei byte avviene DOPO l'unlock (non tocca il bus).
  const uint8_t N = 7;
  uint8_t buf[N] = {0};
  bool    ok = false;

  i2cLock();
  do {
    Wire.beginTransmission((uint8_t)ARCHBB_TP_ADDR);
    Wire.write((uint8_t)0x00);
    if (Wire.endTransmission(false) != 0) break;   // repeated start, non STOP
    uint8_t got = Wire.requestFrom((int)ARCHBB_TP_ADDR, (int)N);
    if (got < N) break;
    for (uint8_t i = 0; i < N; i++) buf[i] = Wire.read();
    ok = true;
  } while (0);
  i2cUnlock();

  if (!ok) return td;   // transazione fallita: valid = false

  // Layout (offset dal registro 0x00):
  //  buf[0] = reserved
  //  buf[1] = GestureID
  //  buf[2] = FingerNum (0=rilascio, 1=tocco)
  //  buf[3] = X_high (bit0..3)   buf[4] = X_low
  //  buf[5] = Y_high (bit0..3)   buf[6] = Y_low
  td.rawGid  = buf[1];
  td.gesture = mapGesture(buf[1]);
  td.fingers = buf[2] & 0x0F;

  // --- coordinate GREZZE dal chip -------------------------------------------
  // Conservate a parte: servono alla calibrazione (che deve vedere cosa dice il
  // chip PRIMA di ogni conversione) e alla diagnostica. Il resto del firmware
  // non le usa mai.
  td.rawX = ((uint16_t)(buf[3] & 0x0F) << 8) | buf[4];
  td.rawY = ((uint16_t)(buf[5] & 0x0F) << 8) | buf[6];

  // --- NIENTE SPECCHIO: il chip e' gia' 1:1 col display ---------------------
  // STORIA (per non ricascarci): a un certo punto avevo aggiunto uno specchio
  // in X, td.rawX = 239 - rawX, convinto che il touch fosse ribaltato. ERRORE,
  // nato dall'aver letto male un dato: avevo scambiato la coordinata "corretta"
  // per la "grezza". Il DIAG a coordinate note ha poi chiarito senza ambiguita':
  //   tocco sul "+" (destra) -> grezzo 239,200  corretto 0,200
  // cioe' il CHIP riporta gia' 239 (destra, giusto), ed ERO IO a ribaltarlo a 0.
  // Il chip e' 1:1, esattamente come diceva lo span del DIAG (rx 1..239, ax=1)
  // fin dall'inizio. La cura era NON fare nulla. Lezione (di nuovo): il dato
  // misurato batte il modello; e "grezzo vs corretto" vanno letti col nome
  // giusto, o si costruisce una correzione per un problema inesistente.
  uint16_t mirX = td.rawX;   // 1:1, nessuno specchio
  uint16_t mirY = td.rawY;

  // --- CORREZIONE FINE (identita' di default) -------------------------------
  // Resta il modello affine per un'eventuale messa a punto fine, ma con
  // TOUCH_CAL_* = identita' e' un no-op: td.x = td.rawX. Il chip va bene cosi'.
  {
    float fx = TOUCH_CAL_AX * (float)mirX + TOUCH_CAL_BX;
    float fy = TOUCH_CAL_AY * (float)mirY + TOUCH_CAL_BY;
    if (fx < 0) fx = 0;
    if (fx > ARCHBB_W - 1) fx = ARCHBB_W - 1;
    if (fy < 0) fy = 0;
    if (fy > ARCHBB_H - 1) fy = ARCHBB_H - 1;
    td.x = (uint16_t)(fx + 0.5f);
    td.y = (uint16_t)(fy + 0.5f);
  }

  // Filtro conservativo: un tocco reale in (0,0) pixel ESATTO e' fisicamente
  // impossibile (l'angolo assoluto non e' raggiungibile col polpastrello). Se
  // arriva (0,0) con dito presente e' un artefatto raro del chip: lo declasso a
  // rilascio (fingers=0). Il test va sul GREZZO ORIGINALE del chip (rawX/rawY,
  // prima dello specchio): e' li' che l'artefatto si manifesta come 0,0.
  if (td.fingers > 0 && td.rawX == 0 && td.rawY == 0) {
    td.fingers = 0;
  }

  // Le coordinate sono valide SOLO quando c'e' un dito. Sul rilascio non le
  // uso e non le invento: coordsFresh riflette semplicemente fingers>0.
  td.coordsFresh = (td.fingers > 0);
  td.valid = true;
  return td;
}
