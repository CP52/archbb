// ============================================================================
//  ArchBB 1.83 — touch_cal.cpp · calibrazione del touch · v1
//  Cesare Pagura · Padova/Noale IT · 17 luglio 2026
// ----------------------------------------------------------------------------
//  Vedi touch_cal.h per il razionale completo. Qui l'implementazione, adattata
//  al disegno DIRETTO su TFT (la 1.83 non usa sprite come la 1.69: niente
//  pushSprite, il disegno e' gia' a schermo).
// ============================================================================
#include "touch_cal.h"
#include "touch.h"
#include "display.h"
#include "config.h"
#include <math.h>

// ----------------------------------------------------------------------------
//  Punti bersaglio: 4 angoli + centro.
// ----------------------------------------------------------------------------
//  CAL_M = margine dai bordi. NON piccolo, e per due ragioni:
//  [1] il polpastrello/stilo non raggiunge l'angolo assoluto;
//  [2] soprattutto: su questo pannello il chip ESPANDE la X (~1.28x), quindi ai
//      bordi riporta valori FUORI SCALA (es. display~0 -> chip legge ~-28 ->
//      satura a 0; display~240 -> chip ~280 -> satura a 239). Un bersaglio
//      troppo al bordo verrebbe letto SATURATO, e il fit su un valore saturato
//      e' spazzatura. Tenendo i bersagli piu' INTERNI (margine 48) si resta
//      nella zona dove il chip riporta valori lineari e non clampati — che e'
//      esattamente dove il fit deve lavorare. Questa e' la lezione del debug dei
//      tasti: l'espansione si vede solo sui punti INTERNI, non ai bordi (dove il
//      clamp la nasconde).
static constexpr int16_t CAL_M = 48;

struct CalPoint { int16_t x, y; const char* name; };
static const CalPoint CAL_PTS[] = {
  { CAL_M,             CAL_M,             "alto-sx"  },
  { ARCHBB_W - CAL_M,  CAL_M,             "alto-dx"  },
  { ARCHBB_W / 2,      ARCHBB_H / 2,      "centro"   },
  { CAL_M,             ARCHBB_H - CAL_M,  "basso-sx" },
  { ARCHBB_W - CAL_M,  ARCHBB_H - CAL_M,  "basso-dx" },
};
static constexpr int N_CAL = 5;

// ----------------------------------------------------------------------------
//  Fit ai minimi quadrati: s = a*t + b
// ----------------------------------------------------------------------------
//  Con N=5 il sistema e' SOVRA-determinato (2 incognite, 5 equazioni): non
//  esiste soluzione esatta, e i minimi quadrati trovano quella che minimizza
//  l'errore complessivo. E' il punto: il RESIDUO che ne esce non e' un artefatto
//  del calcolo, e' una misura di quanto il modello lineare descriva la realta'.
//  Con 2 punti il residuo sarebbe zero per costruzione e non direbbe niente.
static bool fitLinear(const float* t, const float* s, int n, float& a, float& b) {
  float st = 0, ss = 0, stt = 0, sts = 0;
  for (int i = 0; i < n; i++) { st += t[i]; ss += s[i]; stt += t[i]*t[i]; sts += t[i]*s[i]; }
  const float den = n * stt - st * st;
  // den ~ 0: tutti i t sono (quasi) uguali. L'utente ha toccato 5 volte lo
  // stesso punto, oppure il chip riporta sempre lo stesso valore. In entrambi i
  // casi non c'e' nessuna retta da trovare: meglio fallire che inventare.
  if (fabsf(den) < 1e-6f) return false;
  a = (n * sts - st * ss) / den;
  b = (ss - a * st) / n;
  return true;
}

// snprintf("%f") su ESP32 non stampa i float (newlib nano): serve formattazione
// manuale a virgola fissa. E' una trappola gia' pagata sulla 1.69.
static void fmtFixed(char* out, size_t n, float v, int dec) {
  bool neg = (v < 0); if (neg) v = -v;
  long mult = 1; for (int i = 0; i < dec; i++) mult *= 10;
  long scaled = (long)(v * mult + 0.5f);
  long ip = scaled / mult, fp = scaled % mult;
  snprintf(out, n, "%s%ld.%0*ld", neg ? "-" : "", ip, dec, fp);
}

// ----------------------------------------------------------------------------
//  Disegno
// ----------------------------------------------------------------------------
static void drawCrosshair(TFT_eSPI& t, int16_t x, int16_t y, uint16_t col) {
  t.drawCircle(x, y, 14, col);
  t.drawCircle(x, y, 13, col);
  t.drawFastHLine(x - 20, y, 41, col);
  t.drawFastVLine(x, y - 20, 41, col);
  t.fillCircle(x, y, 3, col);
}

static void drawCalScreen(TFT_eSPI& t, int idx, int pass) {
  t.fillScreen(ArchColor::BG);
  t.setTextDatum(TC_DATUM);
  t.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  char h[32]; snprintf(h, sizeof(h), "CALIBRAZIONE  passata %d/2", pass);
  t.drawString(h, ARCHBB_W/2, 4, 2);

  t.setTextColor(ArchColor::ACCENT, ArchColor::BG);
  char p[24]; snprintf(p, sizeof(p), "punto %d/%d", idx + 1, N_CAL);
  t.drawString(p, ARCHBB_W/2, ARCHBB_H/2 - 42, 2);
  t.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  t.drawString("tocca il centro", ARCHBB_W/2, ARCHBB_H/2 + 26, 2);

  drawCrosshair(t, CAL_PTS[idx].x, CAL_PTS[idx].y, ArchColor::AMBER);
}

// ----------------------------------------------------------------------------
//  Raccolta di UNA passata: 5 punti, coordinate GREZZE.
// ----------------------------------------------------------------------------
//  Legge td.rawX/rawY, MAI td.x/td.y: dobbiamo misurare cosa dice il CHIP, non
//  cio' che il firmware ha gia' corretto (altrimenti calibreremmo la nostra
//  stessa correzione — un cane che si morde la coda, e un'altra strada per
//  fabbricare coefficienti che non descrivono l'hardware).
//
// ----------------------------------------------------------------------------
//  PERCHE' QUESTA FUNZIONE E' PIU' COMPLICATA DI "ASPETTA UN TOCCO"
// ----------------------------------------------------------------------------
//  Prima versione (sbagliata): attendi rilascio -> raccogli 4 campioni -> avanti.
//  Sul campo SALTAVA i punti (3 -> 5) e finiva con "non ripetibile".
//
//  Il motivo e' il CST816: allo stacco del dito produce micro-rimbalzi
//  valido/non-valido ravvicinati (lo stesso fenomeno per cui lo scoring ha il
//  debounce sul rilascio). Con 4 campioni a 12ms, bastavano ~48ms di rimbalzi
//  per "completare" da soli il punto successivo, registrando le coordinate del
//  dito che si stava ancora staccando dal punto PRECEDENTE.
//
//  Da qui i due sintomi, che erano lo stesso bug:
//    - il salto (un punto si auto-completava col rimbalzo del precedente);
//    - "punti inaffidabili" (due bersagli diversi con la stessa coordinata ->
//      il fit e' degenere e protesta, giustamente).
//
//  La cura ha tre parti, e servono tutte e tre:
//    [1] RILASCIO CONFERMATO: non basta un campione senza dito, ne servono N
//        consecutivi. E' la stessa tecnica del debounce dello scoring.
//    [2] TOCCO STABILE: si raccoglie solo dopo che il dito e' rimasto giu' per
//        N campioni consecutivi, e si scartano i primi (il transitorio di
//        appoggio, dove la coordinata sta ancora assestandosi).
//    [3] COERENZA: i campioni di un punto devono stare vicini fra loro. Se sono
//        sparsi, il dito si stava muovendo (o e' un rimbalzo): si BUTTA il punto
//        e lo si ripete, invece di infilare spazzatura nel fit.
// ----------------------------------------------------------------------------
static constexpr int  SAMPLES_N      = 5;    // campioni raccolti per punto (era 8: troppi con INT-gating -> "tocco breve")

// ----------------------------------------------------------------------------
//  PERCHE' NON C'E' PIU' UNA SOGLIA FISSA DI DISPERSIONE
// ----------------------------------------------------------------------------
//  v3 aveva MAX_SPREAD_PX=12, v4 l'ha portata a 25: due numeri INVENTATI, e
//  infatti Cesare con lo STILO si vedeva ancora rifiutare troppi punti. Il
//  problema e' che quelle sono unita' GREZZE del chip, e quanto valga un'unita'
//  grezza in pixel e' precisamente cio' che questa calibrazione sta misurando:
//  scegliere la soglia a priori e' circolare, e sbagliarla di poco significa
//  rifiutare tocchi buoni.
//
//  Cura: il filtro si TARA DA SOLO sui dati del punto stesso.
//    [1] si raccolgono SAMPLES_N campioni;
//    [2] si scarta l'outlier peggiore (il campione piu' lontano dalla mediana):
//        e' li' che si annidano il transitorio di appoggio e i rimbalzi;
//    [3] si accetta se i restanti sono coerenti FRA LORO in modo relativo,
//        cioe' entro una frazione della loro stessa dispersione tipica.
//
//  Cosi' non serve sapere in anticipo la scala del chip: si giudica il punto
//  con il metro del punto. Un tocco fermo produce campioni stretti qualunque sia
//  l'unita'; un trascinamento li produce larghi in qualunque unita'.
//
//  SOGLIA: relativa, non assoluta.
//  Un tetto in unita' grezze e' impossibile da scegliere senza sapere la scala
//  del chip (che e' cio' che stiamo misurando): 25 rifiutava tocchi fermi su
//  scala grande, 400 accettava trascinamenti veri su scala piccola. Sbagliato
//  il problema, non il numero.
//
//  Criterio scale-free: la dispersione di UN punto va confrontata con la
//  DISTANZA FRA I BERSAGLI misurata NELLE STESSE UNITA'. Quella distanza non la
//  conosciamo in anticipo... ma la conosciamo DOPO il primo paio di punti, ed e'
//  esattamente il numero che serve. Finche' non c'e', si accetta tutto (meglio
//  un punto sporco che una calibrazione che non parte): sara' il RESIDUO del fit
//  a bocciare l'insieme, ed e' il posto giusto per farlo.
//
//  SPREAD_FRAC = 0.15 -> un punto e' "mosso" se i suoi campioni ballano per piu'
//  del 15% della distanza tipica fra due bersagli. Un tocco fermo sta ordini di
//  grandezza sotto; un trascinamento da un bersaglio all'altro la supera di
//  slancio. La frazione e' adimensionale: vale per QUALUNQUE scala del chip.
static constexpr float SPREAD_FRAC = 0.15f;

// Riferimento di scala, misurato dai punti gia' acquisiti in questa passata.
// <0 = non ancora noto.
static float s_scaleRef = -1.0f;

// Mediana di un array corto (n<=8): sort a bolle, e' piu' che sufficiente qui.
static float medianOf(float* v, int n) {
  float a[SAMPLES_N];
  for (int i = 0; i < n; i++) a[i] = v[i];
  for (int i = 0; i < n - 1; i++)
    for (int j = 0; j < n - 1 - i; j++)
      if (a[j] > a[j+1]) { float t2 = a[j]; a[j] = a[j+1]; a[j+1] = t2; }
  return (n % 2) ? a[n/2] : (a[n/2 - 1] + a[n/2]) * 0.5f;
}

// Attende che il dito sia VERAMENTE staccato.
// ----------------------------------------------------------------------------
//  Attenzione allo stesso tranello della raccolta, qui in versione speculare:
//  col driver INT-gated i "buchi" (valid=false) capitano ANCHE col dito
//  appoggiato e fermo, perche' il chip non genera eventi in continuo. Contare
//  N buchi consecutivi direbbe "staccato" mentre il dito e' ancora giu'.
//
//  Il rilascio va quindi misurato A TEMPO: nessun evento di tocco per un
//  intervallo continuo (REL_QUIET_MS). Se arriva un tocco, il cronometro
//  riparte. Cosi' la condizione e' "silenzio prolungato", che e' cio' che il
//  rilascio effettivamente e'.
static constexpr uint32_t REL_QUIET_MS = 250;   // silenzio continuo = dito su (era 120: troppo severo col gating lento)

static bool waitRelease(uint32_t t0, uint32_t timeout_ms) {
  uint32_t quietFrom = millis();
  while (millis() - quietFrom < REL_QUIET_MS) {
    TouchData td = touchRead();
    if (td.valid && td.fingers > 0) quietFrom = millis();   // ancora giu'
    if (millis() - t0 > timeout_ms) return false;
    delay(8);
  }
  return true;
}

// Raccoglie UN punto. Ritorna false su timeout; *ok=false se il punto va
// ripetuto (campioni incoerenti).
// ----------------------------------------------------------------------------
//  ERRORE DELLA v3 (mai piu'): la raccolta pretendeva SAMPLES_N campioni
//  CONSECUTIVI tutti validi, e al primo campione non valido buttava il punto.
//
//  Ma il driver e' GATED DALL'INT: touchRead() legge l'I2C solo quando il chip
//  segnala un evento, e restituisce valid=false in mezzo. Con lo stilo (o il
//  dito) FERMO il CST816 non genera eventi in continuo — quindi "N campioni
//  consecutivi validi" e' una condizione che l'hardware non puo' soddisfare per
//  costruzione. Il punto non si chiudeva MAI, restava ambra, e ogni tanto
//  compariva "tocco mosso": il filtro pretendeva l'impossibile.
//
//  Cesare toccava con uno STILO: se anche cosi' risultava "mosso", il problema
//  non era il tocco. Quando un filtro "intelligente" produce comportamento
//  strano, il sospetto va al filtro, non a un nuovo fenomeno fisico.
//
//  Logica corretta: si contano i campioni VALIDI dentro una finestra di tempo,
//  tollerando i buchi. Il dito e' considerato staccato solo dopo un numero di
//  buchi CONSECUTIVI (che e' un fatto diverso da "un buco").
// ----------------------------------------------------------------------------
static bool collectOnePoint(TFT_eSPI& t, int idx, int pass,
                            float& outX, float& outY,
                            uint32_t t0, uint32_t timeout_ms, bool& ok,
                            float& outSpread) {
  ok = false;
  outSpread = -1.0f;
  float bx[SAMPLES_N], by[SAMPLES_N];

  // [1] il dito del punto precedente deve essere staccato per davvero
  if (!waitRelease(t0, timeout_ms)) return false;

  // ora si puo' disegnare la mira nuova: prima sarebbe stato inutile, il dito
  // era ancora sullo schermo
  drawCalScreen(t, idx, pass);

  // [2] attende il PRIMO tocco su questo bersaglio (un solo campione valido:
  // e' il segnale che il dito e' arrivato). Non serve la conferma qui — la
  // stabilita' la giudichiamo dopo, sulla dispersione dei campioni raccolti,
  // che e' una misura vera e non un'ipotesi sul numero di eventi.
  while (true) {
    TouchData td = touchRead();
    if (td.valid && td.fingers > 0 && td.coordsFresh) break;
    if (millis() - t0 > timeout_ms) return false;
    delay(8);
  }

  // feedback: da qui sto misurando davvero
  drawCrosshair(t, CAL_PTS[idx].x, CAL_PTS[idx].y, ArchColor::GOOD);

  // scarta il transitorio di appoggio: i primissimi campioni di un tocco hanno
  // la coordinata ancora in assestamento (il polpastrello/stilo si appoggia).
  uint32_t tSettle = millis();
  while (millis() - tSettle < 24) {   // era 60ms: con l'INT-gating buttava via
    touchRead();                       // troppi eventi utili e il tocco naturale
    delay(8);                          // non bastava a raccogliere i campioni
  }

  // [3] raccolta: N campioni VALIDI, tollerando i buchi dell'INT-gating.
  // Anche qui il "dito alzato" si misura A TEMPO (silenzio continuo), non
  // contando buchi: coi buchi normali anche a dito fermo, un contatore direbbe
  // "alzato" mentre lo stilo e' ancora appoggiato. Stesso tranello di
  // waitRelease, e stessa cura.
  static constexpr uint32_t GATHER_MAX_MS = 4000;  // tempo max per un punto
  int n = 0;
  const uint32_t tg = millis();
  uint32_t lastSeen = millis();          // ultimo campione valido visto
  while (n < SAMPLES_N) {
    TouchData td = touchRead();
    if (td.valid && td.fingers > 0 && td.coordsFresh) {
      bx[n] = td.rawX; by[n] = td.rawY; n++;
      lastSeen = millis();
    } else if (millis() - lastSeen > REL_QUIET_MS) {
      // silenzio prolungato: il dito si e' alzato prima di aver raccolto
      // abbastanza campioni -> punto da ripetere (non e' un errore dell'utente,
      // gli si chiede solo di tenere appoggiato un istante in piu').
      return true;   // ok resta false
    }
    if (millis() - tg > GATHER_MAX_MS) return true;   // ok=false: ripeti
    if (millis() - t0 > timeout_ms)    return false;  // timeout del punto
    delay(8);
  }

  // [4] COERENZA — con il metro del punto stesso, non con una soglia inventata.
  // Si prende la MEDIANA (robusta: un outlier non la sposta), si scarta il
  // campione piu' lontano da essa, e si media il resto. Il transitorio di
  // appoggio e i rimbalzi finiscono quasi sempre in quell'unico outlier.
  float mx = medianOf(bx, SAMPLES_N);
  float my = medianOf(by, SAMPLES_N);

  int worst = 0; float worstD = -1.0f;
  for (int i = 0; i < SAMPLES_N; i++) {
    float d = fabsf(bx[i] - mx) + fabsf(by[i] - my);   // distanza L1 dalla mediana
    if (d > worstD) { worstD = d; worst = i; }
  }

  float sx = 0, sy = 0; int n2 = 0;
  float mnx = 1e9f, mxx = -1e9f, mny = 1e9f, mxy = -1e9f;
  for (int i = 0; i < SAMPLES_N; i++) {
    if (i == worst) continue;            // scarta l'outlier peggiore
    sx += bx[i]; sy += by[i]; n2++;
    if (bx[i] < mnx) mnx = bx[i];
    if (bx[i] > mxx) mxx = bx[i];
    if (by[i] < mny) mny = by[i];
    if (by[i] > mxy) mxy = by[i];
  }

  // Unico rifiuto: il caso PATOLOGICO (trascinamento vero e proprio). La soglia
  // e' cosi' larga da non poter mai colpire un tocco fermo, in nessuna scala
  // plausibile del chip. Se scatta, e' un dato reale — non un filtro pignolo.
  outSpread = fmaxf(mxx - mnx, mxy - mny);   // per mostrarlo se rifiutiamo

  // RILEVAMENTO SATURAZIONE: se il grezzo di questo punto e' incollato a 0 o al
  // fondo scala nominale, il chip sta CLAMPando (il bersaglio cade fuori dalla
  // zona lineare del sensore). Un fit su un valore saturato e' spazzatura: si
  // rifiuta il punto con un messaggio dedicato, cosi' l'utente sa che deve
  // toccare piu' verso l'interno invece di credere di aver toccato male.
  {
    float mcx = medianOf(bx, SAMPLES_N), mcy = medianOf(by, SAMPLES_N);
    bool satX = (mcx <= 1.0f) || (mcx >= (ARCHBB_W - 2));
    bool satY = (mcy <= 1.0f) || (mcy >= (ARCHBB_H + 40));   // Y raramente satura
    if (satX || satY) {
      outSpread = -2.0f;   // codice speciale: saturazione, non "mosso"
      return true;         // ok=false
    }
  }

  // Giudizio RELATIVO: si rifiuta solo se la dispersione supera una frazione
  // della distanza tipica fra bersagli, misurata nelle stesse unita' del chip.
  // Se la scala non e' ancora nota (primi punti), si accetta: sara' il residuo
  // del fit a fare da giudice finale.
  if (s_scaleRef > 0.0f && outSpread > s_scaleRef * SPREAD_FRAC) {
    return true;   // ok resta false
  }

  outX = sx / n2;
  outY = sy / n2;
  ok = true;
  return true;
}

static bool collectPass(TFT_eSPI& t, float* tx, float* ty, int pass,
                        uint32_t timeout_ms) {
  s_scaleRef = -1.0f;    // ogni passata rimisura la propria scala
  for (int i = 0; i < N_CAL; i++) {
    bool ok = false;
    // TIMEOUT PER PUNTO, non per passata: ora che ripetere un punto e' normale
    // (e' il meccanismo che protegge il fit), un budget globale verrebbe eroso
    // dalle ripetizioni legittime e la calibrazione morirebbe a meta' per un
    // motivo che con l'utente non c'entra nulla. Ogni punto ha il suo tempo.
    const uint32_t t0 = millis();
    // Ripete finche' il punto non e' pulito. Non c'e' fretta: un punto sbagliato
    // qui avvelena tutta la calibrazione, e ripeterlo costa un secondo.
    while (!ok) {
      float spread = -1.0f;
      if (!collectOnePoint(t, i, pass, tx[i], ty[i], t0, timeout_ms, ok, spread))
        return false;    // timeout DEL PUNTO (nessun tocco per 45s: touch rotto?)
      // NIENTE limite di tentativi. Come il DIAG, CALIBRA e' una modalita' in
      // cui SI RESTA finche' non riesce (o si spegne). Un punto rifiutato si
      // RIPETE all'infinito: sei qui apposta per calibrare, un "quasi" non deve
      // buttarti fuori nello scoring. (Prima c'era attempts>=12 che, sommato al
      // "tocco breve" ripetuto, faceva USCIRE la calibrazione dal terzo punto.)
      if (!ok) {
        t.setTextDatum(BC_DATUM);
        t.setTextColor(ArchColor::AMBER, ArchColor::BG);
        char m[40];
        if (spread <= -2.0f)     snprintf(m, sizeof(m), "bordo saturo - tocca piu' al centro");
        else if (spread < 0)     snprintf(m, sizeof(m), "tieni premuto un istante in piu'");
        else                     snprintf(m, sizeof(m), "mosso (%d) - ritocca fermo", (int)spread);
        t.drawString(m, ARCHBB_W/2, ARCHBB_H - 6, 2);
        delay(600);
      }
    }
    // Stima della SCALA del chip dai punti gia' acquisiti: appena ne abbiamo
    // due, la loro distanza in unita' grezze ci dice quanto "vale" lo schermo
    // in quelle unita' — il metro che mancava. Da li' in poi il filtro puo'
    // giudicare in modo relativo. E' un numero MISURATO, non presunto.
    if (i >= 1) {
      float dx = tx[i] - tx[0], dy = ty[i] - ty[0];
      float d  = sqrtf(dx*dx + dy*dy);
      // distanza fra gli stessi due bersagli in pixel-display (nota per
      // costruzione): il rapporto e' la scala grezzo/pixel.
      float ddx = (float)(CAL_PTS[i].x - CAL_PTS[0].x);
      float ddy = (float)(CAL_PTS[i].y - CAL_PTS[0].y);
      float dpix = sqrtf(ddx*ddx + ddy*ddy);
      if (dpix > 20.0f && d > 1.0f) {
        // scala di riferimento = quanto vale, in unita' grezze, l'intera
        // diagonale utile. E' la grandezza con cui confrontare la dispersione.
        s_scaleRef = d * (280.0f / dpix);
      }
    }

    // conferma visiva del punto acquisito
    drawCrosshair(t, CAL_PTS[i].x, CAL_PTS[i].y, ArchColor::GOOD);
    t.setTextDatum(BC_DATUM);
    t.setTextColor(ArchColor::GOOD, ArchColor::BG);
    t.drawString("ok - stacca il dito", ARCHBB_W/2, ARCHBB_H - 6, 2);
  }
  // l'ultimo punto: il dito deve staccarsi prima di passare alla schermata dopo
  waitRelease(millis(), timeout_ms);
  return true;
}

// ----------------------------------------------------------------------------
//  Risultato a display
// ----------------------------------------------------------------------------
static void drawResult(TFT_eSPI& t, const TouchCalResult& r) {
  t.fillScreen(ArchColor::BG);
  t.setTextDatum(TC_DATUM);

  if (!r.valid) {
    t.setTextColor(ArchColor::BAD, ArchColor::BG);
    t.drawString("CALIBRAZIONE", ARCHBB_W/2, 40, 4);
    t.drawString("FALLITA", ARCHBB_W/2, 74, 4);
    t.setTextColor(ArchColor::TEXT3, ArchColor::BG);
    t.drawString("nessuna modifica applicata", ARCHBB_W/2, 130, 2);
    t.drawString("tocca per uscire", ARCHBB_W/2, ARCHBB_H - 30, 2);
    return;
  }

  t.setTextColor(ArchColor::ACCENT, ArchColor::BG);
  t.drawString("COEFFICIENTI", ARCHBB_W/2, 6, 2);

  char b[40], c1[16], c2[16];
  t.setTextDatum(TL_DATUM);
  int yy = 30;

  fmtFixed(c1, sizeof(c1), r.ax, 5); fmtFixed(c2, sizeof(c2), r.bx, 2);
  t.setTextColor(ArchColor::TEXT, ArchColor::BG);
  snprintf(b, sizeof(b), "AX %s", c1); t.drawString(b, 10, yy, 2); yy += 20;
  snprintf(b, sizeof(b), "BX %s", c2); t.drawString(b, 10, yy, 2); yy += 24;

  fmtFixed(c1, sizeof(c1), r.ay, 5); fmtFixed(c2, sizeof(c2), r.by, 2);
  snprintf(b, sizeof(b), "AY %s", c1); t.drawString(b, 10, yy, 2); yy += 20;
  snprintf(b, sizeof(b), "BY %s", c2); t.drawString(b, 10, yy, 2); yy += 26;

  // Residuo: quanto il modello lineare descrive i 5 punti di UNA passata.
  fmtFixed(c1, sizeof(c1), r.err_max, 1); fmtFixed(c2, sizeof(c2), r.err_rms, 1);
  t.setTextColor(r.err_max > 8.0f ? ArchColor::BAD : ArchColor::TEXT3, ArchColor::BG);
  snprintf(b, sizeof(b), "residuo max %s rms %s px", c1, c2);
  t.drawString(b, 10, yy, 2); yy += 24;

  // Ripetibilita': se DUE passate danno la stessa retta. E' il controllo che
  // sulla 1.69 avrebbe smascherato subito i coefficienti fantasma.
  fmtFixed(c1, sizeof(c1), r.spread_ax, 1); fmtFixed(c2, sizeof(c2), r.spread_ay, 1);
  t.setTextColor(r.repeatable ? ArchColor::GOOD : ArchColor::BAD, ArchColor::BG);
  snprintf(b, sizeof(b), "spread ax %s%% ay %s%%", c1, c2);
  t.drawString(b, 10, yy, 2); yy += 20;

  t.setTextDatum(TC_DATUM);
  if (!r.repeatable) {
    t.setTextColor(ArchColor::BAD, ArchColor::BG);
    t.drawString("NON RIPETIBILE: non usare", ARCHBB_W/2, yy, 2); yy += 18;
    t.setTextColor(ArchColor::TEXT3, ArchColor::BG);
    t.drawString("ripeti toccando piu' preciso", ARCHBB_W/2, yy, 2);
  } else {
    t.setTextColor(ArchColor::GOOD, ArchColor::BG);
    t.drawString("OK: ricopia in config.h", ARCHBB_W/2, yy, 2);
  }
  t.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  t.drawString("tocca per uscire", ARCHBB_W/2, ARCHBB_H - 22, 2);
}

// ----------------------------------------------------------------------------
//  touch_cal_run — due passate + fit + confronto
// ----------------------------------------------------------------------------
TouchCalResult touch_cal_run(TFT_eSPI& t, uint32_t timeout_ms) {
  TouchCalResult r = {};
  r.valid = false;

  float tx1[N_CAL], ty1[N_CAL], tx2[N_CAL], ty2[N_CAL];
  float sx[N_CAL], sy[N_CAL];
  for (int i = 0; i < N_CAL; i++) { sx[i] = CAL_PTS[i].x; sy[i] = CAL_PTS[i].y; }

  if (!collectPass(t, tx1, ty1, 1, timeout_ms)) { drawResult(t, r); return r; }
  if (!collectPass(t, tx2, ty2, 2, timeout_ms)) { drawResult(t, r); return r; }

  // fit indipendente per ciascuna passata
  float ax1, bx1, ay1, by1, ax2, bx2, ay2, by2;
  bool ok = fitLinear(tx1, sx, N_CAL, ax1, bx1) &&
            fitLinear(ty1, sy, N_CAL, ay1, by1) &&
            fitLinear(tx2, sx, N_CAL, ax2, bx2) &&
            fitLinear(ty2, sy, N_CAL, ay2, by2);
  if (!ok) { drawResult(t, r); return r; }

  // spread fra le due passate: la RIPETIBILITA'.
  r.spread_ax = fabsf(ax1 - ax2) / fmaxf(fabsf(ax1), 1e-6f) * 100.0f;
  r.spread_ay = fabsf(ay1 - ay2) / fmaxf(fabsf(ay1), 1e-6f) * 100.0f;
  // Soglia 5%, non 3%: per un tocco a MANO LIBERA con lo stilo, due passate che
  // concordano entro il 5% sono ottime. Il 3% originale bocciava calibrazioni
  // valide per un decimo di percento (spread 3.1% -> "non ripetibile" mentre i
  // coefficienti erano perfettamente usabili sui tasti). La ripetibilita' serve
  // a scartare le calibrazioni FATTE MALE, non a inseguire una precisione da
  // banco ottico che il dito umano non puo' dare.
  r.repeatable = (r.spread_ax <= 5.0f && r.spread_ay <= 5.0f);

  // coefficienti finali: media delle due passate (se ripetibili, sono quasi
  // uguali e la media riduce il rumore; se non lo sono, r.repeatable=false
  // avvisa di NON usarli).
  r.ax = (ax1 + ax2) * 0.5f;  r.bx = (bx1 + bx2) * 0.5f;
  r.ay = (ay1 + ay2) * 0.5f;  r.by = (by1 + by2) * 0.5f;

  // residuo sui 10 punti complessivi, con i coefficienti finali
  float emax = 0, esum = 0; int n = 0;
  for (int i = 0; i < N_CAL; i++) {
    float ex1 = fabsf(r.ax * tx1[i] + r.bx - sx[i]);
    float ey1 = fabsf(r.ay * ty1[i] + r.by - sy[i]);
    float ex2 = fabsf(r.ax * tx2[i] + r.bx - sx[i]);
    float ey2 = fabsf(r.ay * ty2[i] + r.by - sy[i]);
    float es[4] = {ex1, ey1, ex2, ey2};
    for (int k = 0; k < 4; k++) {
      if (es[k] > emax) emax = es[k];
      esum += es[k] * es[k]; n++;
    }
  }
  r.err_max = emax;
  r.err_rms = sqrtf(esum / n);
  r.valid   = true;

  drawResult(t, r);

  // attende un tocco per uscire (con timeout: mai bloccare il dispositivo)
  uint32_t t0 = millis();
  while (millis() - t0 < timeout_ms) {
    TouchData td = touchRead();
    if (td.valid && td.fingers > 0) { delay(300); break; }
    delay(20);
  }
  return r;
}

// ----------------------------------------------------------------------------
//  touch_cal_diag — mostra i NUMERI, non le ipotesi
// ----------------------------------------------------------------------------
//  ERRORE DELLE VERSIONI PRECEDENTI (sintomo: "flikera e mostra tutti zero"):
//
//  [1] TUTTI ZERO — il ciclo faceva UNA touchRead() e poi spendeva ~60ms a
//      ridisegnare. Ma il driver e' INT-GATED: nella stragrande maggioranza dei
//      cicli touchRead() ritorna valid=false con rawX=rawY=0, e il DIAG
//      stampava PROPRIO QUEL campione. Mostrava i buchi, non i tocchi.
//      E' lo stesso tranello dell'INT-gating gia' corretto nella raccolta: qui
//      era rimasto. Cura: si fa POLLING FITTO e si LATCHA l'ultimo campione
//      valido; a schermo si mostra quello, non il vuoto fra un evento e l'altro.
//
//  [2] FLICKER — fillScreen() a ogni ciclo, con disegno diretto sul TFT (la 1.83
//      non ha sprite): lo schermo si svuota e si riempie sotto gli occhi.
//      Cura: si ridisegna SOLO quando i valori cambiano, e si cancellano le
//      singole righe invece dell'intero schermo.
//
//  La lezione e' la stessa gia' pagata due volte in questo modulo: con un driver
//  a eventi, "l'ultimo dato letto" e "il dato adesso" sono cose diverse.
//  [3] USCITA — non c'e'. Il DIAG e' un CAPOLINEA: si esce spegnendo.
//      v6 usciva dopo 2s di pressione continua, ma per leggere gli span bisogna
//      TENERE PREMUTO negli angoli: il gesto di misura ERA il gesto di uscita.
//      Il DIAG si autosabotava, e Cesare non faceva in tempo a trascrivere i
//      numeri. Qualunque gesto d'uscita entrerebbe in conflitto con la misura,
//      quindi la scelta giusta e' non averne nessuno: chi entra qui vuole
//      leggere con calma, e riavviare costa un secondo.
[[noreturn]] void touch_cal_diag(TFT_eSPI& t) {
  uint16_t rxmin = 0xFFFF, rxmax = 0, rymin = 0xFFFF, rymax = 0;
  uint32_t lastSeen = 0;          // quando abbiamo visto l'ultimo tocco valido

  // ULTIMO campione valido (latch): e' cio' che mostriamo.
  uint16_t lrx = 0, lry = 0, lx = 0, ly = 0;
  bool haveSample = false;

  // stato disegnato, per ridisegnare solo al cambio
  uint16_t drx = 0xFFFF, dry = 0xFFFF;
  uint16_t dspanx0 = 0xFFFF, dspanx1 = 0xFFFF, dspany0 = 0xFFFF, dspany1 = 0xFFFF;
  bool firstDraw = true;
  int16_t lastDotX = -1, lastDotY = -1;
  uint32_t nSamples = 0;          // quanti campioni validi visti in totale
  uint32_t dSamples = 0xFFFFFFFF;

  const int Y_RAW = 30, Y_COR = 52, Y_COEF = 80, Y_SPAN = 116,
            Y_SCALE = 148, Y_WARN = 168;

  for (;;) {
    // --- POLLING FITTO: piu' letture per ciclo di disegno -------------------
    // E' il punto: con l'INT-gating gli eventi sono sporadici, e leggere una
    // volta ogni 60ms significa quasi sempre leggere il vuoto.
    for (int k = 0; k < 12; k++) {
      TouchData td = touchRead();
      if (td.valid && td.fingers > 0 && td.coordsFresh) {
        lrx = td.rawX; lry = td.rawY; lx = td.x; ly = td.y;
        haveSample = true;
        nSamples++;
        lastSeen = millis();
        if (td.rawX < rxmin) rxmin = td.rawX;
        if (td.rawX > rxmax) rxmax = td.rawX;
        if (td.rawY < rymin) rymin = td.rawY;
        if (td.rawY > rymax) rymax = td.rawY;
      }
      delay(4);
    }

    // "premuto" = visto un tocco di recente (a tempo, non a campioni: stesso
    // motivo di waitRelease).
    bool pressed = haveSample && (millis() - lastSeen < 150);

    // NIENTE USCITA. Vedi il commento in testa alla funzione: il DIAG e' un
    // capolinea, si esce spegnendo. Qualunque gesto d'uscita entrerebbe in
    // conflitto col gesto di misura.

    // --- DISEGNO: solo cio' che e' cambiato --------------------------------
    if (firstDraw) {
      t.fillScreen(ArchColor::BG);
      t.setTextDatum(TC_DATUM);
      t.setTextColor(ArchColor::ACCENT, ArchColor::BG);
      t.drawString("DIAG TOUCH", ARCHBB_W/2, 4, 2);

      // i coefficienti attivi NEL BINARIO: sono costanti, si disegnano una volta
      char b[40], c1[16], c2[16];
      t.setTextDatum(TL_DATUM);
      t.setTextColor(ArchColor::TEXT3, ArchColor::BG);
      fmtFixed(c1, sizeof(c1), TOUCH_CAL_AX, 5); fmtFixed(c2, sizeof(c2), TOUCH_CAL_BX, 2);
      snprintf(b, sizeof(b), "AX %s  BX %s", c1, c2);
      t.drawString(b, 10, Y_COEF, 1);
      fmtFixed(c1, sizeof(c1), TOUCH_CAL_AY, 5); fmtFixed(c2, sizeof(c2), TOUCH_CAL_BY, 2);
      snprintf(b, sizeof(b), "AY %s  BY %s", c1, c2);
      t.drawString(b, 10, Y_COEF + 14, 1);

      t.setTextColor(ArchColor::BAD, ArchColor::BG);
      t.drawString("(span = dove tocchi, NON il chip)", 10, Y_WARN, 1);

      // Bordi delle ZONE dei tasti DIST+ELEV, visibili subito: cosi' sai DOVE
      // sono ancora prima di toccarle. Tocca dentro un rettangolo e verifica
      // che si accenda: se tocchi il "+" e si accende "PIU", il touch e la zona
      // combaciano e il problema (se resta) e' altrove nello scoring.
      t.drawRect(16, 195, 48, 32, ArchColor::TEXT3);
      t.drawRect(92, 195, 56, 32, ArchColor::TEXT3);
      t.drawRect(176, 195, 48, 32, ArchColor::TEXT3);
      t.setTextDatum(TC_DATUM);
      t.setTextColor(ArchColor::TEXT3, ArchColor::BG);
      t.drawString("-", 40, 232, 1);
      t.drawString("?", 120, 232, 1);
      t.drawString("+", 200, 232, 1);

      t.setTextDatum(BC_DATUM);
      t.setTextColor(ArchColor::TEXT3, ArchColor::BG);
      t.drawString("tocca i 4 angoli - spegni per uscire", ARCHBB_W/2, ARCHBB_H - 4, 1);
      firstDraw = false;
    }

    char b[40];
    t.setTextDatum(TL_DATUM);

    if (lrx != drx || lry != dry) {
      t.fillRect(0, Y_RAW - 2, ARCHBB_W, 44, ArchColor::BG);   // pulisci 2 righe
      t.setTextColor(haveSample ? ArchColor::TEXT : ArchColor::TEXT3, ArchColor::BG);
      if (haveSample) snprintf(b, sizeof(b), "grezzo   %4u , %4u", lrx, lry);
      else            snprintf(b, sizeof(b), "grezzo    --- , ---");
      t.drawString(b, 10, Y_RAW, 2);
      if (haveSample) snprintf(b, sizeof(b), "corretto %4u , %4u", lx, ly);
      else            snprintf(b, sizeof(b), "corretto  --- , ---");
      t.drawString(b, 10, Y_COR, 2);
      drx = lrx; dry = lry;
    }

    uint16_t sx0 = (rxmin == 0xFFFF) ? 0 : rxmin, sy0 = (rymin == 0xFFFF) ? 0 : rymin;
    if (sx0 != dspanx0 || rxmax != dspanx1 || sy0 != dspany0 || rymax != dspany1) {
      t.fillRect(0, Y_SPAN - 2, ARCHBB_W, 30, ArchColor::BG);
      t.setTextColor(ArchColor::AMBER, ArchColor::BG);
      snprintf(b, sizeof(b), "span rx %u..%u  (%u)", sx0, rxmax,
               (unsigned)(rxmax > sx0 ? rxmax - sx0 : 0));
      t.drawString(b, 10, Y_SPAN, 1);
      snprintf(b, sizeof(b), "span ry %u..%u  (%u)", sy0, rymax,
               (unsigned)(rymax > sy0 ? rymax - sy0 : 0));
      t.drawString(b, 10, Y_SPAN + 14, 1);
      dspanx0 = sx0; dspanx1 = rxmax; dspany0 = sy0; dspany1 = rymax;

      // FONDO SCALA STIMATO: e' IL numero che cerchiamo. Se dopo aver toccato i
      // 4 angoli lo span X vale ~240, il chip lavora gia' in pixel-display; se
      // vale molto di piu' (es. ~4095) la scala e' un'altra, ed e' la ragione
      // per cui i tasti da 48px mancavano.
      // NB: resta vero che lo span misura DOVE TOCCHI, non il chip. Serve
      // proprio per questo: ecco perche' l'istruzione e' "tocca i 4 ANGOLI",
      // cosi' lo span si avvicina al fondo scala vero. Non e' un dato da cui
      // ricavare coefficienti — quelli li da' solo CALIBRA.
      t.fillRect(0, Y_SCALE - 2, ARCHBB_W, 16, ArchColor::BG);
      t.setTextColor(ArchColor::ACCENT, ArchColor::BG);
      unsigned spx = (rxmax > sx0) ? rxmax - sx0 : 0;
      if (spx == 0)            snprintf(b, sizeof(b), "scala: tocca i 4 angoli");
      else if (spx < 300)      snprintf(b, sizeof(b), "scala X ~%u  (tipo display)", spx);
      else                     snprintf(b, sizeof(b), "scala X ~%u  (NON display!)", spx);
      t.drawString(b, 10, Y_SCALE, 1);
    }

    // contatore campioni: prova che il polling sta leggendo davvero (se resta a
    // 0 con il dito appoggiato, il problema e' il driver, non la grafica)
    if (nSamples != dSamples) {
      t.fillRect(ARCHBB_W - 70, 4, 66, 12, ArchColor::BG);
      t.setTextDatum(TR_DATUM);
      t.setTextColor(ArchColor::TEXT3, ArchColor::BG);
      snprintf(b, sizeof(b), "n=%lu", (unsigned long)nSamples);
      t.drawString(b, ARCHBB_W - 6, 4, 1);
      t.setTextDatum(TL_DATUM);
      dSamples = nSamples;
    }

    // pallino sotto il dito (coordinate CORRETTE): cancella il precedente invece
    // di ripulire tutto lo schermo.
    if (pressed) {
      if (lastDotX >= 0 && (lastDotX != (int16_t)lx || lastDotY != (int16_t)ly))
        t.fillCircle(lastDotX, lastDotY, 7, ArchColor::BG);
      t.fillCircle(lx, ly, 6, ArchColor::GOOD);
      lastDotX = lx; lastDotY = ly;

      // ZONE REALI dei tasti DIST+ELEV, sovrapposte: e' la richiesta esplicita
      // ("con diag si fa poco perche' non si sa dove cadano quei tasti").
      // Sono ESATTAMENTE le stesse costanti dell'handler (importate da scoring),
      // cosi' vedi il pallino cadere DENTRO o FUORI dal tasto e la questione si
      // chiude a vista, senza dedurre. Il tasto in cui cade il dito si accende.
      auto zone = [&](int x0,int x1,int y0,int y1,uint16_t col,const char* nm){
        bool in = (lx>=x0 && lx<=x1 && ly>=y0 && ly<=y1);
        t.drawRect(x0, y0, x1-x0, y1-y0, in ? ArchColor::GOOD : col);
        if (in) {
          t.setTextDatum(MC_DATUM);
          t.setTextColor(ArchColor::GOOD, ArchColor::BG);
          t.drawString(nm, ARCHBB_W/2, 200, 4);
        }
      };
      // KEY_Y=195..227, tasti X: -[16..64] ?[92..148] +[176..224]
      zone(16, 64, 195, 227, ArchColor::TEXT3, "MENO");
      zone(92, 148, 195, 227, ArchColor::TEXT3, "IGN");
      zone(176, 224, 195, 227, ArchColor::TEXT3, "PIU");
    } else if (lastDotX >= 0) {
      t.fillCircle(lastDotX, lastDotY, 7, ArchColor::BG);
      lastDotX = lastDotY = -1;
      // ripulisce l'etichetta del tasto e ridisegna i bordi spenti
      t.fillRect(0, 190, ARCHBB_W, 44, ArchColor::BG);
      t.drawRect(16, 195, 48, 32, ArchColor::TEXT3);
      t.drawRect(92, 195, 56, 32, ArchColor::TEXT3);
      t.drawRect(176, 195, 48, 32, ArchColor::TEXT3);
    }
  }
}

// ----------------------------------------------------------------------------
//  touch_cal_offer — schermata d'ingresso, OPT-IN
// ----------------------------------------------------------------------------
//  Default = SALTA. La calibrazione e' un'operazione da fare una volta, non a
//  ogni accensione: chi non tocca nulla deve andare avanti come sempre.
int touch_cal_offer(TFT_eSPI& t, uint32_t wait_ms) {
  // Quattro voci ora (Fase 6: +CONFIG): card 38px con gap 8 per starci tutte e
  // quattro nei 284px del pannello. Restano larghe (200px): reggono qualunque
  // errore di touch plausibile.
  const int16_t bw = ARCHBB_W - 40, bh = 38;
  const int16_t calY  = 70;
  const int16_t diagY = calY  + bh + 8;    // DIAG touch
  const int16_t angY  = diagY + bh + 8;    // CLINOMETRO (Fase 18)
  const int16_t cfgY  = angY  + bh + 8;    // CONFIG (Fase 6)

  t.fillScreen(ArchColor::BG);
  t.setTextDatum(TC_DATUM);
  t.setTextColor(ArchColor::ACCENT, ArchColor::BG);
  t.drawString("ArchBB", ARCHBB_W/2, 14, 4);
  t.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  t.drawString("menu di avvio", ARCHBB_W/2, 48, 2);

  // CALIBRA (touch) — ciano
  t.fillRoundRect(20, calY, bw, bh, 8, ArchColor::BG_CARD);
  t.drawRoundRect(20, calY, bw, bh, 8, ArchColor::ACCENT);
  t.setTextColor(ArchColor::ACCENT, ArchColor::BG_CARD);
  t.setTextDatum(MC_DATUM);
  t.drawString("CALIBRA touch", ARCHBB_W/2, calY + bh/2, 2);

  // DIAG touch — ambra
  t.fillRoundRect(20, diagY, bw, bh, 8, ArchColor::BG_CARD);
  t.drawRoundRect(20, diagY, bw, bh, 8, ArchColor::AMBER);
  t.setTextColor(ArchColor::AMBER, ArchColor::BG_CARD);
  t.drawString("DIAG touch", ARCHBB_W/2, diagY + bh/2, 2);

  // CLINOMETRO — verde (Fase 18: assetto assoluto + coerenza gyro/accel)
  t.fillRoundRect(20, angY, bw, bh, 8, ArchColor::BG_CARD);
  t.drawRoundRect(20, angY, bw, bh, 8, ArchColor::GOOD);
  t.setTextColor(ArchColor::GOOD, ArchColor::BG_CARD);
  t.drawString("CLINOMETRO", ARCHBB_W/2, angY + bh/2, 2);

  // CONFIG — viola/testo (Fase 6: lettura parametri NVS + ripristina default)
  t.fillRoundRect(20, cfgY, bw, bh, 8, ArchColor::BG_CARD);
  t.drawRoundRect(20, cfgY, bw, bh, 8, ArchColor::TEXT2);
  t.setTextColor(ArchColor::TEXT, ArchColor::BG_CARD);
  t.drawString("CONFIG", ARCHBB_W/2, cfgY + bh/2, 2);

  t.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  t.setTextDatum(TC_DATUM);
  t.drawString("non toccare = salta", ARCHBB_W/2, cfgY + bh + 10, 2);

  const uint32_t t0 = millis();
  while (millis() - t0 < wait_ms) {
    TouchData td = touchRead();
    if (td.valid && td.fingers > 0 && td.coordsFresh) {
      if (td.y >= calY  && td.y < calY  + bh) { delay(250); return 1; }
      if (td.y >= diagY && td.y < diagY + bh) { delay(250); return 2; }
      if (td.y >= angY  && td.y < angY  + bh) { delay(250); return 3; }
      if (td.y >= cfgY  && td.y < cfgY  + bh) { delay(250); return 4; }
    }
    delay(20);
  }
  return 0;
}
