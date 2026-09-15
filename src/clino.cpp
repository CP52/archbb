// ============================================================================
//  ArchBB 1.83 — clino.cpp · FASE 18 (clinometro di banco)
//  Cesare Pagura · Padova/Noale IT · 31 luglio 2026
// ----------------------------------------------------------------------------
//  Vedi clino.h per il PERCHE'. Qui il COME.
//
//  ARCHITETTURA
//    Un solo ciclo, a due velocita':
//      - LETTURA a ~50 Hz: imu_read_raw -> mount_apply -> angoli + integrali.
//        Veloce perche' la derivata e l'integrale hanno bisogno di campioni
//        fitti; lenta abbastanza da non affamare l'I2C condiviso.
//      - DISEGNO a ~8 Hz: ridisegna SOLO i rettangoli dei valori. Ridisegnare
//        tutto a ogni giro darebbe sfarfallio e occuperebbe il bus SPI senza
//        alcun guadagno di leggibilita': l'occhio non legge 50 numeri al
//        secondo.
//
//  CONVENZIONI ANGOLARI — le stesse di shot_angles.cpp, non una copia riscritta
//    Nel frame CANONICO (ax = verticale/gravita', ay = laterale, az = freccia):
//        alzo = atan2(az, ax)   positivo = punta ALTA
//        cant = atan2(ay, ax)   positivo = inclinato a DESTRA
//    Sono le stesse formule che shot_angles usa sui tiri veri. Se qui i segni
//    tornano al banco, tornano anche li'. Se non tornassero, sarebbe questo il
//    posto in cui accorgersene — non un CSV letto tre giorni dopo.
//
//  CONVENZIONI DI VELOCITA' ANGOLARE — dall'ispezione fisica del chip (F16)
//    Assi reali del QMI8658 nel montaggio attuale (scheda ruotata di 90 a
//    destra): X = giu, Y = asse freccia verso la punta, Z = destra.
//        rotazione di ALZO -> attorno al laterale -> grezzo rgz -> canonico cgy
//        rotazione di CANT -> attorno alla freccia -> grezzo rgy -> canonico cgz
//        rotazione di YAW  -> attorno al verticale -> grezzo rgx -> canonico cgx
//    Da questa geometria si DEDUCEVA che tutte e tre arrivassero nel canonico
//    col segno invertito. La deduzione era giusta per una sola delle tre.
//
//  CONVENZIONI MISURATE — banco del 31/07/2026, e queste comandano
//        rate_alzo = -cgy      confermato: diagonale ALZO a +1 nella pagina
//                              COERENZA, ed e' lo stesso segno che integrate_gy
//                              usa per hold, validato su 55 tiri veri.
//        rate_cant = +cgz      MISURATO. La pagina COERENZA dava -1 sulla
//                              diagonale CANT con l'ipotesi -cgz.
//        rate_yaw  = -cgx      MISURATO a occhio: ruotando a destra la riga
//                              yaw diceva "verso SINISTRA".
//
//    E' la quinta volta che su questi assi una derivazione geometrica perde
//    contro una misura. Vale la pena registrare PERCHE' non c'e' una regola
//    unica che spieghi le tre: se ci fosse — per esempio un'unica inversione
//    globale dovuta al fatto che mount_apply applica al giroscopio la STESSA
//    matrice dell'accelerometro, mentre la velocita' angolare e' uno
//    PSEUDOVETTORE e sotto una riflessione (il flip-Z ha det = -1) cambia segno
//    in piu' — allora tutte e tre sarebbero invertite. Non lo sono: solo l'alzo
//    lo e' rispetto alla deduzione.
//
//    CONSEGUENZA PRATICA, da non dimenticare: questi tre segni valgono per il
//    montaggio CORRENTE. Non sono stati derivati da una trasformazione, quindi
//    non si trasformano insieme a essa. Se un giorno si cambia
//    mount_orientation, vanno RIMISURATI con questa stessa pagina — non
//    ricalcolati a tavolino.
//
//  CHI CONSUMA QUESTI SEGNI (verificato con grep sul firmware, 31/07)
//        gy -> integrate_gy() in shot_angles.cpp = hold_deg. Unico consumatore.
//        gz -> NESSUNO. Viene salvato nel burst, mai usato per una metrica.
//        gx -> NESSUNO.
//    Percio' la correzione di rate_cant e rate_yaw resta confinata a questo
//    file: hold_deg non e' toccato e le 55 sedute non vanno riviste.
//
//  REGOLA §4.1 (la lezione piu' costosa della 1.69)
//    Le costanti di geometria dei pulsanti sono definite UNA volta e usate sia
//    dal disegno sia dal gestore del tocco. Nessun letterale duplicato: un
//    pulsante che si disegna dove non risponde e' un bug garantito.
// ============================================================================

#include "clino.h"

#include <Arduino.h>
#include <math.h>

#include "config.h"
#include "config_store.h"
#include "display.h"      // ArchColor, ARCHBB_W / ARCHBB_H
#include "imu.h"          // imu_read_raw
#include "mount.h"        // mount_apply, g_mount_orientation, mount_name
#include "touch.h"        // touchRead, TouchData

// ============================================================================
//  PARAMETRI DI MISURA
// ============================================================================

// Periodo del ciclo di lettura. 20 ms -> ~50 Hz. L'IMU gira a ~218 Hz reali,
// quindi ogni giro trova sempre un campione fresco: non stiamo interpolando
// nulla, stiamo solo decimando.
static const uint32_t CLINO_LOOP_MS = 20;

// Periodo di ridisegno dei valori.
static const uint32_t CLINO_DRAW_MS = 125;   // 8 Hz

// Filtro sull'accelerometro. EMA a un polo: alpha 0.20 a 50 Hz da' una costante
// di tempo di ~90 ms. Abbastanza per togliere il tremolio della mano, abbastanza
// poco da non ritardare la lettura oltre il tempo di reazione dell'occhio.
static const float CLINO_ALPHA = 0.20f;

// Finestra di confronto accelerometro/giroscopio nella pagina COERENZA.
// 250 ms: lunga abbastanza da accumulare qualche grado con un movimento lento,
// corta abbastanza da restare dentro l'ipotesi "assetto quasi statico".
static const uint32_t CLINO_WIN_MS = 250;

// Soglia di movimento: sotto questo scostamento nella finestra, il campione non
// entra nella statistica. 1.0 gradi su 250 ms sono 4 gradi/s.
// PERCHE' NON PIU' BASSA: sotto i 4 gradi/s il rumore dell'accelerometro
// (frazioni di grado) diventa una frazione non trascurabile del segnale e la
// pendenza si sporca. PERCHE' NON PIU' ALTA: oltre i ~15 gradi/s le
// accelerazioni dinamiche della mano cominciano a falsare l'angolo ricavato
// dalla gravita', che e' il RIFERIMENTO del confronto.
static const float CLINO_MOTO_MIN_DEG = 1.0f;

// Rapporto di dominanza fra i due movimenti. Un campione entra nella riga ALZO
// solo se il movimento di alzo e' almeno 3 volte quello di cant, e viceversa.
// Serve a tenere pulite le celle FUORI diagonale: se l'operatore muove le due
// cose insieme, un eventuale scambio d'assi diventa indistinguibile.
static const float CLINO_DOMINANZA = 3.0f;

// Campioni minimi per dare un verdetto su una riga della matrice.
static const uint16_t CLINO_N_MIN = 40;

// Stabilita' richiesta per consentire l'azzeramento degli offset: |a| deve
// stare entro questa tolleranza da g e la deviazione standard sotto la soglia.
static const float CLINO_G_TOLL   = 0.35f;   // m/s^2
static const float CLINO_SD_MAX   = 0.12f;   // m/s^2
static const float CLINO_G_NOM    = 9.80665f;

// ============================================================================
//  GEOMETRIA DELLO SCHERMO — definita UNA volta (regola §4.1)
// ============================================================================
static const int16_t HDR_H   = 26;

static const int16_t CARD_X  = 8;
static const int16_t CARD_W  = ARCHBB_W - 16;   // 224
static const int16_t CARD_H  = 52;
static const int16_t ALZO_Y  = 32;
static const int16_t CANT_Y  = 88;

// Rettangolo del VALORE dentro la card: e' l'unica zona che si ripulisce a ogni
// ridisegno. Etichetta e cornice restano ferme.
static const int16_t VAL_X   = CARD_X + 74;
static const int16_t VAL_W   = CARD_W - 78;

static const int16_t HOR_Y   = 144;
static const int16_t HOR_H   = 46;

static const int16_t YAW_Y   = 194;
static const int16_t YAW_H   = 18;

static const int16_t RAW_Y   = 214;
static const int16_t RAW_H   = 28;   // due righe da 14

static const int16_t BTN_Y   = 246;
static const int16_t BTN_H   = ARCHBB_H - BTN_Y;   // 38
static const int16_t BTN_W   = ARCHBB_W / 3;       // 80

// Indice del pulsante toccato, o -1. Usa le STESSE costanti del disegno.
static int8_t clino_button_at(uint16_t x, uint16_t y) {
  if (y < BTN_Y) return -1;
  if (x < BTN_W)       return 0;
  if (x < 2 * BTN_W)   return 1;
  return 2;
}

// ============================================================================
//  STATO DEL MODULO
// ============================================================================
enum ClinoPage { PAG_ASSETTO = 0, PAG_COERENZA = 1 };

// Anello per il confronto accelerometro/giroscopio. Tiene angoli DALL'ACCELERO
// e integrali del GIROSCOPIO sullo stesso asse dei tempi: confrontiamo gradi
// con gradi, non gradi con gradi-al-secondo.
//
// PERCHE' INTEGRARE IL GIROSCOPIO INVECE DI DERIVARE L'ACCELEROMETRO
//   Derivare amplifica il rumore, integrare lo media. Su 250 ms la deriva del
//   giroscopio (misurata ~7 gradi/s nei casi peggiori, ma quella e' deriva di
//   BIAS su finestre lunghe) e' irrilevante, mentre il rumore della derivata
//   numerica dell'angolo da gravita' non lo sarebbe affatto.
static const uint8_t RING_N = 32;   // 32 * 20ms = 640 ms di storia: basta e avanza

struct ClinoRing {
  uint32_t t_ms[RING_N];
  float    alzo[RING_N];    // gradi, da accelerometro
  float    cant[RING_N];
  float    cum_ga[RING_N];  // gradi, integrale di rate_alzo
  float    cum_gc[RING_N];
  uint8_t  head;
  uint8_t  n;
};

// Accumulatori per la matrice 2x2 delle pendenze. Minimi quadrati senza
// intercetta: pendenza = somma(x*y) / somma(x*x). L'intercetta non serve perche'
// entrambe le grandezze sono VARIAZIONI: a movimento nullo devono valere zero
// tutt'e due, e forzare il passaggio per l'origine e' fisica, non comodita'.
struct ClinoFit {
  double sxx_a, sxy_aa, sxy_ac;   // riga: movimento di ALZO
  double sxx_c, sxy_cc, sxy_ca;   // riga: movimento di CANT
  uint16_t n_a, n_c;
};

static void clino_fit_reset(ClinoFit& f) {
  f.sxx_a = f.sxy_aa = f.sxy_ac = 0.0;
  f.sxx_c = f.sxy_cc = f.sxy_ca = 0.0;
  f.n_a = f.n_c = 0;
}

// Media e deviazione standard di |a| sulla finestra corta, piu' il giudizio
// FERMO. UNA sola implementazione: la usano sia il disegno (che mostra il
// semaforo) sia il gestore di AZZERA (che decide se lasciar salvare). Se fossero
// due, prima o poi il display direbbe FERMO e il pulsante rifiuterebbe — la
// regola §4.1 nasce esattamente da questo tipo di divergenza.
static bool clino_stat(const float* buf, uint8_t n, float& media, float& sd) {
  media = 0.0f; sd = 0.0f;
  for (uint8_t i = 0; i < n; i++) media += buf[i];
  if (n) media /= n;
  float v = 0.0f;
  for (uint8_t i = 0; i < n; i++) v += (buf[i] - media) * (buf[i] - media);
  if (n >= 2) sd = sqrtf(v / (n - 1));
  return (n >= 2) && (fabsf(media - CLINO_G_NOM) <= CLINO_G_TOLL) && (sd <= CLINO_SD_MAX);
}

// ============================================================================
//  DISEGNO — mattoni comuni
// ============================================================================

// Numero grande con segno. Il font 6 contiene SOLO cifre, punto, due punti e il
// meno: il "+" sparirebbe senza dare errore (e' gia' successo due volte in
// questo progetto, ed e' il motivo per cui controlla.sh lo cerca).
// Quindi: cifre in font 6, segno in font 4 accanto. Il segno "-" lo si potrebbe
// disegnare in font 6, ma tenerli entrambi in font 4 garantisce che occupino la
// stessa larghezza e il numero non balli fra positivo e negativo.
static void drawSignedBig(TFT_eSPI& t, float v, int16_t xRight, int16_t yMid,
                          uint16_t col, uint16_t bg) {
  char cifre[12];
  float a = fabsf(v);
  if (a >= 99.9f) a = 99.9f;
  snprintf(cifre, sizeof(cifre), "%.1f", (double)a);

  t.setTextColor(col, bg);
  t.setTextDatum(MR_DATUM);
  int wDeg = t.drawString("o", xRight, yMid - 8, 2);   // apice "gradi"
  int wNum = t.drawString(cifre, xRight - wDeg - 2, yMid, 6);
  t.drawString((v < 0.0f) ? "-" : "+", xRight - wDeg - wNum - 4, yMid, 4);
}

// Etichetta a sinistra dentro una card.
static void drawCardChrome(TFT_eSPI& t, int16_t y, const char* label, uint16_t col) {
  t.fillRoundRect(CARD_X, y, CARD_W, CARD_H, 8, ArchColor::BG_CARD);
  t.drawRoundRect(CARD_X, y, CARD_W, CARD_H, 8, ArchColor::BORDER);
  t.setTextDatum(ML_DATUM);
  t.setTextColor(col, ArchColor::BG_CARD);
  t.drawString(label, CARD_X + 10, y + CARD_H / 2 - 8, 4);
}

// Barra dei pulsanti: tre celle di larghezza BTN_W. Usa le STESSE costanti del
// gestore del tocco.
static void drawButtons(TFT_eSPI& t, const char* a, uint16_t ca,
                                     const char* b, uint16_t cb,
                                     const char* c, uint16_t cc) {
  const char*    lbl[3] = { a, b, c };
  const uint16_t col[3] = { ca, cb, cc };
  for (int i = 0; i < 3; i++) {
    int16_t x = (int16_t)(i * BTN_W);
    int16_t w = (i == 2) ? (ARCHBB_W - 2 * BTN_W) : BTN_W;
    t.fillRect(x, BTN_Y, w, BTN_H, ArchColor::BG_CARD);
    t.drawRect(x, BTN_Y, w, BTN_H, ArchColor::BORDER);
    t.setTextDatum(MC_DATUM);
    t.setTextColor(col[i], ArchColor::BG_CARD);
    t.drawString(lbl[i], x + w / 2, BTN_Y + BTN_H / 2, 2);
  }
}

static void drawHeader(TFT_eSPI& t, const char* titolo) {
  t.fillRect(0, 0, ARCHBB_W, HDR_H, ArchColor::BG_CARD);
  t.drawFastHLine(0, HDR_H - 1, ARCHBB_W, ArchColor::BORDER);
  t.setTextDatum(ML_DATUM);
  t.setTextColor(ArchColor::ACCENT, ArchColor::BG_CARD);
  t.drawString(titolo, 8, HDR_H / 2, 2);
  t.setTextDatum(MR_DATUM);
  t.setTextColor(ArchColor::TEXT3, ArchColor::BG_CARD);
  t.drawString(mount_name(g_mount_orientation), ARCHBB_W - 8, HDR_H / 2, 2);
}

// ============================================================================
//  PAGINA 1 — ASSETTO
// ============================================================================
static void drawAssettoChrome(TFT_eSPI& tft, bool azzeraArmato) {
  tft.fillScreen(ArchColor::BG);
  drawHeader(tft, "CLINO / assetto");
  drawCardChrome(tft, ALZO_Y, "ALZO", ArchColor::AMBER);
  drawCardChrome(tft, CANT_Y, "CANT", ArchColor::ACCENT);
  tft.drawRoundRect(CARD_X, HOR_Y, CARD_W, HOR_H, 6, ArchColor::BORDER);
  drawButtons(tft,
              azzeraArmato ? "CONFERMA" : "AZZERA",
              azzeraArmato ? ArchColor::AMBER : ArchColor::TEXT2,
              "COERENZA", ArchColor::GOOD,
              "ESCI",     ArchColor::TEXT2);
}

static void drawAssettoValues(TFT_eSPI& tft,
                              float alzo, float cant, float yaw_rate,
                              float rax, float ray, float raz,
                              float gmod, float gsd,
                              bool fermo, bool offsetAttivi) {
  // --- valori grandi -------------------------------------------------------
  tft.fillRect(VAL_X, ALZO_Y + 4, VAL_W, CARD_H - 8, ArchColor::BG_CARD);
  drawSignedBig(tft, alzo, CARD_X + CARD_W - 8, ALZO_Y + 20,
                ArchColor::AMBER, ArchColor::BG_CARD);
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG_CARD);
  tft.drawString(fabsf(alzo) < 0.5f ? "in piano"
                                    : (alzo > 0 ? "punta ALTA" : "punta BASSA"),
                 CARD_X + CARD_W - 8, ALZO_Y + CARD_H - 12, 1);

  tft.fillRect(VAL_X, CANT_Y + 4, VAL_W, CARD_H - 8, ArchColor::BG_CARD);
  drawSignedBig(tft, cant, CARD_X + CARD_W - 8, CANT_Y + 20,
                ArchColor::ACCENT, ArchColor::BG_CARD);
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG_CARD);
  tft.drawString(fabsf(cant) < 0.5f ? "a piombo"
                                    : (cant > 0 ? "inclina DESTRA" : "inclina SINISTRA"),
                 CARD_X + CARD_W - 8, CANT_Y + CARD_H - 12, 1);

  // --- indicatore grafico --------------------------------------------------
  //  Due segmenti che si incrociano al centro:
  //    - il LUNGO e' l'asse freccia, ruotato dell'alzo (sale a destra se la
  //      punta e' alta). E' l'informazione che l'arciere gia' possiede dalla
  //      scala di mira, quindi e' quella a cui l'occhio va per prima.
  //    - il CORTO e' l'asse verticale dell'arco, ruotato del cant (la cima va a
  //      destra se il cant e' positivo). Il segno si legge senza pensarci: e'
  //      il motivo per cui esiste, dato che il numero da solo richiede di
  //      ricordare una convenzione.
  const int16_t cx = CARD_X + CARD_W / 2;
  const int16_t cy = HOR_Y + HOR_H / 2;
  tft.fillRect(CARD_X + 1, HOR_Y + 1, CARD_W - 2, HOR_H - 2, ArchColor::BG);

  // riferimento: orizzonte vero e verticale vera, in grigio
  tft.drawFastHLine(CARD_X + 6, cy, CARD_W - 12, ArchColor::BG_CARD);
  tft.drawFastVLine(cx, HOR_Y + 4, HOR_H - 8, ArchColor::BG_CARD);

  const float ra = (float)(alzo * M_PI / 180.0);
  const float rc = (float)(cant * M_PI / 180.0);
  const float LA = 92.0f;   // semi-lunghezza del segmento freccia
  const float LC = 17.0f;   // semi-lunghezza del segmento verticale arco

  tft.drawLine((int)(cx - LA * cosf(ra)), (int)(cy + LA * sinf(ra)),
               (int)(cx + LA * cosf(ra)), (int)(cy - LA * sinf(ra)),
               ArchColor::AMBER);
  tft.drawLine((int)(cx + LC * sinf(rc)), (int)(cy - LC * cosf(rc)),
               (int)(cx - LC * sinf(rc)), (int)(cy + LC * cosf(rc)),
               ArchColor::ACCENT);
  tft.fillCircle(cx, cy, 3, ArchColor::TEXT);

  // --- yaw: SOLO velocita', mai angolo -------------------------------------
  //  La gravita' non osserva la rotazione attorno alla verticale: integrare gx
  //  qui produrrebbe una deriva monotona spacciata per un angolo. Mostrare la
  //  velocita' e' l'unica cosa onesta che si possa fare senza magnetometro.
  tft.fillRect(0, YAW_Y, ARCHBB_W, YAW_H, ArchColor::BG);
  char buf[48];
  snprintf(buf, sizeof(buf), "yaw %+.1f gradi/s  %s", (double)yaw_rate,
           (fabsf(yaw_rate) < 2.0f) ? "fermo" : (yaw_rate > 0 ? "verso DESTRA" : "verso SINISTRA"));
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(ArchColor::GOOD, ArchColor::BG);
  tft.drawString(buf, ARCHBB_W / 2, YAW_Y + YAW_H / 2, 1);

  // --- piede: i grezzi ------------------------------------------------------
  //  Non servono all'arciere: servono a chi verifica che la mappatura di
  //  montaggio sia quella dichiarata. Se un giorno i numeri grandi mentissero,
  //  la bugia si vede qui.
  tft.fillRect(0, RAW_Y, ARCHBB_W, RAW_H, ArchColor::BG);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  snprintf(buf, sizeof(buf), "grezzi  x %+6.2f  y %+6.2f  z %+6.2f", (double)rax, (double)ray, (double)raz);
  tft.drawString(buf, ARCHBB_W / 2, RAW_Y, 1);

  snprintf(buf, sizeof(buf), "|a| %.2f  sd %.3f  %s  offset %s",
           (double)gmod, (double)gsd,
           fermo ? "FERMO" : "in moto",
           offsetAttivi ? "ON" : "off");
  tft.setTextColor(fermo ? ArchColor::GOOD : ArchColor::TEXT3, ArchColor::BG);
  tft.drawString(buf, ARCHBB_W / 2, RAW_Y + 14, 1);
}

// ============================================================================
//  PAGINA 2 — COERENZA
// ============================================================================
static void drawCoerenzaChrome(TFT_eSPI& tft) {
  tft.fillScreen(ArchColor::BG);
  drawHeader(tft, "CLINO / coerenza");

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(ArchColor::TEXT2, ArchColor::BG);
  tft.drawString("muovi LENTAMENTE, un asse per volta", ARCHBB_W / 2, HDR_H + 6, 1);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  tft.drawString("prima punta su/giu, poi cant dx/sx", ARCHBB_W / 2, HDR_H + 18, 1);

  // Intestazione della matrice.
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  tft.drawString("gyro ALZO", 150, 66, 1);
  tft.drawString("gyro CANT", 224, 66, 1);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("muovo ALZO", 8,  84, 1);
  tft.drawString("muovo CANT", 8, 106, 1);

  tft.drawFastHLine(8, 78, 216, ArchColor::BORDER);
  tft.drawFastVLine(78, 62, 62, ArchColor::BORDER);

  drawButtons(tft, "AZZERA",  ArchColor::TEXT2,
                   "ASSETTO", ArchColor::AMBER,
                   "ESCI",    ArchColor::TEXT2);
}

// Scrive una cella della matrice. Verde se e' quello che ci si aspetta,
// rosso se contraddice l'ipotesi, grigio se non ci sono ancora dati.
static void drawCella(TFT_eSPI& tft, int16_t xr, int16_t y,
                      bool haDati, float valore, bool attesaUno) {
  tft.fillRect(xr - 66, y - 2, 68, 18, ArchColor::BG);
  tft.setTextDatum(TR_DATUM);
  if (!haDati) {
    tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
    tft.drawString("--", xr, y, 2);
    return;
  }
  bool ok = attesaUno ? (valore > 0.70f && valore < 1.30f)
                      : (fabsf(valore) < 0.30f);
  char b[12];
  snprintf(b, sizeof(b), "%+.2f", (double)valore);
  tft.setTextColor(ok ? ArchColor::GOOD : ArchColor::BAD, ArchColor::BG);
  tft.drawString(b, xr, y, 2);
}

static void drawCoerenzaValues(TFT_eSPI& tft, const ClinoFit& f) {
  const bool okA = (f.n_a >= CLINO_N_MIN) && (f.sxx_a > 1e-6);
  const bool okC = (f.n_c >= CLINO_N_MIN) && (f.sxx_c > 1e-6);
  const float paa = okA ? (float)(f.sxy_aa / f.sxx_a) : 0.0f;
  const float pac = okA ? (float)(f.sxy_ac / f.sxx_a) : 0.0f;
  const float pcc = okC ? (float)(f.sxy_cc / f.sxx_c) : 0.0f;
  const float pca = okC ? (float)(f.sxy_ca / f.sxx_c) : 0.0f;

  drawCella(tft, 150, 84,  okA, paa, true);
  drawCella(tft, 224, 84,  okA, pac, false);
  drawCella(tft, 150, 106, okC, pca, false);
  drawCella(tft, 224, 106, okC, pcc, true);

  // Contatori: dicono se il verdetto e' gia' formulabile o se serve muoversi
  // ancora. Un verdetto dato su pochi campioni sarebbe peggio di nessun
  // verdetto, perche' verrebbe creduto.
  char b[64];
  tft.fillRect(0, 130, ARCHBB_W, 14, ArchColor::BG);
  snprintf(b, sizeof(b), "campioni  alzo %u / %u    cant %u / %u",
           (unsigned)f.n_a, (unsigned)CLINO_N_MIN,
           (unsigned)f.n_c, (unsigned)CLINO_N_MIN);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  tft.drawString(b, ARCHBB_W / 2, 130, 1);

  // --- verdetto ------------------------------------------------------------
  const char* riga1 = "raccolta in corso";
  const char* riga2 = "muovi l'arco lentamente";
  uint16_t col = ArchColor::TEXT3;

  if (okA && okC) {
    const bool diagOk = (paa > 0.70f && paa < 1.30f) && (pcc > 0.70f && pcc < 1.30f);
    const bool fuoriOk = (fabsf(pac) < 0.30f) && (fabsf(pca) < 0.30f);
    const bool scambio = (fabsf(pac) > 0.70f) || (fabsf(pca) > 0.70f);

    // Il verdetto dice QUALE asse e QUALE codice va toccato. "Segno invertito"
    // e basta manderebbe a correggere anche cio' che e' giusto: l'alzo tocca
    // hold_deg (55 tiri validati), il cant non tocca niente. Confonderli
    // costerebbe una regressione su dati buoni.
    const bool invAlzo = (paa < -0.70f && paa > -1.30f);
    const bool invCant = (pcc < -0.70f && pcc > -1.30f);

    if (diagOk && fuoriOk)      { riga1 = "ASSI E SEGNI COERENTI";
                                  riga2 = "gyro e accelerometro concordano";
                                  col = ArchColor::GOOD; }
    else if (scambio)           { riga1 = "ASSI SCAMBIATI";
                                  riga2 = "rivedi mount_apply, non i segni";
                                  col = ArchColor::BAD; }
    else if (invAlzo && invCant){ riga1 = "SEGNO INVERTITO: ALZO e CANT";
                                  riga2 = "attenzione: l'alzo tocca hold_deg";
                                  col = ArchColor::BAD; }
    else if (invAlzo)           { riga1 = "SEGNO INVERTITO: ALZO";
                                  riga2 = "clino.cpp E integrate_gy insieme";
                                  col = ArchColor::BAD; }
    else if (invCant)           { riga1 = "SEGNO INVERTITO: CANT";
                                  riga2 = "solo clino.cpp: nessuno usa gz";
                                  col = ArchColor::BAD; }
    else                        { riga1 = "INCOERENTE";
                                  riga2 = "rifai piu' lentamente prima di dedurre";
                                  col = ArchColor::AMBER; }
  }

  tft.fillRect(0, 150, ARCHBB_W, 40, ArchColor::BG);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(col, ArchColor::BG);
  tft.drawString(riga1, ARCHBB_W / 2, 152, 2);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  tft.drawString(riga2, ARCHBB_W / 2, 174, 1);

  // Promemoria dell'ipotesi sotto esame: senza, la matrice e' quattro numeri
  // senza significato fra tre mesi.
  tft.fillRect(0, 194, ARCHBB_W, 46, ArchColor::BG);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  tft.drawString("ipotesi sotto esame:", ARCHBB_W / 2, 196, 1);
  tft.drawString("rate_alzo = -cgy   rate_cant = +cgz", ARCHBB_W / 2, 210, 1);
  tft.drawString("attesi: diagonale +1, fuori 0", ARCHBB_W / 2, 224, 1);
}

// ============================================================================
//  clino_run
// ============================================================================
void clino_run(TFT_eSPI& tft) {
  // --- stato di lettura ----------------------------------------------------
  float fx = 0, fy = 0, fz = 0;      // accel GREZZI filtrati (frame sensore)
  bool  primo = true;

  // deviazione standard di |a| su finestra corta, per il semaforo FERMO
  static const uint8_t SD_N = 16;
  float sdBuf[SD_N];
  uint8_t sdHead = 0, sdFill = 0;

  ClinoRing ring;
  ring.head = 0; ring.n = 0;

  ClinoFit fit;
  clino_fit_reset(fit);

  double cum_ga = 0.0, cum_gc = 0.0;   // integrali del giroscopio [gradi]
  uint32_t tPrev = millis();

  // Ultimi valori validi. Vivono FUORI dal blocco di lettura: se una lettura
  // I2C fallisce (timeout di imu_read_raw), il disegno e il tocco devono
  // continuare a funzionare con l'ultimo dato buono. Legare l'interfaccia al
  // successo del sensore significherebbe che un singolo NACK sul bus lascia il
  // dispositivo senza pulsante ESCI.
  float alzo_raw = 0, cant_raw = 0, yaw_rate = 0;

  ClinoPage pagina = PAG_ASSETTO;
  bool azzeraArmato = false;
  uint32_t azzeraArmatoMs = 0;

  bool ridisegnaChrome = true;
  uint32_t tDraw = 0;
  bool ditoGiu = false;   // per accettare un tocco per volta (fronte di discesa)

  for (;;) {
    const uint32_t now = millis();

    // ---------------------------------------------------------------------
    //  1) LETTURA
    // ---------------------------------------------------------------------
    float rax, ray, raz, rgx, rgy, rgz;
    if (imu_read_raw(rax, ray, raz, rgx, rgy, rgz)) {
      if (primo) { fx = rax; fy = ray; fz = raz; primo = false; }
      else {
        fx += CLINO_ALPHA * (rax - fx);
        fy += CLINO_ALPHA * (ray - fy);
        fz += CLINO_ALPHA * (raz - fz);
      }

      // deviazione standard del modulo, per sapere se l'arco e' davvero fermo
      sdBuf[sdHead] = sqrtf(rax * rax + ray * ray + raz * raz);
      sdHead = (uint8_t)((sdHead + 1) % SD_N);
      if (sdFill < SD_N) sdFill++;

      // --- frame canonico -------------------------------------------------
      //  mount_apply applica il flip-Z hardware e la rotazione di montaggio.
      //  Da qui in poi ax = verticale, ay = laterale, az = freccia.
      float cax = fx, cay = fy, caz = fz;
      float cgx = rgx, cgy = rgy, cgz = rgz;
      mount_apply(g_mount_orientation, cax, cay, caz, cgx, cgy, cgz);

      alzo_raw = (float)(atan2((double)caz, (double)cax) * 180.0 / M_PI);
      cant_raw = (float)(atan2((double)cay, (double)cax) * 180.0 / M_PI);
      yaw_rate = -cgx;  // MISURATO 31/07: era +cgx, dava destra e sinistra scambiate

      // Velocita' angolari nelle convenzioni del progetto (vedi testata).
      const float rate_alzo = -cgy;   // confermato (hold, 55 tiri + coerenza)
      const float rate_cant = +cgz;   // MISURATO 31/07: la derivazione diceva -cgz

      // --- integrale del giroscopio ---------------------------------------
      const float dt_s = (float)(now - tPrev) / 1000.0f;
      tPrev = now;
      if (dt_s > 0.0f && dt_s < 0.2f) {
        cum_ga += (double)rate_alzo * (double)dt_s;
        cum_gc += (double)rate_cant * (double)dt_s;
      }

      // --- anello ----------------------------------------------------------
      ring.t_ms[ring.head]   = now;
      ring.alzo[ring.head]   = alzo_raw;
      ring.cant[ring.head]   = cant_raw;
      ring.cum_ga[ring.head] = (float)cum_ga;
      ring.cum_gc[ring.head] = (float)cum_gc;
      ring.head = (uint8_t)((ring.head + 1) % RING_N);
      if (ring.n < RING_N) ring.n++;

      // --- statistica di coerenza (solo nella pagina che la usa) ----------
      if (pagina == PAG_COERENZA && ring.n >= 4) {
        // Cerca il campione piu' vecchio ancora dentro la finestra.
        int8_t idx = -1;
        for (uint8_t k = 1; k < ring.n; k++) {
          uint8_t j = (uint8_t)((ring.head + RING_N - 1 - k) % RING_N);
          if (now - ring.t_ms[j] <= CLINO_WIN_MS) idx = (int8_t)j;
          else break;
        }
        if (idx >= 0) {
          uint8_t cur = (uint8_t)((ring.head + RING_N - 1) % RING_N);
          const float dA = ring.alzo[cur]   - ring.alzo[idx];     // accelerometro
          const float dC = ring.cant[cur]   - ring.cant[idx];
          const float gA = ring.cum_ga[cur] - ring.cum_ga[idx];   // giroscopio
          const float gC = ring.cum_gc[cur] - ring.cum_gc[idx];

          const float aA = fabsf(dA), aC = fabsf(dC);
          if (aA >= CLINO_MOTO_MIN_DEG && aA > CLINO_DOMINANZA * aC) {
            fit.sxx_a  += (double)dA * dA;
            fit.sxy_aa += (double)dA * gA;
            fit.sxy_ac += (double)dA * gC;
            fit.n_a++;
          } else if (aC >= CLINO_MOTO_MIN_DEG && aC > CLINO_DOMINANZA * aA) {
            fit.sxx_c  += (double)dC * dC;
            fit.sxy_cc += (double)dC * gC;
            fit.sxy_ca += (double)dC * gA;
            fit.n_c++;
          }
        }
      }

    }   // fine del blocco "lettura riuscita"

    // -----------------------------------------------------------------------
    //  2) DISEGNO  — fuori dal blocco di lettura, per i motivi detti sopra
    // -----------------------------------------------------------------------
    {
      if (ridisegnaChrome) {
        if (pagina == PAG_ASSETTO) drawAssettoChrome(tft, azzeraArmato);
        else                       drawCoerenzaChrome(tft);
        ridisegnaChrome = false;
        tDraw = 0;
      }

      if (now - tDraw >= CLINO_DRAW_MS) {
        tDraw = now;
        if (pagina == PAG_ASSETTO) {
          float m, sd;
          const bool fermo = clino_stat(sdBuf, sdFill, m, sd);

          const bool offAttivi = (fabsf(g_config.offset_alzo_deg) > 1e-4f) ||
                                 (fabsf(g_config.offset_cant_deg) > 1e-4f);
          drawAssettoValues(tft,
                            alzo_raw - g_config.offset_alzo_deg,
                            cant_raw - g_config.offset_cant_deg,
                            yaw_rate,                  // yaw: rate, mai angolo
                            fx, fy, fz, m, sd, fermo, offAttivi);
        } else {
          drawCoerenzaValues(tft, fit);
        }
      }

      // ---------------------------------------------------------------------
      //  3) TOCCO
      // ---------------------------------------------------------------------
      TouchData td = touchRead();
      if (td.valid && td.fingers > 0 && td.coordsFresh) {
        if (!ditoGiu) {
          ditoGiu = true;
          const int8_t b = clino_button_at(td.x, td.y);
          if (pagina == PAG_ASSETTO) {
            if (b == 0) {
              // AZZERA in due tempi: il primo tocco arma, il secondo conferma.
              // Scrivere in NVS al primo tocco sarebbe un incidente in attesa.
              float m, sd;
              const bool fermo = clino_stat(sdBuf, sdFill, m, sd);

              if (!azzeraArmato) {
                if (fermo) { azzeraArmato = true; azzeraArmatoMs = now; ridisegnaChrome = true; }
              } else {
                if (fermo) {
                  // L'offset e' cio' che il sensore legge quando l'arco E' in
                  // assetto di riferimento: freccia orizzontale, arco a piombo.
                  // Si sottrae DOPO l'atan2, esattamente come in shot_angles.
                  g_config.offset_alzo_deg = alzo_raw;
                  g_config.offset_cant_deg = cant_raw;
                  config_save();
                }
                azzeraArmato = false;
                ridisegnaChrome = true;
              }
            } else if (b == 1) {
              pagina = PAG_COERENZA; azzeraArmato = false; ridisegnaChrome = true;
            } else if (b == 2) {
              return;
            }
          } else {
            if (b == 0)      { clino_fit_reset(fit); }
            else if (b == 1) { pagina = PAG_ASSETTO; ridisegnaChrome = true; }
            else if (b == 2) { return; }
          }
        }
      } else {
        // Qualunque frame senza dita vale come rilascio, anche se il driver
        // marca il campione non valido: legare il rilascio a valid==true
        // significherebbe che dopo il primo tocco nessun altro viene accettato.
        ditoGiu = false;
      }

      // L'armamento di AZZERA scade da solo: se l'operatore si distrae, non
      // resta un pulsante armato in attesa di essere premuto per sbaglio.
      if (azzeraArmato && (now - azzeraArmatoMs > 4000)) {
        azzeraArmato = false; ridisegnaChrome = true;
      }
    }

    delay(CLINO_LOOP_MS);
  }
}
