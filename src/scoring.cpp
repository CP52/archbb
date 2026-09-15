// ============================================================================
//  ArchBB 1.83 — scoring.cpp · FASE 3 (porting scoring on-device) · v3
//  Cesare Pagura · Padova/Noale IT · 11 luglio 2026
// ----------------------------------------------------------------------------
//  CORREZIONI v3 (feedback di campo):
//   1) Azione "nervosa" -> DEBOUNCE + COOLDOWN. Il rilascio deve essere stabile
//      per N campioni consecutivi prima di contare come tap (elimina i micro-
//      rimbalzi valido/non-valido del CST816 allo stacco del dito), e un
//      cooldown minimo tra due azioni impedisce il doppio-scatto. Reattivo ma
//      stabile (tecnica standard: debounce-out + throttle "primo evento").
//   2) BREADCRUMB in alto nelle schermate ZONA e DISTANZA: mostra le scelte
//      gia' fatte (esito, e nella distanza anche la zona), cosi' si ha sempre
//      contezza del percorso, non solo alla fine.
//   3) NIENTE "INDIETRO" nella schermata ESITO: e' il primo passo, non c'e' un
//      "prima". Li' resta solo SALTA. L'INDIETRO vive in ZONA e DISTANZA.
// ============================================================================
#include "scoring.h"
#include "display.h"
#include "config_store.h"     // F19: i due raggi del cerchio d'impatto
#include "tempo.h"             // F20: TEMPO_NOT_SET
#include <TFT_eSPI.h>
#include <math.h>

static constexpr uint32_t SCORING_TIMEOUT_MS = 60000;

// ============================================================================
//  FASE 19 — SCHERMATA IMPATTO: la geometria, in un posto solo
// ============================================================================
//  Queste costanti sono lette dal DISEGNO, dall'HANDLER del tocco e dalla
//  CONVERSIONE in centimetri. E' la regola §4.1 nella sua forma piu' esposta:
//  qui un letterale duplicato non darebbe un tasto che non risponde, darebbe
//  una MISURA sbagliata — e una misura sbagliata non si vede.
//
//  ==========================================================================
//  FASE 21 — IL DITO COPRE IL PUNTO CHE STA SCEGLIENDO
//  ==========================================================================
//  Il problema, dal campo: nel momento in cui si tocca il cerchio per dire dove
//  ha colpito la freccia, il polpastrello nasconde esattamente il punto scelto.
//  Ci sono 2500 ms per correggere, ma correggere alla cieca non e' correggere.
//
//  E' il "fat finger problem", ed e' documentato dal 1988. Le tre soluzioni
//  storiche sono:
//    - TAKE-OFF / OFFSET CURSOR (Potter, Weldon, Shneiderman 1988): il cursore
//      sta sopra il dito e la selezione avviene al SOLLEVAMENTO, non al
//      contatto: si tocca ovunque e si trascina fino al bersaglio.
//    - SHIFT (Vogel & Baudisch, CHI 2007): al tocco compare un riquadro con la
//      copia dell'area coperta, spostato in una zona libera.
//    - CROSS-KEYS / PRECISION-HANDLE (Albinsson & Zhai 2003): tasti discreti
//      abbinati a un mirino, per la rifinitura fine.
//
//  QUI si adotta CROSS-KEYS, e la scelta e' motivata dal contesto d'uso:
//  Shift richiede di tenere il dito premuto e muoverlo sul vetro, che con
//  l'arco in mano e i guanti e' scomodo; i tasti discreti danno invece un passo
//  QUANTIZZATO E METRICO, che e' anche la cosa giusta per un dato che finisce
//  in un CSV. Il tocco diretto sul cerchio resta per il posizionamento
//  grossolano: e' il take-off, e copre il caso "so gia' dov'e'".
//
//  IL PREZZO, dichiarato: per fare posto ai tasti il cerchio scende da R=100 a
//  R=50 px, quindi la risoluzione del TOCCO DIRETTO dimezza (1,00 cm/px sul
//  colpito, 3,00 sul mancato, contro 0,50 e 1,50). Non e' una perdita netta:
//  prima quei mezzi centimetri erano teorici, perche' il punto era sotto il
//  dito. Adesso la precisione la danno i tasti, un centimetro per tocco, con
//  il punto in chiaro. Si scambia una precisione dichiarata con una ottenibile.
//
//  Verifica di ingombro (240x284, barra nav a y=244) — gli static_assert qui
//  sotto la ricontrollano a ogni compilazione, cosi' se domani qualcuno tocca
//  un numero il compilatore se ne accorge prima del campo.
static constexpr int16_t IMP_CX = ARCHBB_W / 2;   // 120
static constexpr int16_t IMP_CY = 144;            // F21b: sceso, la lettura e' cresciuta
static constexpr int16_t IMP_R_PX = 50;           // era 100 in F19: v. sopra
static constexpr int16_t IMP_MARK_R = 6;          // raggio del marcatore
static constexpr uint8_t IMP_ANELLI = 4;          // anelli graduati per raggio

// --- LA LETTURA IN CENTIMETRI, INGRANDITA (F21b) -------------------------
//  Font 4 invece di 2. E' il numero che si guarda mentre si aggiusta col
//  pollice, e a distanza di braccio col sole in faccia il font 2 non si legge.
//  Font 4 e non 6/7: quelli sono SOLO numerici e le parole "dx" e "giu"
//  sparirebbero senza dare errore (lezione gia' pagata sul "+" degli slider).
static constexpr int16_t IMP_LETT_Y = 20;         // riga della lettura, alta 26 px

// --- IL CERCHIO GIALLO (F21b) --------------------------------------------
//  Sul COLPITO, l'anello a 20 cm e' giallo: e' il giallo della visuale, cioe'
//  il riferimento che l'arciere ha davvero negli occhi quando guarda il
//  bersaglio. Gli altri anelli restano grigi perche' sono una griglia; questo
//  e' un oggetto reale, e merita di essere distinguibile.
//  Se un giorno servisse per faccie diverse, questo e' il punto da rendere
//  parametro di config — non un letterale sparso nel disegno.
static constexpr uint16_t IMP_GIALLO_CM = 20;

// --- TASTI DI SPOSTAMENTO: zona SENSIBILE (hit) --------------------------
//  Le fasce laterali corrono per quasi tutta l'altezza utile e sono larghe 48
//  px: sono bersagli enormi rispetto al polpastrello, e questo ha un effetto
//  collaterale prezioso — su questa schermata la taratura affine del CST816
//  diventa irrilevante. Un errore di qualche pixel su un tasto da 48x196 non
//  esiste.
static constexpr int16_t IMP_KEY_TOP = 50;
static constexpr int16_t IMP_KEY_BOT = 238;
static constexpr int16_t IMP_KEY_W   = 48;   // larghezza fasce SX / DX
static constexpr int16_t IMP_KEY_H   = 36;   // altezza  fasce SU / GIU
static constexpr int16_t IMP_COL_X0  = IMP_KEY_W + 4;               //  52
static constexpr int16_t IMP_COL_X1  = ARCHBB_W - IMP_KEY_W - 4;    // 188

// --- TASTI DI SPOSTAMENTO: zona DISEGNATA (visual) -----------------------
//  Il disegno e' 2 px piu' stretto della zona sensibile su ogni lato. Il verso
//  di questo scarto e' deliberato: la zona che risponde e' sempre PIU' GRANDE
//  di quella che si vede, mai piu' piccola. Un tasto che risponde appena fuori
//  dal bordo disegnato non se ne accorge nessuno; uno che non risponde dentro
//  il bordo disegnato sembra rotto.
static constexpr int16_t IMP_KEY_VX = 2;
static constexpr int16_t IMP_KEY_VW = IMP_KEY_W - 4;

// --- Il rettangolo che si ricancella a ogni spostamento del marcatore ----
//  Deve contenere il cerchio E il marcatore quando il marcatore e' sul bordo,
//  altrimenti resta un mezzo anello di residuo grafico. Da qui il +MARK_R.
static constexpr int16_t IMP_CLR_HALF = IMP_R_PX + IMP_MARK_R + 1;

// Il cerchio (piu' il marcatore al bordo) non deve invadere le fasce SU/GIU.
// Se un giorno qualcuno alza IMP_R_PX per "guadagnare risoluzione", questi due
// assert glielo impediscono a compilazione invece che sul campo.
static_assert(IMP_CY - IMP_CLR_HALF >= IMP_KEY_TOP + IMP_KEY_H,
              "cerchio+marcatore invadono la fascia SU");
static_assert(IMP_CY + IMP_CLR_HALF <= IMP_KEY_BOT - IMP_KEY_H,
              "cerchio+marcatore invadono la fascia GIU");
static_assert(IMP_CX - IMP_CLR_HALF >= IMP_COL_X0,
              "cerchio+marcatore invadono la fascia SX");

// --- BARRA NAV A TRE TASTI, solo per questa schermata --------------------
//  Le altre schermate hanno due azioni e usano drawNavBar/hitNavBar condivise.
//  Qui ne servono tre, e invece di parametrizzare la funzione condivisa (che
//  significherebbe farla crescere per un caso solo, con il rischio di rompere
//  le schermate che gia' funzionano) la barra dell'impatto ha la sua coppia
//  disegno/hit-test. Le costanti restano una sola serie, letta da entrambe.
//
//  LA GERARCHIA E' NEL CORPO DEL CARATTERE, non solo nel colore:
//    INDIETRO  font 1  — ripiego, si usa di rado
//    NON SO    font 2  — uscita legittima ma non frequente
//    OK        font 4  — l'azione normale, quella che si preme ogni tiro
//  Un OK grande e verde si trova col pollice senza guardare, ed e' proprio
//  quello che serve con l'arco nell'altra mano.
static constexpr int16_t IMPNAV_W  = 74;
static constexpr int16_t IMPNAV_X0 = 5;
static constexpr int16_t IMPNAV_X1 = IMPNAV_X0 + IMPNAV_W + 5;    //  84
static constexpr int16_t IMPNAV_X2 = IMPNAV_X1 + IMPNAV_W + 5;    // 163
static_assert(IMPNAV_X2 + IMPNAV_W <= ARCHBB_W - 2, "barra nav impatto fuori schermo");

// --- ACCELERAZIONE A TOCCHI RIPETUTI -------------------------------------
//  NON c'e' auto-ripetizione a dito premuto, ed e' una scelta. La ripetizione
//  richiederebbe di ridisegnare il cerchio 6-8 volte al secondo: ~14 ms di
//  ridisegno ogni 150, cioe' uno sfarfallio visibile per tutta la durata della
//  pressione. Al suo posto: tocchi ripetuti sullo STESSO tasto entro mezzo
//  secondo fanno crescere il passo. Ogni ridisegno resta agganciato a un'azione
//  discreta dell'arciere, che e' esattamente il comportamento gia' collaudato
//  del tocco diretto.
static constexpr uint32_t IMP_ACCEL_MS = 500;   // finestra fra due tocchi "in serie"

// ----------------------------------------------------------------------------
//  F21b — LA CONFERMA AUTOMATICA E' STATA TOLTA
// ----------------------------------------------------------------------------
//  La F19 confermava l'impatto allo scadere di 2500 ms, con una barra che si
//  riempiva. Il ragionamento era: ogni tocco in piu' e' un tocco che qualcuno
//  salta. Il campo ha detto un'altra cosa — una barra che avanza mentre stai
//  ancora decidendo non fa risparmiare un tocco, mette fretta. E la fretta su
//  una misura e' esattamente il contrario di quello che serve.
//
//  Al suo posto c'e' un OK esplicito. Costa un tocco e restituisce il tempo:
//  nessuna scadenza, nessuna barra che si muove nel campo visivo mentre si
//  guarda il bersaglio. Il timeout generale dello scoring (60 s) resta e basta
//  a chiudere una schermata dimenticata.

static constexpr int NAV_H = 40;
static constexpr int NAV_Y = ARCHBB_H - NAV_H;

// ============================================================================
//  LAYOUT DIST + ELEV (portato dalla 1.69 v2.16.10)
// ============================================================================
//  LA LEZIONE PIU' COSTOSA DELLA 1.69, APPLICATA QUI DALL'INIZIO:
//
//    "Se un fatto e' scritto in due posti, prima o poi divergono."
//
//  Sulla 1.69 il disegno usava le costanti (KEY_Y=191, BTN_TOP=235) e l'handler
//  aveva i NUMERI SCRITTI A MANO della versione precedente (190, 150..180).
//  Risultato: toccavi "+" e partiva OK. Otto versioni spese a calibrare un touch
//  che era gia' a posto, mentre il bug era un letterale in una riga.
//
//  Percio' QUI: disegno e handler leggono le STESSE costanti. Nessun letterale
//  di posizione negli handler. Se un tasto si sposta, si sposta in un punto solo.
//
//  GEOMETRIA — perche' si rimappa da sola da 280 (1.69) a 284 (1.83):
//  BTN_TOP e KEY_Y sono DERIVATE da ARCHBB_H, non assolute. I 4px in piu' della
//  1.83 diventano automaticamente respiro fra lo slider ELEV e i tasti (43 ->
//  47px), senza toccare nulla. Gli slider restano a quote assolute perche' sono
//  ancorati alla parte alta.
// ----------------------------------------------------------------------------
static constexpr uint8_t DIST_MIN = 5;
static constexpr uint8_t DIST_MAX = 60;

// --- binario condiviso dai due slider ---
// ============================================================================
//  SLIDER VERTICALE (F15) — una schermata per grandezza
// ============================================================================
//  PERCHE' RIFATTO
//    Gli slider precedenti erano orizzontali, larghi 192 px con una fascia
//    tattile di 32: la zona da prendere era piccola e il dito finiva SOPRA il
//    numero che stava cambiando. E il valore si applicava solo al RILASCIO,
//    quindi non era un vero slider ma due tap.
//
//  COME E' ADESSO
//    - il valore sta IN ALTO, grande, dove il dito non arriva mai
//    - la traccia e' verticale e larga 120 px: si prende senza mirare
//    - il valore si aggiorna MENTRE si trascina, non al rilascio
//    - due zone ai bordi laterali fanno +1 e -1 per l'aggiustamento fine
//    - distanza ed elevazione su DUE schermate successive: una cosa per volta,
//      e ciascuna con tutto lo schermo a disposizione
//
//  Il verso e' quello naturale: in ALTO il valore cresce.
static constexpr int16_t VS_TRACK_X0 = 60;    // traccia: bordo sinistro
static constexpr int16_t VS_TRACK_X1 = 180;   //          bordo destro
static constexpr int16_t VS_TRACK_Y0 = 74;    //          alto  = MASSIMO
static constexpr int16_t VS_TRACK_Y1 = 226;   //          basso = MINIMO
static constexpr int16_t VS_PLUS_X1  = 52;    // zona "+": da 0 a qui
static constexpr int16_t VS_MINUS_X0 = 188;   // zona "-": da qui al bordo
static constexpr int16_t VS_ZONE_Y0  = 74;    // altezza delle due zone laterali
static constexpr int16_t VS_ZONE_Y1  = 226;
static constexpr int16_t VS_TRACK_H  = VS_TRACK_Y1 - VS_TRACK_Y0;

// Barra di navigazione in basso: CORREGGI a sinistra, avanti a destra.
static constexpr int16_t VS_NAV_Y = 234;
static constexpr int16_t VS_NAV_H = 44;
static constexpr int16_t VS_NAV_W = 104;
static constexpr int16_t VS_NAV_M = 8;

// --- conversioni valore <-> y, generiche su min/max -------------------------
//  Un solo paio di funzioni per distanza ed elevazione: due coppie separate
//  sono due occasioni di sbagliare l'arrotondamento in modo diverso (§4.1).
static int16_t valToY(int v, int vmin, int vmax) {
    if (v < vmin) v = vmin;
    if (v > vmax) v = vmax;
    // y cresce verso il BASSO, il valore verso l'ALTO: l'asse e' invertito.
    return VS_TRACK_Y1 - (int16_t)(((int32_t)(v - vmin) * VS_TRACK_H
                                    + (vmax - vmin) / 2) / (vmax - vmin));
}
static int yToVal(int16_t y, int vmin, int vmax) {
    if (y < VS_TRACK_Y0) y = VS_TRACK_Y0;
    if (y > VS_TRACK_Y1) y = VS_TRACK_Y1;
    int v = vmin + (int)(((int32_t)(VS_TRACK_Y1 - y) * (vmax - vmin)
                          + VS_TRACK_H / 2) / VS_TRACK_H);
    if (v < vmin) v = vmin;
    if (v > vmax) v = vmax;
    return v;
}
static bool inTraccia(uint16_t x, uint16_t y) {
    return x >= VS_TRACK_X0 && x <= VS_TRACK_X1
        && y >= VS_TRACK_Y0 - 6 && y <= VS_TRACK_Y1 + 6;
}

// --- Parametri debounce/cooldown -------------------------------------------
// Il loop gira ~ogni 20ms (delay(20) nel main). 2 campioni stabili = ~40ms di
// rilascio confermato. Cooldown 180ms: abbastanza per evitare il doppio-tap,
// ma piu' reattivo per tocchi intenzionali consecutivi (es. tacca -> CONFERMA,
// che con 250ms risultava "pigra").
static constexpr uint8_t  RELEASE_STABLE_N   = 2;
static constexpr uint32_t ACTION_COOLDOWN_MS = 180;

// ----------------------------------------------------------------------------
static ScoringState s_state   = ScoringState::IDLE;
static ScoreResult  s_result;
static uint16_t     s_shotId  = 0;
static uint32_t     s_lastTouchMs  = 0;
static uint32_t     s_lastActionMs = 0;
static bool         s_needRedraw   = true;
// ----------------------------------------------------------------------------
//  Default e MEMORIA del tiro precedente (F13)
// ----------------------------------------------------------------------------
//  Su un percorso 3D i tiri vicini hanno spesso distanza ed elevazione uguali o
//  simili: ripartire ogni volta dallo stesso numero fisso costringe a rifare
//  tutta la corsa dello slider. Riproporre i valori dell'ultimo tiro VALUTATO
//  rende la conferma il caso normale e la modifica l'eccezione.
//  Il default di partenza passa da 18 a 30 m, che e' il centro pratico delle
//  distanze viste sul campo (14-48 m il 28/07), e l'elevazione da "mai
//  misurata" a 0: il tiro in piano e' il caso piu' frequente e non deve
//  costare un tocco.
//
//  NOTA sulla sentinella ELEV_NOT_SET: resta nella struct e nel CSV per i dati
//  vecchi, ma nel flusso normale non si presenta piu' — ogni tiro valutato
//  esce con un'elevazione, foss'anche zero. E' una perdita voluta: distinguere
//  "in piano" da "non misurata" costava un tocco a ogni tiro.
static constexpr uint8_t DIST_DEFAULT = 30;
static constexpr int8_t  ELEV_DEFAULT = 0;
static uint8_t      s_lastDist       = DIST_DEFAULT;   // ultimo VALUTATO
static int8_t       s_lastElev       = ELEV_DEFAULT;

static uint8_t      s_distSel      = DIST_DEFAULT;
// v2.16: elevazione del bersaglio. Parte SEMPRE dalla sentinella, mai da 0:
// "non l'ho ancora misurata" e "e' in piano" sono due fatti diversi, e solo il
// tocco dell'arciere trasforma il primo nel secondo.
static int8_t       s_elevSel      = ELEV_DEFAULT;

// Debounce state.
static bool     s_stableTouch = false;   // stato "tocco" debounced
static uint8_t  s_relCount    = 0;        // campioni consecutivi di non-tocco
static uint16_t s_pressX = 0, s_pressY = 0;

// --- F19: stato della schermata impatto ------------------------------------
static int16_t  s_impXcm = IMP_NOT_SET, s_impYcm = IMP_NOT_SET;
static int16_t  s_impPx = IMP_CX, s_impPy = IMP_CY;   // marcatore, in pixel
static bool     s_impSet = false;                     // marcatore piazzato
// F21b: s_impConferma / s_impMs / s_impBarW sono spariti col temporizzatore.
// Adesso l'unica cosa che conta e' se il marcatore c'e': se c'e', OK e' attivo.

// --- F21: stato dell'accelerazione a tocchi ripetuti ---------------------
//  s_impTapZona e' l'ULTIMO tasto premuto (0 = nessuno / tocco sul cerchio):
//  cambiare tasto azzera la serie, perche' una serie di "destra" non deve
//  regalare velocita' al primo "su".
// --- FASE 22: modo pre-tiro ---------------------------------------------
//  s_pretiro    = le schermate di slider stanno armando il bersaglio, non
//                 correggendo un tiro. Cambia il tasto di sinistra e cambia
//                 dove si va premendo OK sull'elevazione.
//  s_pretiroOk  = si e' usciti confermando (OK sull'elevazione) e non
//                 annullando. Serve al chiamante per sapere se prendere i
//                 valori o lasciare quelli di prima: un flag esplicito, non
//                 una deduzione da un valore "che sembra cambiato".
static bool s_pretiro   = false;
static bool s_pretiroOk = false;

static uint8_t  s_impTapZona = 0;
static uint32_t s_impTapMs   = 0;
static uint8_t  s_impTapRun  = 0;   // quanti tocchi consecutivi sullo stesso tasto

// ============================================================================
int16_t encodeArcs(bool colpito, uint8_t zona, uint8_t distanza) {
  // zona ignota -> cifra delle unita' a 0. Vedi la nota in scoring.h: e' il
  // valore che la codifica non usava, e adesso significa "impatto non rilevato".
  int16_t unita = (zona == ZONA_IGNOTA) ? 0 : (int16_t)(zona + 1);
  int16_t mag = (int16_t)distanza * 10 + unita;
  return colpito ? mag : (int16_t)(-mag);
}

// ----------------------------------------------------------------------------
//  Zona derivata dalle coordinate: terzi del raggio, come la griglia di prima.
// ----------------------------------------------------------------------------
//  La soglia a raggio/3 riproduce ESATTAMENTE la semantica della griglia 3x3
//  della 1.69 (colonne e righe in terzi dello schermo). E' voluto: le sedute
//  vecchie e quelle nuove restano confrontabili sulla colonna `zona`, mentre le
//  colonne nuove aggiungono la risoluzione che prima non c'era.
uint8_t impattoToZona(int16_t x_cm, int16_t y_cm, uint16_t raggio_cm) {
  if (!impIsSet(x_cm) || !impIsSet(y_cm) || raggio_cm == 0) return ZONA_IGNOTA;
  const int16_t soglia = (int16_t)(raggio_cm / 3);
  const uint8_t col = (x_cm < -soglia) ? 0 : ((x_cm > soglia) ? 2 : 1);
  const uint8_t row = (y_cm >  soglia) ? 0 : ((y_cm < -soglia) ? 2 : 1);
  return (uint8_t)(col + 3 * row);
}

static const char* ZONA_NOME[9] = {
  "alto-sx","alto","alto-dx","sx","centro","dx","basso-sx","basso","basso-dx"
};
const char* zonaLabel(bool colpito, uint8_t zona) {
  if (zona == ZONA_IGNOTA) return "n/d";
  if (colpito && zona == ZONA_CENTRO) return "SPOT";
  if (zona < 9) return ZONA_NOME[zona];
  return "";
}
// NB: drawPlus()/drawMinus() sono stati RIMOSSI in v2.16: li usava solo la
// vecchia schermata distanza (tacche 18/30/50 + CONFERMA a barra), sostituita
// dal doppio slider. I glifi +/- ora si disegnano inline nei tasti, che hanno
// una geometria propria. Codice morto tolto invece che lasciato "per sicurezza":
// e' esattamente il tipo di residuo che poi qualcuno riusa per sbaglio.
static void kickTimeout() { s_lastTouchMs = millis(); }

// Breadcrumb: riga in alto con le scelte gia' fatte. Col nuovo ordine il
// contesto (distanza, elevazione) e' sempre disponibile e vale la pena
// mostrarlo: e' cio' che l'arciere ha appena scelto, e vederlo scritto e' la
// difesa piu' economica contro lo slider mosso per sbaglio.
static void drawBreadcrumb(bool conEsito) {
  tft.setTextDatum(TC_DATUM);
  char bc[48];
  if (conEsito) {
    snprintf(bc, sizeof(bc), "%s  %u m  %+d",
             s_result.colpito ? "COLPITO" : "MANCATO",
             (unsigned)s_distSel, (int)s_elevSel);
  } else {
    snprintf(bc, sizeof(bc), "%u m   %+d", (unsigned)s_distSel, (int)s_elevSel);
  }
  uint16_t col = conEsito ? (s_result.colpito ? ArchColor::GOOD : ArchColor::BAD)
                          : ArchColor::TEXT3;
  tft.setTextColor(col, ArchColor::BG);
  tft.drawString(bc, ARCHBB_W/2, 2, 2);
}

// Barra nav: INDIETRO (opzionale) a sx, azione (opzionale) a dx.
//  L'etichetta di sinistra e' un PARAMETRO: sulla schermata ESITO non c'e' un
//  "indietro" (e' la prima) e quello spazio serve per SCARTA. Un secondo
//  disegnatore di barre avrebbe potuto divergere da questo (§4.1).
static void drawNavBar(bool showBack, const char* rightLabel, uint16_t rightColor,
                       const char* leftLabel = "< INDIETRO",
                       uint16_t leftColor = ArchColor::BG_CARD) {
  if (showBack) {
    tft.fillRoundRect(6, NAV_Y + 3, 96, NAV_H - 6, 6, leftColor);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(ArchColor::TEXT2, leftColor);
    tft.drawString(leftLabel, 6 + 48, NAV_Y + NAV_H/2, 2);
  }
  if (rightLabel) {
    tft.fillRoundRect(ARCHBB_W - 6 - 96, NAV_Y + 3, 96, NAV_H - 6, 6, rightColor);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(ArchColor::TEXT, rightColor);
    tft.drawString(rightLabel, ARCHBB_W - 6 - 48, NAV_Y + NAV_H/2, 2);
  }
}
static uint8_t hitNavBar(uint16_t x, uint16_t y, bool hasBack, bool hasRight) {
  if (y < NAV_Y) return 0;
  if (hasBack  && x >= 6 && x < 6 + 96) return 1;
  if (hasRight && x >= ARCHBB_W - 6 - 96 && x < ARCHBB_W - 6) return 2;
  return 0;
}

// ============================================================================
//  Rendering
// ============================================================================
static void drawEsitoScreen() {
  tft.fillScreen(ArchColor::BG);
  drawBreadcrumb(false);            // distanza ed elevazione, gia' scelte
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(ArchColor::ACCENT, ArchColor::BG);
  char t[24]; snprintf(t, sizeof(t), "TIRO #%u", s_shotId);
  tft.drawString(t, ARCHBB_W/2, 18, 2);
  tft.setTextColor(ArchColor::TEXT, ArchColor::BG);
  tft.drawString("Colpito il bersaglio?", ARCHBB_W/2, 34, 2);

  const int bx = 20, bw = ARCHBB_W - 40, bh = 72;
  const int y1 = 54, y2 = 54 + bh + 12;
  tft.fillRoundRect(bx, y1, bw, bh, 10, ArchColor::GOOD);
  tft.fillRoundRect(bx, y2, bw, bh, 10, ArchColor::BAD);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(ArchColor::TEXT, ArchColor::GOOD);
  tft.drawString("COLPITO", ARCHBB_W/2, y1 + bh/2, 4);
  tft.setTextColor(ArchColor::TEXT, ArchColor::BAD);
  tft.drawString("MANCATO", ARCHBB_W/2, y2 + bh/2, 4);

  // Nav: a sinistra INDIETRO (all'elevazione), a destra SALTA — era un tiro ma
  // non lo valuto: va su SD con distanza ed elevazione, senza impatto.
  // SCARTA non e' piu' qui: col nuovo ordine sta sulla PRIMA schermata
  // (distanza), dove costa un tocco solo, come prima.
  drawNavBar(true, "SALTA", ArchColor::BORDER);
}

// ============================================================================
//  FASE 19 — SCHERMATA IMPATTO
// ============================================================================
//  Sostituisce la griglia 3x3. Il motivo non e' la significativita' statistica
//  (una simulazione dice che tre livelli costano appena 4 punti di potenza):
//  e' che tre livelli non hanno UNITA' DI MISURA. Con tre bin si puo' arrivare
//  a dire "si', correla"; non si potra' mai dire "0,45 cm di impatto per cm di
//  previsione", che e' la costante di taratura senza la quale una metrica
//  biomeccanica non diventa mai un consiglio utilizzabile da un arciere.
//
//  Risoluzione ottenuta, con IMP_R_PX = 100:
//     colpito   raggio 50 cm  -> 0,50 cm/px   (dito +-4 px = +-2,0 cm)
//     mancato   raggio 150 cm -> 1,50 cm/px   (dito +-4 px = +-6,0 cm)
//  In entrambi i casi la sorgente d'errore dominante torna a essere l'occhio
//  dell'arciere, che e' come deve essere: lo strumento non deve essere il collo
//  di bottiglia della misura.
// ----------------------------------------------------------------------------

// Raggio in centimetri del cerchio ATTUALE. Dipende dall'esito, che a questo
// punto del flusso e' gia' stato scelto. Il guardiano sullo zero protegge da un
// config corrotto: senza, una divisione per zero nella conversione.
static uint16_t impRaggioCm() {
  uint16_t r = s_result.colpito ? g_config.imp_raggio_colpito_cm
                                : g_config.imp_raggio_mancato_cm;
  return (r == 0) ? 50 : r;
}

// Le DUE conversioni, l'una inversa dell'altra, definite una volta sola. Il
// disegno usa cmToPx (per gli anelli), l'handler usa pxToCm (per la misura).
// Scriverle in due punti diversi vorrebbe dire poterle sbagliare in modo
// asimmetrico, e un errore asimmetrico non si vede guardando lo schermo.
static int16_t pxToCm(int16_t dpx) {
  return (int16_t)lroundf((float)dpx * (float)impRaggioCm() / (float)IMP_R_PX);
}
static int16_t cmToPx(int16_t cm) {
  return (int16_t)lroundf((float)cm * (float)IMP_R_PX / (float)impRaggioCm());
}

// ----------------------------------------------------------------------------
//  PASSO DI UN TOCCO SUI TASTI — un pixel, arrotondato al centimetro.
// ----------------------------------------------------------------------------
//  Perche' proprio un pixel: un passo piu' fine muoverebbe il marcatore meno di
//  un pixel, e un tasto che si preme senza vedere nulla muoversi sembra rotto
//  anche quando il numero in alto cambia. Un passo piu' grosso sprecherebbe la
//  risoluzione che il cerchio ha davvero.
//  Con IMP_R_PX=50: raggio 50 cm -> passo 1 cm; raggio 150 cm -> passo 3 cm.
static uint8_t impPassoCm() {
  int p = (int)lroundf((float)impRaggioCm() / (float)IMP_R_PX);
  return (uint8_t)(p < 1 ? 1 : p);
}

// Moltiplicatore del passo per tocchi ripetuti sullo stesso tasto. I primi tre
// tocchi sono a passo pieno (la rifinitura fine, che e' il caso normale); poi
// la corsa accelera per chi deve attraversare mezzo bersaglio.
static uint8_t impMoltiplicatore() {
  if (s_impTapRun < 3) return 1;
  if (s_impTapRun < 8) return 3;
  return 8;
}

// Riga di lettura in centimetri, sotto il breadcrumb. Ridisegnata da sola.
// La SCALA: raggio e passo, minuscoli, agli angoli della riga del breadcrumb.
// Senza il raggio i centimetri non hanno significato, senza il passo non si sa
// quanto vale un tocco. Stanno negli unici due angoli che nessun dito copre e
// che nessun testo lungo raggiunge.
static void impDisegnaScala() {
  char sc[12];
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  snprintf(sc, sizeof(sc), "R%u", (unsigned)impRaggioCm());
  tft.setTextDatum(TL_DATUM);
  tft.drawString(sc, 2, 5, 1);
  snprintf(sc, sizeof(sc), "+%u", (unsigned)impPassoCm());
  tft.setTextDatum(TR_DATUM);
  tft.drawString(sc, ARCHBB_W - 2, 5, 1);
}

// Riga di lettura in centimetri. F21b: font 4 — e' il numero che si guarda
// mentre si aggiusta col pollice, e deve leggersi a distanza di braccio.
static void impDisegnaLettura() {
  tft.fillRect(0, IMP_LETT_Y, ARCHBB_W, 27, ArchColor::BG);
  tft.setTextDatum(TC_DATUM);
  if (!s_impSet) {
    tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
    tft.drawString("dove ha colpito?", ARCHBB_W/2, IMP_LETT_Y + 5, 2);
    return;
  }
  char b[48];
  snprintf(b, sizeof(b), "%d %s  %d %s",
           (int)abs(s_impXcm), s_impXcm >= 0 ? "dx" : "sx",
           (int)abs(s_impYcm), s_impYcm >= 0 ? "su" : "giu");
  tft.setTextColor(ArchColor::AMBER, ArchColor::BG);
  tft.drawString(b, ARCHBB_W/2, IMP_LETT_Y, 4);
}

// Cerchio, anelli graduati, croce di mira e marcatore. Ridisegna tutta l'area
// del cerchio: spostare il marcatore cancellando solo il vecchio richiederebbe
// di ricostruire quel pezzo di anello sotto, e i tocchi sono rari abbastanza
// perche' non valga la complicazione.
static void impDisegnaCerchio() {
  const uint16_t raggio = impRaggioCm();
  const uint16_t colBordo = s_result.colpito ? ArchColor::GOOD : ArchColor::AMBER;

  // Il rettangolo di pulizia include il raggio del marcatore: se non lo
  // includesse, un marcatore appoggiato al bordo lascerebbe mezza corona di
  // residuo al passaggio successivo. La costante e' una sola (IMP_CLR_HALF) ed
  // e' la stessa che gli static_assert usano per verificare gli ingombri.
  tft.fillRect(IMP_CX - IMP_CLR_HALF, IMP_CY - IMP_CLR_HALF,
               2*IMP_CLR_HALF, 2*IMP_CLR_HALF, ArchColor::BG);

  // anelli interni, uno ogni raggio/IMP_ANELLI centimetri
  for (uint8_t i = 1; i < IMP_ANELLI; i++) {
    tft.drawCircle(IMP_CX, IMP_CY, (IMP_R_PX * i) / IMP_ANELLI, ArchColor::BG_CARD);
  }

  // F21b — IL GIALLO. Solo sul COLPITO: sul mancato il cerchio ha un'altra
  // scala e un anello a 20 cm sarebbe un puntino senza significato.
  //  Doppio tratto come il bordo esterno: un anello a un pixel solo, giallo su
  //  nero, a distanza di braccio non si vede.
  if (s_result.colpito) {
    int16_t rg = cmToPx((int16_t)IMP_GIALLO_CM);
    if (rg > 3 && rg < IMP_R_PX - 1) {
      tft.drawCircle(IMP_CX, IMP_CY, rg,     ArchColor::AMBER);
      tft.drawCircle(IMP_CX, IMP_CY, rg - 1, ArchColor::AMBER);
    }
  }
  // croce del punto di mira: e' l'ORIGINE del sistema di coordinate, non il
  // centro della sagoma. Vedi la nota in scoring.h.
  tft.drawFastHLine(IMP_CX - IMP_R_PX, IMP_CY, 2*IMP_R_PX, ArchColor::BG_CARD);
  tft.drawFastVLine(IMP_CX, IMP_CY - IMP_R_PX, 2*IMP_R_PX, ArchColor::BG_CARD);
  tft.fillCircle(IMP_CX, IMP_CY, 2, colBordo);

  // sul MANCATO, il confine della sagoma dichiarata: un promemoria visivo, non
  // una zona proibita. Se l'arciere tocca dentro, il dato viene preso lo stesso:
  // una zona morta al centro dello schermo, con l'arco in mano, e' un invito a
  // sbagliare — e togliergli la possibilita' di correggersi non aggiunge nulla.
  if (!s_result.colpito) {
    int16_t rInt = cmToPx((int16_t)g_config.imp_raggio_colpito_cm);
    if (rInt > 4 && rInt < IMP_R_PX) tft.drawCircle(IMP_CX, IMP_CY, rInt, ArchColor::BAD);
  }

  tft.drawCircle(IMP_CX, IMP_CY, IMP_R_PX,     colBordo);
  tft.drawCircle(IMP_CX, IMP_CY, IMP_R_PX - 1, colBordo);

  // F21b — NIENTE etichetta numerica dentro al cerchio. Era il "25" a meta'
  // raggio: un numero di sei pixel appoggiato su una griglia di anelli, che
  // nessuno leggeva e che sporcava l'unica zona in cui serve vedere bene il
  // marcatore. Il raggio pieno resta scritto in alto a sinistra, e li' basta.
  (void)raggio;

  // F21b — IL MARCATORE E' BIANCO, non piu' ambra.
  //  Con l'anello dei 20 cm diventato giallo, un marcatore ambra ci si perdeva
  //  dentro proprio nella zona dove serve leggerlo meglio: due oggetti dello
  //  stesso colore, uno sopra l'altro, a sei pixel di raggio. Il bianco e'
  //  l'unico colore che non e' gia' in uso qui e che stacca su tutto — griglia
  //  grigia, giallo, verde del bordo e rosso del confine sagoma.
  //  La lettura in cm resta ambra: li' l'ambra non compete con niente.
  if (s_impSet) {
    tft.drawCircle(s_impPx, s_impPy, IMP_MARK_R,     ArchColor::TEXT);
    tft.drawCircle(s_impPx, s_impPy, IMP_MARK_R - 1, ArchColor::TEXT);
    tft.fillCircle(s_impPx, s_impPy, 2, ArchColor::TEXT);
  }
}

// ============================================================================
//  FASE 21 — I QUATTRO TASTI DI SPOSTAMENTO
// ============================================================================
//  Identificatori delle zone. Sono un enum e non quattro numeri sparsi perche'
//  li leggono TRE funzioni diverse (disegno, hit-test, spostamento): e' la
//  regola §4.1 applicata a un'etichetta invece che a una coordinata.
enum : uint8_t { IMPK_NONE = 0, IMPK_SX, IMPK_DX, IMPK_SU, IMPK_GIU };

// Zona toccata fra i quattro tasti; IMPK_NONE se il tocco e' altrove.
//  Legge LE STESSE costanti del disegno. Nessun letterale di posizione.
static uint8_t impZonaTasto(uint16_t x, uint16_t y) {
  if (y < IMP_KEY_TOP || y >= IMP_KEY_BOT) return IMPK_NONE;
  if (x < IMP_KEY_W)                 return IMPK_SX;
  if (x >= ARCHBB_W - IMP_KEY_W)     return IMPK_DX;
  if (x >= IMP_COL_X0 && x < IMP_COL_X1) {
    if (y <  IMP_KEY_TOP + IMP_KEY_H) return IMPK_SU;
    if (y >= IMP_KEY_BOT - IMP_KEY_H) return IMPK_GIU;
  }
  return IMPK_NONE;
}

// Disegno dei quattro tasti. Statici: si tracciano una volta all'ingresso nella
// schermata e non vengono piu' toccati — il ridisegno del marcatore lavora
// dentro IMP_CLR_HALF, che per costruzione (static_assert) non li sfiora.
//
// I glifi sono TRIANGOLI DISEGNATI, non caratteri. I font 6/7/8 di TFT_eSPI
// sono soli numerici: scriverci una freccia non da' errore, fa sparire il
// segno. E' lo stesso inciampo del "+" sugli slider, gia' pagato una volta.
static void impDisegnaTasti() {
  const int16_t hy = IMP_KEY_TOP;
  const int16_t hh = IMP_KEY_BOT - IMP_KEY_TOP;
  const int16_t cy = IMP_KEY_TOP + hh/2;                 // centro verticale fasce
  const int16_t dxr = ARCHBB_W - IMP_KEY_VX - IMP_KEY_VW;

  auto fascia = [&](int16_t x, int16_t y, int16_t w, int16_t h) {
    tft.fillRoundRect(x, y, w, h, 8, ArchColor::BG_CARD);
    tft.drawRoundRect(x, y, w, h, 8, ArchColor::BORDER);
  };

  // SX e DX: due colonne alte quasi quanto l'area utile.
  fascia(IMP_KEY_VX, hy, IMP_KEY_VW, hh);
  fascia(dxr,        hy, IMP_KEY_VW, hh);
  const int16_t sxc = IMP_KEY_VX + IMP_KEY_VW/2;
  const int16_t dxc = dxr        + IMP_KEY_VW/2;
  tft.fillTriangle(sxc - 11, cy,      sxc + 8, cy - 14, sxc + 8, cy + 14, ArchColor::ACCENT);
  tft.fillTriangle(dxc + 11, cy,      dxc - 8, cy - 14, dxc - 8, cy + 14, ArchColor::ACCENT);

  // SU e GIU: due fasce nella colonna centrale, sopra e sotto il cerchio.
  const int16_t cw = IMP_COL_X1 - IMP_COL_X0;
  fascia(IMP_COL_X0, IMP_KEY_TOP,               cw, IMP_KEY_H);
  fascia(IMP_COL_X0, IMP_KEY_BOT - IMP_KEY_H,   cw, IMP_KEY_H);
  const int16_t suc = IMP_KEY_TOP + IMP_KEY_H/2;
  const int16_t giuc = IMP_KEY_BOT - IMP_KEY_H/2;
  tft.fillTriangle(IMP_CX, suc - 11,  IMP_CX - 14, suc + 8,  IMP_CX + 14, suc + 8,  ArchColor::ACCENT);
  tft.fillTriangle(IMP_CX, giuc + 11, IMP_CX - 14, giuc - 8, IMP_CX + 14, giuc - 8, ArchColor::ACCENT);
}

// Barra nav dell'impatto: INDIETRO · NON SO · OK.
//  L'OK e' SPENTO finche' non c'e' un marcatore. Spento e non nascosto: un
//  tasto che compare e scompare sposta l'attenzione a ogni tocco, e un tasto
//  grigio dice da solo "manca qualcosa prima". La zona resta comunque inerte
//  nell'hit-test, quindi non esiste il caso di un OK che si vede grigio e
//  conferma lo stesso.
static void impDisegnaNav() {
  const int16_t y = NAV_Y + 3, h = NAV_H - 6;
  const int16_t cy = NAV_Y + NAV_H/2;
  tft.fillRect(0, NAV_Y, ARCHBB_W, NAV_H, ArchColor::BG);

  tft.fillRoundRect(IMPNAV_X0, y, IMPNAV_W, h, 6, ArchColor::BG_CARD);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(ArchColor::TEXT2, ArchColor::BG_CARD);
  tft.drawString("< INDIETRO", IMPNAV_X0 + IMPNAV_W/2, cy, 1);

  // NON SO: l'uscita esplicita per le quindici frecce infisse nella sagoma.
  tft.fillRoundRect(IMPNAV_X1, y, IMPNAV_W, h, 6, ArchColor::BG_CARD);
  tft.setTextColor(ArchColor::TEXT2, ArchColor::BG_CARD);
  tft.drawString("NON SO", IMPNAV_X1 + IMPNAV_W/2, cy, 2);

  const uint16_t okCol = s_impSet ? ArchColor::GOOD : ArchColor::BORDER;
  tft.fillRoundRect(IMPNAV_X2, y, IMPNAV_W, h, 6, okCol);
  tft.setTextColor(s_impSet ? ArchColor::TEXT : ArchColor::TEXT3, okCol);
  tft.drawString("OK", IMPNAV_X2 + IMPNAV_W/2, cy, 4);
}

// 1 = INDIETRO · 2 = NON SO · 3 = OK (solo se il marcatore c'e') · 0 = altro.
// Legge LE STESSE costanti del disegno.
static uint8_t impHitNav(uint16_t x, uint16_t y) {
  if (y < NAV_Y) return 0;
  if (x >= IMPNAV_X0 && x < IMPNAV_X0 + IMPNAV_W) return 1;
  if (x >= IMPNAV_X1 && x < IMPNAV_X1 + IMPNAV_W) return 2;
  if (x >= IMPNAV_X2 && x < IMPNAV_X2 + IMPNAV_W) return s_impSet ? 3 : 0;
  return 0;
}

// ----------------------------------------------------------------------------
//  Spostamento del marcatore di N centimetri. UNICO punto in cui i tasti
//  modificano il dato.
// ----------------------------------------------------------------------------
//  Due sottigliezze che non si vedono e contano:
//
//  1) Il valore in CENTIMETRI e' quello primario, e i passi si accumulano su
//     quello. Se si accumulasse sui pixel e si riconvertisse ogni volta, gli
//     arrotondamenti si sommerebbero e dieci tocchi da 1 cm non farebbero
//     10 cm. Il pixel e' solo la resa a schermo del centimetro, mai viceversa.
//
//  2) La SATURAZIONE al bordo ricalcola i centimetri DAL pixel saturato. Un
//     marcatore fermo sul bordo che continuasse a dichiarare valori crescenti
//     sarebbe una bugia silenziosa: quello che si legge e quello che si salva
//     devono restare la stessa cosa. E' la stessa regola gia' applicata al
//     rientro da CORREGGI (impRipristina).
static void impSposta(int16_t dxcm, int16_t dycm) {
  // Primo tocco su un tasto senza marcatore: si parte dal punto di mira, che e'
  // l'origine del sistema di riferimento e l'unico punto di partenza che non
  // inventa nulla.
  if (!s_impSet) { s_impXcm = 0; s_impYcm = 0; s_impSet = true; }

  int16_t nx = (int16_t)(s_impXcm + dxcm);
  int16_t ny = (int16_t)(s_impYcm + dycm);

  float px = (float)cmToPx(nx);
  float py = (float)cmToPx(ny);            // positivo = ALTO
  float r  = sqrtf(px*px + py*py);
  if (r > (float)IMP_R_PX) {
    px = px * (float)IMP_R_PX / r;
    py = py * (float)IMP_R_PX / r;
    nx = pxToCm((int16_t)lroundf(px));
    ny = pxToCm((int16_t)lroundf(py));
  }

  s_impXcm = nx;
  s_impYcm = ny;
  s_impPx  = (int16_t)(IMP_CX + lroundf(px));
  s_impPy  = (int16_t)(IMP_CY - lroundf(py));   // lo schermo cresce in giu'

  // Il primo spostamento accende l'OK: la barra va ridisegnata una volta.
  // Le volte successive non serve, ma ridisegnare tre pulsanti costa poco e
  // condizionare il ridisegno a "era spento un attimo fa" vorrebbe dire tenere
  // un quarto flag di stato per risparmiare due millisecondi.
  impDisegnaCerchio();
  impDisegnaLettura();
  impDisegnaNav();
}

static void drawImpattoScreen() {
  tft.fillScreen(ArchColor::BG);
  drawBreadcrumb(true);
  impDisegnaScala();
  impDisegnaLettura();
  impDisegnaTasti();
  impDisegnaCerchio();
  impDisegnaNav();
}

// --- Helper: binario slider con porzione attiva e tacca ---------------------
// Fattorizzato perche' i due slider differiscono SOLO per dove parte la porzione
// attiva: la distanza riempie da SINISTRA (grandezza monopolare: "quanto"),
// l'elevazione riempie DALLO ZERO CENTRALE (bipolare: "da che parte, e quanto").
// Rendere visibile questa differenza e' il punto: la barra comunica la natura
// del dato prima ancora che si legga il numero.
// ============================================================================
//  Disegno dello slider verticale
// ============================================================================
//  drawSliderVert disegna TUTTO; aggiornaSliderVert ridisegna solo le due cose
//  che cambiano trascinando (numero e riempimento). Serve perche' ridisegnare
//  l'intero schermo a ogni campione di touch renderebbe il trascinamento a
//  scatti — ed era proprio la sensazione da eliminare.
struct SliderCfg {
    const char* titolo;
    const char* unita;
    int   vmin, vmax;
    uint16_t colore;
    bool  segno;        // true: mostra il + davanti ai positivi (elevazione)
    // F19: il tasto di SINISTRA cambia significato con la posizione nel flusso.
    // Sulla PRIMA schermata non esiste un "indietro", e quello spazio serve per
    // SCARTA — la difesa contro i falsi trigger, che sui dati del 28/07 erano
    // 26 catture su 66. Parametrizzarlo qui evita un secondo disegnatore di
    // barre che potrebbe divergere da questo (§4.1).
    const char* sinistra;
    uint16_t colSinistra;
};

// I FONT GRANDI DI TFT_eSPI SONO SOLO NUMERICI.
//  Il 6 e il 7 contengono cifre, punto, due punti e il MENO — ma non il piu',
//  e il 7 nemmeno le lettere. Scriverci "+5" o "30m" fa sparire in silenzio il
//  segno o l'unita': niente errore, solo un carattere che non c'e'.
//  Quindi: il numero (col meno) nel font grande, il "+" DISEGNATO come due
//  barrette, e l'unita' in un font alfanumerico piccolo accanto.
//
//  FASE 24a — le due funzioni che disegnavano il piu' e il meno VIVEVANO QUI,
//  private di questo file, mentre display.cpp ne aveva bisogno per la card di
//  ATTESA. Era la premessa esatta del difetto piu' costoso della 1.69: la
//  stessa costante (o lo stesso disegno) in due posti, che divergono al primo
//  ritocco. Adesso stanno in display.cpp, pubbliche, e qui ci sono solo due
//  alias locali che ne conservano il nome breve usato nel resto del file.
static inline void disegnaPiu(int16_t cx, int16_t cy, int16_t l, int16_t sp, uint16_t col) {
    displaySegnoPiu(cx, cy, l, sp, col);
}
static inline void disegnaMeno(int16_t cx, int16_t cy, int16_t l, int16_t sp, uint16_t col) {
    displaySegnoMeno(cx, cy, l, sp, col);
}

// Solo il numero, col meno se serve. Segno positivo e unita' li disegna il
// chiamante: nel font grande non esistono.
static void formattaValore(char* out, size_t n, int v, const SliderCfg& c) {
    (void)c;
    snprintf(out, n, "%d", v);
}

static void aggiornaSliderVert(int v, const SliderCfg& c) {
    // --- numero, IN ALTO: il dito sta in mezzo e non lo copre mai ------------
    tft.fillRect(0, 12, ARCHBB_W, 50, ArchColor::BG);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(c.colore, ArchColor::BG);

    // --- FASE 24a: la grandezza BIPOLARE la disegna displayElevazione -------
    //  Lo slider dell'elevazione e la card di ATTESA mostrano lo STESSO numero
    //  a due secondi di distanza. Devono essere identici, o l'arciere impara
    //  che "il numero cambia aspetto" e smette di fidarsi del colpo d'occhio.
    //  Un solo disegnatore, chiamato da due posti: nessuna possibilita' che
    //  divergano. In piu' qui si guadagna gratis il colore e la freccia.
    if (c.segno) {
        displayElevazione(ARCHBB_W / 2, 36, (int8_t)v, 7, ArchColor::BG);
    } else {
        char buf[12]; formattaValore(buf, sizeof(buf), v, c);

        // Grandezza MONOPOLARE (la distanza): numero + unita', nessun segno.
        // Larghezza dei pezzi accessori, per centrare l'insieme e non il numero.
        const int16_t wNum  = tft.textWidth(buf, 7);
        const int16_t wUni  = (c.unita[0]) ? tft.textWidth(c.unita, 4) + 4 : 0;
        const int16_t x0    = (ARCHBB_W - (wNum + wUni)) / 2;

        tft.setTextDatum(ML_DATUM);
        tft.drawString(buf, x0, 36, 7);
        if (wUni) {
            tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
            tft.drawString(c.unita, x0 + wNum + 4, 44, 4);
        }
        tft.setTextDatum(MC_DATUM);
    }

    // --- traccia: parte piena dal basso fino al valore -----------------------
    const int16_t y = valToY(v, c.vmin, c.vmax);
    tft.fillRect(VS_TRACK_X0, VS_TRACK_Y0, VS_TRACK_X1 - VS_TRACK_X0,
                 VS_TRACK_H, ArchColor::BG_CARD);
    tft.fillRect(VS_TRACK_X0, y, VS_TRACK_X1 - VS_TRACK_X0,
                 VS_TRACK_Y1 - y, c.colore);
    tft.drawRect(VS_TRACK_X0, VS_TRACK_Y0, VS_TRACK_X1 - VS_TRACK_X0,
                 VS_TRACK_H, ArchColor::BORDER);
    // Cursore: una barra chiara alla quota corrente, che sporge dai lati cosi'
    // resta visibile anche col dito appoggiato in mezzo.
    tft.fillRect(VS_TRACK_X0 - 8, y - 3, (VS_TRACK_X1 - VS_TRACK_X0) + 16, 6,
                 ArchColor::TEXT);
}

static void drawSliderVert(int v, const SliderCfg& c) {
    tft.fillScreen(ArchColor::BG);

    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
    tft.drawString(c.titolo, ARCHBB_W / 2, 2, 2);

    // --- zone laterali + e - ------------------------------------------------
    //  A ridosso dei bordi: il pollice le trova senza guardare, e non
    //  interferiscono con la traccia al centro.
    tft.fillRoundRect(2, VS_ZONE_Y0, VS_PLUS_X1 - 2, VS_ZONE_Y1 - VS_ZONE_Y0,
                      8, ArchColor::BG_CARD);
    tft.fillRoundRect(VS_MINUS_X0, VS_ZONE_Y0, ARCHBB_W - VS_MINUS_X0 - 2,
                      VS_ZONE_Y1 - VS_ZONE_Y0, 8, ArchColor::BG_CARD);
    const int16_t cy = (VS_ZONE_Y0 + VS_ZONE_Y1) / 2;
    disegnaPiu (VS_PLUS_X1 / 2, cy, 30, 7, ArchColor::TEXT2);
    disegnaMeno((VS_MINUS_X0 + ARCHBB_W) / 2, cy, 30, 7, ArchColor::TEXT2);

    // --- estremi della scala, accanto alla traccia --------------------------
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
    // Qui il font 1 e' alfanumerico completo: segno e unita' si possono scrivere.
    char lo[12], hi[12];
    if (c.segno) { snprintf(hi, sizeof(hi), "%+d%s", c.vmax, c.unita);
                   snprintf(lo, sizeof(lo), "%+d%s", c.vmin, c.unita); }
    else         { snprintf(hi, sizeof(hi), "%d%s",  c.vmax, c.unita);
                   snprintf(lo, sizeof(lo), "%d%s",  c.vmin, c.unita); }
    tft.drawString(hi, VS_TRACK_X1 + 4, VS_TRACK_Y0 + 6, 1);
    tft.drawString(lo, VS_TRACK_X1 + 4, VS_TRACK_Y1 - 6, 1);

    // --- navigazione in basso ----------------------------------------------
    tft.fillRoundRect(VS_NAV_M, VS_NAV_Y, VS_NAV_W, VS_NAV_H, 8,
                      ArchColor::BG_CARD);
    tft.drawRoundRect(VS_NAV_M, VS_NAV_Y, VS_NAV_W, VS_NAV_H, 8,
                      c.colSinistra);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(c.colSinistra, ArchColor::BG_CARD);
    tft.drawString(c.sinistra, VS_NAV_M + VS_NAV_W / 2, VS_NAV_Y + VS_NAV_H / 2, 2);

    const int16_t okx = ARCHBB_W - VS_NAV_M - VS_NAV_W;
    tft.fillRoundRect(okx, VS_NAV_Y, VS_NAV_W, VS_NAV_H, 8, ArchColor::ACCENT);
    tft.setTextColor(ArchColor::BG, ArchColor::ACCENT);
    tft.drawString("OK", okx + VS_NAV_W / 2, VS_NAV_Y + VS_NAV_H / 2, 4);

    aggiornaSliderVert(v, c);
}

// Le due configurazioni. Un solo posto: la schermata, il trascinamento e i
// pulsanti +/- leggono da qui, non da numeri sparsi.
static const SliderCfg CFG_DIST = { "DISTANZA", "m", DIST_MIN, DIST_MAX,
                                    ArchColor::ACCENT, false,
                                    "SCARTA", ArchColor::BAD };

// ============================================================================
//  FASE 22 — MODO PRE-TIRO
// ============================================================================
//  Le stesse due schermate di slider servono adesso a due scopi diversi:
//    - PRIMA del tiro (pre-tiro): armano il bersaglio. Si arriva dalla card in
//      ATTESA e si torna in ATTESA.
//    - DOPO il tiro (rientro da CORREGGI): correggono un tiro gia' registrato.
//  Non si duplica il disegnatore — sarebbero due funzioni che divergono al
//  primo ritocco (§4.1). Cambia solo la CONFIGURAZIONE, che il disegnatore
//  gia' accetta come parametro.
//
//  L'unica differenza reale e' il tasto di sinistra sulla prima schermata:
//  dopo il tiro e' SCARTA (la difesa contro i falsi trigger); prima del tiro
//  non c'e' niente da scartare, e quel tasto e' ANNULLA — si esce lasciando
//  armato quello che era armato prima.
static const SliderCfg CFG_DIST_PRE = { "PRE-TIRO  DISTANZA", "m", DIST_MIN, DIST_MAX,
                                        ArchColor::ACCENT, false,
                                        "ANNULLA", ArchColor::BORDER };
static const SliderCfg CFG_ELEV = { "ELEVAZIONE", "", ELEV_MIN, ELEV_MAX,
                                    ArchColor::AMBER, true,
                                    "< INDIETRO", ArchColor::BORDER };
static const SliderCfg CFG_ELEV_PRE = { "PRE-TIRO  ELEVAZIONE", "", ELEV_MIN, ELEV_MAX,
                                        ArchColor::AMBER, true,
                                        "< INDIETRO", ArchColor::BORDER };

// Una sola funzione decide quale configurazione vale, e la usano SIA il disegno
// SIA l'handler del tocco. Se il disegno usasse una cfg e l'handler un'altra, i
// limiti vmin/vmax potrebbero divergere: e' esattamente la classe di bug del
// §4.1, applicata a un parametro invece che a una coordinata.
static const SliderCfg& cfgDist() { return s_pretiro ? CFG_DIST_PRE : CFG_DIST; }
static const SliderCfg& cfgElev() { return s_pretiro ? CFG_ELEV_PRE : CFG_ELEV; }

static void drawDistanzaScreen()   { drawSliderVert(s_distSel, cfgDist()); }
static void drawElevazioneScreen() { drawSliderVert(s_elevSel, cfgElev()); }

// ============================================================================
//  Trascinamento e tap sulle due schermate di slider
// ============================================================================
//  Il TRASCINAMENTO agisce mentre il dito e' giu' (nessuna attesa del rilascio:
//  era quello a far sembrare lo slider due tap). Il TAP sulle zone +/- e sulla
//  navigazione agisce al rilascio, come le altre schermate.
static void dragSlider(uint16_t x, uint16_t y, int& valore, const SliderCfg& c) {
    if (!inTraccia(x, y)) return;
    int nuovo = yToVal(y, c.vmin, c.vmax);
    if (nuovo != valore) {
        valore = nuovo;
        aggiornaSliderVert(valore, c);   // ridisegno PARZIALE: resta fluido
    }
}

// Ritorna 1 = CORREGGI, 2 = OK, 0 = niente. Le zone +/- le applica da sola.
static uint8_t tapSlider(uint16_t x, uint16_t y, int& valore, const SliderCfg& c) {
    if (y >= VS_NAV_Y && y < VS_NAV_Y + VS_NAV_H) {
        if (x >= VS_NAV_M && x < VS_NAV_M + VS_NAV_W) return 1;
        const int16_t okx = ARCHBB_W - VS_NAV_M - VS_NAV_W;
        if (x >= okx && x < okx + VS_NAV_W) return 2;
        return 0;
    }
    if (y >= VS_ZONE_Y0 && y <= VS_ZONE_Y1) {
        int prima = valore;
        if (x < VS_PLUS_X1 && valore < c.vmax) valore++;
        else if (x >= VS_MINUS_X0 && valore > c.vmin) valore--;
        if (valore != prima) aggiornaSliderVert(valore, c);
    }
    return 0;
}

// ----------------------------------------------------------------------------
//  Azzeramento del marcatore. Serve ogni volta che si (ri)entra in IMPATTO e
//  ogni volta che cambia l'esito: colpito e mancato hanno raggi diversi, e un
//  marcatore ereditato dall'altra scala sarebbe un dato falso, non un residuo
//  grafico.
static void impReset() {
  s_impXcm = IMP_NOT_SET; s_impYcm = IMP_NOT_SET;
  s_impPx = IMP_CX; s_impPy = IMP_CY;
  s_impSet = false;
  // F21: anche la serie di accelerazione appartiene a QUESTO tiro. Lasciarla
  // viva significherebbe che il primo tocco del tiro successivo eredita la
  // velocita' dell'ultimo del precedente.
  s_impTapZona = 0; s_impTapMs = 0; s_impTapRun = 0;
}

// ----------------------------------------------------------------------------
//  Ripristino del marcatore da un risultato gia' chiuso (rientro da CORREGGI).
// ----------------------------------------------------------------------------
//  Le coordinate salvate sono in CENTIMETRI, che sono assoluti; i pixel no. Se
//  nel frattempo e' cambiato il raggio in config, il punto si ricolloca sulla
//  nuova scala — e se cade fuori dal cerchio viene riportato sul bordo, con i
//  centimetri RICALCOLATI dal pixel saturato. Cosi' quello che si legge a
//  schermo e quello che si salverebbe restano la stessa cosa: un marcatore sul
//  bordo che dichiarasse ancora il valore originale sarebbe una bugia
//  silenziosa, e le bugie silenziose sono quelle che sopravvivono.
static void impRipristina() {
  impReset();
  if (!impIsSet(s_result.imp_x_cm) || !impIsSet(s_result.imp_y_cm)) return;

  float px = (float)cmToPx(s_result.imp_x_cm);
  float py = (float)cmToPx(s_result.imp_y_cm);      // positivo = alto
  float r  = sqrtf(px*px + py*py);
  if (r > (float)IMP_R_PX) { px = px * (float)IMP_R_PX / r; py = py * (float)IMP_R_PX / r; }

  s_impPx  = (int16_t)(IMP_CX + lroundf(px));
  s_impPy  = (int16_t)(IMP_CY - lroundf(py));
  s_impXcm = pxToCm((int16_t)lroundf(px));
  s_impYcm = pxToCm((int16_t)lroundf(py));
  s_impSet = true;
}

// Chiusura del percorso normale. Un solo punto in cui il risultato diventa
// definitivo: la zona si DERIVA qui, l'ARCS si codifica qui.
static void finalizzaImpatto() {
  s_result.imp_x_cm      = s_impXcm;
  s_result.imp_y_cm      = s_impYcm;
  s_result.imp_raggio_cm = impRaggioCm();
  s_result.zona     = impattoToZona(s_impXcm, s_impYcm, s_result.imp_raggio_cm);
  s_result.arcs     = encodeArcs(s_result.colpito, s_result.zona, s_result.distanza_m);
  s_result.valutato = true;
  s_result.esito    = EsitoScoring::VALUTATO;
  s_state = ScoringState::DONE;
}

// --- schermata 1: DISTANZA. Il tasto di sinistra e' SCARTA ------------------
static void handleDistanzaTap(uint16_t x, uint16_t y) {
    int v = s_distSel;
    uint8_t nav = tapSlider(x, y, v, cfgDist());
    s_distSel = (uint8_t)v;
    if (nav == 1) {
      if (s_pretiro) {
        // ANNULLA: si esce senza toccare i valori armati. s_pretiroOk resta
        // false, e il chiamante tiene quelli che aveva.
        s_pretiroOk = false;
        s_state = ScoringState::DONE;
        return;
      }
      s_result.valutato = false;             // SCARTA: non era un tiro
      s_result.esito    = EsitoScoring::SCARTATO;
      s_state = ScoringState::DONE;
    } else if (nav == 2) {
      // Il valore si fissa SUBITO nel risultato, non alla fine del percorso: se
      // lo scoring si interrompe piu' avanti, la distanza e' comunque salvata.
      s_result.distanza_m = s_distSel;
      s_lastDist = s_distSel;
      s_state = ScoringState::ELEVAZIONE; s_needRedraw = true;
    }
}

// --- schermata 2: ELEVAZIONE ------------------------------------------------
static void handleElevazioneTap(uint16_t x, uint16_t y) {
    int v = s_elevSel;
    uint8_t nav = tapSlider(x, y, v, cfgElev());
    s_elevSel = (int8_t)v;
    if (nav == 1) { s_state = ScoringState::DISTANZA; s_needRedraw = true; }
    else if (nav == 2) {
      s_result.elev_deg = s_elevSel;
      s_lastElev = s_elevSel;
      if (s_pretiro) {
        // Fine del pre-tiro: NON si passa a ESITO — non c'e' ancora nessun
        // tiro da valutare. Si chiude, e il chiamante torna in ATTESA.
        s_pretiroOk = true;
        s_state = ScoringState::DONE;
        return;
      }
      s_state = ScoringState::ESITO; s_needRedraw = true;
    }
}

// --- schermata 3: ESITO -----------------------------------------------------
static void handleEsitoTap(uint16_t x, uint16_t y) {
  uint8_t nav = hitNavBar(x, y, true, true);
  if (nav == 1) {                            // INDIETRO -> elevazione
    s_state = ScoringState::ELEVAZIONE; s_needRedraw = true; return;
  }
  if (nav == 2) {                            // SALTA: tiro senza impatto
    s_result.valutato = false;
    s_result.esito    = EsitoScoring::SALTATO;
    s_state = ScoringState::DONE; return;
  }
  const int bh = 72, y1 = 54, y2 = 54 + bh + 12;
  if (y >= y1 && y < y1 + bh) {
    s_result.colpito = true;  impReset();
    s_state = ScoringState::IMPATTO; s_needRedraw = true;
  } else if (y >= y2 && y < y2 + bh) {
    s_result.colpito = false; impReset();
    s_state = ScoringState::IMPATTO; s_needRedraw = true;
  }
}

// --- schermata 4: IMPATTO ---------------------------------------------------
static void handleImpattoTap(uint16_t x, uint16_t y) {
  uint8_t nav = impHitNav(x, y);
  if (nav == 1) {                            // INDIETRO -> esito
    s_state = ScoringState::ESITO; s_needRedraw = true; return;
  }
  if (nav == 2) {                            // NON SO: sentinella, non zero
    impReset();
    finalizzaImpatto();
    return;
  }
  if (nav == 3) {                            // OK: conferma esplicita (F21b)
    finalizzaImpatto();
    return;
  }
  if (y >= NAV_Y) return;

  // --- F21: TASTI DI SPOSTAMENTO ------------------------------------------
  //  Vanno interrogati PRIMA del cerchio: le fasce laterali e il cerchio non si
  //  sovrappongono per costruzione (static_assert), ma l'ordine esplicito
  //  rende la precedenza un fatto del codice e non una conseguenza della
  //  geometria — che domani potrebbe cambiare.
  const uint8_t k = impZonaTasto(x, y);
  if (k != IMPK_NONE) {
    const uint32_t now = millis();
    // Serie di tocchi: stesso tasto entro IMP_ACCEL_MS -> il passo cresce.
    // Tasto diverso, o troppo tempo passato -> si riparte da capo.
    if (k == s_impTapZona && (now - s_impTapMs) <= IMP_ACCEL_MS) {
      if (s_impTapRun < 255) s_impTapRun++;
    } else {
      s_impTapRun = 1;
    }
    s_impTapZona = k;
    s_impTapMs   = now;

    const int16_t passo = (int16_t)impPassoCm() * (int16_t)impMoltiplicatore();
    switch (k) {
      case IMPK_SX:  impSposta(-passo, 0); break;
      case IMPK_DX:  impSposta(+passo, 0); break;
      case IMPK_SU:  impSposta(0, +passo); break;   // +y = ALTO (v. scoring.h)
      case IMPK_GIU: impSposta(0, -passo); break;
      default: break;
    }
    return;
  }
  // Tocco fuori dai tasti: la serie di accelerazione finisce qui. Senza questo
  // azzeramento, "destra destra destra, tocco sul cerchio, destra" ripartirebbe
  // dal quarto passo accelerato invece che dal primo.
  s_impTapZona = IMPK_NONE;
  s_impTapRun  = 0;

  float dx = (float)x - (float)IMP_CX;
  float dy = (float)y - (float)IMP_CY;
  float r  = sqrtf(dx*dx + dy*dy);

  // ========================================================================
  //  F21b — FUORI DAL CERCHIO IL TOCCO SI IGNORA. Prima si SATURAVA sul bordo.
  // ========================================================================
  //  La regola della F19 diceva: un tocco ignorato costringe a ripetere, un
  //  tocco saturato dice "almeno cosi' fuori". Era ragionevole quando il
  //  cerchio era R=100 px e occupava quasi tutto lo schermo: li' un tocco
  //  fuori dal cerchio era un tocco fuori dal bersaglio, e riportarlo sul
  //  bordo diceva la verita'.
  //
  //  Con R=50 px non e' piu' cosi'. La corona fra il cerchio e le fasce e'
  //  stretta, sta proprio dove il pollice passa scendendo, e un tocco che la
  //  sfiora produce un SALTO al limite estremo — dieci, trenta, centocinquanta
  //  centimetri in un colpo, secondo la scala. E' il difetto visto in campo sul
  //  mancato scendendo dall'alto: nessun errore di aritmetica, la saturazione
  //  che fa esattamente quello per cui era stata scritta, in un contesto in cui
  //  non serve piu'.
  //
  //  Adesso quella zona e' inerte, e non si perde niente: per andare oltre il
  //  bordo ci sono i quattro tasti, che saturano un passo per volta e sotto gli
  //  occhi. Un tocco ignorato costa un tocco; un salto di centocinquanta
  //  centimetri accettato per distrazione costa il dato.
  if (r > (float)IMP_R_PX) return;

  s_impPx  = (int16_t)(IMP_CX + lroundf(dx));
  s_impPy  = (int16_t)(IMP_CY + lroundf(dy));
  s_impXcm = pxToCm((int16_t)lroundf(dx));
  s_impYcm = pxToCm((int16_t)lroundf(-dy));   // lo schermo cresce in giu', il dato in su
  const bool primo = !s_impSet;
  s_impSet = true;
  impDisegnaCerchio();
  impDisegnaLettura();
  if (primo) impDisegnaNav();                 // l'OK si accende
}


// ============================================================================
//  API
// ============================================================================
void scoringStart(uint16_t shotId, uint8_t dist_m, int8_t elev_deg) {
  s_shotId  = shotId;
  s_pretiro = false;
  // FASE 22 — si parte da ESITO, non piu' da DISTANZA.
  //  Il contesto (distanza, elevazione) e' gia' stato armato PRIMA dello
  //  scocco: chiederlo di nuovo adesso significherebbe chiedere due volte la
  //  stessa cosa e dare all'arciere l'occasione di rispondere in modo diverso.
  //  Quello che resta da raccogliere e' solo l'OSSERVAZIONE: dove e' andata.
  //  Il contesto resta comunque raggiungibile all'indietro (ESITO -> ELEVAZIONE
  //  -> DISTANZA), perche' accorgersi dopo il tiro che la distanza era quella
  //  della piazzola prima e' un caso reale, e serve un modo di rimediare.
  s_state   = ScoringState::ESITO;
  // ATTENZIONE al reset di elev_deg: NON puo' essere 0. L'initializer
  // posizionale della Fase 3 aveva 7 campi; ora la struct ne ha 8, e lasciare
  // che elev_deg cada a 0 per default significherebbe dichiarare "tiro in
  // piano" su OGNI tiro mai misurato — proprio il bug che la sentinella esiste
  // per prevenire. Uso quindi i nomi dei campi: se domani la struct cresce
  // ancora, il compilatore non silenzia l'errore.
  s_result = ScoreResult{};
  s_result.elev_deg = ELEV_NOT_SET;
  s_result.esito = EsitoScoring::SCARTATO;   // finche' non si decide altro
  // Stessa ragione della sentinella elev: lo ZERO e' un valore legittimo delle
  // coordinate d'impatto (centro esatto), quindi non puo' fare da "non
  // rilevato". L'azzeramento della struct lascerebbe 0/0 e mentirebbe.
  s_result.zona     = ZONA_IGNOTA;
  s_result.imp_x_cm = IMP_NOT_SET;
  s_result.imp_y_cm = IMP_NOT_SET;
  s_result.imp_raggio_cm = 0;
  // F20: stessa disciplina. Lo zero e' un tempo di mira legittimo (rilascio
  // immediato), quindi non puo' significare "mai misurato".
  s_result.tempo_mira_ms   = TEMPO_NOT_SET;
  s_result.tempo_alzata_ms = TEMPO_NOT_SET;
  s_result.assetto_ok      = 2;   // 2 = angoli non calcolati
  impReset();

  // FASE 22 — il bersaglio armato entra SUBITO nel risultato.
  //  Non alla fine del percorso: se lo scoring si interrompe (timeout, SALTA),
  //  la riga che finisce su microSD deve comunque portare la distanza a cui si
  //  stava tirando. E' la stessa regola gia' applicata alla distanza nella F19
  //  ("il valore si fissa subito nel risultato"), applicata un passo prima.
  s_result.distanza_m = dist_m;
  s_result.elev_deg   = elev_deg;
  s_distSel = dist_m;
  s_elevSel = elev_deg;
  s_lastDist = dist_m;
  s_lastElev = elev_deg;

  s_needRedraw   = true;
  s_stableTouch  = false;
  s_relCount     = 0;
  s_lastActionMs = millis();
  kickTimeout();
}

// ----------------------------------------------------------------------------
//  FASE 22 — pre-tiro: le stesse schermate, prima dello scocco.
// ----------------------------------------------------------------------------
void scoringStartPretiro(uint8_t dist_m, int8_t elev_deg) {
  s_pretiro   = true;
  s_pretiroOk = false;
  s_state     = ScoringState::DISTANZA;
  s_distSel   = dist_m;
  s_elevSel   = elev_deg;
  // s_result NON si tocca: qui non c'e' nessun tiro. Verra' azzerato da
  // scoringStart() quando la freccia partira' davvero. Toccarlo adesso
  // significherebbe tenere in giro un mezzo risultato senza un tiro dietro.
  s_needRedraw   = true;
  s_stableTouch  = false;
  s_relCount     = 0;
  s_lastActionMs = millis();
  kickTimeout();
}

bool scoringPretiroConfermato() { return s_pretiroOk; }

void scoringGetPretiro(uint8_t& dist_m, int8_t& elev_deg) {
  dist_m   = s_distSel;
  elev_deg = s_elevSel;
}

void scoringResume() {
  // Re-editing: NON azzero s_result. Riparto da ESITO ma le scelte precedenti
  // restano il punto di partenza. Se il tiro era gia' valutato, riporto
  // s_distSel al valore scelto cosi' la schermata distanza lo ripropone.
  // F19: il CORREGGI del riepilogo rientra DIRETTAMENTE nella schermata
  // impatto, non in cima al percorso. Col nuovo ordine l'impatto e' l'ultima
  // schermata, ed e' anche l'unica cosa che si vorra' davvero correggere: farsi
  // rifare quattro schermate per spostare un puntino di dieci centimetri era un
  // effetto collaterale del riordino, non una scelta. Il percorso completo
  // resta raggiungibile risalendo con l'INDIETRO che c'e' gia'.
  s_state = ScoringState::IMPATTO;
  if (s_result.distanza_m > 0) s_distSel = s_result.distanza_m;
  // v2.16: idem per l'elevazione. Qui la sentinella si propaga NATURALMENTE: se
  // il tiro era stato chiuso senza misurarla, s_result.elev_deg vale
  // ELEV_NOT_SET e ripartiamo da "non impostata" — che e' giusto, perche' il
  // CORREGGI e' proprio il momento in cui rimediare a quella dimenticanza.
  s_elevSel = s_result.elev_deg;
  if (!elevIsSet(s_elevSel)) s_elevSel = ELEV_DEFAULT;
  impRipristina();             // marcatore dov'era, countdown fermo
  s_result.valutato = false;   // torna "in editing" finche' non riconferma
  s_needRedraw   = true;
  s_stableTouch  = false;
  s_relCount     = 0;
  s_lastActionMs = millis();
  kickTimeout();
}
bool scoringActive() {
  return s_state != ScoringState::IDLE && s_state != ScoringState::DONE;
}
ScoringState scoringGetState() { return s_state; }
ScoreResult  scoringGetResult() { return s_result; }

ScoringState scoringUpdate(const TouchData& td) {
  if (!scoringActive()) return s_state;

  if (s_needRedraw) {
    switch (s_state) {
      case ScoringState::DISTANZA:   drawDistanzaScreen();   break;
      case ScoringState::ELEVAZIONE: drawElevazioneScreen(); break;
      case ScoringState::ESITO:      drawEsitoScreen();      break;
      case ScoringState::IMPATTO:    drawImpattoScreen();    break;
      default: break;
    }
    s_needRedraw = false;
  }

  if (millis() - s_lastTouchMs > SCORING_TIMEOUT_MS) {
    // TIMEOUT = SCARTA. Se lo scoring resta aperto un minuto, quasi sempre
    // significa che il trigger ha preso un movimento e nessuno sta valutando
    // nulla: scriverlo su SD produce righe di maneggio come le 26 del 28/07.
    // Meglio perdere un tiro vero dimenticato che sporcare la sessione.
    s_result.valutato = false;
    s_result.esito    = EsitoScoring::SCARTATO;
    s_state = ScoringState::DONE;
    return s_state;
  }

  // ---- DEBOUNCE ------------------------------------------------------------
  // Stato istantaneo del tocco.
  bool rawTouch = td.valid && (td.fingers > 0);
  if (rawTouch) {
    kickTimeout();
    s_pressX = td.x; s_pressY = td.y;   // memorizzo l'ultima posizione premuta
    s_relCount = 0;
    // TRASCINAMENTO DAL VIVO. Prima il valore si applicava solo al rilascio, e
    // per questo lo slider si comportava come due tap invece che come uno
    // slider. Qui agisce mentre il dito e' giu', con ridisegno parziale.
    if (s_state == ScoringState::DISTANZA) {
      int v = s_distSel; dragSlider(td.x, td.y, v, CFG_DIST);
      s_distSel = (uint8_t)v;
    } else if (s_state == ScoringState::ELEVAZIONE) {
      int v = s_elevSel; dragSlider(td.x, td.y, v, CFG_ELEV);
      s_elevSel = (int8_t)v;
    }
    if (!s_stableTouch) s_stableTouch = true;   // press: immediato (reattivo)
  } else {
    // Rilascio: conta i campioni di non-tocco. Solo dopo N stabili confermo.
    if (s_stableTouch) {
      if (++s_relCount >= RELEASE_STABLE_N) {
        // Rilascio confermato: e' un tap. Applico SE fuori dal cooldown.
        s_stableTouch = false;
        s_relCount = 0;
        uint32_t now = millis();
        if (now - s_lastActionMs >= ACTION_COOLDOWN_MS) {
          s_lastActionMs = now;
          switch (s_state) {
            case ScoringState::DISTANZA:   handleDistanzaTap(s_pressX, s_pressY);   break;
            case ScoringState::ELEVAZIONE: handleElevazioneTap(s_pressX, s_pressY); break;
            case ScoringState::ESITO:      handleEsitoTap(s_pressX, s_pressY);      break;
            case ScoringState::IMPATTO:    handleImpattoTap(s_pressX, s_pressY);    break;
            default: break;
          }
        }
      }
    }
  }

  return s_state;
}
