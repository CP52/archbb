// ============================================================================
//  ArchBB 1.83 — sd_storage.h · FASE 5 (persistenza microSD)
//  Cesare Pagura · Padova/Noale IT · 20 luglio 2026
// ----------------------------------------------------------------------------
//  PERCHE' ESISTE QUESTO MODULO (il motivo della migrazione a 1.83)
//    La 1.69 mandava tutto via BLE all'app e non salvava nulla a bordo. La 1.83
//    ha lo slot microSD: il burst grezzo che cattura in PSRAM (Fase 4b) puo'
//    finalmente essere RIVERSATO su card, cosi' le sessioni restano a bordo,
//    ri-analizzabili a freddo. Questo modulo e' quel riversamento.
//
//  IL BUS SPI DELLA SD (fatto hardware, misurato dal pinout ufficiale 1.83-S3)
//    MOSI=GPIO1, SCK=GPIO2, MISO=GPIO3, CS=GPIO42. E' un bus SPI TUTTO SUO,
//    separato dal display (che gira su SCLK=6/MOSI=7 con HSPI). Nessuna contesa:
//    usiamo l'host FSPI con un SPIClass dedicato. Un bus, un chip, niente
//    transazioni condivise da sincronizzare — la situazione piu' semplice.
//
//  STRUTTURA FILE — robusta PER COSTRUZIONE (v. §struttura sotto)
//    Il principio guida: il dato che NON si puo' ricreare (i risultati dello
//    scoring) vive in un file piccolo ad APPEND, chiuso e flushato a ogni tiro,
//    cosi' un crash perde al massimo l'ultimo tiro. Il burst grezzo, che e'
//    SACRIFICABILE (se si corrompe hai comunque i risultati nel CSV), sta in un
//    file suo per tiro: se la sua scrittura fallisce, il CSV e' gia' al sicuro.
//
//      /ARCHBB/
//         SESS_0001/
//            shots.csv        <- 1 riga per tiro, append+flush (dato prezioso)
//            burst_0001.bin   <- burst grezzo tiro 1 (ri-analizzabile)
//            burst_0002.bin
//            session.txt      <- metadati: avvio, versione fw, n. tiri
//         SESS_0002/ ...
//
//    NUMERAZIONE DERIVATA DAL FILESYSTEM, non da un contatore in RAM/NVS:
//    all'avvio della sessione si scandisce /ARCHBB/ e si prende il primo
//    SESS_NNNN libero; il numero del tiro deriva da quante righe ha gia'
//    shots.csv. La SD *e'* lo stato: niente da tenere sincronizzato altrove,
//    robusto ai reset. (Quando arrivera' la Fase NVS, la coerenza sara' gia'
//    garantita dal filesystem, non da un secondo store da allineare.)
//
//  FORMATO DEL BURST BINARIO (.bin) — zero-parsing
//    Header di 16 byte (magic "ABB1" + versione + ODR + n_campioni +
//    trigger_idx + shot_id) seguito da n * sizeof(ImuSample) byte COSI' COME
//    SONO IN RAM. ImuSample e' packed e static_assert-ato a 30 byte: il file e'
//    direttamente rileggibile con uno struct a passo 30, senza conversioni,
//    da uno script Python o dalla companion app.
// ============================================================================
#pragma once
#include <Arduino.h>
#include "config.h"
#include "scoring.h"   // ScoreResult (i risultati da serializzare nel CSV)

// ----------------------------------------------------------------------------
//  Pin del bus SPI della microSD (1.83-S3) — vedi header sopra.
// ----------------------------------------------------------------------------
//  Non passano dai build_flags perche' il modulo SD e' l'unico a usarli: li
//  teniamo qui come costanti, vicino al codice che li adopera. Coerente con la
//  scelta fatta per i pin IMU (costanti in config.h, non -D).
static constexpr int SD_PIN_MOSI = 1;
static constexpr int SD_PIN_SCK  = 2;
static constexpr int SD_PIN_MISO = 3;
static constexpr int SD_PIN_CS   = 42;

// Frequenza del bus SD. Si parte prudenti (20 MHz): "prima acceso e pulito, poi
// si alza", stessa filosofia del display in Fase 1. Le microSD reggono molto di
// piu', ma su fili non schermati e bus condiviso col resto della scheda 20 MHz
// e' un compromesso sicuro per il bring-up.
static constexpr uint32_t SD_SPI_FREQ_HZ = 20000000;

// ----------------------------------------------------------------------------
//  Stato globale del modulo (definito in sd_storage.cpp)
// ----------------------------------------------------------------------------
extern volatile bool g_sd_ok;          // true se la card e' montata e scrivibile
extern volatile bool g_session_open;   // true se una sessione e' attualmente aperta

// Diagnosi leggibile dell'ultimo tentativo di mount (Fase 5). In caso di NO SD
// dice PERCHE': "nessun dialogo (pin/card assente)", "card ok ma non FAT32",
// ecc. Mostrata a schermo dal main quando la card manca, cosi' la causa si
// legge sul dispositivo senza collegare la seriale.
extern char g_sd_diag[48];

// ----------------------------------------------------------------------------
//  API — inizializzazione
// ----------------------------------------------------------------------------

// Monta la microSD sull'host FSPI dedicato (pin sopra). Crea /ARCHBB/ se manca.
// Ritorna true se la card risponde ed e' scrivibile. NON apre nessuna sessione:
// il montaggio della card e l'apertura di una sessione sono due cose distinte
// (si puo' avere la card montata e nessuna sessione in corso). Da chiamare in
// setup(), dopo display/touch/IMU. Idempotente: una seconda chiamata non rompe.
bool sd_init();

// ----------------------------------------------------------------------------
//  API — gestione sessione
// ----------------------------------------------------------------------------

// Apre una NUOVA sessione: sceglie il primo SESS_NNNN libero in /ARCHBB/, crea
// la cartella, scrive session.txt (metadati) e l'header di shots.csv. Da questo
// momento sd_save_shot() scrive nella nuova sessione. Ritorna il numero di
// sessione (>=1) o 0 se fallisce (card assente/piena). Se una sessione era gia'
// aperta, la chiude prima (una sola sessione attiva alla volta).
uint16_t sd_session_start();

// Riapre l'ultima sessione ESISTENTE su card e vi ACCODA i nuovi tiri (il
// contatore riparte dal numero di righe gia' in shots.csv, i burst_NNNN.bin
// riprendono da dove eravamo). Ritorna il numero di sessione ripresa, o 0 se
// non c'e' nessuna sessione da riprendere (in tal caso usare sd_session_start).
uint16_t sd_session_resume_last();

// True se esiste almeno una sessione su card che si potrebbe riprendere. Il
// main la usa per decidere se offrire la scelta "riprendi / nuova" oppure
// aprire direttamente una nuova (se non c'e' nulla da riprendere).
bool sd_has_previous_session();

// Info sull'ultima sessione esistente (per il testo della schermata di scelta):
// scrive in name il nome cartella (es. "SESS_20260721_1430" o "SESS_0001") e in
// shots quanti tiri contiene. Se non c'e' nessuna sessione, name[0]='\0'. Non
// modifica lo stato: e' pura lettura.
void sd_last_session_info(char* name, size_t nameSz, uint16_t& shots);

// ----------------------------------------------------------------------------
//  API — elenco sessioni (FASE 7, per il BLE / companion app)
// ----------------------------------------------------------------------------
//  Record leggero di una sessione: basename cartella + n. tiri. Definito qui
//  (non nel modulo BLE) perche' la SCANSIONE della card e' responsabilita' di
//  sd_storage; il BLE si limita a impacchettarlo in una notify. Cosi' domani
//  un'altra funzione (es. export su USB) puo' riusare lo stesso elenco senza
//  passare dal BLE.
struct SdSessionInfo {
  char     name[26];   // basename, es "SESS_20260721_1430"
  uint16_t shots;      // tiri salvati nella sessione
};

// Quante sessioni (cartelle SESS_*) ci sono su card. 0 se card assente/vuota.
// Query di sola lettura: scandisce /ARCHBB/ senza modificare nulla.
uint16_t sd_session_total();

// Enumera le sessioni su card riempiendo out[] (al massimo maxN elementi),
// ORDINATE dalla piu' RECENTE alla piu' vecchia (i nomi SESS_* ordinano bene
// come stringhe: l'ultimo alfabetico e' il piu' nuovo). Scrive in total il
// numero REALE di sessioni presenti (che puo' superare maxN: in tal caso out[]
// contiene solo le prime maxN piu' recenti). Ritorna quanti record ha
// effettivamente scritto in out[] (= min(total, maxN)). Sola lettura.
uint16_t sd_enumerate_sessions(SdSessionInfo* out, uint16_t maxN, uint16_t& total);

// Quante sessioni al MASSIMO l'enumerazione tiene in memoria alla volta.
// ┌─ §4.1 — COSTANTE UNICA ────────────────────────────────────────────────┐
// │ Prima questo 64 era un letterale sparso in ble_service.cpp             │
// │ (do_list_sessions). Adesso e' UNA costante letta da tutti i            │
// │ consumatori: l'elenco BLE, il risolutore indice→nome, e chiunque       │
// │ arrivera' dopo. Se un giorno diventasse 128, cambia in un punto solo   │
// │ e l'indice resta coerente ovunque — che e' esattamente il tipo di      │
// │ divergenza che sulla 1.69 e' costato otto versioni di firmware.        │
// └────────────────────────────────────────────────────────────────────────┘
static constexpr uint16_t SD_MAX_ENUM_SESSIONS = 64;

// Risolve l'INDICE di sessione (lo stesso 0-based che il BLE manda nei
// SessionRecord) nel nome cartella. E' il ponte fra "la sessione #3 che vedo
// nell'app" e "/ARCHBB/SESS_20260725_1648": senza questa funzione l'indice del
// download e quello dell'elenco potrebbero riferirsi a sessioni diverse.
// Usa la STESSA sd_enumerate_sessions dell'elenco -> stesso ordinamento, stesso
// significato di indice, per costruzione. Ritorna false se l'indice non esiste.
bool sd_session_name_by_index(uint16_t index, char* out, size_t outSz);

// ----------------------------------------------------------------------------
//  API — LETTURA di un file di sessione (FASE 10: download via BLE)
// ----------------------------------------------------------------------------
//  PERCHE' UN "READER" CON STATO E NON UNA READ STATELESS
//    La tentazione era: size(index,file) + read(index,file,offset,n), senza
//    handle aperti. Piu' difensivo in astratto, ma qui MOLTO piu' costoso:
//    ogni read dovrebbe ri-enumerare /ARCHBB/ (che apre e conta le righe di
//    ogni shots.csv) solo per ritrovare la cartella. Su un file da 5 blocchi
//    sarebbero 5 scansioni complete della card.
//    Il reader invece risolve la cartella UNA volta e tiene un File aperto.
//    Il rischio dello stato globale qui non c'e': apertura, streaming e
//    chiusura avvengono dentro UNA SOLA funzione del ble_task (do_send_file),
//    mai a cavallo di due callback NimBLE. Lo stato non sopravvive alla
//    funzione che lo crea, quindi non e' stato condiviso: e' una variabile
//    locale che, per ragioni di dimensione, vive nel modulo.
//
//  NB: un solo reader alla volta. sd_reader_open() su un reader gia' aperto
//  chiude il precedente (l'ultimo vince): non si accumulano handle.

// Apre un file DENTRO la sessione index-esima in sola lettura.
//   fname : nome relativo, es. "shots.csv" o "burst_0003.bin"
//   size  : riceve la dimensione totale in byte
// Ritorna false se: card assente, indice inesistente, file mancante.
bool sd_reader_open(uint16_t index, const char* fname, uint32_t& size);

// Legge fino a n byte in sequenza dal file aperto. Ritorna i byte letti
// (0 = fine file o nessun file aperto). La posizione avanza da sola.
size_t sd_reader_read(uint8_t* buf, size_t n);

// Chiude il file aperto. Idempotente: chiamarla senza file aperto e' un no-op.
// DEVE essere chiamata anche sui percorsi d'errore (altrimenti l'handle resta
// appeso fino al prossimo open).
void sd_reader_close();

// Chiude la sessione corrente: aggiorna session.txt col conteggio finale dei
// tiri e marca g_session_open=false. Non cancella nulla. No-op se non c'e'
// sessione aperta. Da chiamare al tap-lungo di stop, o prima di aprirne una nuova.
void sd_session_stop();

// Numero della sessione attualmente aperta (0 se nessuna).
uint16_t sd_session_id();

// Quanti tiri sono stati salvati finora nella sessione corrente.
uint16_t sd_session_shot_count();

// ----------------------------------------------------------------------------
//  API — salvataggio del tiro (il cuore del modulo)
// ----------------------------------------------------------------------------

// Salva un tiro completo: appende la riga risultati a shots.csv (chiudendo il
// file subito per flush) e, se il burst e' fornito, scrive burst_NNNN.bin.
//
//   res     : il ScoreResult del tiro (esito, zona, dist, elev, angoli, metriche)
//   burst   : puntatore ai campioni grezzi del burst (frame canonico), o nullptr
//             se non disponibile (tiro manuale senza IMU / buffer non READY)
//   n       : numero di campioni nel burst (0 se burst==nullptr)
//   trigIdx : indice del campione di scocco entro il burst (= pre-samples)
//   odr     : l'ODR con cui il burst e' stato campionato (per l'header .bin)
//
// Se non c'e' una sessione aperta, ne apre UNA automaticamente (auto-start al
// primo tiro): l'arciere che spara senza aver premuto "start" non perde il dato.
//
// Ritorna true se ALMENO il CSV e' stato scritto (il dato prezioso e' salvo);
// il fallimento del solo .bin non rende false il risultato, ma viene loggato.
//   preValidi : F24b — campioni pre-trigger appartenenti davvero a questo tiro
//               (da circular_buffer_get_burst). Finisce nell'header del burst e
//               in coda al CSV. Il default 0xFFFF vale "tutti", per i chiamanti
//               di banco che non hanno un buffer circolare dietro.
bool sd_save_shot(const ScoreResult& res,
                  const ImuSample* burst, uint16_t n, uint16_t trigIdx,
                  OdrSetting odr, uint16_t preValidi = 0xFFFF);

// ----------------------------------------------------------------------------
//  API — spazio libero (per la barra di stato in ATTESA)
// ----------------------------------------------------------------------------

// Megabyte liberi sulla card (0 se card assente). Ritorna un valore in CACHE:
// e' istantaneo e NON bloccante, sicuro da chiamare a ogni refresh della UI. Il
// conto vero (scansione FAT, costoso e potenzialmente lento) lo fa
// sd_free_refresh(), invocato solo quando lo spazio cambia davvero (al mount e
// dopo un salvataggio). NB: prima questo valore era ricalcolato a ogni refresh
// (ogni 3s in ATTESA) e una scansione FAT lenta bloccava la UI: la cache elimina
// quel rischio alla radice.
uint32_t sd_free_mb();

// Megabyte totali della card (0 se assente). Anche questo dalla cache.
uint32_t sd_total_mb();

// Ricalcola spazio libero/totale (scansione FAT) e aggiorna la cache. Costoso:
// chiamarlo SOLO quando lo spazio e' cambiato per davvero (mount, salvataggio),
// MAI nel loop di refresh della UI.
void sd_free_refresh();

// ============================================================================
//  FASE 24a — LOG ENERGETICO  /ARCHBB/ENERGIA.CSV
// ============================================================================
//  PERCHE' UN FILE A PARTE E NON UNA COLONNA IN shots.csv
//    L'energia non ha la cadenza dei tiri: scorre anche mentre si cammina fra
//    le piazzole, ed e' proprio li' che vogliamo sapere cosa succede. Legarla
//    alla riga del tiro vorrebbe dire non avere nessun campione nelle due ore
//    in cui il consumo conta di piu'.
//
//  PERCHE' NELLA RADICE E NON NELLA SESSIONE
//    Lo spegnimento del 12/09 ha interrotto la sessione piu' volte. Un log
//    energetico che si spezza insieme alla sessione perde esattamente il punto
//    che vogliamo guardare — la giuntura. Qui il file e' UNO, continuo, e le
//    sessioni ci passano dentro.
//
//  CHE COSA CI SI LEGGE (e cosa risponde)
//    - la curva di scarica reale, da cui si ricava il consumo medio in mA se
//      si dichiara la capacita' della cella;
//    - il DELTA fra tensione di batteria e di sistema sotto carico: se il
//      sistema crolla mentre la batteria tiene, il collo di bottiglia e' il sag;
//    - a che tensione e' avvenuto lo spegnimento: se e' molto sopra i 3,0 V la
//      colpevole e' la soglia VOFF del PMU, non la cella.
//
//  Formato: righe di dati piu' righe di NOTA (prefisso '#') per gli eventi.
//  Costo: 30 s di periodo -> 120 righe/ora, ~8 KB al giorno. Irrilevante.

// Scrive una riga di campionamento. Ritorna false se la card non c'e'.
// Chiamata dal loop con un proprio cronometro: il modulo non ne tiene uno suo,
// cosi' chi legge il main vede la cadenza scritta dove viene decisa.
bool sd_energia_log(float vBatt, float vSys, int pct, bool inCarica,
                    uint8_t blPct, uint32_t cpuMhz, uint8_t statoPower,
                    float tempC);

// Scrive una riga di nota (evento datato): avvio, censimento dei rail, motivo
// dell'ultimo reset, apertura/chiusura sessione. Sono gli appigli che rendono
// leggibile la curva — senza, si hanno numeri senza storia.
bool sd_energia_nota(const char* testo);
