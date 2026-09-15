// ============================================================================
//  ArchBB 1.83 — display.h · FASE 1 (bring-up ST7789V2)
//  Cesare Pagura · Padova/Noale IT · 11 luglio 2026
// ----------------------------------------------------------------------------
//  Interfaccia del modulo display. Isola in un solo punto:
//    - la GEOMETRIA del pannello 1.83 (240x284, offset y=20)
//    - la PALETTE ArchBB (colori coerenti con l'app companion)
//    - i PROTOTIPI delle funzioni di bring-up
//  Cosi' il display.cpp resta pulito e le costanti non si sparpagliano.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include "config.h"   // geometria pannello (ARCHBB_W/H/OFFSET) + fatti hardware

// Istanza display condivisa fra i moduli (definita in display.cpp). Lo scoring
// disegna direttamente su questa senza duplicare l'istanza.
extern TFT_eSPI tft;

// ----------------------------------------------------------------------------
//  GEOMETRIA PANNELLO 1.83
// ----------------------------------------------------------------------------
//  Il controller ST7789V2 e' 240x320. Il pannello 1.83 mostra una finestra
//  240x284 con offset verticale di 20 px (identico alla 1.69, che era 240x280).
//  L'offset serve perche' la GRAM del controller e' piu' alta del vetro: senza
//  offset comparirebbe una riga di "garbage" in cima o in fondo.
//
//  ARCHBB_W / ARCHBB_H / ARCHBB_OFFSET_* sono definite in CONFIG.H, non qui:
//  sono un fatto hardware (come i pin), e serve anche al driver del touch, che
//  non deve includere TFT_eSPI per conoscerle. Qui le ereditiamo e basta —
//  scriverle in due posti e' la ricetta garantita perche' divergano.

// ----------------------------------------------------------------------------
//  PALETTE ArchBB (RGB565)
// ----------------------------------------------------------------------------
//  Colori scelti per coerenza con l'app companion (fondo scuro, accenti vivi).
//  In RGB565: 5 bit R, 6 bit G, 5 bit B. Uso helper tft.color565 a runtime dove
//  serve precisione; qui costanti pronte per i casi comuni.
//
//  NB: se color-order o invert non sono ancora tarati, questi colori sono anche
//  il TEST: "esito verde" deve essere verde, non magenta; il nero di sfondo deve
//  essere nero, non bianco.
namespace ArchColor {
  constexpr uint16_t BG      = 0x0000;  // nero (sfondo)
  constexpr uint16_t BG_CARD = 0x1082;  // grigio-blu molto scuro (card)
  constexpr uint16_t TEXT    = 0xFFFF;  // bianco
  constexpr uint16_t TEXT2   = 0xBDF7;  // grigio chiaro
  constexpr uint16_t TEXT3   = 0x7BEF;  // grigio medio (didascalie)
  constexpr uint16_t ACCENT  = 0x05FF;  // ciano ArchBB
  constexpr uint16_t GOOD    = 0x2FEB;  // verde "esito centro"
  constexpr uint16_t BAD     = 0xE8A6;  // rosso "esito fuori"
  constexpr uint16_t AMBER   = 0xFD20;  // ambra (warning/trigger)
  constexpr uint16_t BORDER  = 0x39C7;  // grigio bordo
}

// ----------------------------------------------------------------------------
//  API del modulo
// ----------------------------------------------------------------------------

// Inizializza display, backlight, rotazione, offset e flag. Da chiamare in setup().
void displayInit();

// "Ciao mondo" ArchBB: schermata identificativa con nome, versione, e cornice.
// Valida testo, font e posizionamento.
void displaySplash();

// Banco di prova visivo che verifica A OCCHIO i 6 rischi della tabella §6:
//   color order, invert, offset y, altezza 284, clock, memory_type (implicito).
// Disegna barre di colore puro + bordi ai 4 lati + testo diagnostico.
void displayDiagnosticPattern();

// Test chirurgico dell'offset verticale: righe bianche sul primo e ultimo pixel.
// Serve a isolare il SOLO problema geometrico (offset y / altezza 284).
void displayOffsetTest();

// Schermata "esito" di scoring usata come test finale (colori pieni + testo
// grande): il caso perfetto per validare rosso/verde e i font grandi.
//   good=true  -> pannello verde "CENTRO"
//   good=false -> pannello rosso "FUORI"
void displayEsito(bool good);

// Utility: accende/spegne backlight in modo esplicito.
// Dalla Fase 24a e' un involucro su displayBacklightPercent (0 o livello base):
// resta per i chiamanti storici, ma il controllo vero e' quello percentuale.
void displayBacklight(bool on);

// ----------------------------------------------------------------------------
//  FASE 24a — RETROILLUMINAZIONE PWM e SLEEP DEL PANNELLO
// ----------------------------------------------------------------------------
//  Qui c'e' il MECCANISMO; la POLITICA (quando attenuare, quando spegnere) vive
//  in power_mgr. La separazione non e' pedanteria: il display possiede il suo
//  hardware (GPIO40 e il canale LEDC), power_mgr possiede la macchina a stati.
//  Se un giorno la politica cambia, questo file non si tocca.
//
//  ATTENZIONE AL CORE 3.x: l'API LEDC e' cambiata. Da Arduino-ESP32 3.0
//  ledcSetup()/ledcAttachPin() non esistono piu' e si usa
//  ledcAttach(pin, freq, bit) + ledcWrite(pin, duty) — il duty e' indicizzato
//  sul PIN, non sul canale. Il progetto compila su core >= 3.0.5, ma la guardia
//  di versione e' dentro display.cpp cosi' il file resta compilabile anche se
//  un domani si tornasse a un core 2.x.

// Imposta la retroilluminazione in percentuale (0..100). 0 = spenta davvero
// (duty 0), non "quasi spenta". La curva applicata NON e' lineare: v. commento
// in display.cpp (la percezione della luminosita' non lo e').
void displayBacklightPercent(uint8_t pct);

// Percentuale attualmente impostata (per il log energetico e la diagnostica).
uint8_t displayBacklightGet();

// Manda il pannello ST7789 in sleep (comando 0x10) o lo risveglia (0x11).
// Il risveglio richiede ~120 ms di attesa prima di poterci ridisegnare sopra:
// la funzione li aspetta lei, cosi' nessun chiamante puo' dimenticarsene.
// NB: risvegliare NON ridisegna. Il contenuto della GRAM sopravvive, ma chi
// risveglia dovrebbe comunque ridisegnare la schermata corrente: e' l'occasione
// giusta per rinfrescare ora, batteria e stato sessione.
void displayPanelSleep(bool sleep);

// ----------------------------------------------------------------------------
//  FASE 24a — PRIMITIVE DEL SEGNO (disegnate, non scritte)
// ----------------------------------------------------------------------------
//  I font 6, 7 e 8 di TFT_eSPI contengono cifre, punto, due punti e IL MENO —
//  il meno c'e' davvero (verificato sulle tabelle di larghezza dei font:
//  larghezza 17 nel font 6, 32 nel font 7, quanto una cifra). Quello che NON
//  c'e' e' il PIU'.
//
//  Il segno mancante in campo il 12/09 NON era dunque un problema di font: era
//  la card di ATTESA che stampava deliberatamente il valore ASSOLUTO, con la
//  direzione affidata a una didascalia in font 1 sotto il numero. A tre metri
//  di distanza, con la luce di taglio e l'arco in mano, quella didascalia non
//  si legge. Il numero si legge.
//
//  Rimedio: il segno si DISEGNA. Disegnarlo toglie la dipendenza dal font una
//  volta per tutte, scala con la zona invece che col carattere, ed e' l'unico
//  modo di avere un "+" grande quanto il "-".
void displaySegnoPiu (int16_t cx, int16_t cy, int16_t lung, int16_t spess, uint16_t col);
void displaySegnoMeno(int16_t cx, int16_t cy, int16_t lung, int16_t spess, uint16_t col);

// Freccia piena verso l'alto / verso il basso, iscritta nel rettangolo w x h
// centrato in (cx,cy). E' il TERZO canale della codifica (forma), quello che
// funziona anche a colori spenti e per chi non distingue le tinte.
void displayFrecciaSu (int16_t cx, int16_t cy, int16_t w, int16_t h, uint16_t col);
void displayFrecciaGiu(int16_t cx, int16_t cy, int16_t w, int16_t h, uint16_t col);

// Colore convenzionale dell'elevazione secondo ELEV_PALETTE (v. config.h).
// Lo zero ha un colore suo (neutro): "in piano" non e' ne' su' ne' giu'.
uint16_t displayColoreElev(int8_t deg);

// ----------------------------------------------------------------------------
//  displayElevazione — L'UNICO disegnatore dell'elevazione in tutto il firmware
// ----------------------------------------------------------------------------
//  Disegna, centrato in (cx,cy):   [freccia] [segno] [cifre]
//  nel font grande richiesto (6 o 7), col colore dato da ELEV_PALETTE.
//
//  Esiste in un posto solo per la regola §4.1: card di ATTESA e slider del
//  pre-tiro mostrano lo STESSO numero in due momenti diversi. Due disegnatori
//  separati sarebbero divergiti al primo ritocco — e' gia' successo, ed e' il
//  motivo per cui il "+" e il "-" si comportavano in modo diverso.
//
//  bg serve per il fondo del testo (BG oppure BG_CARD): TFT_eSPI disegna i font
//  con fondo opaco, e passare il fondo sbagliato lascia un rettangolo nero
//  dentro la card.
void displayElevazione(int16_t cx, int16_t cy, int8_t deg, uint8_t font, uint16_t bg);

// ----------------------------------------------------------------------------
//  FASE 2 — banco di prova touch
// ----------------------------------------------------------------------------
// Disegna la cornice fissa del banco di prova touch (titolo, etichette campi).
// Da chiamare una volta all'ingresso nella modalita' test touch.
void displayTouchTestFrame();

// Aggiorna i valori a schermo: coordinate grezze, dita, gesture, e un crocino
// nella posizione toccata. x,y grezzi; fingers e nome gesture come stringa.
// touched=false pulisce il crocino (rilascio).
void displayTouchUpdate(bool touched, uint16_t x, uint16_t y,
                        uint8_t fingers, const char* gestureName, uint8_t rawGid);

// ----------------------------------------------------------------------------
//  FASE 3 — schermate di supporto allo scoring
// ----------------------------------------------------------------------------
// Schermata di attesa: invita a "sparare" (in Fase 3: toccare per simulare il
// tiro e avviare lo scoring). Mostra il contatore tiri.
void displayAttesaTiro(uint16_t nextShotId);

// Schermata di riepilogo del risultato dello scoring appena concluso.
// Mostra esito, zona (SPOT se centro+colpito), distanza, ARCS. Se non valutato
// (SKIP/timeout) lo segnala.
// v2.16: +elev. La sentinella ELEV_NOT_SET viene mostrata come "non impost."
// (ambra): e' l'ultimo momento in cui l'arciere puo' accorgersi che manca.
//
// FASE 4c: il riepilogo mostra anche le metriche biomeccaniche dal burst.
// Per non allungare a dismisura la lista di parametri (e non doverla toccare a
// ogni metrica nuova), passiamo direttamente il ScoreResult: contiene gia'
// tutto (esito, zona, distanza, ARCS, elev, angoli, hold, release) coi rispettivi
// flag di validita'. La firma diventa stabile rispetto all'aggiunta di metriche.
struct ScoreResult;  // fwd-decl (definita in scoring.h)
void displayScoreRiepilogo(const ScoreResult& r, const char* zonaTxt);

// Zona toccata nel riepilogo: 1 = CORREGGI (riedita), 2 = OK (conferma), 0 = altro.
uint8_t hitRiepilogo(uint16_t x, uint16_t y);

// ----------------------------------------------------------------------------
//  FASE 4a — attesa del TIRO REALE (IMU armata)
// ----------------------------------------------------------------------------
// Come displayAttesaTiro, ma comunica che ora e' l'IMU a rilevare il tiro:
// mostra il contatore, un pallino di stato "ARMATO" e la temperatura on-chip
// (utile a confermare a colpo d'occhio che l'IMU risponde). Il tocco resta
// disponibile come trigger manuale a banco (indicato in piccolo).
//   tempC: temperatura IMU in gradi C (da imu_read_temperature()); NAN = n/d.
void displayAttesaTiroIMU(uint16_t nextShotId, float tempC);

// ----------------------------------------------------------------------------
//  FASE 5 — barra di stato in ATTESA (batteria + SD + sessione)
// ----------------------------------------------------------------------------
//  Raccoglie i dati di stato da mostrare tra un tiro e l'altro. Li impacchettiamo
//  in una struct per non allungare all'infinito la firma di displayAttesaTiroIMU
//  ogni volta che aggiungiamo un'informazione (stessa scelta fatta col
//  ScoreResult nel riepilogo). Sentinelle esplicite per "non disponibile":
//    battPct = -1  -> batteria n/d (PMU assente)
//    sdOk   = false -> card assente: mostra "no SD", non un falso "0 MB"
struct StatusInfo {
  int      battPct;      // 0..100, -1 = n/d
  bool     battCharging; // true = in carica (mostra icona spina)
  bool     sdOk;         // true = card montata
  uint32_t sdFreeMb;     // MB liberi (valido solo se sdOk)
  const char* sdDiag;    // diagnosi breve se !sdOk (puntatore a g_sd_diag)
  bool     sessionOpen;  // true = sessione di tiro aperta (badge REC)
  uint16_t sessionId;    // numero sessione (valido solo se sessionOpen)
  uint16_t sessionShots; // tiri gia' salvati nella sessione corrente
  bool     hasTime;      // true = RTC valido -> mostra l'orologio
  char     clock[6];     // "HH:MM" se hasTime (Fase 6)
};

// Disegna la barra di stato in cima allo schermo (batteria a sinistra, SD a
// destra, badge sessione al centro). Alta ~18px; il resto della schermata di
// attesa si disegna sotto. Chiamata da displayAttesaTiroIMU (Fase 5).
void displayStatusBar(const StatusInfo& st);

// Variante Fase 5 della schermata di attesa: come displayAttesaTiroIMU ma con la
// barra di stato in cima. Manteniamo anche la vecchia firma per compatibilita'
// (la nuova la richiama passando uno stato "vuoto" quando non serve).
// FASE 22: la schermata ATTESA mostra la CARD PRE-TIRO coi valori armati.
//  distM / elevDeg = bersaglio attualmente armato (sempre validi: hanno un
//  default, non una sentinella — in ATTESA un bersaglio c'e' sempre).
//  confermato = i valori sono stati confermati nel pre-tiro DOPO l'ultimo
//  scocco (bordo ACCENT) oppure sono ereditati dal tiro precedente (AMBER).
void displayAttesaTiroIMU_v5(uint16_t nextShotId, float tempC, const StatusInfo& st,
                             uint8_t distM, int8_t elevDeg, bool confermato);

// true se il tocco cade sulla card pre-tiro. Legge le STESSE costanti del
// disegno: e' l'unico modo di garantire che dove si vede il riquadro sia anche
// dove il firmware crede che sia (§4.1).
bool displayAttesaHitCard(uint16_t x, uint16_t y);

// ----------------------------------------------------------------------------
//  FASE 20 — riga di stato dal vivo della macchina dei tempi.
// ----------------------------------------------------------------------------
//  Occupa la STESSA riga che prima portava il testo fisso "In attesa dello
//  SCOCCO": stesso posto, piu' informazione, nessun cambio di impaginazione.
//  Va chiamata dal loop mentre si e' in ATTESA; ridisegna solo se il testo
//  cambia davvero, quindi puo' essere chiamata a ogni giro senza sfarfallio.
//
//  Serve a validare le soglie di tempo.h, che sono scelte e non misurate:
//  senza un modo di vedere la macchina mentre gira, resterebbero un'opinione
//  fino alla lettura del CSV a fine seduta.
void displayTempoRiga(uint8_t stato, uint32_t msNelloStato, bool forza);

// Avviso breve (Fase 5): l'arciere ha toccato per gestire la sessione ma non c'e'
// microSD. Lampeggia un messaggio invece di fingere che sia successo qualcosa.
// Bloccante ~800ms, poi il loop ridisegna l'attesa normale.
void displaySessionNoSd();

// ----------------------------------------------------------------------------
//  FASE 5 — scelta sessione (riprendi ultima / nuova) e feedback salvataggio
// ----------------------------------------------------------------------------
// Schermata modale: c'e' gia' una sessione su card, l'arciere ha toccato per
// aprirne una. Chiede se RIPRENDERE l'ultima (accoda) o iniziarne una NUOVA.
//   lastId    : numero dell'ultima sessione esistente (per il testo)
//   lastShots : quanti tiri contiene (per il testo)
// Disegna due pulsanti grandi; la scelta la legge hitSessionChoice().
void displaySessionChoice(const char* lastName, uint16_t lastShots);

// Zona toccata nella schermata di scelta sessione:
//   1 = RIPRENDI, 2 = NUOVA, 0 = fuori dai pulsanti.
uint8_t hitSessionChoice(uint16_t x, uint16_t y);

// ----------------------------------------------------------------------------
//  FASE 24b — modale di conferma per il modo BLE
// ----------------------------------------------------------------------------
//  Il PWRKEY e' sul fianco, la mano di scocco porta la patella, e il modo BLE
//  SOSPENDE imu_task e trigger_task: una pressione accidentale non e' un
//  fastidio, e' una freccia che non esiste. La modale ha il NO come risposta di
//  default e come esito del timeout. Il trigger resta armato mentre e' a schermo.
// Schermata di spegnimento imminente (pressione lunga del PWRKEY intercettata).
// Schermata di spegnimento imminente. pct = avanzamento della barra (0..100),
// annullato = il tasto e' stato rilasciato prima della soglia del PMU.
// Ridisegna tutto solo al primo giro e al cambio di esito: negli altri fa
// avanzare la sola barra, o lo schermo sfarfallerebbe a ogni tick.
void displaySpegnimento(uint8_t pct, bool annullato);

// Preavviso di batteria basato sulle SOGLIE IN VOLT, non sulla percentuale
// (stimata dalla tensione dall'AXP2101, inutilizzabile sotto carico). Le soglie
// vengono dal ginocchio della curva di scarica misurata il 13/09.
void displayAvvisoBatteria(const char* tempoResiduo, bool critico);

void displayBleConfirm(bool sessioneAperta, uint8_t secondiRimasti);
void displayBleConfirmTick(uint8_t secondiRimasti);   // solo il conto alla rovescia
uint8_t hitBleConfirm(uint16_t x, uint16_t y);        // 0 fuori, 1 resta, 2 entra

// Doppio flash verde "SALVATO" (Fase 5): feedback di scrittura SD riuscita.
// Due lampeggi brevi di un riquadro verde col numero del tiro salvato. Bloccante
// (~360ms totali): abbastanza da vedersi, abbastanza breve da non rallentare il
// ritmo di tiro. Zero dipendenze audio.
// F13 — lampeggio rosso breve: cattura SCARTATA, niente sulla microSD.
//  Serve un riscontro visivo: senza, uno scarto e un salvataggio sono
//  indistinguibili dall'esterno e l'arciere non sa se il dato c'e' o no.
void displayScartato();

void displaySavedFlash(uint16_t shotNum);

// Lampeggio di conferma "TIRO RILEVATO": un flash ambra a tutto schermo con il
// picco di az del trigger. Breve, bloccante (~120ms): serve solo come feedback
// visivo istantaneo prima che parta la prima schermata di scoring.
void displayTriggerFlash(uint16_t shotId, float azPeak);

// ----------------------------------------------------------------------------
//  FASE 7 — displayBleMode: schermata "colloquio con smartphone" (modo BLE).
// ----------------------------------------------------------------------------
//  Mostra che il dispositivo e' in ascolto BLE: nome advertising, stato
//  connessione (in attesa / connesso), un riassunto (batteria, SD, orologio) e
//  l'istruzione per uscire (ripremere il tasto ingranaggio). Ridisegnata a ogni
//  cambio di stato connessione, non nel loop stretto. connected = true quando
//  un'app e' agganciata; devName e' il nome che appare nello scanner.
void displayBleMode(bool connected, const char* devName, const StatusInfo& st);
