// ============================================================================
//  ArchBB 1.83 — main_raw_diag.cpp · DIAGNOSTICA SCOCCO REALE
//  Cesare Pagura · Padova/Noale IT · 24 luglio 2026
// ----------------------------------------------------------------------------
//  PERCHE' ESISTE
//    Sulla 1.83 il trigger scatta con uno scuotimento a mano ma NON con la
//    freccia vera. Sulla 1.69, 44 tiri reali mostrano una firma inequivocabile:
//      - asse dominante al picco: az in 44 casi su 44
//      - picco mediano |az| = 38.4 m/s^2 (nessun tiro sotto i 20)
//      - 13 campioni consecutivi sopra soglia (evento largo ~60ms)
//    Uno scocco vero e' quindi PIU' facile da rilevare di uno scuotimento. Se
//    sulla 1.83 non scatta, il segnale che arriva al chip e' diverso — ma non
//    abbiamo MAI registrato uno scocco vero su questa scheda: stiamo ragionando
//    al buio. Questo firmware serve a togliere il buio.
//
//  COSA FA
//    1) PEAK-HOLD a schermo: massimo di |ax|, |ay|, |az| dall'ultimo azzeramento.
//       Tiri UNA freccia e leggi i tre numeri. Risposta immediata:
//         - picco grosso su ay o ax e az piccolo -> l'energia esiste ma finisce
//           su un ALTRO asse (geometria/montaggio da rivedere)
//         - tutti e tre piccoli -> l'energia non arriva al sensore
//           (smorzamento del supporto, aggancio non rigido, ...)
//    2) LOG CONTINUO su SD: registra OGNI campione (nessun trigger di mezzo),
//       cosi' si analizza a posteriori la forma d'onda completa dello scocco e
//       la si confronta con i 44 tiri della 1.69.
//
//  COMANDI (touch)
//    - META' SUPERIORE  -> azzera i picchi
//    - META' INFERIORE  -> avvia/ferma la registrazione su SD
//
//  FILE PRODOTTI:  /ARCHBB/RAW_NNNN.BIN
//    header 16B: "ABBR" + ver(u16) + odr_hz(u16) + n_samples(u32) + riserva(u32)
//    poi N record ImuSample da 30B (stesso layout dei burst: seq,ts,ax..gz)
//    NB: n_samples nell'header viene riscritto alla chiusura del file.
//
//  METODO: nessun trigger, nessuna soglia, nessuna interpretazione. Si registra
//  cio' che il sensore vede e si guardano i numeri. "I dati comandano."
// ============================================================================
#ifdef ARCHBB_RAW_DIAG

#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <SD.h>

#include "config.h"
#include "display.h"
#include "touch.h"
#include "imu.h"
#include "sd_storage.h"

// ----------------------------------------------------------------------------
//  PEAK-HOLD — aggiornato dall'hook (imu_task, Core 1), letto dal loop UI.
// ----------------------------------------------------------------------------
//  volatile: scritti da un task e letti da un altro. Sono float singoli: su
//  Xtensa la scrittura e' atomica, non serve mutex per un valore diagnostico.
static volatile float s_pk_ax = 0, s_pk_ay = 0, s_pk_az = 0;
static volatile float s_cur_mag = 0;          // |a| corrente (per capire se e' fermo)
static volatile uint32_t s_fed = 0;           // campioni visti dall'hook

// ----------------------------------------------------------------------------
//  DOPPIO BUFFER per il log su SD.
// ----------------------------------------------------------------------------
//  L'hook (Core 1, hot loop) NON deve mai scrivere su SD: riempie un buffer in
//  RAM e, quando e' pieno, lo marca "pronto" e passa all'altro. Il loop UI
//  scrive su SD il buffer pronto. Cosi' la scrittura (lenta, a blocchi) non
//  rallenta mai il campionamento.
//  256 campioni x 30B = 7.7KB per buffer, ~1.14s di registrazione a 224Hz.
#define RAW_CHUNK 256
static ImuSample s_bufA[RAW_CHUNK];
static ImuSample s_bufB[RAW_CHUNK];
static volatile uint16_t s_fill      = 0;      // indice di riempimento nel buffer attivo
static volatile bool     s_useA      = true;   // quale buffer sta riempiendo l'hook
static volatile bool     s_readyA    = false;  // A pieno, da scrivere
static volatile bool     s_readyB    = false;  // B pieno, da scrivere
static volatile bool     s_recording = false;
static volatile uint32_t s_dropped   = 0;      // chunk persi (SD troppo lenta)

static File     s_file;
static uint32_t s_written  = 0;                // campioni scritti su file
static char     s_fileName[32] = "";

// ============================================================================
//  raw_diag_feed — HOOK: chiamato da imu_task per OGNI campione.
// ----------------------------------------------------------------------------
//  Deve essere velocissimo: nessuna I/O, nessun lock, nessuna Serial.
// ============================================================================
void raw_diag_feed(const ImuSample& s) {
  s_fed++;

  // --- peak-hold (moduli) ---
  float aax = fabsf(s.ax), aay = fabsf(s.ay), aaz = fabsf(s.az);
  if (aax > s_pk_ax) s_pk_ax = aax;
  if (aay > s_pk_ay) s_pk_ay = aay;
  if (aaz > s_pk_az) s_pk_az = aaz;
  s_cur_mag = sqrtf(s.ax*s.ax + s.ay*s.ay + s.az*s.az);

  // --- accodamento per il log ---
  if (!s_recording) return;
  ImuSample* buf = s_useA ? s_bufA : s_bufB;
  buf[s_fill++] = s;
  if (s_fill >= RAW_CHUNK) {
    // Buffer pieno: marcalo pronto e passa all'altro.
    if (s_useA) {
      if (s_readyA) s_dropped++;   // il loop non ha ancora scritto: chunk perso
      s_readyA = true;
    } else {
      if (s_readyB) s_dropped++;
      s_readyB = true;
    }
    s_useA = !s_useA;
    s_fill = 0;
  }
}

// ============================================================================
//  Gestione file
// ============================================================================
static void writeHeader(File& f, uint32_t nSamples) {
  uint8_t hdr[16] = {0};
  hdr[0]='A'; hdr[1]='B'; hdr[2]='B'; hdr[3]='R';
  uint16_t ver = 1, odr = 224;
  memcpy(hdr+4, &ver, 2);
  memcpy(hdr+6, &odr, 2);
  memcpy(hdr+8, &nSamples, 4);
  f.write(hdr, sizeof(hdr));
}

static void recStart() {
  if (s_recording || !g_sd_ok) return;
  // Numero progressivo libero.
  for (uint16_t i = 1; i < 9999; i++) {
    snprintf(s_fileName, sizeof(s_fileName), "/ARCHBB/RAW_%04u.BIN", i);
    if (!SD.exists(s_fileName)) break;
  }
  s_file = SD.open(s_fileName, FILE_WRITE);
  if (!s_file) { s_fileName[0] = '\0'; return; }
  writeHeader(s_file, 0);             // n_samples riscritto alla chiusura
  s_written = 0; s_dropped = 0;
  s_fill = 0; s_useA = true; s_readyA = s_readyB = false;
  s_recording = true;
}

static void recStop() {
  if (!s_recording) return;
  s_recording = false;
  // Scarica quel che resta nel buffer parziale.
  ImuSample* buf = s_useA ? s_bufA : s_bufB;
  uint16_t rest = s_fill;
  if (rest && s_file) {
    s_file.write((const uint8_t*)buf, (size_t)rest * sizeof(ImuSample));
    s_written += rest;
  }
  if (s_file) {
    // Riscrive il conteggio nell'header, poi chiude.
    s_file.flush();
    s_file.seek(8);
    s_file.write((const uint8_t*)&s_written, 4);
    s_file.close();
  }
  s_fill = 0; s_readyA = s_readyB = false;
}

// Scrive su SD i chunk pronti. Chiamata dal loop UI (mai dall'hook).
static void serviceWrites() {
  if (!s_recording || !s_file) return;
  if (s_readyA) {
    s_file.write((const uint8_t*)s_bufA, sizeof(s_bufA));
    s_written += RAW_CHUNK; s_readyA = false;
  }
  if (s_readyB) {
    s_file.write((const uint8_t*)s_bufB, sizeof(s_bufB));
    s_written += RAW_CHUNK; s_readyB = false;
  }
}

// ============================================================================
//  DISPLAY
// ============================================================================
static void draw() {
  tft.fillScreen(ArchColor::BG);
  tft.drawRect(0, 0, ARCHBB_W, ARCHBB_H, ArchColor::BORDER);

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(ArchColor::ACCENT, ArchColor::BG);
  tft.drawString("PICCHI |a|", ARCHBB_W/2, 6, 2);

  // --- i tre picchi, grandi. Verde quello dominante: si legge a colpo d'occhio.
  float pax = s_pk_ax, pay = s_pk_ay, paz = s_pk_az;
  float mx = fmaxf(pax, fmaxf(pay, paz));
  char line[24];
  int y = 34; const int dy = 40;

  auto row = [&](const char* nome, float v) {
    bool dom = (v >= mx - 0.01f) && (mx > 0.5f);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(dom ? ArchColor::GOOD : ArchColor::TEXT2, ArchColor::BG);
    tft.drawString(nome, 14, y + 8, 2);
    tft.setTextDatum(TR_DATUM);
    snprintf(line, sizeof(line), "%6.1f", v);
    tft.drawString(line, ARCHBB_W - 14, y, 6);
    y += dy;
  };
  row("aX", pax);
  row("aY", pay);
  row("aZ", paz);

  // --- riferimento: cosa ci aspettiamo da uno scocco vero (dalla 1.69).
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  tft.drawString("scocco 1.69: aZ ~38 m/s2", ARCHBB_W/2, y + 2, 1);

  // --- stato registrazione ---
  y += 20;
  if (s_recording) {
    tft.setTextColor(ArchColor::BAD, ArchColor::BG);
    snprintf(line, sizeof(line), "REC  %lu campioni", (unsigned long)s_written);
    tft.drawString(line, ARCHBB_W/2, y, 2);
    if (s_dropped) {
      tft.setTextColor(ArchColor::AMBER, ArchColor::BG);
      snprintf(line, sizeof(line), "persi %lu chunk", (unsigned long)s_dropped);
      tft.drawString(line, ARCHBB_W/2, y + 18, 1);
    }
  } else if (s_fileName[0]) {
    tft.setTextColor(ArchColor::GOOD, ArchColor::BG);
    snprintf(line, sizeof(line), "salvato %lu camp.", (unsigned long)s_written);
    tft.drawString(line, ARCHBB_W/2, y, 2);
    tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
    tft.drawString(s_fileName + 8, ARCHBB_W/2, y + 18, 1);   // salta "/ARCHBB/"
  } else {
    tft.setTextColor(g_sd_ok ? ArchColor::TEXT3 : ArchColor::AMBER, ArchColor::BG);
    tft.drawString(g_sd_ok ? "pronto a registrare" : "SD assente", ARCHBB_W/2, y, 2);
  }

  // --- istruzioni touch ---
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  tft.drawString("tocca SOPRA = azzera picchi", ARCHBB_W/2, ARCHBB_H - 26, 1);
  tft.setTextColor(s_recording ? ArchColor::BAD : ArchColor::ACCENT, ArchColor::BG);
  tft.drawString(s_recording ? "tocca SOTTO = FERMA rec"
                             : "tocca SOTTO = avvia rec", ARCHBB_W/2, ARCHBB_H - 12, 1);
}

// ============================================================================
//  SETUP / LOOP
// ============================================================================
void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);

  displayInit();
  touchInit();                       // avvia Wire + crea il mutex I2C

  bool imuOk = imu_init(ODR_250HZ, 8, 512);
  bool sdOk  = sd_init();

  tft.fillScreen(ArchColor::BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(ArchColor::ACCENT, ArchColor::BG);
  tft.drawString("RAW DIAG", ARCHBB_W/2, ARCHBB_H/2 - 30, 4);
  tft.setTextColor(imuOk ? ArchColor::GOOD : ArchColor::BAD, ArchColor::BG);
  tft.drawString(imuOk ? "IMU OK" : "IMU FAIL", ARCHBB_W/2, ARCHBB_H/2 + 4, 2);
  tft.setTextColor(sdOk ? ArchColor::GOOD : ArchColor::AMBER, ArchColor::BG);
  tft.drawString(sdOk ? "SD OK" : "SD assente", ARCHBB_W/2, ARCHBB_H/2 + 26, 2);
  delay(1500);

  // SOLO l'imu_task: niente trigger, niente scoring. I campioni arrivano
  // all'hook raw_diag_feed(), che fa peak-hold e accoda per il log.
  if (imuOk) xTaskCreatePinnedToCore(imu_task, "imu", 4096, nullptr, 5, nullptr, 1);

  if ((bool)Serial) {
    Serial.println();
    Serial.println(F("### ArchBB 1.83 — RAW DIAG ###"));
    Serial.println(F("Tocca SOPRA: azzera picchi. Tocca SOTTO: avvia/ferma REC."));
    Serial.println(F("Riferimento scocco vero (1.69): picco |az| ~38 m/s^2."));
  }
  draw();
}

void loop() {
  // 1) Scrivi su SD i chunk pronti (mai dall'hook: qui, con calma).
  serviceWrites();

  // 2) Touch: sopra = azzera picchi, sotto = avvia/ferma REC.
  TouchData td = touchRead();
  static uint32_t lastTouch = 0;
  if (td.valid && millis() - lastTouch > 400) {
    lastTouch = millis();
    if (td.y < ARCHBB_H / 2) {
      s_pk_ax = s_pk_ay = s_pk_az = 0.0f;       // azzera picchi
    } else {
      if (s_recording) recStop(); else recStart();
    }
    draw();
  }

  // 3) Ridisegna ~5 Hz (i picchi cambiano di rado; niente sfarfallio).
  static uint32_t lastDraw = 0;
  if (millis() - lastDraw > 200) { lastDraw = millis(); draw(); }

  // 4) Report seriale ~1 Hz.
  static uint32_t lastSer = 0;
  if (millis() - lastSer > 1000) {
    lastSer = millis();
    if ((bool)Serial && Serial.availableForWrite() > 0) {
      Serial.printf("picchi |ax|=%.1f |ay|=%.1f |az|=%.1f | |a|now=%.1f | camp=%lu | rec=%d scritti=%lu persi=%lu\n",
                    s_pk_ax, s_pk_ay, s_pk_az, s_cur_mag,
                    (unsigned long)s_fed, (int)s_recording,
                    (unsigned long)s_written, (unsigned long)s_dropped);
    }
  }

  delay(10);
}

#endif // ARCHBB_RAW_DIAG
