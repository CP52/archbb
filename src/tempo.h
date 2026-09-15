// ============================================================================
//  ArchBB 1.83 — tempo.h · FASE 20 (tempo di mira e di alzata)
//  Cesare Pagura · Padova/Noale IT · 1 agosto 2026
// ----------------------------------------------------------------------------
//  COSA MISURA, E PERCHE' NON BASTAVA IL BURST
//
//  Il burst copre 2000 ms prima dello scocco. Misurato sui 47 tiri validi della
//  seduta del 28/07, il tempo di quiete che precede il rilascio ha mediana
//  1924 ms con quartili 1908/1934, contro un buffer che si estende a 2058 ms:
//  la misura e' SATURATA CONTRO IL BORDO. Quando il buffer si apre l'arco e'
//  gia' in trazione da un pezzo. Solo tre tiri su 47 mostrano un punto di
//  assestamento dentro la finestra, e sono verosimilmente riassestamenti a
//  meta' mira, non l'inizio della trazione.
//
//  Allungare la finestra avrebbe funzionato (30 byte a campione: da 2 a 6
//  secondi il burst passa da 20 a 46 KB, la PSRAM non se ne accorge) ma avrebbe
//  spostato il problema invece di risolverlo: chi trattiene otto secondi
//  sarebbe di nuovo tagliato. Qui non serve la TRACCIA della trazione, servono
//  tre ISTANTI. Tre istanti si tengono in tre variabili.
//
//  I TRE ISTANTI
//    t_alzata    l'arco lascia la quiete e si muove  (|omega| oltre soglia)
//    t_ancora    comincia l'ultima quiete continua   (|omega| e ||a|-g| sotto
//                soglia per T_QUIETE_MS, in postura di tiro)
//    t_rilascio  lo scocco, gia' rilevato dal trigger
//
//  da cui   tempo_mira_ms   = t_rilascio - t_ancora
//           tempo_alzata_ms = t_ancora  - t_alzata
//
//  ══ CORREZIONE DEL 15/08 — dove va letta la misura ══
//
//  La prima stesura calcolava  mira = t_trigger − t_ancora,  leggendo lo stato
//  vivo dentro doFire(). Sul campo ha prodotto un tempo su DODICI tiri.
//
//  Motivo, verificato sui burst del 15/08: il movimento del rilascio supera la
//  soglia di MOTO da 30 a 80 ms PRIMA che il trigger scatti. La macchina passa
//  correttamente a MOTO e dimentica l'ancora — giustamente, perche' l'arco si
//  sta muovendo davvero — e quando doFire() legge, l'ancora non c'e' piu'. I
//  due tiri riusciti sono esattamente i due in cui il picco del giroscopio e'
//  arrivato al trigger o dopo.
//
//  Il numero era gia' noto: il briefing del 31/07 misura il rilascio a mediana
//  −46 ms dal trigger. E' lo stesso fatto che fonda la finestra causale dei
//  17 ms, e la macchina era scritta come se non lo fosse.
//
//  Correzione: la durata si CONGELA QUANDO ANCORA FINISCE, non quando scatta il
//  trigger. Non e' una toppa, e' la definizione giusta — l'uscita da ANCORA
//  *e'* il rilascio, rilevato dalla stessa soglia che l'analisi causale usa. Si
//  guadagna in piu' l'eliminazione del ritardo di 46 ms del trigger, che era
//  stato dichiarato "irrilevante, sotto l'1,5%" ma restava un errore
//  sistematico gratuito.
//
//  doFire() accetta la durata congelata solo se l'ancora e' finita da meno di
//  TEMPO_LATCH_MAX_MS: senza quel limite, un tiro erediterebbe l'ancora di due
//  minuti prima.
//
//  AVVERTENZA SULL'ALZATA (banco Fase 20, mai finita nel file consegnato per un
//  str.replace senza assert — la regola c'era, e' stata violata lo stesso).
//  Il cronometro dell'alzata parte quando l'arco entra IN POSTURA, non quando
//  comincia a muoversi da terra: il gate di postura ha bisogno di qualche
//  decimo perche' la stima della verticale segua il cambio di assetto. Su uno
//  scenario con 900 ms di alzata la macchina ne dichiara 620. Il tempo di mira
//  invece e' esatto, perche' e' ancorato all'inizio di una quiete e non a un
//  fronte di movimento. Due metriche di qualita' diversa: la prima e'
//  indicativa, la seconda e' una misura.
//
//  DEFINIZIONE ONESTA: quello che si misura e' il TEMPO DI QUIETE PRIMA DEL
//  RILASCIO. Coincide col tempo di mira solo se la trazione e' un movimento
//  continuo — ed e' cosi' che va inteso. Un riassestamento a meta' mira supera
//  la soglia di moto e FA RIPARTIRE il conteggio: e' voluto, perche' la
//  grandezza che la letteratura chiama "aiming duration" e' l'ultima fase
//  stabile prima del rilascio, non il tempo totale trascorso in trazione.
//
//  PRECISIONE. Il trigger scatta sull'urto dello scocco, cioe' ~29 ms dopo che
//  la freccia e' partita (briefing 31/07). Su una misura di 2-4 secondi sono
//  meno dell'1,5%: irrilevante qui, mentre sulla finestra causale di 17 ms era
//  tutto. La stessa imprecisione cambia peso a seconda di cosa si misura, e va
//  detto ogni volta, perche' la tentazione di riusare un numero "gia'
//  validato" fuori dal suo contesto e' esattamente l'errore dei 5 gradi.
//
//  LE SOGLIE NON SONO MISURATE. Sono scelte con margine largo a partire dal
//  rumore noto (2,9 dps nella finestra di mira, briefing 31/07) e vanno
//  validate sul campo. Per questo la Fase 20 porta con se' la riga di stato
//  dal vivo nella schermata di attesa: senza un modo di vedere la macchina
//  mentre gira, le soglie resterebbero un'opinione.
// ============================================================================
#pragma once
#include <Arduino.h>
#include "config.h"

// ----------------------------------------------------------------------------
//  Soglie — PROVVISORIE, da validare sul campo
// ----------------------------------------------------------------------------
//  Margini: l'oscillazione di mira misurata sta sotto i 3 dps, la trazione
//  ruota il riser di decine di gradi in circa un secondo (quindi 30-60 dps).
//  Le due soglie stanno larghe ai due lati di quell'intervallo vuoto.
static constexpr float    TEMPO_MOTO_DPS    = 25.0f;  // sopra = l'arco si muove
static constexpr float    TEMPO_QUIETE_DPS  =  8.0f;  // sotto = fermo
static constexpr float    TEMPO_QUIETE_MS2  =  0.8f;  // sotto = fermo (||a|-g|)
static constexpr uint16_t TEMPO_QUIETE_MS   = 150;    // quiete continua per entrare in ANCORA
static constexpr float    TEMPO_POSTURA_COS = 0.85f;  // stessa soglia del gate del trigger
static constexpr uint32_t TEMPO_MAX_MS      = 60000;  // oltre = non e' piu' una mira
// Quanto puo' essere "vecchia" un'ancora conclusa per valere ancora come quella
// di questo tiro. Il rilascio precede il trigger di ~46 ms (mediana, quartili
// −51/−39): 300 ms sono sei volte il margine, e restano largamente sotto
// l'intervallo fra due tiri.
static constexpr uint32_t TEMPO_LATCH_MAX_MS = 300;

// Sentinella: 0xFFFF = non misurato. Lo ZERO non poteva farla, e' un valore
// legittimo (rilascio immediato). Stessa disciplina di ELEV_NOT_SET e
// IMP_NOT_SET: una grandezza mai misurata non deve poter passare per zero.
static constexpr uint16_t TEMPO_NOT_SET = 0xFFFF;

// Stato della macchina, esposto per la riga di stato dal vivo.
// Perche' un tiro non ha una misura. Serve per la diagnosi sul campo: senza,
// un campo vuoto nel CSV non distingue "non sono mai stato fermo" da "l'ancora
// era troppo vecchia", e le due cose si correggono in modi opposti.
enum TempoMotivo : uint8_t {
  TEMPO_OK             = 0,
  TEMPO_NO_ANCORA      = 1,   // mai raggiunta una quiete confermata
  TEMPO_ANCORA_VECCHIA = 2    // c'era, ma finita da oltre TEMPO_LATCH_MAX_MS
};

enum TempoStato : uint8_t {
  TEMPO_RIPOSO = 0,   // fermo ma fuori postura, oppure appena riavviato
  TEMPO_MOTO   = 1,   // l'arco si sta muovendo (alzata o riassestamento)
  TEMPO_ANCORA = 2,   // quiete confermata in postura di tiro
};

// Risultato latchato all'istante dello scocco.
struct TempoTiro {
  uint16_t mira_ms;     // TEMPO_NOT_SET se non misurata
  uint16_t alzata_ms;   // TEMPO_NOT_SET se non si e' mai vista l'alzata
  uint8_t  motivo;      // TempoMotivo, quando mira_ms e' TEMPO_NOT_SET
};

void      tempo_init();
// Un campione, chiamato per OGNI campione IMU dal medesimo punto che alimenta
// il trigger: due percorsi di alimentazione diversi sarebbero due occasioni di
// perdere campioni in modo diverso.
void      tempo_feed_sample(const ImuSample& s);
// Chiamata da doFire(): congela i due tempi e li rende leggibili.
void      tempo_on_fire(uint32_t t_scocco_us);
// Ultimo risultato congelato.
TempoTiro tempo_ultimo();
// Stato corrente e millisecondi trascorsi nello stato (per la riga dal vivo).
TempoStato tempo_stato();
uint32_t   tempo_ms_nello_stato();
