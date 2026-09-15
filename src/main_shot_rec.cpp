// ============================================================================
//  ArchBB 1.83 — main_shot_rec.cpp · REGISTRATORE CONTINUO (diagnostica scocco)
//  Cesare Pagura · Padova/Noale IT · 24 luglio 2026
// ----------------------------------------------------------------------------
//  PERCHE' ESISTE
//    Gli scocchi VERI non vengono rilevati, gli scuotimenti a mano si'. Sono
//    state fatte piu' ipotesi (soglia, campionamento del trigger, filtro LPF)
//    e nessuna ha risolto. Il motivo di fondo e' che NON ABBIAMO UN SOLO DATO
//    di uno scocco vero: tutti i burst salvati finora erano scuotimenti.
//    Finche' non si vede cosa legge DAVVERO il sensore quando parte la freccia,
//    ogni teoria e' infalsificabile.
//
//    Questo firmware non giudica e non decide: REGISTRA TUTTO, senza trigger,
//    senza soglie. Poi i dati parlano.
//
//  COSA FA
//    - Registra OGNI campione IMU in PSRAM (nessuna scrittura SD durante la
//      cattura: zero latenze che potrebbero alterare la misura).
//    - Mostra a schermo il PEAK-HOLD di |ax| |ay| |az|: dopo il tiro leggi
//      subito il massimo raggiunto, senza aspettare l'analisi al PC.
//    - A fine registrazione scarica tutto su microSD in un unico file.
//
//  SCELTA DELL'ODR: 500 Hz (non 250 come il firmware di produzione).
//    1) Raddoppia la risoluzione temporale: un impulso di 10 ms passa da ~2 a
//       ~5 campioni, quindi la forma d'onda e' leggibile.
//    2) Il filtro passa-basso del QMI8658 (LPF_MODE_2) taglia a una PERCENTUALE
//       dell'ODR: raddoppiando l'ODR raddoppia anche la banda passante. Se lo
//       scocco a 500 Hz risultasse ben visibile mentre a 250 no, avremmo la
//       prova che il filtro/ODR era la causa.
//
//  USO
//    pio run -e archbb_183_shotrec -t upload
//    - Tocca lo schermo: START. Registra fino a ~40 s (o STOP col tocco).
//    - TIRA 2-3 FRECCE VERE durante la registrazione.
//    - Tocca di nuovo: STOP e salvataggio su SD (/ARCHBB/REC/rec_NNNN.bin).
//    - Leggi il peak |az| a schermo: e' la risposta immediata.
// ============================================================================
#ifdef ARCHBB_SHOT_REC

#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <SD.h>
#include <SPI.h>

#include "config.h"
#include "display.h"
#include "touch.h"
#include "imu.h"
#include "mount.h"
#include "sd_storage.h"

// ----------------------------------------------------------------------------
//  Buffer di registrazione in PSRAM
// ----------------------------------------------------------------------------
//  40 s a 500 Hz = 20000 campioni x 30 B = ~600 KB. La PSRAM ne ha 8 MB: nessun
//  problema. Registrare in RAM ed esportare DOPO evita che la latenza della SD
//  disturbi la cattura proprio nell'istante che ci interessa.
static constexpr uint32_t REC_MAX_SAMPLES = 20000;
static ImuSample* s_rec = nullptr;
static uint32_t   s_recN = 0;

enum class RecState : uint8_t { PRONTO, REC, SALVATAGGIO, FATTO, ERRORE };
static RecState s_state = RecState::PRONTO;

// Peak-hold (moduli, azzerati a ogni START)
static float s_pkAx = 0, s_pkAy = 0, s_pkAz = 0, s_pkG = 0;
// Valori correnti (per il display "vivo")
static float s_curAx = 0, s_curAy = 0, s_curAz = 0;
static uint32_t s_recStartMs = 0;
static char s_lastFile[40] = "";

// ============================================================================
//  Salvataggio su SD
// ============================================================================
//  Formato: header 16 B + n x 30 B (ImuSample packed, identico ai burst).
//    [0..3]   magic "ABBR"  (Recording, per distinguerlo dai burst "ABB1")
//    [4..5]   versione formato = 1
//    [6..9]   n_samples (uint32: qui possono essere >65535)
//    [10..11] odr_hz nominale
//    [12..13] flags: bit0 = mount applicato (frame canonico)
//    [14..15] riservato
static bool saveRecording() {
  if (!g_sd_ok || s_recN == 0) return false;

  if (!SD.exists("/ARCHBB"))     SD.mkdir("/ARCHBB");
  if (!SD.exists("/ARCHBB/REC")) SD.mkdir("/ARCHBB/REC");

  // Numerazione progressiva dal filesystem (come le sessioni: il disco e' lo stato).
  char path[64];
  uint16_t id = 1;
  for (; id < 1000; id++) {
    snprintf(path, sizeof(path), "/ARCHBB/REC/rec_%04u.bin", id);
    if (!SD.exists(path)) break;
  }

  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;

  uint8_t hdr[16] = {0};
  memcpy(hdr, "ABBR", 4);
  hdr[4] = 1; hdr[5] = 0;                          // versione formato
  hdr[6]  = (uint8_t)(s_recN & 0xFF);
  hdr[7]  = (uint8_t)((s_recN >> 8) & 0xFF);
  hdr[8]  = (uint8_t)((s_recN >> 16) & 0xFF);
  hdr[9]  = (uint8_t)((s_recN >> 24) & 0xFF);
  uint16_t hz = odrToHz(ODR_500HZ);
  hdr[10] = (uint8_t)(hz & 0xFF);
  hdr[11] = (uint8_t)((hz >> 8) & 0xFF);
  hdr[12] = 1;                                     // flags: mount applicato

  f.write(hdr, sizeof(hdr));

  // Scrittura a blocchi (più efficiente di un unico write enorme).
  const uint32_t CHUNK = 256;                      // campioni per blocco
  for (uint32_t i = 0; i < s_recN; i += CHUNK) {
    uint32_t k = (s_recN - i < CHUNK) ? (s_recN - i) : CHUNK;
    f.write((const uint8_t*)&s_rec[i], k * sizeof(ImuSample));
  }
  f.close();

  snprintf(s_lastFile, sizeof(s_lastFile), "rec_%04u.bin", id);
  return true;
}

// ============================================================================
//  DISPLAY
// ============================================================================
static void drawScreen() {
  tft.fillScreen(ArchColor::BG);
  tft.drawRect(0, 0, ARCHBB_W, ARCHBB_H, ArchColor::BORDER);
  tft.setTextDatum(TC_DATUM);

  // --- Titolo / stato ------------------------------------------------------
  const char* st = "PRONTO";
  uint16_t stCol = ArchColor::TEXT2;
  switch (s_state) {
    case RecState::PRONTO:      st = "PRONTO";     stCol = ArchColor::ACCENT; break;
    case RecState::REC:         st = "REC";        stCol = ArchColor::BAD;    break;
    case RecState::SALVATAGGIO: st = "SALVO...";   stCol = ArchColor::AMBER;  break;
    case RecState::FATTO:       st = "SALVATO";    stCol = ArchColor::GOOD;   break;
    case RecState::ERRORE:      st = "ERRORE SD";  stCol = ArchColor::BAD;    break;
  }
  tft.setTextColor(stCol, ArchColor::BG);
  tft.drawString(st, ARCHBB_W / 2, 8, 4);

  // --- PEAK-HOLD: e' il dato che conta -------------------------------------
  //  Numeri grandi: dopo il tiro devi poterli leggere a colpo d'occhio.
  tft.setTextDatum(TL_DATUM);
  char line[32];
  int y = 46;

  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  tft.drawString("PICCHI (m/s2)", 14, y, 2);
  y += 24;

  auto peakRow = [&](const char* nm, float pk, uint16_t col) {
    tft.setTextColor(ArchColor::TEXT2, ArchColor::BG);
    tft.drawString(nm, 14, y, 4);
    tft.setTextColor(col, ArchColor::BG);
    snprintf(line, sizeof(line), "%6.1f", pk);
    tft.drawString(line, 70, y, 4);
    y += 32;
  };
  peakRow("aX", s_pkAx, ArchColor::TEXT);
  peakRow("aY", s_pkAy, ArchColor::TEXT);
  // az e' l'asse dello scocco: evidenziato. Verde se supererebbe la soglia 20.
  peakRow("aZ", s_pkAz, s_pkAz > 20.0f ? ArchColor::GOOD : ArchColor::AMBER);

  // --- Contatore campioni / durata ----------------------------------------
  y += 4;
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  if (s_state == RecState::REC) {
    float sec = (millis() - s_recStartMs) / 1000.0f;
    snprintf(line, sizeof(line), "%.1fs  %lu campioni", sec, (unsigned long)s_recN);
  } else if (s_state == RecState::FATTO) {
    snprintf(line, sizeof(line), "%s  (%lu)", s_lastFile, (unsigned long)s_recN);
  } else {
    snprintf(line, sizeof(line), "SD %s  ODR 500Hz", g_sd_ok ? "ok" : "ASSENTE");
  }
  tft.drawString(line, 14, y, 2);

  // --- Piede: istruzioni ---------------------------------------------------
  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  const char* hint = (s_state == RecState::REC) ? "tocca per FERMARE e salvare"
                                                : "tocca per REGISTRARE";
  tft.drawString(hint, ARCHBB_W / 2, ARCHBB_H - 8, 2);
}

// ----------------------------------------------------------------------------
//  drawPeaksOnly — aggiornamento PARZIALE dei soli numeri di picco.
// ----------------------------------------------------------------------------
//  CRITICO: durante la registrazione NON si puo' fare un fillScreen. Un
//  ridisegno completo blocca il loop per decine di millisecondi e a 500 Hz
//  significa perdere 10-25 campioni: se lo scocco cadesse proprio li', la
//  diagnosi sarebbe falsata dallo strumento stesso. Qui riscriviamo solo le tre
//  celle dei numeri (pochi ms), lasciando intatto il resto della schermata.
static void drawPeaksOnly() {
  tft.setTextDatum(TL_DATUM);
  char line[32];
  const int xNum = 70, wNum = 120, hRow = 32;
  int y = 70;   // stessa origine delle righe in drawScreen()

  auto cell = [&](float pk, uint16_t col) {
    tft.fillRect(xNum, y, wNum, 28, ArchColor::BG);   // pulisce solo il numero
    tft.setTextColor(col, ArchColor::BG);
    snprintf(line, sizeof(line), "%6.1f", pk);
    tft.drawString(line, xNum, y, 4);
    y += hRow;
  };
  cell(s_pkAx, ArchColor::TEXT);
  cell(s_pkAy, ArchColor::TEXT);
  cell(s_pkAz, s_pkAz > 20.0f ? ArchColor::GOOD : ArchColor::AMBER);

  // Contatore: una riga sola, area piccola.
  tft.fillRect(14, y + 4, ARCHBB_W - 28, 20, ArchColor::BG);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  float sec = (millis() - s_recStartMs) / 1000.0f;
  snprintf(line, sizeof(line), "%.1fs  %lu campioni", sec, (unsigned long)s_recN);
  tft.drawString(line, 14, y + 4, 2);
}

// ============================================================================
//  SETUP / LOOP
// ============================================================================
void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);

  displayInit();
  touchInit();

  // ODR 500 Hz: massima risoluzione temporale e banda passante doppia rispetto
  // al firmware di produzione (vedi nota in testa al file).
  bool imuOk = imu_init(ODR_500HZ, 8, 512);
  bool sdOk  = sd_init();

  // Buffer in PSRAM.
  s_rec = (ImuSample*)ps_malloc(REC_MAX_SAMPLES * sizeof(ImuSample));
  if (!s_rec) s_rec = (ImuSample*)malloc(REC_MAX_SAMPLES * sizeof(ImuSample));

  tft.fillScreen(ArchColor::BG);
  tft.setTextDatum(MC_DATUM);
  if (imuOk && s_rec) {
    tft.setTextColor(ArchColor::GOOD, ArchColor::BG);
    tft.drawString("REGISTRATORE", ARCHBB_W/2, ARCHBB_H/2 - 20, 4);
    tft.setTextColor(ArchColor::TEXT2, ArchColor::BG);
    char l[40]; snprintf(l, sizeof(l), "IMU ok  SD %s  500Hz", sdOk ? "ok" : "NO");
    tft.drawString(l, ARCHBB_W/2, ARCHBB_H/2 + 16, 2);
    s_state = RecState::PRONTO;
  } else {
    tft.setTextColor(ArchColor::BAD, ArchColor::BG);
    tft.drawString(imuOk ? "PSRAM FAIL" : "IMU FAIL", ARCHBB_W/2, ARCHBB_H/2, 4);
    s_state = RecState::ERRORE;
  }
  delay(1500);
  drawScreen();

  if ((bool)Serial) {
    Serial.println();
    Serial.println(F("### ArchBB — REGISTRATORE SCOCCO ###"));
    Serial.println(F("Tocca per START/STOP. Tira 2-3 frecce VERE durante la REC."));
    Serial.printf ("Buffer: %lu campioni (~%.0f s a 500Hz)\n",
                   (unsigned long)REC_MAX_SAMPLES, REC_MAX_SAMPLES / 500.0f);
  }
}

void loop() {
  // --- 1) Acquisizione: ogni campione disponibile, senza filtri ------------
  //  Chiamiamo imu_read_raw() (grezzi in frame SENSORE) e applichiamo il mount
  //  come fa il firmware di produzione, cosi' l'az registrato e' ESATTAMENTE
  //  quello che il trigger valuterebbe.
  if (s_state == RecState::REC && s_rec) {
    float ax, ay, az, gx, gy, gz;
    if (imu_read_raw(ax, ay, az, gx, gy, gz)) {
      mount_apply(g_mount_orientation, ax, ay, az, gx, gy, gz);

      s_curAx = ax; s_curAy = ay; s_curAz = az;
      if (fabsf(ax) > s_pkAx) s_pkAx = fabsf(ax);
      if (fabsf(ay) > s_pkAy) s_pkAy = fabsf(ay);
      if (fabsf(az) > s_pkAz) s_pkAz = fabsf(az);
      float g = sqrtf(ax*ax + ay*ay + az*az);
      if (g > s_pkG) s_pkG = g;

      if (s_recN < REC_MAX_SAMPLES) {
        ImuSample& s = s_rec[s_recN];
        s.seq          = (uint16_t)(s_recN & 0xFFFF);
        s.timestamp_us = (uint32_t)esp_timer_get_time();
        s.ax = ax; s.ay = ay; s.az = az;
        s.gx = gx; s.gy = gy; s.gz = gz;
        s_recN++;
      } else {
        // Buffer pieno: ferma e salva da solo.
        s_state = RecState::SALVATAGGIO;
        drawScreen();
        s_state = saveRecording() ? RecState::FATTO : RecState::ERRORE;
        drawScreen();
      }
    }
  }

  // --- 2) Tocco: START / STOP ---------------------------------------------
  //  Durante la REC leggiamo il touch solo ogni ~150 ms: ogni lettura e' traffico
  //  I2C che ruba tempo al campionamento (a 500 Hz il budget e' 2 ms/campione).
  static uint32_t lastTouchPoll = 0;
  TouchData td = {};   // valid=false finche non lo leggiamo davvero
  if (s_state != RecState::REC || millis() - lastTouchPoll > 150) {
    lastTouchPoll = millis();
    td = touchRead();
  }
  static uint32_t lastTouch = 0;
  if (td.valid && millis() - lastTouch > 500) {
    lastTouch = millis();
    if (s_state == RecState::REC) {
      s_state = RecState::SALVATAGGIO;
      drawScreen();
      s_state = saveRecording() ? RecState::FATTO : RecState::ERRORE;
      if ((bool)Serial) {
        Serial.printf("STOP: %lu campioni. Picchi |ax|=%.1f |ay|=%.1f |az|=%.1f |g|max=%.1f\n",
                      (unsigned long)s_recN, s_pkAx, s_pkAy, s_pkAz, s_pkG);
      }
      drawScreen();
    } else {
      // START: azzera peak-hold e contatore.
      s_recN = 0;
      s_pkAx = s_pkAy = s_pkAz = s_pkG = 0;
      s_recStartMs = millis();
      s_state = RecState::REC;
      if ((bool)Serial) Serial.println(F("START registrazione — tira le frecce!"));
      drawScreen();
    }
  }

  // --- 3) Refresh PARZIALE dei picchi durante la REC -----------------------
  //  Solo i numeri (pochi ms), mai un fillScreen: vedi drawPeaksOnly().
  static uint32_t lastDraw = 0;
  if (s_state == RecState::REC && millis() - lastDraw > 300) {
    lastDraw = millis();
    drawPeaksOnly();
  }

  // Nessun delay in REC: vogliamo pescare ogni campione disponibile.
  if (s_state != RecState::REC) delay(20);
}

#endif // ARCHBB_SHOT_REC
