// ============================================================================
//  ArchBB 1.83 — display.cpp · FASE 1 (bring-up ST7789V2) · v2
//  Cesare Pagura · Padova/Noale IT · 11 luglio 2026
// ----------------------------------------------------------------------------
//  CORREZIONE v2 — la banda bianca in alto ("i dati comandano"):
//  In v1 applicavo l'offset y=20 con setViewport(0,20,240,284). ERRORE:
//  setViewport NON sposta l'indirizzamento della GRAM del controller, ritaglia
//  solo l'area di disegno SOFTWARE dentro un frame gia' mappato. Cosi' facendo
//  sommavo un secondo offset a quello che TFT_eSPI applica gia' via CGRAM_OFFSET
//  + setRotation, e la fascia 0..20 restava SCOPERTA -> GRAM non inizializzata
//  visibile = i ~20 pixel bianchi che vedevi in cima.
//
//  Fix corretto: RIMOSSO setViewport. L'offset di pannello e' responsabilita'
//  del driver (CGRAM_OFFSET nel .ini + dimensioni 240x284). La doc TFT_eSPI:
//  "gli offset sono determinati in setRotation e applicati a ogni operazione
//  grafica" e "puo' servire chiamare setRotation DOPO init() perche' si
//  applichino". Quindi: init() -> setRotation() -> fillScreen(), e basta.
//  Le coordinate (0,0) del disegno corrispondono al primo pixel visibile:
//  l'offset e' gestito dentro la libreria, trasparente al resto del codice.
// ============================================================================
#include "display.h"
#include "scoring.h"       // v2.16: elevIsSet/ELEV_NOT_SET per il riepilogo
#include "shot_angles.h"   // FASE 4c: nomi classi hold/release, shot_angle_to_cdeg
#include "mount.h"         // FASE 4c: mount_apply per il DIAG a 4 montaggi
#include <math.h>

// Istanza display condivisa (dichiarata extern in display.h). Scoring e altri
// moduli disegnano su questa stessa istanza.
TFT_eSPI tft = TFT_eSPI();

// ============================================================================
//  FASE 24a — RETROILLUMINAZIONE PWM (era: acceso/spento su GPIO)
// ============================================================================
//  Prima della 24a il backlight era digitalWrite(ARCHBB_BL_PIN, HIGH): acceso al
//  dal boot allo spegnimento. E' la voce di consumo piu' grossa del dispositivo
//  e l'unica che stava sempre al massimo anche mentre la scheda pendeva dal
//  riser nella custodia fra una piazzola e l'altra.
//
//  Il pin resta GPIO40, active-HIGH: cambia solo COSA ci mettiamo sopra.
//
//  PERCHE' LA CURVA NON E' LINEARE
//    La sensazione di luminosita' cresce all'incirca come una potenza della
//    luce emessa (legge di Stevens; in grafica e' il "gamma", ~2.2). Un duty
//    del 33% non sembra un terzo della luce: sembra circa il 60%. Tradotto in
//    energia: chiedendo "60" si consuma un terzo invece che due terzi, e
//    l'occhio non se ne accorge. La curva non e' un vezzo estetico, e' il
//    risparmio piu' economico del progetto — zero compromessi di leggibilita'.
//
//  TRAPPOLA DEL CORE 3.x
//    Da Arduino-ESP32 3.0 le vecchie ledcSetup()/ledcAttachPin() NON ESISTONO
//    piu': si usa ledcAttach(pin, freq, risoluzione) e si scrive il duty sul
//    PIN. Compilare con l'API vecchia su core 3.0.5 da' "not declared in this
//    scope" — errore chiaro, ma solo se si sa perche'. La guardia sotto tiene
//    vivi entrambi i mondi.
// ----------------------------------------------------------------------------
static uint8_t s_blPct    = 0;      // ultimo valore richiesto (0..100)
static bool    s_blReady  = false;  // LEDC gia' agganciato al pin?

// Conversione percentuale percepita -> duty, con gamma 2.2 e un pavimento:
// qualunque percentuale > 0 deve produrre almeno un filo di luce, altrimenti
// "1%" e "spento" diventerebbero la stessa cosa e la penombra non si vedrebbe.
static uint32_t blDutyFromPct(uint8_t pct) {
  if (pct == 0) return 0;
  if (pct > 100) pct = 100;
  const uint32_t maxDuty = (1u << ENERGIA_BL_PWM_BIT) - 1u;
  float lin = powf((float)pct / 100.0f, 2.2f);
  uint32_t d = (uint32_t)lroundf(lin * (float)maxDuty);
  if (d < 4) d = 4;                  // pavimento: mai buio pesto per errore
  return d;
}

void displayBacklightPercent(uint8_t pct) {
  if (pct > 100) pct = 100;
  s_blPct = pct;

  if (!s_blReady) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(ARCHBB_BL_PIN, ENERGIA_BL_PWM_HZ, ENERGIA_BL_PWM_BIT);
#else
    // Core 2.x: canale 7 (il piu' alto, meno conteso da altre librerie).
    ledcSetup(7, ENERGIA_BL_PWM_HZ, ENERGIA_BL_PWM_BIT);
    ledcAttachPin(ARCHBB_BL_PIN, 7);
#endif
    s_blReady = true;
  }

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(ARCHBB_BL_PIN, blDutyFromPct(pct));
#else
  ledcWrite(7, blDutyFromPct(pct));
#endif
}

uint8_t displayBacklightGet() { return s_blPct; }

// Involucro storico. Chi chiamava displayBacklight(true) nel bring-up continua
// a funzionare; "acceso" adesso significa "al livello base di configurazione",
// non "al massimo".
void displayBacklight(bool on) {
  displayBacklightPercent(on ? ENERGIA_BL_BASE_PCT : 0);
}

// ----------------------------------------------------------------------------
//  Sleep del pannello ST7789 (comandi 0x10 / 0x11)
// ----------------------------------------------------------------------------
//  Spenta la retro, il pannello continua comunque a rinfrescare la GRAM e a
//  pilotare le colonne: qualche mA che se ne va per illuminare niente. Lo
//  SLEEP IN lo ferma. Lo SLEEP OUT richiede 120 ms prima che il controller
//  accetti dati validi — li aspettiamo QUI dentro, perche' un ritardo
//  dimenticato dal chiamante si manifesta come schermata sporca a caso, cioe'
//  come il peggior tipo di bug: intermittente e non riproducibile a comando.
void displayPanelSleep(bool sleep) {
#if ENERGIA_PANEL_SLEEP
  if (sleep) {
    tft.writecommand(0x10);      // SLPIN
    delay(5);                    // il datasheet chiede >5 ms prima di altro
  } else {
    tft.writecommand(0x11);      // SLPOUT
    delay(120);                  // obbligatori: il pannello risale dal sonno
  }
#else
  (void)sleep;
#endif
}

// ----------------------------------------------------------------------------
//  Inizializzazione completa.
// ----------------------------------------------------------------------------
void displayInit() {
  // 1) Backlight PRIMA dell'init: durante il reset del pannello si vede almeno
  //    che la scheda e' viva.
  displayBacklight(true);

  // 2) Init libreria: reset HW (TFT_RST=38), sequenza ST7789, applica
  //    TFT_INVERSION_ON e TFT_RGB_ORDER=TFT_BGR dai -D.
  tft.init();

  // 3) FASE 24a — RIAGGANCIO DEL LEDC DOPO tft.init(). CINTURA E BRETELLE.
  //    Il proprietario del piedino ora e' solo questo file (in platformio.ini
  //    non esiste piu' -D TFT_BL, e senza quel define TFT_eSPI::init() non
  //    tocca il pin). Ma la funzione che lo riprendeva sta ancora dentro la
  //    libreria, dietro un #ifdef: basterebbe che qualcuno rimettesse quel
  //    -D "per simmetria con gli altri pin del display" e il difetto
  //    tornerebbe identico, muto, e questa volta con la spiegazione gia'
  //    scritta e gia' dimenticata.
  //
  //    Costa una riga: si dichiara il canale "da riagganciare" e la prima
  //    scrittura successiva rifa' ledcAttach. Se il pin non e' stato rubato,
  //    riagganciarlo non fa nulla di male.
  s_blReady = false;
  displayBacklightPercent(s_blPct);

  // 4) setRotation DOPO init: e' QUI che TFT_eSPI applica l'offset CGRAM.
  //    Con CGRAM_OFFSET + TFT_HEIGHT=284, la libreria mappa la coordinata
  //    software (0,0) sulla riga fisica corretta. Nessun setViewport.
  tft.setRotation(0);   // portrait nativo 240x284

  // 5) Pulizia a nero SU TUTTA l'area: ora copre anche l'ex-fascia bianca.
  tft.fillScreen(ArchColor::BG);
}

// ----------------------------------------------------------------------------
//  displaySplash — "ciao mondo" ArchBB.
// ----------------------------------------------------------------------------
void displaySplash() {
  tft.fillScreen(ArchColor::BG);

  // Cornice: se l'offset e' giusto, il bordo superiore e' visibile e attaccato
  // al bordo del vetro, SENZA banda bianca sopra.
  tft.drawRect(0, 0, ARCHBB_W, ARCHBB_H, ArchColor::BORDER);
  tft.drawRect(2, 2, ARCHBB_W - 4, ARCHBB_H - 4, ArchColor::ACCENT);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(ArchColor::ACCENT, ArchColor::BG);
  tft.drawString("ArchBB", ARCHBB_W / 2, 70, 4);

  tft.setTextColor(ArchColor::TEXT, ArchColor::BG);
  tft.drawString("1.83  Fase 1", ARCHBB_W / 2, 110, 2);

  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  tft.drawString("bring-up display", ARCHBB_W / 2, 140, 2);
  tft.drawString("ST7789V2  240x284", ARCHBB_W / 2, 160, 2);

  const int cx = ARCHBB_W / 2, cy = 215;
  tft.drawCircle(cx, cy, 34, ArchColor::TEXT3);
  tft.drawCircle(cx, cy, 22, ArchColor::TEXT2);
  tft.fillCircle(cx, cy, 8,  ArchColor::BAD);   // "10" centrale, ora rosso vero
  tft.drawFastHLine(cx - 44, cy, 88, ArchColor::BORDER);
  tft.drawFastVLine(cx, cy - 44, 88, ArchColor::BORDER);
}

// ----------------------------------------------------------------------------
//  displayOffsetTest — verifica CHIRURGICA dell'offset verticale.
// ----------------------------------------------------------------------------
//  Riempie di nero, poi disegna:
//   - una riga BIANCA spessa 3px ESATTAMENTE sulla prima riga (y=0..2)
//   - una riga BIANCA spessa 3px ESATTAMENTE sull'ultima riga (y=281..283)
//  Se l'offset e' perfetto: vedi entrambe le righe attaccate ai bordi vetro,
//  niente bianco extra sopra, niente taglio sotto. Se resta bianco sopra la
//  riga alta -> offset ancora sbagliato. Se la riga bassa e' tagliata ->
//  altezza < 284. E' il test che isola il SOLO problema geometrico.
// ----------------------------------------------------------------------------
void displayOffsetTest() {
  tft.fillScreen(ArchColor::BG);
  tft.fillRect(0, 0,            ARCHBB_W, 3, ArchColor::TEXT);   // top
  tft.fillRect(0, ARCHBB_H - 3, ARCHBB_W, 3, ArchColor::TEXT);   // bottom
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(ArchColor::ACCENT, ArchColor::BG);
  tft.drawString("OFFSET TEST", ARCHBB_W / 2, ARCHBB_H / 2 - 10, 4);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  tft.drawString("righe bianche = bordi", ARCHBB_W / 2, ARCHBB_H / 2 + 20, 2);
  tft.drawString("niente bianco extra?", ARCHBB_W / 2, ARCHBB_H / 2 + 38, 2);
}

// ----------------------------------------------------------------------------
//  displayDiagnosticPattern — verifica a occhio i 6 rischi §6.
// ----------------------------------------------------------------------------
void displayDiagnosticPattern() {
  tft.fillScreen(ArchColor::BG);

  const int barH = 40, barY = 30;
  tft.fillRect(10,  barY, 60, barH, TFT_RED);
  tft.fillRect(90,  barY, 60, barH, TFT_GREEN);
  tft.fillRect(170, barY, 60, barH, TFT_BLUE);

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(ArchColor::TEXT, ArchColor::BG);
  tft.drawString("R",  40,  barY + barH + 4, 2);
  tft.drawString("G", 120,  barY + barH + 4, 2);
  tft.drawString("B", 200,  barY + barH + 4, 2);

  tft.drawFastHLine(0, 0,            ARCHBB_W, TFT_YELLOW);      // top
  tft.drawFastHLine(0, ARCHBB_H - 1, ARCHBB_W, TFT_MAGENTA);    // bottom
  tft.drawFastVLine(0, 0,            ARCHBB_H, TFT_CYAN);        // left
  tft.drawFastVLine(ARCHBB_W - 1, 0, ARCHBB_H, TFT_ORANGE);     // right

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(ArchColor::TEXT, ArchColor::BG);
  tft.drawString("DIAGNOSTICA S6", ARCHBB_W / 2, 130, 2);

  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  tft.drawString("giallo=top magenta=bot", ARCHBB_W / 2, 155, 2);
  tft.drawString("ciano=sx arancio=dx",    ARCHBB_W / 2, 172, 2);
  tft.drawString("sfondo NERO, R-G-B ok?", ARCHBB_W / 2, 189, 2);

  for (int i = 0; i < 30; i++) {
    uint8_t v = map(i, 0, 29, 0, 255);
    tft.drawFastHLine(20, 210 + i, ARCHBB_W - 40, tft.color565(v, v, v));
  }
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  tft.drawString("gradiente: liscio?", ARCHBB_W / 2, 255, 2);
}

// ----------------------------------------------------------------------------
//  displayEsito — banco di prova scoring.
// ----------------------------------------------------------------------------
void displayEsito(bool good) {
  const uint16_t bg  = good ? ArchColor::GOOD : ArchColor::BAD;
  const char*    txt = good ? "CENTRO" : "FUORI";
  const char*    sub = good ? "colpo valido" : "fuori bersaglio";

  tft.fillScreen(bg);
  tft.fillRect(0, 0, ARCHBB_W, 40, ArchColor::BG);
  tft.fillRect(0, ARCHBB_H - 40, ARCHBB_W, 40, ArchColor::BG);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(ArchColor::ACCENT, ArchColor::BG);
  tft.drawString("ESITO TIRO", ARCHBB_W / 2, 20, 2);

  tft.setTextColor(ArchColor::TEXT, bg);
  tft.drawString(txt, ARCHBB_W / 2, ARCHBB_H / 2 - 10, 4);   // font 4: testo

  tft.setTextColor(ArchColor::TEXT, bg);
  tft.drawString(sub, ARCHBB_W / 2, ARCHBB_H / 2 + 35, 2);

  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  tft.drawString("ArchBB 1.83", ARCHBB_W / 2, ARCHBB_H - 20, 2);
}

// ============================================================================
//  FASE 2 — banco di prova touch (rendering)
// ============================================================================

// Zone di layout del banco di prova (coordinate fisse).
static constexpr int TT_TITLE_Y = 16;
static constexpr int TT_DATA_Y  = 55;    // inizio blocco dati testuali
static constexpr int TT_ROW_H   = 26;    // passo verticale righe dati
static constexpr int TT_AREA_Y0 = 165;   // inizio area "touchpad" visiva
static constexpr int TT_AREA_Y1 = ARCHBB_H - 8;

// Memoria dell'ultimo crocino per cancellarlo senza ridisegnare tutto.
static int  s_lastCrossX = -1, s_lastCrossY = -1;

// Disegna la cornice fissa (chiamata una volta).
void displayTouchTestFrame() {
  tft.fillScreen(ArchColor::BG);
  tft.drawRect(0, 0, ARCHBB_W, ARCHBB_H, ArchColor::BORDER);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(ArchColor::ACCENT, ArchColor::BG);
  tft.drawString("TOUCH CST816", ARCHBB_W / 2, TT_TITLE_Y, 4);

  // Etichette statiche a sinistra; i valori li aggiorna displayTouchUpdate.
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  tft.drawString("X:",   12, TT_DATA_Y + TT_ROW_H * 0, 4);
  tft.drawString("Y:",   12, TT_DATA_Y + TT_ROW_H * 1, 4);
  tft.drawString("dita:", 12, TT_DATA_Y + TT_ROW_H * 2, 2);
  tft.drawString("gest:", 12, TT_DATA_Y + TT_ROW_H * 3, 2);

  // Riquadro dell'area "touchpad" visiva dove appare il crocino.
  tft.drawRect(6, TT_AREA_Y0, ARCHBB_W - 12, TT_AREA_Y1 - TT_AREA_Y0,
               ArchColor::BORDER);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  tft.drawString("tocca qui", ARCHBB_W / 2, (TT_AREA_Y0 + TT_AREA_Y1) / 2, 2);

  s_lastCrossX = s_lastCrossY = -1;
}

// Disegna un crocino (mirino ArchBB in miniatura) alla posizione data.
static void drawCross(int x, int y, uint16_t color) {
  tft.drawFastHLine(x - 8, y, 17, color);
  tft.drawFastVLine(x, y - 8, 17, color);
  tft.drawCircle(x, y, 5, color);
}

// Aggiorna i valori numerici e il crocino.
void displayTouchUpdate(bool touched, uint16_t x, uint16_t y,
                        uint8_t fingers, const char* gestureName, uint8_t rawGid) {
  // --- Blocco valori testuali: riscrivo con sfondo per cancellare i vecchi ---
  char line[24];
  tft.setTextDatum(ML_DATUM);

  // X
  tft.setTextColor(ArchColor::TEXT, ArchColor::BG);
  snprintf(line, sizeof(line), "%-4u", touched ? x : 0);
  tft.fillRect(70, TT_DATA_Y - 14 + TT_ROW_H * 0, 120, 24, ArchColor::BG);
  tft.drawString(line, 70, TT_DATA_Y + TT_ROW_H * 0, 4);

  // Y
  snprintf(line, sizeof(line), "%-4u", touched ? y : 0);
  tft.fillRect(70, TT_DATA_Y - 14 + TT_ROW_H * 1, 120, 24, ArchColor::BG);
  tft.drawString(line, 70, TT_DATA_Y + TT_ROW_H * 1, 4);

  // dita
  tft.setTextColor(ArchColor::TEXT2, ArchColor::BG);
  snprintf(line, sizeof(line), "%u  ", fingers);
  tft.fillRect(80, TT_DATA_Y - 8 + TT_ROW_H * 2, 100, 18, ArchColor::BG);
  tft.drawString(line, 80, TT_DATA_Y + TT_ROW_H * 2, 2);

  // gesture (nome + id grezzo per diagnostica)
  snprintf(line, sizeof(line), "%s (0x%02X)", gestureName, rawGid);
  tft.fillRect(80, TT_DATA_Y - 8 + TT_ROW_H * 3, 150, 18, ArchColor::BG);
  tft.setTextColor(ArchColor::AMBER, ArchColor::BG);
  tft.drawString(line, 80, TT_DATA_Y + TT_ROW_H * 3, 2);

  // --- Crocino nell'area touchpad ------------------------------------------
  // Cancella il vecchio crocino (ridisegnandolo col colore di sfondo).
  if (s_lastCrossX >= 0) {
    drawCross(s_lastCrossX, s_lastCrossY, ArchColor::BG);
    // Ripristina il bordo dell'area se il crocino lo aveva sfiorato: semplice
    // ridisegno del rettangolo (poco costoso a questa cadenza).
    tft.drawRect(6, TT_AREA_Y0, ARCHBB_W - 12, TT_AREA_Y1 - TT_AREA_Y0,
                 ArchColor::BORDER);
  }

  if (touched) {
    // Clampo dentro l'area visiva del touchpad per il disegno del crocino.
    int cx = constrain((int)x, 12, ARCHBB_W - 12);
    int cy = constrain((int)y, TT_AREA_Y0 + 8, TT_AREA_Y1 - 8);
    drawCross(cx, cy, ArchColor::GOOD);
    s_lastCrossX = cx;
    s_lastCrossY = cy;
  } else {
    s_lastCrossX = s_lastCrossY = -1;
  }
}

// ============================================================================
//  FASE 3 — schermate di supporto allo scoring
// ============================================================================

void displayAttesaTiro(uint16_t nextShotId) {
  tft.fillScreen(ArchColor::BG);
  tft.drawRect(0, 0, ARCHBB_W, ARCHBB_H, ArchColor::BORDER);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(ArchColor::ACCENT, ArchColor::BG);
  tft.drawString("ArchBB", ARCHBB_W/2, 40, 4);

  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  tft.drawString("scoring on-device", ARCHBB_W/2, 68, 2);

  // Mirino centrale.
  const int cx = ARCHBB_W/2, cy = 150;
  tft.drawCircle(cx, cy, 40, ArchColor::TEXT3);
  tft.drawCircle(cx, cy, 26, ArchColor::TEXT2);
  tft.fillCircle(cx, cy, 10, ArchColor::BAD);
  tft.drawFastHLine(cx - 52, cy, 104, ArchColor::BORDER);
  tft.drawFastVLine(cx, cy - 52, 104, ArchColor::BORDER);

  tft.setTextColor(ArchColor::TEXT, ArchColor::BG);
  char t[24]; snprintf(t, sizeof(t), "prossimo: tiro #%u", nextShotId);
  tft.drawString(t, ARCHBB_W/2, 220, 2);

  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(ArchColor::AMBER, ArchColor::BG);
  tft.drawString("TOCCA per registrare un tiro", ARCHBB_W/2, ARCHBB_H - 10, 2);
}

// ----------------------------------------------------------------------------
//  Helper interni al riepilogo 4c
// ----------------------------------------------------------------------------
//  Colore di una classe hold: verde=tenuto, ambra=lieve, rosso=abbassato/alzato.
static uint16_t holdColor(uint8_t c) {
  switch (c) {
    case HOLD_TENUTO:                   return ArchColor::GOOD;
    case HOLD_LIEVE:                    return ArchColor::AMBER;
    case HOLD_ABBASSATO: case HOLD_ALZATO: return ArchColor::BAD;
    default:                            return ArchColor::TEXT3;
  }
}
//  Colore di una classe release: verde=pulito, ambra=medio, rosso=strappo.
static uint16_t releaseColor(uint8_t c) {
  switch (c) {
    case RELEASE_PULITO:  return ArchColor::GOOD;
    case RELEASE_MEDIO:   return ArchColor::AMBER;
    case RELEASE_STRAPPO: return ArchColor::BAD;
    default:              return ArchColor::TEXT3;
  }
}

// ----------------------------------------------------------------------------
//  Layout del riepilogo 4c — costanti condivise da display E hit-test.
// ----------------------------------------------------------------------------
//  Stessa lezione delle costanti duplicate (§4.1 compendio): disegno e hit-test
//  DEVONO leggere le stesse Y, o prima o poi un tocco cade fuori dal pulsante
//  che vede a schermo. Le metto qui, un solo posto.
// Geometria del riepilogo (F13). Un solo posto: disegno e hit-test leggono
// queste, mai numeri scritti a mano (§4.1).
static constexpr int RIEP_ESITO_H  = 56;    // fascia COLPITO/MANCATO, font 6
static constexpr int RIEP_BTN_MARG = 8;     // distanza dai bordi laterali
static constexpr int RIEP_BTN_W    = 92;    // larghezza dei due pulsanti
static constexpr int RIEP_BTN_H    = 58;    // altezza
static constexpr int RIEP_BTN_Y    = ARCHBB_H - RIEP_BTN_H - 10;

void displayScoreRiepilogo(const ScoreResult& r, const char* zonaTxt) {
  const bool valutato = r.valutato;
  const bool colpito  = r.colpito;

  tft.fillScreen(ArchColor::BG);

  // --- FASCIA ESITO: grande, si legge al volo -------------------------------
  //  Prima era 40 px con font 4; ora 56 px con font 6. Il riepilogo si guarda
  //  con l'arco in mano e il sole in faccia: qui la leggibilita' conta piu'
  //  della densita' di informazione.
  uint16_t topColor = !valutato ? ArchColor::BORDER
                     : (colpito ? ArchColor::GOOD : ArchColor::BAD);
  tft.fillRect(0, 0, ARCHBB_W, RIEP_ESITO_H, topColor);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(ArchColor::TEXT, topColor);
  const char* esito = !valutato ? "NON VALUT." : (colpito ? "COLPITO" : "MANCATO");
  // FONT 4, non 6. Il 6 di TFT_eSPI e' SOLO NUMERICO (cifre, punto, due punti,
  // meno e poche lettere per l'AM/PM): scriverci "COLPITO" non da' errore, fa
  // sparire i caratteri. E' lo stesso inciampo del "+" sugli slider.
  //  Il font 4 e' alfanumerico completo. Per compensare l'altezza minore, la
  //  fascia resta da 56 px e il testo e' centrato: si legge lo stesso, perche'
  //  a distanza di braccio conta il contrasto della fascia colorata piu' della
  //  dimensione del carattere.
  tft.drawString(esito, ARCHBB_W/2, RIEP_ESITO_H/2, 4);

  // --- DUE SEMAFORI: tenuta e rilascio --------------------------------------
  //  Il colore E' l'informazione; il numero e' il dettaglio per chi lo vuole.
  //  Un quadrato verde si legge in un decimo di secondo, "-2.81 deg" no.
  //  Le soglie sono quelle del firmware (HoldClass/ReleaseClass): il display
  //  non ne inventa di proprie, altrimenti il colore e la classe salvata nel
  //  CSV potrebbero dire cose diverse (§4.1).
  const int qy = RIEP_ESITO_H + 10;
  const int qs = 46;                        // lato del quadrato
  const int col1 = 18, col2 = ARCHBB_W/2 + 6;

  auto semaforo = [&](int x, const char* etichetta, bool valido, uint8_t classe,
                      const char* valore) {
    uint16_t c = ArchColor::BORDER;
    if (valido) {
      // verde = buono, giallo = intermedio, rosso = da correggere.
      c = (classe == 0) ? ArchColor::GOOD
        : (classe == 1) ? ArchColor::AMBER
                        : ArchColor::BAD;
    }
    tft.fillRoundRect(x, qy, qs, qs, 8, c);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
    tft.drawString(etichetta, x + qs + 8, qy + 4, 2);
    tft.setTextColor(valido ? ArchColor::TEXT : ArchColor::TEXT3, ArchColor::BG);
    tft.drawString(valore, x + qs + 8, qy + 26, 2);
  };

  char hv[12], rv[12];
  if (r.hold_valid) snprintf(hv, sizeof(hv), "%+.1f", r.hold_cdeg / 100.0f);
  else              snprintf(hv, sizeof(hv), "n/d");
  if (r.release_valid) snprintf(rv, sizeof(rv), "%u", r.release_jerk);
  else                 snprintf(rv, sizeof(rv), "n/d");

  semaforo(col1, "TENUTA",   r.hold_valid,    r.hold_class,    hv);
  semaforo(col2, "RILASCIO", r.release_valid, r.release_class, rv);

  // --- DISTANZA ed ELEVAZIONE, grandi -------------------------------------
  //  Sono i due numeri che l'arciere ha appena inserito e che vuole
  //  ricontrollare prima di confermare: font 4, non 2.
  const int dy = qy + qs + 14;
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  tft.drawString("DIST", col1, dy, 1);
  tft.drawString("ELEV", col2, dy, 1);

  char dd[10], ee[10];
  if (r.distanza_m == 0) snprintf(dd, sizeof(dd), "--");
  else                   snprintf(dd, sizeof(dd), "%um", r.distanza_m);
  if (!elevIsSet(r.elev_deg)) snprintf(ee, sizeof(ee), "n/i");
  else                        snprintf(ee, sizeof(ee), "%+d", (int)r.elev_deg);

  tft.setTextColor(ArchColor::TEXT, ArchColor::BG);
  tft.drawString(dd, col1, dy + 12, 4);
  tft.setTextColor(elevIsSet(r.elev_deg) && r.elev_deg != 0
                   ? ArchColor::AMBER : ArchColor::TEXT, ArchColor::BG);
  tft.drawString(ee, col2, dy + 12, 4);

  // --- F21: PICCO D'URTO ----------------------------------------------------
  //  Sta qui, sotto distanza ed elevazione e sopra i pulsanti, nella fascia che
  //  era vuota. NIENTE COLORE, a differenza dei due semafori sopra: un verde o
  //  un rosso direbbero che si sa quale picco e' buono, e non si sa. E' un
  //  numero da guardare crescere nel corso della seduta, non un giudizio.
  //
  //  L'unita' e' scritta per esteso proprio perche' il numero NON e' un
  //  punteggio: 31,5 senza unita' sembrerebbe un voto su 100.
  {
    const int py = RIEP_BTN_Y - 48;
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
    tft.drawString("URTO", col1, py, 1);
    if (r.picco_valid) {
      char pv[12];
      snprintf(pv, sizeof(pv), "%.1f", r.picco_cms2 / 100.0f);
      tft.setTextColor(ArchColor::TEXT, ArchColor::BG);
      // drawString ritorna la larghezza disegnata: l'unita' si aggancia al
      // numero invece che a una x scritta a mano, cosi' resta attaccata anche
      // quando il valore passa da una a tre cifre (§4.1 applicata al testo).
      int w = tft.drawString(pv, col1, py + 10, 4);
      tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
      tft.drawString("m/s2", col1 + w + 6, py + 20, 2);
    } else {
      tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
      tft.drawString("n/d", col1, py + 10, 4);
    }
  }

  // F21b — ARCS TOLTO DAL RIEPILOGO.
  //  Era scritto in font 1 in un angolo, e non serviva a nessuno: e' una
  //  CODIFICA, non una misura. Distanza e zona sono gia' a schermo, grandi, e
  //  l'ARCS non e' altro che le due rimesse in un numero solo. Il valore resta
  //  in ScoreResult e nel CSV, dove serve alla compatibilita' coi dati 1.69:
  //  toglierlo dallo schermo non toglie niente al dato.
  //  Resta la ZONA, che invece e' leggibile ("SPOT", "alto-dx") ed e' la
  //  conferma di quello che si e' appena inserito.
  if (valutato) {
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
    tft.drawString(zonaTxt, ARCHBB_W - 10, dy + 2, 1);
  }

  // --- PULSANTI VERTICALI, contro i bordi ----------------------------------
  //  Prima erano due barre orizzontali sovrapposte al centro. Verticali e ai
  //  lati si premono col pollice senza spostare la presa e senza coprire il
  //  contenuto: la mano entra dal bordo, non dal mezzo dello schermo.
  tft.fillRoundRect(RIEP_BTN_MARG, RIEP_BTN_Y, RIEP_BTN_W, RIEP_BTN_H, 8,
                    ArchColor::BG_CARD);
  tft.drawRoundRect(RIEP_BTN_MARG, RIEP_BTN_Y, RIEP_BTN_W, RIEP_BTN_H, 8,
                    ArchColor::BORDER);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(ArchColor::TEXT2, ArchColor::BG_CARD);
  tft.drawString("CORREGGI", RIEP_BTN_MARG + RIEP_BTN_W/2,
                 RIEP_BTN_Y + RIEP_BTN_H/2, 2);

  const int okx = ARCHBB_W - RIEP_BTN_MARG - RIEP_BTN_W;
  tft.fillRoundRect(okx, RIEP_BTN_Y, RIEP_BTN_W, RIEP_BTN_H, 8, ArchColor::ACCENT);
  tft.setTextColor(ArchColor::BG, ArchColor::ACCENT);
  tft.drawString("OK", okx + RIEP_BTN_W/2, RIEP_BTN_Y + RIEP_BTN_H/2, 4);
}

// Zone toccabili del riepilogo: 1=CORREGGI (sinistra), 2=OK (destra), 0=altro.
//  Usa LE STESSE costanti del disegno. La regola §4.1 nasce proprio qui: nella
//  1.69 i numeri di posizione duplicati fra disegno e hit-test hanno prodotto
//  pulsanti che si vedevano in un posto e rispondevano in un altro.
uint8_t hitRiepilogo(uint16_t x, uint16_t y) {
  if (y < RIEP_BTN_Y || y >= RIEP_BTN_Y + RIEP_BTN_H) return 0;
  if (x >= RIEP_BTN_MARG && x < RIEP_BTN_MARG + RIEP_BTN_W) return 1;
  const int okx = ARCHBB_W - RIEP_BTN_MARG - RIEP_BTN_W;
  if (x >= okx && x < okx + RIEP_BTN_W) return 2;
  return 0;
}

// ============================================================================
//  FASE 4a — attesa del TIRO REALE (IMU armata)
// ============================================================================
void displayAttesaTiroIMU(uint16_t nextShotId, float tempC) {
  tft.fillScreen(ArchColor::BG);
  tft.drawRect(0, 0, ARCHBB_W, ARCHBB_H, ArchColor::BORDER);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(ArchColor::ACCENT, ArchColor::BG);
  tft.drawString("ArchBB", ARCHBB_W/2, 38, 4);

  // Riga di stato IMU: pallino verde pulsante "ARMATO" + temperatura on-chip.
  // La temperatura e' un check vitale a colpo d'occhio: se e' un valore
  // plausibile (~20-40 C) l'IMU sta rispondendo davvero, non e' un placeholder.
  const int badgeY = 66;
  tft.fillCircle(ARCHBB_W/2 - 58, badgeY, 6, ArchColor::GOOD);
  tft.setTextColor(ArchColor::GOOD, ArchColor::BG);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("IMU ARMATA", ARCHBB_W/2 - 46, badgeY, 2);
  if (!isnan(tempC)) {
    char tt[16]; snprintf(tt, sizeof(tt), "%.0f\xB0""C", tempC);  // \xB0 = grado
    tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
    tft.setTextDatum(MR_DATUM);
    tft.drawString(tt, ARCHBB_W - 16, badgeY, 2);
  }

  // Mirino centrale (identico all'attesa Fase 3, per continuita' visiva).
  const int cx = ARCHBB_W/2, cy = 150;
  tft.drawCircle(cx, cy, 40, ArchColor::TEXT3);
  tft.drawCircle(cx, cy, 26, ArchColor::TEXT2);
  tft.fillCircle(cx, cy, 10, ArchColor::BAD);
  tft.drawFastHLine(cx - 52, cy, 104, ArchColor::BORDER);
  tft.drawFastVLine(cx, cy - 52, 104, ArchColor::BORDER);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(ArchColor::TEXT, ArchColor::BG);
  char t[24]; snprintf(t, sizeof(t), "prossimo: tiro #%u", nextShotId);
  tft.drawString(t, ARCHBB_W/2, 218, 2);

  // F20: la riga viene disegnata dalla macchina dei tempi. Il forza=true la
  // obbliga a ridisegnare anche se il testo non e' cambiato: lo schermo e'
  // appena stato ripulito e la sua cache non lo sa.
  displayTempoRiga(0, 0, true);

  // Il tocco resta come trigger manuale a banco: lo si dice in piccolo, non e'
  // piu' l'azione primaria (ora e' la freccia a comandare).
  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  tft.drawString("(tocca = tiro manuale)", ARCHBB_W/2, ARCHBB_H - 10, 2);
}

// ----------------------------------------------------------------------------
//  FASE 20 — riga di stato dal vivo (vedi display.h)
// ----------------------------------------------------------------------------
//  La quota e l'altezza della fascia sono costanti condivise fra il disegno
//  completo della schermata e il ridisegno parziale. Se fossero due numeri
//  distinti, il ridisegno parziale cancellerebbe una fascia diversa da quella
//  che scrive — ed e' il tipo di errore che si vede solo come una riga di
//  pixel sporchi che nessuno riesce a spiegare (§4.1).
static constexpr int16_t TEMPO_RIGA_Y = 244;
static constexpr int16_t TEMPO_RIGA_H = 17;

void displayTempoRiga(uint8_t stato, uint32_t msNelloStato, bool forza) {
  char testo[28];
  uint16_t colore;
  switch (stato) {
    case 1:   // TEMPO_MOTO
      snprintf(testo, sizeof(testo), "arco in movimento");
      colore = ArchColor::ACCENT; break;
    case 2:   // TEMPO_ANCORA
      snprintf(testo, sizeof(testo), "in mira  %lu.%lu s",
               (unsigned long)(msNelloStato / 1000),
               (unsigned long)((msNelloStato % 1000) / 100));
      colore = ArchColor::GOOD; break;
    default:  // TEMPO_RIPOSO
      // FASE 22b — A RIPOSO LA RIGA E' VUOTA.
      //  Diceva "In attesa dello SCOCCO", che e' pleonastico: la schermata di
      //  attesa e' gia' la schermata di attesa, e il numero del prossimo tiro
      //  campeggia sopra. In piu' costava: la fascia e' centrata su y=244 e
      //  alta 17, quindi arriva a 235 e mordeva la base delle cifre del numero
      //  del tiro, che il font 7 porta fino a 242. Ogni ridisegno della riga
      //  cancellava il fondo dei sette segmenti.
      //  La riga NON sparisce come meccanismo: resta viva per gli stati che
      //  portano informazione davvero (arco in movimento, cronometro di mira),
      //  e in quei momenti lo spazio se lo prende perche' serve. A riposo si
      //  limita a ripulire la fascia.
      testo[0] = '\0';
      colore = ArchColor::TEXT3; break;
  }

  // Cache: senza, questa funzione ridisegnerebbe cinquanta volte al secondo e
  // la riga sfarfallerebbe. Con la cache ridisegna solo ai cambi di stato e a
  // ogni decimo di secondo mentre il cronometro corre.
  static char ultimo[28] = "";
  if (!forza && strncmp(ultimo, testo, sizeof(ultimo)) == 0) return;
  strncpy(ultimo, testo, sizeof(ultimo) - 1); ultimo[sizeof(ultimo)-1] = '\0';

  tft.fillRect(0, TEMPO_RIGA_Y - TEMPO_RIGA_H/2, ARCHBB_W, TEMPO_RIGA_H,
               ArchColor::BG);
  if (testo[0] == '\0') return;    // fascia gia' ripulita: niente da scrivere
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(colore, ArchColor::BG);
  tft.drawString(testo, ARCHBB_W/2, TEMPO_RIGA_Y, 2);
}

// ----------------------------------------------------------------------------
//  FASE 5 — displayStatusBar: batteria (sx) + sessione (centro) + SD (dx)
// ----------------------------------------------------------------------------
//  Estetica: una fascia scura in cima, tre gruppi ben distanziati, icona
//  batteria DISEGNATA (non testuale) col livello colorato per soglia — verde
//  sopra 40%, ambra 15..40%, rosso sotto. Il badge sessione e' un pallino REC
//  rosso pulsante quando aperta, un cerchietto vuoto quando chiusa. Lo spazio SD
//  a destra. Tutto in font 1/2 per non rubare spazio al mirino sottostante.
void displayStatusBar(const StatusInfo& st) {
  const int barH = 20;
  // Sfondo fascia + linea di separazione sotto.
  tft.fillRect(0, 0, ARCHBB_W, barH, ArchColor::BG_CARD);
  tft.drawFastHLine(0, barH, ARCHBB_W, ArchColor::BORDER);

  // --- BATTERIA (sinistra) --------------------------------------------------
  //  Icona: corpo 22x11 + polo +. Riempimento proporzionale, colore per soglia.
  const int bx = 6, by = 5, bw = 22, bh = 11;
  tft.drawRect(bx, by, bw, bh, ArchColor::TEXT2);               // corpo
  tft.fillRect(bx + bw, by + 3, 2, bh - 6, ArchColor::TEXT2);   // polo +
  if (st.battPct >= 0) {
    int lvl = (st.battPct * (bw - 2)) / 100;                    // px di riempimento
    uint16_t col = (st.battPct > 40) ? ArchColor::GOOD
                 : (st.battPct > 15) ? ArchColor::AMBER
                                     : ArchColor::BAD;
    if (lvl > 0) tft.fillRect(bx + 1, by + 1, lvl, bh - 2, col);
    // Percentuale accanto all'icona.
    char pb[10]; snprintf(pb, sizeof(pb), "%d%%", st.battPct);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(ArchColor::TEXT, ArchColor::BG_CARD);
    tft.drawString(pb, bx + bw + 6, by + bh/2, 2);
    // Spina "in carica" (piccola freccia ambra) se sotto carica.
    if (st.battCharging) {
      tft.setTextColor(ArchColor::AMBER, ArchColor::BG_CARD);
      tft.drawString("\x18", bx + bw + 34, by + bh/2, 1);       // 0x18 = up-arrow
    }
  } else {
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(ArchColor::TEXT3, ArchColor::BG_CARD);
    tft.drawString("n/d", bx + bw + 6, by + bh/2, 2);
  }

  // --- SESSIONE (centro) ----------------------------------------------------
  //  REC ● rosso + conteggio tiri se aperta; ○ "no sess" grigio se chiusa.
  //  FASE 6b: non mostriamo piu' "S%03u" (il numero interno e' slegato dal nome
  //  cartella datato: sarebbe ingannevole). Conta il numero di TIRI, che e' il
  //  dato utile all'arciere.
  tft.setTextDatum(MC_DATUM);
  if (st.sessionOpen) {
    char sb[18]; snprintf(sb, sizeof(sb), "REC  %u tiri", st.sessionShots);
    tft.fillCircle(ARCHBB_W/2 - 42, barH/2, 4, ArchColor::BAD);   // pallino REC
    tft.setTextColor(ArchColor::TEXT, ArchColor::BG_CARD);
    tft.drawString(sb, ARCHBB_W/2 + 6, barH/2, 2);
  } else {
    tft.drawCircle(ARCHBB_W/2 - 34, barH/2, 4, ArchColor::TEXT3); // cerchietto vuoto
    tft.setTextColor(ArchColor::TEXT3, ArchColor::BG_CARD);
    tft.drawString("no sess", ARCHBB_W/2 + 4, barH/2, 2);
  }

  // --- SD (destra) ----------------------------------------------------------
  tft.setTextDatum(MR_DATUM);
  if (st.sdOk) {
    char db[14];
    if (st.sdFreeMb >= 1024) snprintf(db, sizeof(db), "SD %.1fG", st.sdFreeMb / 1024.0f);
    else                     snprintf(db, sizeof(db), "SD %luM", (unsigned long)st.sdFreeMb);
    tft.setTextColor(ArchColor::ACCENT, ArchColor::BG_CARD);
    tft.drawString(db, ARCHBB_W - 6, barH/2, 2);
  } else {
    tft.setTextColor(ArchColor::BAD, ArchColor::BG_CARD);
    tft.drawString("no SD", ARCHBB_W - 6, barH/2, 2);
  }
}

// ----------------------------------------------------------------------------
//  FASE 5 — schermata di attesa CON barra di stato in cima.
// ----------------------------------------------------------------------------
//  Riusa il layout di displayAttesaTiroIMU ma lascia i primi ~22px alla barra.
//  Per non duplicare il corpo, ridisegniamo qui la scena spostata di poco sotto
//  la barra. (Il mirino centrale resta al suo posto: 284px di altezza danno
//  spazio a sufficienza.)
// ============================================================================
//  FASE 22 — ATTESA con la CARD PRE-TIRO
// ============================================================================
//  Cosa e' cambiato e perche'. Dalla F22 distanza ed elevazione si armano PRIMA
//  di scoccare, non si dichiarano dopo. Questo impone due cose alla schermata:
//
//   1) i valori armati devono STARE A SCHERMO mentre si incocca. Se l'arciere
//      non li vede, non ha modo di accorgersi che sta tirando col bersaglio
//      della piazzola precedente. E' il rischio che l'inversione introduce, e
//      l'unica difesa e' che il dato sia in vista.
//   2) devono essere TOCCABILI. La card e' il pulsante: toccarla apre il
//      pre-tiro, toccare altrove gestisce la sessione. Distinzione SPAZIALE e
//      non temporale — niente doppio tap, che significherebbe attendere ~300 ms
//      per sapere se un tocco e' singolo e riaprire il bug "parte sempre come
//      breve" che il tap-lungo era costato.
//
//  COSA SPARISCE: il mirino decorativo che occupava 104 px al centro. Non
//  portava informazione, e lo spazio serviva. Il numero del tiro invece RESTA,
//  e anzi cresce: era in font 2 in una riga di testo, ora e' in font 7 sotto la
//  card. E' l'altro dato che si guarda fra un tiro e l'altro.
//
//  ATTENZIONE AI FONT. I numeri grandi sono in font 6 (card) e font 7 (tiro),
//  che sono SOLI NUMERICI: cifre, punto, due punti, meno. Niente '+', niente
//  lettere. Per questo l'elevazione mostra il VALORE ASSOLUTO e la direzione
//  sta nell'etichetta sotto ("gradi SU" / "gradi GIU" / "in piano"): non serve
//  disegnare un '+' geometrico, e si legge anche meglio di un segno.
// ----------------------------------------------------------------------------
//  Geometria della card: UNA sola serie di costanti, letta dal disegno e
//  dall'hit-test (§4.1). La zona sensibile e' 4 px piu' grande del disegno su
//  ogni lato — verso deliberato: risponde appena fuori dal bordo, mai dentro.
// ============================================================================
//  FASE 24a — PRIMITIVE DEL SEGNO e DISEGNATORE UNICO DELL'ELEVAZIONE
// ============================================================================
void displaySegnoPiu(int16_t cx, int16_t cy, int16_t lung, int16_t spess, uint16_t col) {
  tft.fillRect(cx - lung/2, cy - spess/2, lung, spess, col);   // barra orizzontale
  tft.fillRect(cx - spess/2, cy - lung/2, spess, lung, col);   // barra verticale
}

void displaySegnoMeno(int16_t cx, int16_t cy, int16_t lung, int16_t spess, uint16_t col) {
  tft.fillRect(cx - lung/2, cy - spess/2, lung, spess, col);
}

// Frecce: triangolo pieno. fillTriangle vuole i tre vertici; li calcolo dal
// rettangolo contenitore cosi' chi chiama ragiona in ingombri, non in punti.
void displayFrecciaSu(int16_t cx, int16_t cy, int16_t w, int16_t h, uint16_t col) {
  tft.fillTriangle(cx, cy - h/2,  cx - w/2, cy + h/2,  cx + w/2, cy + h/2, col);
}
void displayFrecciaGiu(int16_t cx, int16_t cy, int16_t w, int16_t h, uint16_t col) {
  tft.fillTriangle(cx, cy + h/2,  cx - w/2, cy - h/2,  cx + w/2, cy - h/2, col);
}

uint16_t displayColoreElev(int8_t deg) {
  if (deg == 0) return ArchColor::TEXT;          // in piano: bianco, neutro
#if ELEV_PALETTE == 1
  return (deg > 0) ? ArchColor::GOOD : ArchColor::BAD;     // verde / rosso
#else
  return (deg > 0) ? ArchColor::AMBER : ArchColor::ACCENT; // ambra / ciano
#endif
}

// ----------------------------------------------------------------------------
//  displayElevazione — tre canali per un'informazione sola
// ----------------------------------------------------------------------------
//  Composizione, da sinistra a destra:   [freccia] [segno]  [cifre]
//
//  Perche' TRE canali (forma, segno, colore) per un numero di due cifre:
//    - il COLORE da solo esclude circa un maschio su dodici (deficit
//      rosso-verde) e sparisce con gli occhiali da sole polarizzati gialli,
//      che mezzo campo di tiro indossa;
//    - il SEGNO da solo e' un rettangolo di pochi pixel: il canale piu' debole
//      che esista, ed e' esattamente quello che il 12/09 non si e' visto;
//    - la FRECCIA da sola non dice quanto.
//  Insieme, ognuno copre il buco dell'altro. E' codifica ridondante, ed e' la
//  raccomandazione standard di ogni linea guida di leggibilita': mai affidare
//  un'informazione a un canale solo.
//
//  L'insieme viene centrato su cx: si misura prima la larghezza totale, poi si
//  disegna. Centrare il solo numero farebbe ballare la composizione fra -5 e 5.
void displayElevazione(int16_t cx, int16_t cy, int8_t deg, uint8_t font, uint16_t bg) {
  const uint16_t col = displayColoreElev(deg);
  const int8_t   val = (deg < 0) ? (int8_t)(-deg) : deg;

  char cifre[8];
  snprintf(cifre, sizeof(cifre), "%d", (int)val);

  // Proporzioni derivate dall'altezza del font: cambiando font 6 -> 7 l'insieme
  // scala da solo e nessuno deve ritoccare numeri a mano.
  const int16_t hF     = (int16_t)tft.fontHeight(font);
  const int16_t wNum   = (int16_t)tft.textWidth(cifre, font);
  const int16_t wFre   = (deg == 0) ? 0 : (int16_t)(hF * 0.42f);
  const int16_t wSeg   = (deg == 0) ? 0 : (int16_t)(hF * 0.34f);
  const int16_t gap    = 4;
  const int16_t wTot   = wFre + (wFre ? gap : 0) + wSeg + (wSeg ? gap : 0) + wNum;

  int16_t x = cx - wTot/2;

  if (deg != 0) {
    const int16_t fcx = x + wFre/2;
    if (deg > 0) displayFrecciaSu (fcx, cy, wFre, (int16_t)(hF * 0.52f), col);
    else         displayFrecciaGiu(fcx, cy, wFre, (int16_t)(hF * 0.52f), col);
    x += wFre + gap;

    const int16_t scx   = x + wSeg/2;
    const int16_t spess = (int16_t)(hF * 0.11f) | 1;   // dispari: resta simmetrico
    if (deg > 0) displaySegnoPiu (scx, cy, wSeg, spess, col);
    else         displaySegnoMeno(scx, cy, wSeg, spess, col);
    x += wSeg + gap;
  }

  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(col, bg);
  tft.drawString(cifre, x, cy, font);
  tft.setTextDatum(MC_DATUM);
}

namespace AttesaCard {
  static constexpr int16_t X = 6, Y = 40, W = ARCHBB_W - 12, H = 98;
  static constexpr int16_t MARG_TOCCO = 4;
  static constexpr int16_t COL1 = X + W/4;          //  63  centro colonna sinistra
  static constexpr int16_t COL2 = X + (3*W)/4;      // 177  centro colonna destra
  static constexpr int16_t DIV  = X + W/2;          // 120  divisorio verticale
}

bool displayAttesaHitCard(uint16_t x, uint16_t y) {
  using namespace AttesaCard;
  return (int16_t)x >= X - MARG_TOCCO && (int16_t)x < X + W + MARG_TOCCO &&
         (int16_t)y >= Y - MARG_TOCCO && (int16_t)y < Y + H + MARG_TOCCO;
}

void displayAttesaTiroIMU_v5(uint16_t nextShotId, float tempC, const StatusInfo& st,
                             uint8_t distM, int8_t elevDeg, bool confermato) {
  using namespace AttesaCard;
  tft.fillScreen(ArchColor::BG);
  tft.drawRect(0, 0, ARCHBB_W, ARCHBB_H, ArchColor::BORDER);

  // Barra di stato in cima (Fase 5): batteria, sessione, SD.
  displayStatusBar(st);

  // Se manca la SD, la CAUSA (pin/formato) si legge sul dispositivo senza
  // collegare la seriale.
  if (!st.sdOk && st.sdDiag) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(ArchColor::BAD, ArchColor::BG);
    tft.drawString(st.sdDiag, ARCHBB_W/2, 30, 1);
  } else {
    // Riga di servizio: identita' a sinistra, orologio a destra. Font 1: sono
    // informazioni che si cercano, non che si guardano.
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
    tft.drawString("ArchBB", 8, 30, 1);
    if (st.hasTime) {
      tft.setTextDatum(MR_DATUM);
      tft.drawString(st.clock, ARCHBB_W - 8, 30, 1);
    }
  }

  // --- LA CARD ------------------------------------------------------------
  //  Il COLORE DEL BORDO porta un'informazione che non costa niente:
  //    ACCENT  = confermata nel pre-tiro dopo l'ultimo scocco
  //    AMBER   = ereditata dal tiro precedente, nessuno l'ha piu' toccata
  //  Un cambio di colore nel campo visivo mentre si incocca e' il promemoria
  //  piu' economico che esista contro il tiro con la distanza della piazzola
  //  prima. Non impedisce l'errore — niente lo impedisce — ma lo rende visibile.
  const uint16_t bordo = confermato ? ArchColor::ACCENT : ArchColor::AMBER;
  tft.fillRoundRect(X, Y, W, H, 10, ArchColor::BG_CARD);
  tft.drawRoundRect(X, Y, W, H, 10, bordo);
  tft.drawRoundRect(X+1, Y+1, W-2, H-2, 9, bordo);
  tft.drawFastVLine(DIV, Y + 8, H - 16, ArchColor::BORDER);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG_CARD);
  tft.drawString("DISTANZA",   COL1, Y + 12, 1);
  tft.drawString("ELEVAZIONE", COL2, Y + 12, 1);

  char b[12];
  tft.setTextColor(ArchColor::TEXT, ArchColor::BG_CARD);
  snprintf(b, sizeof(b), "%u", (unsigned)distM);
  tft.drawString(b, COL1, Y + 50, 6);

  // --- ELEVAZIONE: IL PUNTO CORRETTO IN FASE 24a --------------------------
  //  Prima qui c'era il VALORE ASSOLUTO, con la direzione delegata a "gradi SU"
  //  / "gradi GIU" in font 1 sotto il numero. In campo il 12/09 quella
  //  didascalia non e' stata letta: l'occhio prende il numero grande e se ne va.
  //  Il commento originale attribuiva la scelta al font ("il font 6 non ha il
  //  '+'"): vero per il piu', FALSO per il meno, che nel font 6 c'e' eccome
  //  (larghezza 17 nella tabella dei caratteri). Il segno mancante era una
  //  nostra decisione, non un limite della libreria — ed e' il tipo di errore
  //  che sopravvive mesi proprio perche' e' documentato in modo convincente.
  //
  //  Ora il disegnatore unico mette freccia + segno + cifre, con il colore di
  //  ELEV_PALETTE. La didascalia sotto resta (quarto canale, e a costo zero).
  displayElevazione(COL2, Y + 50, elevDeg, 6, ArchColor::BG_CARD);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG_CARD);
  tft.drawString("metri", COL1, Y + 84, 1);
  const char* dir = (elevDeg > 0) ? "gradi SU"
                  : (elevDeg < 0) ? "gradi GIU" : "in piano";
  tft.drawString(dir, COL2, Y + 84, 1);

  // Istruzione, una riga sola sotto la card.
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  tft.drawString("tocca la scheda per armare il bersaglio", ARCHBB_W/2, Y + H + 8, 1);

  // --- IMU armata + temperatura -------------------------------------------
  const int badgeY = 162;
  tft.fillCircle(ARCHBB_W/2 - 58, badgeY, 6, ArchColor::GOOD);
  tft.setTextColor(ArchColor::GOOD, ArchColor::BG);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("IMU ARMATA", ARCHBB_W/2 - 46, badgeY, 2);
  if (!isnan(tempC)) {
    char tt[16]; snprintf(tt, sizeof(tt), "%.0f\xB0""C", tempC);
    tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
    tft.setTextDatum(MR_DATUM);
    tft.drawString(tt, ARCHBB_W - 16, badgeY, 2);
  }

  // --- NUMERO DEL TIRO ----------------------------------------------------
  //  Font 7 (sette segmenti, 48 px): e' un numero, e i sette segmenti si
  //  leggono a colpo d'occhio meglio di qualunque font proporzionale.
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  tft.drawString("PROSSIMO TIRO", ARCHBB_W/2, 180, 1);
  snprintf(b, sizeof(b), "%u", (unsigned)nextShotId);
  tft.setTextColor(ArchColor::ACCENT, ArchColor::BG);
  // Il font 7 e' alto 48 px e il datum e' centrale: le cifre occupano
  // 210 +/- 24, cioe' 186..234. La fascia del gesto comincia a 235
  // (TEMPO_RIGA_Y - TEMPO_RIGA_H/2). Due pixel di aria, per costruzione: se
  // qualcuno cambiera' uno dei due numeri, questo commento dice quale vincolo
  // ha appena rotto.
  tft.drawString(b, ARCHBB_W/2, 210, 7);

  // Riga di stato del gesto (F20). Disegnata dalla sua funzione, che possiede
  // la fascia 244..261: qui non si scrive niente a mano, o le due si
  // sovrascriverebbero a vicenda a ogni refresh.
  displayTempoRiga(0, 0, true);

  // Suggerimento sessione, in fondo. Font 1 e non 2: in font 2 la riga saliva
  // fino a y=258 e mordeva la fascia del gesto.
  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  //  Il font 1 e' a passo fisso da 6 px: 40 caratteri sono 240 px, cioe' TUTTA
  //  la larghezza. Queste due stringhe stanno sotto i 36 per costruzione — un
  //  testo piu' lungo non darebbe errore, verrebbe solo tagliato a meta' parola.
  tft.drawString(st.sessionOpen ? "fuori dalla scheda: CHIUDI sessione"
                                : "fuori dalla scheda: APRI sessione",
                 ARCHBB_W/2, ARCHBB_H - 6, 1);
}

// ----------------------------------------------------------------------------
//  FASE 5 — displaySessionNoSd: avviso "nessuna microSD".
// ----------------------------------------------------------------------------
//  Feedback onesto quando si tocca per gestire la sessione ma la card manca:
//  meglio dirlo che fingere. Un riquadro ambra centrale, breve e bloccante.
void displaySessionNoSd() {
  const int bw = 200, bh = 70;
  const int bx = (ARCHBB_W - bw)/2, by = (ARCHBB_H - bh)/2;
  tft.fillRoundRect(bx, by, bw, bh, 8, ArchColor::BG_CARD);
  tft.drawRoundRect(bx, by, bw, bh, 8, ArchColor::AMBER);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(ArchColor::AMBER, ArchColor::BG_CARD);
  tft.drawString("NESSUNA microSD", ARCHBB_W/2, by + 24, 2);
  tft.setTextColor(ArchColor::TEXT2, ArchColor::BG_CARD);
  tft.drawString("inserisci una card FAT32", ARCHBB_W/2, by + 46, 2);
  delay(800);
}

// ----------------------------------------------------------------------------
//  FASE 5 — scelta sessione: geometria CONDIVISA disegno+hit (lezione §4.1)
// ----------------------------------------------------------------------------
//  Disegno e handler leggono le STESSE costanti: niente letterali duplicati che
//  prima o poi divergono (la cantonata piu' costosa della 1.69).
namespace SChoice {
  constexpr int BTN_X   = 20;
  constexpr int BTN_W   = ARCHBB_W - 40;      // pulsanti larghi, centrati
  constexpr int BTN_H   = 56;
  constexpr int RESUME_Y = 120;               // top del pulsante RIPRENDI
  constexpr int NEW_Y    = 190;               // top del pulsante NUOVA
}

// ============================================================================
//  FASE 24b — SCHERMATA DI SPEGNIMENTO E PREAVVISO DI BATTERIA
// ============================================================================
//  displaySpegnimento: compare quando il PMU ha segnalato la pressione lunga e
//  mancano pochi secondi allo stacco. Non chiede niente e non si puo' annullare
//  (lo spegnimento e' hardware): dice soltanto che la sessione e' stata chiusa
//  come si deve. Serve a distinguere, per chi guarda, "si e' spento" da "si e'
//  rotto" — e a far togliere il dito, se la pressione era accidentale.
void displaySpegnimento(uint8_t pct, bool annullato) {
  static bool primo = true;
  static bool eraAnnullato = false;

  if (primo || annullato != eraAnnullato) {
    primo = false; eraAnnullato = annullato;
    tft.fillScreen(ArchColor::BG);
    tft.setTextDatum(MC_DATUM);
    if (annullato) {
      tft.setTextColor(ArchColor::GOOD, ArchColor::BG);
      tft.drawString("ANNULLATO", ARCHBB_W/2, ARCHBB_H/2 - 24, 4);
      tft.setTextColor(ArchColor::TEXT2, ArchColor::BG);
      tft.drawString("la sessione prosegue", ARCHBB_W/2, ARCHBB_H/2 + 10, 2);
      return;
    }
    tft.setTextColor(ArchColor::AMBER, ArchColor::BG);
    tft.drawString("SPEGNIMENTO", ARCHBB_W/2, ARCHBB_H/2 - 60, 4);
    tft.setTextColor(ArchColor::TEXT2, ArchColor::BG);
    tft.drawString("sessione chiusa", ARCHBB_W/2, ARCHBB_H/2 - 30, 2);
    // L'ISTRUZIONE, che era il pezzo mancante. Lo spegnimento lo decide il PMU
    // dopo dieci secondi di pressione CONTINUA: la IRQ che ci ha portati qui
    // scatta molto prima, quindi al momento in cui questa scritta compare
    // mancano ancora parecchi secondi di dito sul tasto. Senza dirlo, chiunque
    // molla — ed e' esattamente quello che e' successo in campo.
    tft.setTextColor(ArchColor::TEXT, ArchColor::BG);
    tft.drawString("TIENI PREMUTO", ARCHBB_W/2, ARCHBB_H/2 + 6, 4);
    tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
    tft.drawString("lascia per annullare", ARCHBB_W/2, ARCHBB_H/2 + 66, 1);
    tft.drawRoundRect(30, ARCHBB_H/2 + 30, ARCHBB_W - 60, 18, 4, ArchColor::BORDER);
  }

  if (annullato) return;

  // La barra e' una GUIDA, non un cronometro esatto: la soglia vera la conta il
  // PMU in hardware e noi non sappiamo a che punto sia. Serve a dire "manca
  // ancora un po'", che e' l'unica informazione che cambia il comportamento.
  if (pct > 100) pct = 100;
  const int16_t w = (int16_t)((ARCHBB_W - 64) * (int32_t)pct / 100);
  tft.fillRect(32, ARCHBB_H/2 + 32, w, 14, ArchColor::AMBER);
}

// ----------------------------------------------------------------------------
//  displayAvvisoBatteria — il preavviso che la percentuale non sa dare
// ----------------------------------------------------------------------------
//  La colonna pct dell'AXP2101 e' stimata dalla tensione e sotto carico salta:
//  inutilizzabile per decidere se si finisce il percorso. La curva di scarica
//  misurata il 13/09 pero' ha un ginocchio netto, e due soglie in VOLT danno un
//  preavviso onesto: da 3,70 V restano circa 1h25m, da 3,60 V una ventina di
//  minuti. Sotto, crolla.
//
//  Compare una volta sola per soglia (non si ripete a ogni giro) e non chiede
//  niente: informa e sparisce. Un avviso che va confermato, in mezzo a un
//  percorso, e' un avviso che si impara a chiudere senza leggerlo.
void displayAvvisoBatteria(const char* tempoResiduo, bool critico) {
  const uint16_t col = critico ? ArchColor::BAD : ArchColor::AMBER;
  tft.fillScreen(ArchColor::BG);
  tft.drawRect(0, 0, ARCHBB_W, ARCHBB_H, col);
  tft.drawRect(1, 1, ARCHBB_W-2, ARCHBB_H-2, col);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(col, ArchColor::BG);
  tft.drawString("BATTERIA", ARCHBB_W/2, ARCHBB_H/2 - 44, 4);
  tft.setTextColor(ArchColor::TEXT, ArchColor::BG);
  tft.drawString(tempoResiduo, ARCHBB_W/2, ARCHBB_H/2, 4);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  tft.drawString(critico ? "chiudi la sessione" : "stima dalla tensione",
                 ARCHBB_W/2, ARCHBB_H/2 + 40, 2);
}

// ============================================================================
//  FASE 24b — CONFERMA PRIMA DI ENTRARE IN MODO BLE
// ============================================================================
//  PERCHE' ESISTE QUESTA SCHERMATA
//    Il PWRKEY sta sul fianco della scheda, e la mano di scocco porta la
//    patella. Il 12/09 il dispositivo e' entrato in BLE piu' volte da solo. Il
//    danno non e' il fastidio: enterBleMode() SOSPENDE imu_task e trigger_task,
//    quindi ogni freccia scoccata in quella finestra non esiste — nessuna riga,
//    nessun burst, nessun segnale. E' l'unico modo in cui questo strumento puo'
//    perdere un dato senza dichiararlo, ed era raggiungibile col dorso della mano.
//
//  IL VERSO DELLA SICUREZZA
//    Il caso di default e' NO, e il timeout decade su NO. Una pressione
//    accidentale costa una schermata che si ignora per cinque secondi; una
//    pressione voluta costa un tocco in piu'. Lo sbilanciamento e' deliberato:
//    fra "entrare per sbaglio" e "non entrare al primo colpo", solo il primo
//    perde dati.
//
//    Il trigger resta ARMATO per tutta la durata della modale. Se una freccia
//    parte mentre la domanda e' a schermo, viene registrata: la domanda
//    riguarda il futuro, non sospende il presente.
namespace BleConf {
  constexpr int BTN_X  = 20;
  constexpr int BTN_W  = ARCHBB_W - 40;
  constexpr int BTN_H  = 56;
  constexpr int NO_Y   = 120;     // il NO sta SOPRA: e' la risposta di default,
  constexpr int SI_Y   = 190;     // e sta dove cade il pollice per primo
}

void displayBleConfirm(bool sessioneAperta, uint8_t secondiRimasti) {
  using namespace BleConf;
  tft.fillScreen(ArchColor::BG);
  tft.drawRect(0, 0, ARCHBB_W, ARCHBB_H, ArchColor::BORDER);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(ArchColor::AMBER, ArchColor::BG);
  tft.drawString("MODO BLE?", ARCHBB_W/2, 34, 4);

  // L'avvertenza cambia peso secondo il contesto: a sessione aperta si sta per
  // smettere di registrare, e va detto con le parole che contano.
  if (sessioneAperta) {
    tft.setTextColor(ArchColor::BAD, ArchColor::BG);
    tft.drawString("SESSIONE APERTA", ARCHBB_W/2, 64, 2);
    tft.setTextColor(ArchColor::TEXT2, ArchColor::BG);
    tft.drawString("i tiri NON saranno", ARCHBB_W/2, 84, 2);
    tft.drawString("registrati", ARCHBB_W/2, 100, 2);
  } else {
    tft.setTextColor(ArchColor::TEXT2, ArchColor::BG);
    tft.drawString("trasferimento dati", ARCHBB_W/2, 72, 2);
    tft.drawString("verso lo smartphone", ARCHBB_W/2, 92, 2);
  }

  // NO in evidenza: bordo spesso e conto alla rovescia dentro il pulsante, cosi'
  // si capisce senza leggere istruzioni che lasciando fare si resta a tirare.
  tft.fillRoundRect(BTN_X, NO_Y, BTN_W, BTN_H, 8, ArchColor::BG_CARD);
  tft.drawRoundRect(BTN_X, NO_Y, BTN_W, BTN_H, 8, ArchColor::GOOD);
  tft.drawRoundRect(BTN_X+1, NO_Y+1, BTN_W-2, BTN_H-2, 8, ArchColor::GOOD);
  tft.setTextColor(ArchColor::GOOD, ArchColor::BG_CARD);
  tft.drawString("RESTA A TIRARE", ARCHBB_W/2, NO_Y + 18, 4);
  char sub[24];
  snprintf(sub, sizeof(sub), "automatico fra %u s", (unsigned)secondiRimasti);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG_CARD);
  tft.drawString(sub, ARCHBB_W/2, NO_Y + 44, 1);

  tft.fillRoundRect(BTN_X, SI_Y, BTN_W, BTN_H, 8, ArchColor::BG_CARD);
  tft.drawRoundRect(BTN_X, SI_Y, BTN_W, BTN_H, 8, ArchColor::ACCENT);
  tft.setTextColor(ArchColor::ACCENT, ArchColor::BG_CARD);
  tft.drawString("ENTRA IN BLE", ARCHBB_W/2, SI_Y + 20, 4);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG_CARD);
  tft.drawString("sospende la misura", ARCHBB_W/2, SI_Y + 44, 2);
}

// Aggiorna SOLO il conto alla rovescia: ridisegnare tutta la schermata ogni
// secondo farebbe sfarfallare i pulsanti e costerebbe SPI per niente.
void displayBleConfirmTick(uint8_t secondiRimasti) {
  using namespace BleConf;
  char sub[24];
  snprintf(sub, sizeof(sub), "automatico fra %u s", (unsigned)secondiRimasti);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG_CARD);
  tft.fillRect(BTN_X+2, NO_Y + 36, BTN_W-4, 16, ArchColor::BG_CARD);
  tft.drawString(sub, ARCHBB_W/2, NO_Y + 44, 1);
}

// 0 = fuori, 1 = RESTA A TIRARE, 2 = ENTRA IN BLE
uint8_t hitBleConfirm(uint16_t x, uint16_t y) {
  using namespace BleConf;
  if (x < BTN_X || x > BTN_X + BTN_W) return 0;
  if (y >= NO_Y && y <= NO_Y + BTN_H) return 1;
  if (y >= SI_Y && y <= SI_Y + BTN_H) return 2;
  return 0;
}

void displaySessionChoice(const char* lastName, uint16_t lastShots) {
  using namespace SChoice;
  tft.fillScreen(ArchColor::BG);
  tft.drawRect(0, 0, ARCHBB_W, ARCHBB_H, ArchColor::BORDER);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(ArchColor::ACCENT, ArchColor::BG);
  tft.drawString("SESSIONE", ARCHBB_W/2, 40, 4);
  tft.setTextColor(ArchColor::TEXT2, ArchColor::BG);
  tft.drawString("c'e' gia' una sessione", ARCHBB_W/2, 74, 2);

  // Pulsante RIPRENDI (verde): accoda all'ultima.
  tft.fillRoundRect(BTN_X, RESUME_Y, BTN_W, BTN_H, 8, ArchColor::BG_CARD);
  tft.drawRoundRect(BTN_X, RESUME_Y, BTN_W, BTN_H, 8, ArchColor::GOOD);
  tft.setTextColor(ArchColor::GOOD, ArchColor::BG_CARD);
  tft.drawString("RIPRENDI", ARCHBB_W/2, RESUME_Y + 18, 4);
  // Nome cartella (datato o numerato) + conteggio tiri. Il nome puo' essere
  // lungo (SESS_20260721_1430): font 1 per starci.
  char sub[36];
  snprintf(sub, sizeof(sub), "%s (%u tiri)", lastName ? lastName : "?", lastShots);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG_CARD);
  tft.drawString(sub, ARCHBB_W/2, RESUME_Y + 44, 1);

  // Pulsante NUOVA (ciano): apre una sessione vergine.
  tft.fillRoundRect(BTN_X, NEW_Y, BTN_W, BTN_H, 8, ArchColor::BG_CARD);
  tft.drawRoundRect(BTN_X, NEW_Y, BTN_W, BTN_H, 8, ArchColor::ACCENT);
  tft.setTextColor(ArchColor::ACCENT, ArchColor::BG_CARD);
  tft.drawString("NUOVA", ARCHBB_W/2, NEW_Y + 20, 4);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG_CARD);
  tft.drawString("inizia da capo", ARCHBB_W/2, NEW_Y + 44, 2);
}

uint8_t hitSessionChoice(uint16_t x, uint16_t y) {
  using namespace SChoice;
  if (x >= BTN_X && x <= BTN_X + BTN_W) {
    if (y >= RESUME_Y && y <= RESUME_Y + BTN_H) return 1;   // RIPRENDI
    if (y >= NEW_Y    && y <= NEW_Y    + BTN_H) return 2;    // NUOVA
  }
  return 0;
}

// ----------------------------------------------------------------------------
//  FASE 5 — displaySavedFlash: doppio flash verde "SALVATO".
// ----------------------------------------------------------------------------
void displayScartato() {
    tft.fillScreen(ArchColor::BAD);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(ArchColor::TEXT, ArchColor::BAD);
    tft.drawString("SCARTATO", ARCHBB_W / 2, ARCHBB_H / 2 - 12, 4);
    tft.setTextColor(ArchColor::TEXT2, ArchColor::BAD);
    tft.drawString("niente sulla card", ARCHBB_W / 2, ARCHBB_H / 2 + 22, 2);
    delay(700);
}

void displaySavedFlash(uint16_t shotNum) {
  const int bw = 200, bh = 80;
  const int bx = (ARCHBB_W - bw)/2, by = (ARCHBB_H - bh)/2;
  char t[24]; snprintf(t, sizeof(t), "tiro #%u", shotNum);

  for (int i = 0; i < 2; i++) {         // due lampeggi
    tft.fillRoundRect(bx, by, bw, bh, 10, ArchColor::GOOD);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(ArchColor::BG, ArchColor::GOOD);
    tft.drawString("SALVATO", ARCHBB_W/2, by + 30, 4);
    tft.drawString(t, ARCHBB_W/2, by + 58, 2);
    delay(110);
    tft.fillRoundRect(bx, by, bw, bh, 10, ArchColor::BG);   // spegni
    tft.drawRect(0, 0, ARCHBB_W, ARCHBB_H, ArchColor::BORDER);
    delay(70);
  }
}

// ============================================================================
//  Flash di conferma "TIRO RILEVATO"
// ============================================================================
void displayTriggerFlash(uint16_t shotId, float azPeak) {
  tft.fillScreen(ArchColor::AMBER);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(ArchColor::BG, ArchColor::AMBER);   // nero su ambra
  tft.drawString("TIRO", ARCHBB_W/2, ARCHBB_H/2 - 40, 4);
  tft.drawString("RILEVATO", ARCHBB_W/2, ARCHBB_H/2 - 8, 4);

  char t[24]; snprintf(t, sizeof(t), "#%u", shotId);
  tft.drawString(t, ARCHBB_W/2, ARCHBB_H/2 + 34, 6);   // font 6 numerico: ok

  char p[28]; snprintf(p, sizeof(p), "az picco %.0f m/s2", azPeak);
  tft.setTextColor(ArchColor::BG, ArchColor::AMBER);
  tft.drawString(p, ARCHBB_W/2, ARCHBB_H/2 + 74, 2);

  delay(120);   // breve, bloccante: e' l'unico delay volutamente accettato qui
}

// ----------------------------------------------------------------------------
//  FASE 7 — displayBleMode: schermata del modo "colloquio con smartphone".
// ----------------------------------------------------------------------------
//  Layout (dall'alto):
//    - icona Bluetooth stilizzata + titolo "MODO BLE"
//    - stato connessione: "IN ASCOLTO..." (ambra pulsante) o "CONNESSO" (verde)
//    - card col nome device (quello che appare nello scanner dell'app)
//    - riga riassunto: batteria | SD | orologio
//    - piede: istruzione per uscire (ripremere l'ingranaggio)
//
//  Coerente con la palette ArchBB (fondo nero, accento ciano, card grigio-blu).
//  Nessuna animazione nel loop: la ridisegniamo solo quando cambia lo stato di
//  connessione (il main tiene memoria dell'ultimo stato disegnato).
void displayBleMode(bool connected, const char* devName, const StatusInfo& st) {
  tft.fillScreen(ArchColor::BG);
  tft.drawRect(0, 0, ARCHBB_W, ARCHBB_H, ArchColor::BORDER);

  // --- Glifo Bluetooth stilizzato (semplice "runa" a segmenti) -------------
  //  Non serve un font-icona: due triangoli speculari attorno a un'asta danno
  //  la classica rune BT riconoscibile. Colore = stato (ambra/attesa, verde/on).
  const uint16_t accent = connected ? ArchColor::GOOD : ArchColor::ACCENT;
  const int cx = ARCHBB_W / 2, gy = 40;
  tft.drawLine(cx, gy - 16, cx, gy + 16, accent);          // asta verticale
  tft.drawLine(cx, gy - 16, cx + 10, gy - 6, accent);      // alette alto
  tft.drawLine(cx + 10, gy - 6, cx - 10, gy + 6, accent);
  tft.drawLine(cx, gy + 16, cx + 10, gy + 6, accent);      // alette basso
  tft.drawLine(cx + 10, gy + 6, cx - 10, gy - 6, accent);

  // --- Titolo --------------------------------------------------------------
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(ArchColor::ACCENT, ArchColor::BG);
  tft.drawString("MODO BLE", cx, gy + 40, 4);

  // --- Stato connessione ---------------------------------------------------
  if (connected) {
    tft.setTextColor(ArchColor::GOOD, ArchColor::BG);
    tft.drawString("APP CONNESSA", cx, gy + 72, 2);
  } else {
    tft.setTextColor(ArchColor::AMBER, ArchColor::BG);
    tft.drawString("in ascolto...", cx, gy + 72, 2);
  }

  // --- Card col nome device ------------------------------------------------
  const int cardX = 16, cardY = 130, cardW = ARCHBB_W - 32, cardH = 44;
  tft.fillRoundRect(cardX, cardY, cardW, cardH, 8, ArchColor::BG_CARD);
  tft.drawRoundRect(cardX, cardY, cardW, cardH, 8, ArchColor::BORDER);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG_CARD);
  tft.drawString("cerca nell'app:", cx, cardY + 12, 1);
  tft.setTextColor(ArchColor::TEXT, ArchColor::BG_CARD);
  tft.drawString(devName ? devName : "ArchBB-183", cx, cardY + 30, 2);

  // --- Riga riassunto: batteria | SD | orologio ----------------------------
  int ry = 196;
  char line[40];
  // Batteria
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(ArchColor::TEXT2, ArchColor::BG);
  if (st.battPct >= 0) snprintf(line, sizeof(line), "Batt %d%%%s",
                                st.battPct, st.battCharging ? " (in carica)" : "");
  else                 snprintf(line, sizeof(line), "Batt n/d");
  tft.drawString(line, cardX, ry, 2);
  // SD
  ry += 22;
  if (st.sdOk) snprintf(line, sizeof(line), "SD  %lu MB liberi", (unsigned long)st.sdFreeMb);
  else         snprintf(line, sizeof(line), "SD  assente");
  tft.setTextColor(st.sdOk ? ArchColor::TEXT2 : ArchColor::AMBER, ArchColor::BG);
  tft.drawString(line, cardX, ry, 2);
  // Orologio
  ry += 22;
  if (st.hasTime) snprintf(line, sizeof(line), "Ora  %s", st.clock);
  else            snprintf(line, sizeof(line), "Ora  non impostata");
  tft.setTextColor(st.hasTime ? ArchColor::TEXT2 : ArchColor::TEXT3, ArchColor::BG);
  tft.drawString(line, cardX, ry, 2);

  // --- Piede: come uscire --------------------------------------------------
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  tft.drawString("premi l'ingranaggio per uscire", cx, ARCHBB_H - 12, 1);
}
