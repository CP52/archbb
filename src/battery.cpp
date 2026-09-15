// ============================================================================
//  ArchBB 1.83 — battery.cpp · FASE 5 (stato batteria via AXP2101)
//  Cesare Pagura · Padova/Noale IT · 20 luglio 2026
// ----------------------------------------------------------------------------
//  Vedi battery.h. Punto chiave: il PMU sta sul bus I2C GIA' ATTIVO, quindi
//  begin() riceve l'istanza Wire viva (NON facciamo Wire.begin()). Abilitiamo
//  le misure che ci servono (batteria connessa, tensione, tensione sistema) e
//  leggiamo valori gia' calcolati dal chip.
// ============================================================================
#include "battery.h"
#include "imu.h"        // i2cLock/i2cUnlock (mutex del bus I2C condiviso)
#include <Wire.h>

// IMPORTANTE (dettaglio scoperto leggendo XPowersLib.h): la libreria fa il
// typedef di XPowersPMU SOLO dentro un #if sul chip selezionato. Senza questa
// macro PRIMA dell'include si finisce nel ramo #else, che include tutte le
// classi ma NON definisce XPowersPMU -> errore "XPowersPMU non dichiarato".
// La 1.83 monta l'AXP2101: lo dichiariamo qui. (E' quel che fa l'esempio ufficiale.)
#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>

volatile bool g_batt_ok = false;

// Oggetto PMU generico (come nell'esempio ufficiale AXP2101). L'istanza vive
// per tutta la vita del firmware: una sola, statica.
static XPowersPMU s_pmu;

bool battery_init() {
  if (g_batt_ok) return true;

  // begin() sul bus GIA' ATTIVO: passiamo Wire (avviato da touchInit) e i pin
  // solo per firma; XPowersLib non re-inizializza il bus se e' gia' vivo, ma
  // per sicurezza usiamo la stessa istanza Wire condivisa da touch/IMU.
  // AXP2101_SLAVE_ADDRESS e' l'indirizzo standard del PMU (0x34).
  if (!s_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IMU_I2C_SDA, IMU_I2C_SCL)) {
    DBG("BATT: AXP2101 non risponde");
    g_batt_ok = false;
    return false;
  }

  // Abilita le misure che leggeremo. enableBattDetection fa sapere al chip di
  // stimare la percentuale; le *VoltageMeasure abilitano gli ADC interni.
  s_pmu.enableBattDetection();
  s_pmu.enableBattVoltageMeasure();
  s_pmu.enableSystemVoltageMeasure();

  // --- FASE 7: tasto PWRKEY come ingresso del modo BLE ----------------------
  //  Il PWRKEY del PMU e' il pulsante utente VERO della 1.83 (a differenza di
  //  GPIO0, che e' uno strapping pin e causava blocchi). Lo leggiamo via IRQ del
  //  PMU, in polling non-bloccante. Qui: puliamo eventuali flag pendenti e
  //  abilitiamo SOLO l'interrupt di pressione BREVE (short press). La pressione
  //  lunga resta al PMU per lo spegnimento hardware, che non tocchiamo.
  //  Tutto sotto il mutex del bus I2C (condiviso con IMU/touch/RTC).
  i2cLock();
  s_pmu.clearIrqStatus();                                 // parti da stato pulito
  //  F24b: si abilita ANCHE la pressione lunga. Non per spegnere — quello lo fa
  //  il PMU da solo, in hardware, e non glielo togliamo — ma per VEDERLA
  //  ARRIVARE. La IRQ di long press scatta molto prima dei secondi di
  //  power-off: in quella finestra il firmware chiude la sessione pulita e
  //  scrive perche' si sta spegnendo. Il 12/09 uno spegnimento accidentale ha
  //  lasciato una sessione senza ended_ms e mi e' costato una ricostruzione a
  //  posteriori di tutta la timeline.
  s_pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ |
                  XPOWERS_AXP2101_PKEY_LONG_IRQ);
  s_pmu.clearIrqStatus();                                 // scarta flag spuri iniziali
  i2cUnlock();

  g_batt_ok = true;
  DBG("BATT: AXP2101 ok (%d%%, %.2fV, %s)",
      battery_percent(), battery_voltage(),
      battery_charging() ? "in carica" : "a batteria");
  return true;
}

int battery_percent() {
  if (!g_batt_ok) return -1;
  // Lettura I2C sotto il mutex del bus (chiamata dal loop, concorrente imu_task).
  // getBatteryPercent ritorna gia' -1 se la batteria non e' connessa.
  i2cLock();
  int p = s_pmu.getBatteryPercent();
  i2cUnlock();
  return p;
}

float battery_voltage() {
  if (!g_batt_ok) return NAN;
  i2cLock();
  uint16_t mv = s_pmu.getBattVoltage();   // millivolt
  i2cUnlock();
  if (mv == 0) return NAN;   // 0 = non misurabile
  return mv / 1000.0f;
}

bool battery_charging() {
  if (!g_batt_ok) return false;
  i2cLock();
  bool c = s_pmu.isCharging();
  i2cUnlock();
  return c;
}

// ----------------------------------------------------------------------------
//  FASE 7 — power_key_short_pressed(): pressione BREVE del tasto PWRKEY.
// ----------------------------------------------------------------------------
//  Polling non-bloccante, da chiamare nel loop. Ritorna true UNA volta per ogni
//  pressione breve del tasto power del PMU. Meccanismo AXP2101 (dal sorgente
//  XPowersLib): getIrqStatus() legge i registri di stato IRQ nella struttura
//  interna, poi isPekeyShortPressIrq() dice se c'e' stata una pressione breve;
//  clearIrqStatus() azzera i flag cosi' la prossima chiamata riparte pulita.
//
//  Perche' questo e non GPIO0: il PWRKEY e' il pulsante utente vero della
//  scheda, letto via I2C dal PMU. Non e' uno strapping pin, non ha i problemi
//  elettrici di GPIO0 che causavano i blocchi (fix2). Tutto sotto il mutex del
//  bus I2C (condiviso con IMU/touch/RTC).
//  F24b — UNA SOLA lettura dei registri IRQ per entrambi gli eventi.
//   getIrqStatus() popola statusRegister[] e clearIrqStatus() lo azzera TUTTO:
//   due funzioni indipendenti che leggono e puliscono a turno si mangerebbero
//   gli eventi a vicenda, e il sintomo sarebbe una pressione su tre che sparisce
//   — intermittente, quindi impossibile da inseguire. Si legge una volta, si
//   memorizzano i due esiti, si pulisce una volta.
static bool s_pkeyShort = false;
static bool s_pkeyLong  = false;

static void pkeyPoll() {
  if (!g_batt_ok) return;
  i2cLock();
  s_pmu.getIrqStatus();
  const bool sh = s_pmu.isPekeyShortPressIrq();
  const bool lo = s_pmu.isPekeyLongPressIrq();
  if (sh || lo) s_pmu.clearIrqStatus();
  i2cUnlock();
  if (sh) s_pkeyShort = true;
  if (lo) s_pkeyLong  = true;
}

bool power_key_short_pressed() {
  pkeyPoll();
  if (!s_pkeyShort) return false;
  s_pkeyShort = false;
  return true;
}

bool power_key_long_pressed() {
  pkeyPoll();
  if (!s_pkeyLong) return false;
  s_pkeyLong = false;
  return true;
}

// ============================================================================
//  FASE 24a — gestione dell'energia lato PMU (vedi battery.h per il perche')
// ============================================================================

void battery_power_tuning() {
  if (!g_batt_ok) return;

  i2cLock();
  // --- F24b: pressione lunga portata a 10 secondi --------------------------
  //  La mano di scocco porta la patella e sfiora il fianco della scheda. Il
  //  12/09 questo e' bastato a spegnerla a percorso iniziato. Dieci secondi
  //  sono oltre qualunque sfregamento accidentale e restano comodi da fare
  //  apposta: nessuna funzione persa, solo una soglia spostata dove serve.
  s_pmu.setPowerKeyPressOffTime(XPOWERS_POWEROFF_10S);
  // LED di carica spento. In campo illumina il terreno; la percentuale di
  // carica l'arciere la legge nella barra di stato, dove l'ha sempre letta.
  s_pmu.setChargingLedMode(XPOWERS_CHG_LED_OFF);
  i2cUnlock();

  DBG("BATT: LED spento, power-off a 10 s; VOFF attuale %u mV",
      (unsigned)battery_voff_mv());
}

float battery_voltage_sys() {
  if (!g_batt_ok) return NAN;
  i2cLock();
  uint16_t mv = s_pmu.getSystemVoltage();
  i2cUnlock();
  if (mv == 0) return NAN;
  return mv / 1000.0f;
}

uint16_t battery_voff_mv() {
  if (!g_batt_ok) return 0;
  i2cLock();
  uint16_t mv = s_pmu.getSysPowerDownVoltage();
  i2cUnlock();
  return mv;
}

bool battery_set_voff_mv(uint16_t mv) {
  if (!g_batt_ok) return false;
  // Sbarramento: il datasheet AXP2101 ammette 2600..3300 mV a passi di 100.
  // Scrivere fuori range non da' errore, da' un registro con dentro un valore
  // che non significa niente — la classe di guasto peggiore, perche' silenziosa.
  if (mv < 2600 || mv > 3300) return false;
  i2cLock();
  bool ok = s_pmu.setSysPowerDownVoltage(mv);
  i2cUnlock();
  DBG("BATT: VOFF -> %u mV (%s)", (unsigned)mv, ok ? "ok" : "rifiutata");
  return ok;
}

// ----------------------------------------------------------------------------
//  Censimento dei rail. Sola LETTURA: qui non si spegne niente.
// ----------------------------------------------------------------------------
//  Una macro locale evita dodici righe copiate a mano che differiscono per un
//  numero — e quindi evita che una delle dodici resti indietro al primo
//  ritocco. E' la stessa ragione per cui il disegno del segno sta in un posto
//  solo (§4.1), applicata a una riga di diagnostica.
void battery_rail_report(char* out, size_t outSz) {
  if (!out || outSz == 0) return;
  out[0] = '\0';
  if (!g_batt_ok) { snprintf(out, outSz, "PMU assente"); return; }

  size_t used = 0;
  #define RAIL(nome, attivo, volt)                                            \
    do {                                                                      \
      if (used + 20 < outSz) {                                                \
        int n = (attivo) ? snprintf(out + used, outSz - used, "%s=%u ",        \
                                    nome, (unsigned)(volt))                    \
                         : snprintf(out + used, outSz - used, "%s=off ", nome);\
        if (n > 0) used += (size_t)n;                                          \
      }                                                                        \
    } while (0)

  i2cLock();
  RAIL("DC1",   s_pmu.isEnableDC1(),    s_pmu.getDC1Voltage());
  RAIL("DC2",   s_pmu.isEnableDC2(),    s_pmu.getDC2Voltage());
  RAIL("DC3",   s_pmu.isEnableDC3(),    s_pmu.getDC3Voltage());
  RAIL("DC4",   s_pmu.isEnableDC4(),    s_pmu.getDC4Voltage());
  RAIL("DC5",   s_pmu.isEnableDC5(),    s_pmu.getDC5Voltage());
  RAIL("ALDO1", s_pmu.isEnableALDO1(),  s_pmu.getALDO1Voltage());
  RAIL("ALDO2", s_pmu.isEnableALDO2(),  s_pmu.getALDO2Voltage());
  RAIL("ALDO3", s_pmu.isEnableALDO3(),  s_pmu.getALDO3Voltage());
  RAIL("ALDO4", s_pmu.isEnableALDO4(),  s_pmu.getALDO4Voltage());
  RAIL("BLDO1", s_pmu.isEnableBLDO1(),  s_pmu.getBLDO1Voltage());
  RAIL("BLDO2", s_pmu.isEnableBLDO2(),  s_pmu.getBLDO2Voltage());
  RAIL("DLDO1", s_pmu.isEnableDLDO1(),  s_pmu.getDLDO1Voltage());
  i2cUnlock();

  #undef RAIL
}
