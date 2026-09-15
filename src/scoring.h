// ============================================================================
//  ArchBB 1.83 — scoring.h · FASE 3 (porting scoring on-device)
//  Cesare Pagura · Padova/Noale IT · 11 luglio 2026
// ----------------------------------------------------------------------------
//  Macchina a stati dello scoring on-device, portata dalla 1.69 (v2.11.3) e
//  adattata alla 1.83. In Fase 3 NON ci sono ancora IMU e BLE: portiamo solo la
//  UI (le 3 schermate) e la logica. L'output e' una struct ScoreResult
//  autosufficiente: quando arriveranno IMU/BLE bastera' leggerla per popolare
//  lo ScorePacket, senza toccare la UI.
//
//  DIFFERENZE CHIAVE dalla 1.69 (perche' non e' un copia-incolla):
//   - Touch: usa il nostro modulo touch.h/.cpp gia' validato (config chip
//     corretta), NON il driver embedded in scoring_ui.cpp della 1.69.
//   - Pannello 240x284 (non 280): soglie zona ricalcolate in terzi puliti.
//   - Nessuno stato globale di firmware: lo scoring e' una sotto-macchina
//     autonoma che il main avvia e interroga.
// ----------------------------------------------------------------------------
#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <limits.h>
#include "touch.h"

// ----------------------------------------------------------------------------
//  Stati della sotto-macchina scoring.
// ----------------------------------------------------------------------------
//  FASE 19 — ORDINE NUOVO, e non e' un dettaglio estetico.
//
//  Prima era: ESITO -> ZONA -> DISTANZA -> ELEVAZIONE.
//  Adesso e': DISTANZA -> ELEVAZIONE -> ESITO -> IMPATTO.
//
//  Il motivo e' fisico, non di gusto. Distanza ed elevazione sono CONTESTO gia'
//  noto alla piazzola: si sanno prima ancora di scoccare. L'impatto e'
//  OSSERVAZIONE, e si compie dopo, guardando il bersaglio. Metterli nell'ordine
//  in cui la realta' li produce ha una conseguenza pratica: se lo scoring viene
//  abbandonato a meta' (il 31% dei tiri del 28/07 e' finito con arcs=0), quello
//  che resta salvato e' comunque utile, invece di essere il nulla.
enum class ScoringState : uint8_t {
  IDLE,        // inattiva (fuori dallo scoring)
  DISTANZA,    // schermata 1: DISTANZA, slider verticale a tutto schermo
  ELEVAZIONE,  // schermata 2: ELEVAZIONE, idem (F15: una grandezza per volta)
  ESITO,       // schermata 3: colpito il bersaglio? (verde/rosso)
  IMPATTO,     // schermata 4: dove ha colpito, cerchio graduato in centimetri
  DONE         // completata: ScoreResult pronto
};

// ----------------------------------------------------------------------------
//  Zona 3x3. Indice = colonna + 3*riga (come la 1.69).
// ----------------------------------------------------------------------------
//    0 = alto-sx    1 = alto     2 = alto-dx
//    3 = sx         4 = CENTRO   5 = dx
//    6 = basso-sx   7 = basso    8 = basso-dx
//  Semantica SPOT: se colpito E zona==4 (centro), l'etichetta e' "SPOT".
static constexpr uint8_t ZONA_CENTRO = 4;

// ----------------------------------------------------------------------------
//  FASE 19 — la zona non si sceglie piu': si DERIVA dalle coordinate d'impatto.
// ----------------------------------------------------------------------------
//  Restava una sola fonte di verita' possibile fra "zona" e "coordinate", e la
//  scelta e' obbligata: le coordinate contengono la zona, la zona non contiene
//  le coordinate. Memorizzare entrambe significherebbe due campi che possono
//  divergere — la regola §4.1 applicata ai dati invece che alle costanti.
//
//  ZONA_IGNOTA copre il caso reale del 3D: quindici frecce infisse nella
//  sagoma e nessun modo di sapere quale sia quella appena tirata. Obbligare a
//  indicare un punto in quel caso non produce un dato impreciso, produce un
//  dato INVENTATO — che e' peggio del dato mancante, perche' entra nelle
//  statistiche senza dichiararsi.
static constexpr uint8_t ZONA_IGNOTA = 255;

// ----------------------------------------------------------------------------
//  Coordinate d'impatto, in centimetri sul piano del bersaglio.
// ----------------------------------------------------------------------------
//  ORIGINE = IL PUNTO DI MIRA, non il centro della sagoma. E' la scelta che
//  rende il dato indipendente dalla forma dell'animale 3D (a sviluppo
//  verticale, orizzontale, a quattro zampe...) e che lo mette nello STESSO
//  sistema di riferimento in cui la fisica fa le previsioni: quello che si
//  vuole misurare e' lo scarto fra dove volevo mandarla e dove e' andata.
//
//  I SEGNI coincidono con le convenzioni misurate al banco il 31/07:
//     imp_x_cm > 0  =  destra   (stesso verso di yaw positivo)
//     imp_y_cm > 0  =  alto     (stesso verso di alzo positivo)
//  Non e' un vezzo: la regressione impatto/metrica si scrive senza cambi di
//  segno da ricordare, e i cambi di segno da ricordare sono la sorgente di
//  errore piu' documentata di questo progetto.
//
//  AVVERTENZA (elevazione): con bersagli a +-20 gradi il piano del bersaglio e'
//  inclinato. imp_y_cm e' misurato SULLA FACCIA, non in verticale vera: per la
//  verticale vera serve moltiplicare per cos(elev_deg). elev_deg e' registrato
//  accanto, quindi il dato e' recuperabile — ma va saputo adesso, non scoperto
//  fra sei mesi guardando una regressione che non torna.
static constexpr int16_t IMP_NOT_SET = INT16_MIN;

inline bool impIsSet(int16_t v) { return v != IMP_NOT_SET; }

// Deriva la zona 3x3 dalle coordinate. Le soglie sono a un terzo del raggio,
// cosi' la semantica resta quella della 1.69 e i dati vecchi e nuovi si
// confrontano. raggio_cm = il raggio del cerchio usato per quel tiro.
uint8_t impattoToZona(int16_t x_cm, int16_t y_cm, uint16_t raggio_cm);

// ----------------------------------------------------------------------------
//  ELEVAZIONE del bersaglio (v2.16 — portata dalla 1.69)
// ----------------------------------------------------------------------------
//  Il dato viene dal TELEMETRO (letto dall'arciere e digitato), NON dall'IMU.
//  Questa e' la differenza che governa tutto il design:
//
//    hold/release/cant/alzo nascono dai campioni grezzi -> se si perdono, si
//    RICALCOLANO. L'elevazione no: se non viene registrata al momento del tiro,
//    e' persa per sempre. Nessun calcolo la recupera.
//
//  Da qui la sentinella esplicita: ELEV_NOT_SET distingue "mai misurata" da
//  "misurata, tiro in piano". Lo ZERO NON puo' fare da sentinella perche' e' il
//  valore piu' FREQUENTE e legittimo (il tiro in piano). Confonderli
//  significherebbe non sapere piu', a posteriori, se un 0 nel CSV e' un dato o
//  un buco.
//
//    -128  = mai misurata (unico int8 fuori dal range +-127: non collide)
//       0  = misurata, tiro in piano
//   +-30   = range utile (un 3D oltre i 30 gradi non esiste in pratica)
static constexpr int8_t ELEV_MIN     = -30;
static constexpr int8_t ELEV_MAX     =  30;
static constexpr int8_t ELEV_NOT_SET = -128;

inline bool elevIsSet(int8_t e) { return e != ELEV_NOT_SET; }

// ----------------------------------------------------------------------------
//  Risultato dello scoring — la struct che il porting futuro leggera'.
// ----------------------------------------------------------------------------
//  Contiene TUTTO cio' che serve per costruire lo ScorePacket 1.69 (pkt_type
//  0x06) e la codifica ARCS, tranne gli angoli biomeccanici (cant/alzo) che
//  arriveranno con l'IMU. I campi angolo sono gia' previsti qui a 0 per non
//  cambiare la struct dopo.
// ----------------------------------------------------------------------------
//  Come si e' concluso lo scoring — decide COSA finisce sulla microSD.
// ----------------------------------------------------------------------------
//  Nasce dai dati del 28/07: su 66 catture, 26 non erano tiri ma l'arco
//  maneggiato o abbassato (|cant| medio 14,8 gradi contro 2,4, jerk 47 contro
//  214). Alcune sono state punteggiate per sbaglio, altri tiri veri sono
//  rimasti senza esito. Servono tre uscite distinte, non due.
enum class EsitoScoring : uint8_t {
  VALUTATO = 0,   // scoring completato: riga CSV + burst
  SALTATO  = 1,   // "e' un tiro ma non lo valuto": riga CSV + burst, senza punteggio
  SCARTATO = 2,   // "non era un tiro": NULLA sulla microSD
};

struct ScoreResult {
  bool     valutato;    // true se lo scoring e' stato completato (non SKIP/timeout)
  bool     colpito;     // true = freccia sul bersaglio
  uint8_t  zona;        // 0..8 (valida solo se colpito); 4=centro
  uint8_t  distanza_m;  // metri: 0=ignota, altrimenti 5..60 (INCLINATA, line-of-sight)
  int16_t  arcs;        // codifica ARCS = ±(dist*10 + (zona+1)); 0 = non valutato
  // --- ANGOLI d'assetto (Fase 4c: popolati da shot_angles_compute) ---
  int16_t  cant_cdeg;   // centesimi di grado (+dx / -sx)
  int16_t  alzo_cdeg;   // centesimi di grado (+punta su / -giu)
  // --- v2.16: elevazione del bersaglio dal telemetro ---
  // Campo NUOVO in coda alla struct: stessa regola dei pacchetti BLE e delle
  // colonne CSV (i campi nuovi vanno SEMPRE in fondo, mai in mezzo).
  int8_t   elev_deg;    // gradi interi; ELEV_NOT_SET = mai misurata

  // ==========================================================================
  //  FASE 4c — METRICHE BIOMECCANICHE dal burst IMU
  // ==========================================================================
  //  Aggiunte IN CODA (mai in mezzo): quando arrivera' il ScorePacket BLE e il
  //  salvataggio su SD, questi campi si serializzano in fondo, senza rompere la
  //  compatibilita' dei formati gia' esistenti. Sono l'output diretto di
  //  ShotAngles, gia' pronti per il display e per la futura persistenza.
  //
  //  I *_valid distinguono \"non calcolato\" da \"calcolato = 0\": un hold_deg=0
  //  con hold_valid=false e' un buco (burst troncato), non una tenuta perfetta.
  //  Stessa filosofia della sentinella elev, applicata alle metriche.
  bool     angles_valid;   // finestra pre-scocco sufficiente -> cant/alzo validi
  bool     angles_stable;  // finestra STABILE (|g|~9.8, sd bassa) -> assetto affidabile
  //  cant/alzo sono gia' sopra in centesimi (cant_cdeg/alzo_cdeg).
  bool     hold_valid;     // post-scocco sufficiente -> hold calcolato
  int16_t  hold_cdeg;      // follow-through: variazione elevazione a +900ms [cdeg]
  uint8_t  hold_class;     // HoldClass (TENUTO/LIEVE/ABBASSATO/ALZATO/NA)
  bool     release_valid;  // finestra 40ms sufficiente -> jerk calcolato
  uint16_t release_jerk;   // pulizia rilascio: max |d(ay)/dt| nei 40ms [m/s^3]
  uint8_t  release_class;  // ReleaseClass (PULITO/MEDIO/STRAPPO/NA)

  // --- F13: esito dello scoring (in CODA, come ogni campo nuovo) ------------
  //  `valutato` resta per compatibilita' ed e' ridondante con esito==VALUTATO.
  //  Chi decide se scrivere su SD deve guardare QUESTO campo: il booleano da
  //  solo non distingue "salta" da "scarta", e sono due cose opposte.
  EsitoScoring esito;

  // --- FASE 19: coordinate d'impatto (in CODA, come ogni campo nuovo) -------
  //  IMP_NOT_SET su entrambe = "non rilevato" (l'arciere ha scelto NON SO, o
  //  ha saltato). zona resta ZONA_IGNOTA in quel caso.
  int16_t  imp_x_cm;    // + = destra rispetto al punto di mira
  int16_t  imp_y_cm;    // + = alto   rispetto al punto di mira
  uint16_t imp_raggio_cm;  // raggio del cerchio usato: senza, i cm non hanno scala

  // --- FASE 20: tempi del gesto (in CODA, come ogni campo nuovo) -----------
  //  TEMPO_NOT_SET (0xFFFF) = non misurato. Vedi tempo.h: quello che si misura
  //  e' il tempo di QUIETE prima del rilascio, che coincide col tempo di mira
  //  solo se la trazione e' un movimento continuo.
  uint16_t tempo_mira_ms;
  uint16_t tempo_alzata_ms;

  // --- FASE 20b: plausibilita' dell'assetto --------------------------------
  //  Le sedute del 15/08 contenevano due record con alzo −19,9° e −35,4° e
  //  cant fino a +45,7°: non tiri, ma l'arco maneggiato con il trigger armato.
  //  Erano indistinguibili dai tiri veri, e in una regressione avrebbero
  //  pesato piu' di tutti gli altri proprio perche' estremi.
  //  0 = assetto plausibile · 1 = fuori dai limiti · 2 = angoli non calcolati.
  //  Si MARCA e non si scarta: la decisione di escludere un tiro sta a chi
  //  analizza, non al firmware, che non sa cosa si stia cercando.
  uint8_t  assetto_ok;

  // ==========================================================================
  //  FASE 21 — PICCO D'URTO (in coda, come ogni campo nuovo)
  // ==========================================================================
  //  Ampiezza massima dell'impulso che il rilascio trasmette al riser, nella
  //  finestra a cavallo dello scocco. La grandezza e' LA STESSA che il trigger
  //  confronta con la soglia — | |a| - g | — e non e' un dettaglio: significa
  //  che il numero mostrato nel riepilogo e la soglia impostata via BLE sono
  //  sullo stesso metro. Con due grandezze diverse, "picco 31" e "soglia 7"
  //  sarebbero due numeri incomparabili scritti nella stessa unita'.
  //
  //  PERCHE' NON SI CHIAMA "ENERGIA"
  //    Sui 18 tiri del 15/08 l'integrale dell'impulso al quadrato correlava col
  //    picco a r = 0,96: le due grandezze dicono la stessa cosa, e il picco ha
  //    il vantaggio di avere un'unita' di misura vera (m/s^2). L'integrale ha
  //    unita' (m/s^2)^2*s, che non significa niente per nessuno: chiamarlo
  //    "energia" sarebbe dare un nome fisico a un indice — lo stesso errore che
  //    la griglia 3x3 aveva fatto con l'ARCS. Se un giorno il cronografo
  //    confermera' che il picco traccia la V0, allora la casella potra'
  //    diventare una stima di energia CON una costante tarata, e sara' onesta.
  //
  //  COSA CI SI ASPETTA DI VEDERE (seduta del 15/08, 18 tiri, 5 m)
  //    correlazione col numero del tiro: r = +0,72 (p = 0,001)
  //    primi sei tiri  media 14,5 m/s^2   ultimi sei  media 31,5 m/s^2
  //  L'ipotesi in piedi e' che sia l'allungo che cresce nel corso della seduta.
  //  La verifica decisiva e' il cronografo, non questo numero: qui si REGISTRA
  //  soltanto, per avere la serie storica quando la verifica arrivera'.
  //
  //  Unita': CENTESIMI di m/s^2 (come i cdeg per gli angoli). Un uint16 copre
  //  0..655,35 m/s^2 con risoluzione 0,01: il fondo scala dell'accelerometro
  //  e' molto piu' basso, quindi non satura mai in pratica.
  bool     picco_valid;   // finestra sufficiente -> picco calcolato
  uint16_t picco_cms2;    // picco | |a| - g | nella finestra [centesimi di m/s^2]
};

// Limite oltre il quale l'assetto di mira non e' credibile per un tiro. 45° e'
// larghissimo — l'elevazione massima nel 3D sta sotto i 25° — ma e' scelto per
// non escludere nulla di legittimo: serve a prendere i record grossolanamente
// impossibili, non a fare da filtro fine.
static constexpr int16_t ASSETTO_LIMITE_CDEG = 4500;

// ----------------------------------------------------------------------------
//  Codifica ARCS (identica alla 1.69, verificata su 1026 casi).
// ----------------------------------------------------------------------------
//  ARCS = ±(distanza*10 + (zona+1)). zona+1 in [1..9] cosi' le unita' non sono
//  mai 0 (distinguibile da "non valutato"=0). Segno: + se colpito, - se fuori.
//
//  FASE 19 — terzo caso, che la codifica accoglie senza cambiare formato:
//  zona == ZONA_IGNOTA produce unita' 0, cioe' ±(distanza*10). Il valore resta
//  distinguibile sia da "non valutato" (0 secco, perche' li' anche la distanza
//  e' 0) sia da qualunque zona nota. Nessun bit nuovo, nessuna colonna in piu':
//  la cifra delle unita' aveva gia' un valore libero e adesso significa
//  qualcosa. I file vecchi restano validi: nessuno di essi puo' contenere
//  unita' 0 con distanza diversa da 0.
int16_t encodeArcs(bool colpito, uint8_t zona, uint8_t distanza);

// Etichetta leggibile della zona: "SPOT" se colpito+centro, altrimenti
// direzionale (alto-sx, centro, dx, ...).
const char* zonaLabel(bool colpito, uint8_t zona);

// ----------------------------------------------------------------------------
//  API della sotto-macchina
// ----------------------------------------------------------------------------

// Avvia lo scoring: entra nello stato ESITO e disegna la prima schermata.
// shotId: identificativo del tiro (per il futuro ScorePacket; in Fase 3 e'
// solo un contatore mostrato a schermo).
// FASE 22: lo scoring parte da ESITO e riceve il bersaglio GIA' ARMATO nel
// pre-tiro. I due valori sono parametri e non globali lette di nascosto: chi
// avvia uno scoring deve dichiarare a che bersaglio si stava tirando.
void scoringStart(uint16_t shotId, uint8_t dist_m, int8_t elev_deg);

// ----------------------------------------------------------------------------
//  FASE 22 — PRE-TIRO
// ----------------------------------------------------------------------------
//  Apre le due schermate di slider PRIMA dello scocco, per armare il bersaglio.
//  Si guida con lo stesso scoringUpdate() dello scoring normale; quando questo
//  ritorna DONE il pre-tiro e' finito.
//
//  Uso tipico:
//      scoringStartPretiro(distCorrente, elevCorrente);
//      ... scoringUpdate(td) fino a DONE ...
//      if (scoringPretiroConfermato()) scoringGetPretiro(dist, elev);
//
//  scoringPretiroConfermato() distingue OK da ANNULLA. E' un flag esplicito e
//  non una deduzione del tipo "i valori sembrano cambiati": annullare dopo aver
//  mosso lo slider e rimesso il valore di partenza deve restare un annullamento.
void scoringStartPretiro(uint8_t dist_m, int8_t elev_deg);
bool scoringPretiroConfermato();
void scoringGetPretiro(uint8_t& dist_m, int8_t& elev_deg);

// Riapre lo scoring del tiro CORRENTE mantenendo le scelte gia' fatte (per il
// re-editing dal riepilogo tramite CORREGGI). Riparte dalla schermata ESITO ma
// colpito/zona/distanza restano quelli precedenti come punto di partenza.
void scoringResume();

// Da chiamare nel loop passando l'ultimo campione touch. Fa avanzare la
// macchina, ridisegna se serve, gestisce il timeout. Ritorna lo stato corrente.
// Quando ritorna DONE, il risultato e' pronto (scoringGetResult).
ScoringState scoringUpdate(const TouchData& td);

// True se la macchina e' attiva (non IDLE, non DONE).
bool scoringActive();

// Stato corrente.
ScoringState scoringGetState();

// Risultato finale (valido dopo DONE).
ScoreResult scoringGetResult();
