// ============================================================================
//  ArchBB 1.83 — sd_storage.cpp · FASE 5 (persistenza microSD)
//  Cesare Pagura · Padova/Noale IT · 20 luglio 2026
// ----------------------------------------------------------------------------
//  Vedi sd_storage.h per l'architettura (bus SPI dedicato, struttura file,
//  formato burst). Qui c'e' l'implementazione. Tre scelte pratiche guidano
//  tutto il file:
//
//   1) Un SPIClass dedicato sull'host FSPI. Il display usa HSPI; noi FSPI. I due
//      bus non si toccano mai -> nessuna transazione condivisa da gestire.
//
//   2) Apri-scrivi-CHIUDI a ogni operazione. Non teniamo mai file aperti fra un
//      tiro e l'altro. close() forza il flush su card: se la batteria muore, cio'
//      che era chiuso e' salvo. E' il pattern anti-corruzione dei datalogger.
//
//   3) Lo STATO vive sulla card, non in RAM. Numero sessione e numero tiro si
//      DERIVANO dal filesystem (prime cartelle/righe libere). Un reset non perde
//      il filo: si riprende leggendo cosa c'e' gia' su card.
// ============================================================================
#include "sd_storage.h"
#include "rtc_clock.h"   // FASE 6: data/ora per nomi cartella e session.txt
#include "mount.h"       // FASE 11: g_mount_orientation, mount_name()
#include "config_store.h" // FASE 11: g_config (offset di montaggio in session.txt)
#include "tempo.h"       // FASE 20: TEMPO_NOT_SET
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <math.h>       // FASE 24a: isnan() per le sentinelle del log energetico

// ----------------------------------------------------------------------------
//  Stato globale
// ----------------------------------------------------------------------------
volatile bool g_sd_ok        = false;
volatile bool g_session_open = false;

// SPIClass dedicato all'host FSPI (separato dall'HSPI del display).
static SPIClass s_sdSPI(FSPI);

// Stato della sessione corrente (in RAM, ma sempre RICOSTRUIBILE dalla card).
static uint16_t s_sessionId   = 0;   // 0 = nessuna sessione aperta
static uint16_t s_shotCount   = 0;   // tiri salvati nella sessione corrente
// Istante e data di APERTURA. Vanno conservati: session.txt viene riscritto a
// ogni tiro, e se started_ms/datetime si ricalcolassero ogni volta la sessione
// sembrerebbe iniziata all'ultimo tiro.
static uint32_t s_sessionStartMs = 0;
static char     s_sessionStartIso[24] = {0};
static uint16_t s_odrRealeCentiHz = 0;   // F24b: misurato, 0 = non ancora noto
static char     s_lastShotIso[24] = {0}; // F24b: ora del tiro, non della scrittura
static char     s_sessDir[48] = "";  // es. "/ARCHBB/SESS_20260721_1430" o "/ARCHBB/SESS_0001"

// Versione del formato CSV: se un domani cambiano le colonne, l'app la legge in
// testa al file e sa come interpretarlo. Stessa disciplina degli ScorePacket.
// v6 (F24b): due colonne nuove IN CODA, scarto_cdeg e pre_validi.
// v5 (F21): una colonna nuova IN CODA, picco_cms2.
// v4 (F20b): una colonna nuova IN CODA, assetto_ok.
// v3 (F20): due colonne nuove IN CODA, tempo_mira_ms e tempo_alzata_ms.
// v2 (F19): due colonne nuove IN CODA, imp_x_cm e imp_y_cm, piu' imp_raggio_cm.
// L'app mappa per NOME colonna, quindi i file v1 restano leggibili senza
// modifiche: e' esattamente il motivo per cui i campi nuovi vanno solo in coda.
static constexpr uint8_t CSV_FORMAT_VER = 6;

// ----------------------------------------------------------------------------
//  L'INTESTAZIONE DEL CSV, UNA VOLTA SOLA
// ----------------------------------------------------------------------------
//  Fino alla F18 questa stringa era scritta a mano in DUE punti (creazione
//  sessione e ripristino di una cartella senza CSV). Erano identiche per
//  fortuna, non per costruzione: aggiungere una colonna in uno solo dei due
//  avrebbe prodotto file con intestazioni diverse nella stessa card. E' la
//  §4.1 applicata a una stringa invece che a una costante di posizione.
//
//  Ordine delle colonne: le nuove SEMPRE in coda, prima di burst_file... no —
//  anche burst_file scorre. La regola vera e' che nessuna colonna esistente
//  cambia posizione RELATIVA alle precedenti; chi legge per nome non se ne
//  accorge, chi legge per indice si accorgerebbe comunque della lunghezza.
static const char* const CSV_HEADER =
    "shot,ts_ms,colpito,zona,dist_m,elev_deg,arcs,"
    "cant_cdeg,alzo_cdeg,angoli_stabili,"
    "hold_cdeg,hold_class,release_jerk,release_class,"
    "burst_file,imp_x_cm,imp_y_cm,imp_raggio_cm,"
    "tempo_mira_ms,tempo_alzata_ms,assetto_ok,picco_cms2,"
    // --- FASE 24b -----------------------------------------------------------
    //  scarto_cdeg : |alzo misurato - elevazione armata|, in centesimi di grado.
    //    Dalla F22 l'elevazione e' DICHIARATA prima del tiro e MISURATA dopo:
    //    due misure indipendenti della stessa grandezza, il cui scarto e' un
    //    rivelatore di catture spurie che non costa niente. Sui 41 tiri del
    //    12/09 i tiri valutati avevano scarto mediano 1,62 gradi, quelli che
    //    l'arciere ha saltato 9,57. Qui viene solo CALCOLATO e SCRITTO: non
    //    decide nulla, e non decidera' nulla finche' non ci saranno ~180 tiri
    //    a dire se la soglia regge (con 8 saltati l'intervallo di confidenza
    //    sulla sensibilita' va da 0,53 a 0,98, cioe' non dice niente).
    //  pre_validi : campioni pre-trigger appartenenti davvero a questo tiro.
    //    < trigger_idx significa che il trigger e' scattato poco dopo il riarmo
    //    e parte della finestra e' residuo dell'anello.
    "scarto_cdeg,pre_validi";

// Magic + versione dell'header del burst binario.
static const char BURST_MAGIC[4] = { 'A', 'B', 'B', '1' };
// FASE 24b — versione 2: i quattro byte riservati dell'header adesso portano
//  due fatti che il file da solo non poteva dichiarare (v. writeBurstBin).
//  La DIMENSIONE dell'header non cambia e i campioni restano a passo 30: un
//  lettore v1 che ignora i riservati continua a funzionare sui file v2.
static constexpr uint8_t BURST_FORMAT_VER = 2;

// Radice di tutti i dati ArchBB sulla card.
static const char* ROOT_DIR = "/ARCHBB";

// ----------------------------------------------------------------------------
//  CACHE dello spazio libero (fix blocco in ATTESA)
// ----------------------------------------------------------------------------
//  SD.usedBytes() SCANDISCE l'intera FAT a ogni chiamata: e' costoso e, su
//  alcune card / stati del filesystem, puo' stallare a lungo. Prima sd_free_mb()
//  lo ricalcolava a OGNI refresh (ogni 3s in ATTESA) -> bastava una scansione
//  lenta/incagliata per bloccare la UI. Ma lo spazio libero cambia di ~20 KB a
//  tiro: ricalcolarlo di continuo e' inutile oltre che pericoloso.
//
//  SOLUZIONE: calcolare il valore pesante UNA VOLTA (al mount) e poi SOLO quando
//  cambia davvero (dopo un salvataggio). sd_free_mb() ritorna il valore in
//  cache, istantaneo e non-bloccante. sd_free_refresh() fa il conto vero, ed e'
//  chiamato nei pochi punti in cui lo spazio e' cambiato per davvero.
static uint32_t s_freeMbCache  = 0;      // MB liberi memorizzati
static uint32_t s_totalMbCache = 0;      // MB totali (fissi: si leggono una volta)
static bool     s_spaceValid   = false;  // false finche' non calcolato almeno una volta

// ----------------------------------------------------------------------------
//  Helper interni
// ----------------------------------------------------------------------------

// Costruisce "/ARCHBB/SESS_NNNN" in out.
static void buildSessDir(char* out, size_t outSz, uint16_t id) {
  snprintf(out, outSz, "%s/SESS_%04u", ROOT_DIR, id);
}

// Costruisce il path del burst del tiro n dentro la sessione corrente.
static void buildBurstPath(char* out, size_t outSz, uint16_t shotNum) {
  snprintf(out, outSz, "%s/burst_%04u.bin", s_sessDir, shotNum);
}

// Trova il primo numero di sessione libero scandendo /ARCHBB/. Non tiene un
// contatore: CHIEDE alla card. Cosi' dopo un reset non si sovrascrive nulla.
static uint16_t firstFreeSessionId() {
  for (uint16_t id = 1; id <= 9999; id++) {
    char dir[24];
    buildSessDir(dir, sizeof(dir), id);
    if (!SD.exists(dir)) return id;   // primo buco = nostra sessione
  }
  return 0;   // improbabile (9999 sessioni): card da svuotare
}

// Trova l'ULTIMA sessione esistente enumerando le cartelle reali in /ARCHBB e
// prendendo il nome piu' alto in ordine alfabetico. Funziona con ENTRAMBI gli
// schemi di naming: sia "SESS_0001" (zero-padded) sia "SESS_20260721_1430"
// (datato) ordinano correttamente come stringhe -> l'ultimo alfabetico e' il
// piu' recente. Scrive il path completo in outDir. Ritorna true se trovata.
//  (Sostituisce la vecchia scansione per indice numerico, che non vedeva le
//   cartelle datate — bug evitato: enumerare il filesystem, non indovinare i nomi.)
static bool lastExistingSessionDir(char* outDir, size_t outSz) {
  File root = SD.open(ROOT_DIR);
  if (!root || !root.isDirectory()) { if (root) root.close(); return false; }

  char best[48] = "";
  File e = root.openNextFile();
  while (e) {
    if (e.isDirectory()) {
      const char* nm = e.name();
      // e.name() puo' essere il path completo o il solo nome a seconda del core:
      // isoliamo il basename dopo l'ultimo '/'.
      const char* base = strrchr(nm, '/');
      base = base ? base + 1 : nm;
      if (strncmp(base, "SESS_", 5) == 0) {
        if (strcmp(base, best) > 0) {
          snprintf(best, sizeof(best), "%s", base);
        }
      }
    }
    e.close();
    e = root.openNextFile();
  }
  root.close();

  if (best[0] == '\0') return false;
  snprintf(outDir, outSz, "%s/%s", ROOT_DIR, best);
  return true;
}

// Conta i tiri gia' salvati in uno shots.csv: numero di righe-dati (esclusi il
// commento '#' e l'header colonne). Serve a far ripartire il contatore dal
// punto giusto quando si RIPRENDE una sessione, cosi' i nuovi burst non
// sovrascrivono i vecchi (burst_NNNN.bin numerati da dove eravamo).
// ----------------------------------------------------------------------------
//  writeSessionTxt — l'UNICO posto che scrive session.txt
// ----------------------------------------------------------------------------
//  PERCHE' UNO SOLO (§4.1)
//    Prima erano due blocchi quasi identici, in sd_session_start e in
//    sd_session_stop. Due copie di un formato divergono: e' bastato aggiungere
//    mount/off_* per doverli toccare entrambi, e la seconda volta ci si
//    dimentica.
//
//  PERCHE' SI RISCRIVE A OGNI TIRO
//    Il difetto trovato nella seduta del 28/07 (SESS_20260728_1005): session.txt
//    dichiarava `shots=39` mentre nella cartella c'erano 51 righe e 51 burst.
//    La sessione era stata chiusa dopo il tiro 39 e poi RIPRESA, e nessuno ha
//    riscritto session.txt alla fine — l'ultimo tiro ha un timestamp un milione
//    di millisecondi dopo `ended_ms`. Chi legge il file si fida del contatore e
//    perde dodici tiri.
//    Aggiornarlo solo alla chiusura presuppone che la chiusura avvenga sempre:
//    a batteria che si scarica, o spegnendo a fine percorso, non avviene.
//    Riscriverlo a ogni tiro costa un file di ~300 byte accanto ai 20 KB del
//    burst che stiamo gia' scrivendo: irrilevante, e il file e' sempre vero.
//
//  chiusa = true  -> scrive ended_ms/ended_datetime (fine sessione)
//           false -> scrive started_ms/datetime e last_shot_datetime
static void writeSessionTxt(bool chiusa) {
    if (!g_sd_ok || s_sessDir[0] == '\0') return;

    char p[64]; snprintf(p, sizeof(p), "%s/session.txt", s_sessDir);
    File f = SD.open(p, FILE_WRITE);        // FILE_WRITE tronca e riscrive
    if (!f) return;

    f.printf("ArchBB session %u\n", s_sessionId);
    f.printf("fw=%s\n", ARCHBB_FW_VERSION);
    f.printf("csv_format=%u\n", CSV_FORMAT_VER);
    f.printf("burst_format=%u\n", BURST_FORMAT_VER);
    // F24b — la frequenza DICHIARATA resta (e' il codice di configurazione),
    // ma accanto ci va quella MISURATA sui timestamp dei burst di questa
    // sessione. Se le due divergono, il file lo dice da solo invece di lasciare
    // che se ne accorga qualcuno fra sei mesi ricostruendo una timeline.
    if (s_odrRealeCentiHz)
      f.printf("odr_hz_reale=%u.%02u\n", s_odrRealeCentiHz / 100u,
               s_odrRealeCentiHz % 100u);
    f.printf("odr_hz=%u\n", odrToHz(ODR_250HZ));
    // Montaggio e taratura di QUESTA sessione: senza, un file letto fra sei mesi
    // non dice su quale asse stava il cant ne' quanto offset e' stato sottratto.
    f.printf("mount=%u\n", (unsigned)g_mount_orientation);
    f.printf("mount_lato=%s\n", mount_name(g_mount_orientation));
    f.printf("off_cant_deg=%.2f\n", g_config.offset_cant_deg);
    f.printf("off_alzo_deg=%.2f\n", g_config.offset_alzo_deg);
    // Marca esplicita del segno di hold. Dal F16 il segno e' quello giusto
    // (negativo = braccio abbassato). Le sedute precedenti non hanno questa
    // riga, e chi le rilegge deve invertirlo: un confronto sulla stringa di
    // versione sarebbe fragile, questa e' una dichiarazione.
    f.printf("hold_sign=corretto\n");
    // F19: la scala delle coordinate d'impatto. Senza queste due righe i
    // centimetri salvati sarebbero numeri senza unita' di misura per chi
    // rilegge la seduta dopo un cambio di parametro.
    f.printf("imp_raggio_colpito_cm=%u\n", g_config.imp_raggio_colpito_cm);
    f.printf("imp_raggio_mancato_cm=%u\n", g_config.imp_raggio_mancato_cm);

    char iso[24]; rtc_iso_string(iso, sizeof(iso));
    if (chiusa) {
        f.printf("ended_ms=%lu\n", (unsigned long)millis());
        f.printf("ended_datetime=%s\n", iso);
    } else {
        f.printf("started_ms=%lu\n", (unsigned long)s_sessionStartMs);
        f.printf("datetime=%s\n", s_sessionStartIso);
        // F24b — L'ORA DELL'ULTIMO TIRO, NON L'ORA DI ADESSO.
        //  Prima qui andava sempre `iso`, cioe' l'istante della scrittura. Alla
        //  RIPRESA di una sessione (sd_session_resume_last -> writeSessionTxt)
        //  s_shotCount arriva gia' > 0 dal CSV, e il file finiva per dichiarare
        //  un ultimo tiro che non era mai avvenuto: nella sessione 1215 del
        //  12/09 diceva 13:10:56 mentre l'ultimo tiro vero era delle 13:04:27.
        //  Sei minuti e mezzo di bugia sullo stesso file che il commento qui
        //  sopra difende dal difetto gemello del 28/07.
        //
        //  Adesso l'ora si memorizza QUANDO il tiro avviene, e la ripresa non la
        //  tocca. Se e' vuota (sessione ereditata da un avvio precedente, di cui
        //  non sappiamo l'ora) NON si scrive la riga: meglio un campo assente di
        //  un campo che mente.
        if (s_shotCount > 0 && s_lastShotIso[0])
            f.printf("last_shot_datetime=%s\n", s_lastShotIso);
    }
    // Il contatore e' SEMPRE quello vero al momento della scrittura.
    f.printf("shots=%u\n", s_shotCount);
    f.close();
}

static uint16_t countCsvRows(const char* csvPath) {
  File f = SD.open(csvPath, FILE_READ);
  if (!f) return 0;
  uint16_t rows = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (line.length() == 0) continue;
    if (line[0] == '#') continue;                 // commento di versione
    if (line.startsWith("shot,")) continue;       // header colonne
    rows++;
  }
  f.close();
  return rows;
}

// ----------------------------------------------------------------------------
//  sd_init — monta la card sull'host FSPI dedicato (VERSIONE DIAGNOSTICA)
// ----------------------------------------------------------------------------
//  "I dati comandano": se il mount fallisce, non basta sapere CHE fallisce,
//  serve sapere PERCHE'. Questa versione distingue le cause tipiche:
//    - card assente / nessun dialogo sul bus   -> pin o hardware
//    - card risponde ma filesystem illeggibile -> formato (exFAT invece di FAT32)
//    - mount ok solo a frequenza bassa          -> bus troppo veloce/piste
//  Prova due frequenze (prima prudente 400kHz-ish via 4MHz, poi la nominale):
//  molte card fanno l'handshake iniziale solo a bassa frequenza.
//
//  Il risultato diagnostico e' loggato (env debug) E memorizzato in g_sd_diag
//  cosi' il main puo' mostrarlo a schermo. In campo, muto: conta solo g_sd_ok.
// ----------------------------------------------------------------------------
char g_sd_diag[48] = "non inizializzata";   // ultima diagnosi leggibile

static bool tryMount(uint32_t freq) {
  // SD.begin puo' essere richiamata piu' volte: se un tentativo fallisce,
  // SD.end() libera lo stato prima del successivo.
  SD.end();
  return SD.begin(SD_PIN_CS, s_sdSPI, freq);
}

bool sd_init() {
  if (g_sd_ok) return true;   // gia' montata: idempotente

  // Avvia il bus FSPI sui NOSTRI pin (ordine SPIClass: SCK, MISO, MOSI, SS).
  s_sdSPI.begin(SD_PIN_SCK, SD_PIN_MISO, SD_PIN_MOSI, SD_PIN_CS);

  // --- Tentativo 1: frequenza BASSA (handshake robusto) --------------------
  //  4 MHz e' conservativo: se la card e le piste sono ok, monta di sicuro qui.
  bool mounted = tryMount(4000000UL);
  uint32_t usedFreq = 4000000UL;

  // --- Tentativo 2: frequenza nominale, solo se il primo e' andato ---------
  //  Se il mount basso e' riuscito, riproviamo alla frequenza piena: se anche
  //  quella regge, lavoriamo veloci; se no, restiamo a 4 MHz (comunque ok per
  //  ~20 KB a tiro).
  if (mounted) {
    if (tryMount(SD_SPI_FREQ_HZ)) usedFreq = SD_SPI_FREQ_HZ;
    else {
      tryMount(4000000UL);        // ripristina il mount basso che funzionava
      usedFreq = 4000000UL;
    }
  }

  if (!mounted) {
    // Nessun mount nemmeno a bassa frequenza. Distinguiamo "card assente" da
    // "card presente ma filesystem illeggibile": cardType() interroga il bus.
    uint8_t ct = SD.cardType();
    if (ct == CARD_NONE) {
      snprintf(g_sd_diag, sizeof(g_sd_diag), "nessun dialogo (pin/card assente)");
    } else {
      // La card risponde sul bus (cardType != NONE) ma begin fallisce -> quasi
      // sempre e' il FILESYSTEM: card formattata exFAT (o >32GB di fabbrica),
      // che la lib SD di Arduino-ESP32 non legge. Serve FAT32.
      snprintf(g_sd_diag, sizeof(g_sd_diag), "card ok ma non FAT32 (riformatta)");
    }
    DBG("SD: mount FALLITO -> %s", g_sd_diag);
    g_sd_ok = false;
    return false;
  }

  // Mount riuscito: doppio-controllo che non sia CARD_NONE (SD.begin talvolta
  // passa con card assente su alcuni core).
  uint8_t ct = SD.cardType();
  if (ct == CARD_NONE) {
    snprintf(g_sd_diag, sizeof(g_sd_diag), "mount vuoto (CARD_NONE)");
    DBG("SD: %s", g_sd_diag);
    g_sd_ok = false;
    return false;
  }

  // Assicura la cartella radice /ARCHBB.
  if (!SD.exists(ROOT_DIR)) {
    if (!SD.mkdir(ROOT_DIR)) {
      snprintf(g_sd_diag, sizeof(g_sd_diag), "montata ma sola-lettura?");
      DBG("SD: impossibile creare %s", ROOT_DIR);
      g_sd_ok = false;
      return false;
    }
  }

  // Nome tipo card per la diagnosi (utile a colpo d'occhio).
  const char* ctName = (ct == CARD_MMC) ? "MMC" :
                       (ct == CARD_SD)  ? "SDSC" :
                       (ct == CARD_SDHC)? "SDHC" : "?";
  g_sd_ok = true;   // prima del calcolo spazio: sd_free_refresh lo richiede

  // Calcolo dello spazio UNA VOLTA al mount (popola la cache). Da qui in poi la
  // UI legge sempre la cache: il refresh in ATTESA non tocca piu' la FAT.
  sd_free_refresh();

  snprintf(g_sd_diag, sizeof(g_sd_diag), "%s %luMB @%luMHz",
           ctName, (unsigned long)sd_total_mb(), (unsigned long)(usedFreq/1000000UL));

  DBG("SD: montata (%s, %lu MB liberi)", g_sd_diag, (unsigned long)sd_free_mb());
  return true;
}

// ----------------------------------------------------------------------------
//  buildHybridSessDir — nome cartella IBRIDO (datato se RTC valido, numerato se no)
// ----------------------------------------------------------------------------
//  FASE 6: se l'RTC e' impostato, la cartella e' PARLANTE: SESS_20260721_1430.
//  Se l'RTC non e' ancora impostato (finche' non arriva il BLE), si ricade sul
//  nome NUMERATO SESS_0001 — sempre univoco dal filesystem. Cosi' i nomi datati
//  arrivano appena l'ora e' valida, ma NON si rompe nulla adesso.
//
//  Anti-collisione: due sessioni nello stesso minuto (raro) avrebbero lo stesso
//  nome datato. Aggiungiamo un suffisso 'b','c',... finche' il nome e' libero,
//  cosi' non si sovrascrive MAI una sessione esistente.
//  Ritorna true se ha prodotto un nome datato, false se numerato (fallback).
static bool buildHybridSessDir(char* out, size_t outSz) {
  char stamp[14];
  rtc_compact_string(stamp, sizeof(stamp));   // "YYYYMMDD_HHMM" o "" se non valido

  if (stamp[0] != '\0') {
    // RTC valido -> nome datato, con suffisso anti-collisione se serve.
    snprintf(out, outSz, "%s/SESS_%s", ROOT_DIR, stamp);
    if (!SD.exists(out)) return true;         // libero: fatto
    for (char suf = 'b'; suf <= 'z'; suf++) {
      snprintf(out, outSz, "%s/SESS_%s%c", ROOT_DIR, stamp, suf);
      if (!SD.exists(out)) return true;
    }
    // 25 sessioni nello stesso minuto: impossibile in pratica. Cade al numerato.
  }

  // Fallback numerato (RTC non valido, o collisione estrema).
  uint16_t id = firstFreeSessionId();
  buildSessDir(out, outSz, id);
  return false;
}

// ----------------------------------------------------------------------------
//  sd_session_start — apre una nuova sessione
// ----------------------------------------------------------------------------
uint16_t sd_session_start() {
  if (!g_sd_ok) return 0;

  // Se c'e' gia' una sessione aperta, la chiudiamo pulita prima di aprirne una
  // nuova: una sola sessione attiva alla volta.
  if (g_session_open) sd_session_stop();

  // Nome cartella ibrido: datato se RTC valido, numerato altrimenti.
  buildHybridSessDir(s_sessDir, sizeof(s_sessDir));
  if (!SD.mkdir(s_sessDir)) {
    DBG("SD: mkdir sessione %s fallito", s_sessDir);
    s_sessDir[0] = '\0';
    return 0;
  }
  // ID progressivo per compatibilita' interna (session.txt, contatori): resta il
  // primo-libero numerico, indipendente dal nome cartella.
  uint16_t id = firstFreeSessionId();

  // Lo STATO va impostato PRIMA di scrivere: writeSessionTxt stampa s_sessionId
  // e s_shotCount, che qui erano assegnati piu' in basso. Scrivendo prima, il
  // file usciva con "ArchBB session 0" e il conteggio della sessione
  // precedente — un errore silenzioso e per niente ovvio da rileggere.
  s_sessionId      = id;
  s_shotCount      = 0;
  s_sessionStartMs = millis();
  rtc_iso_string(s_sessionStartIso, sizeof(s_sessionStartIso));
  writeSessionTxt(false);

  // --- shots.csv: header con versione e nomi colonna -------------------------
  //  L'header vive una volta sola, in testa al file. I campi nuovi andranno
  //  SEMPRE in coda (come nei pacchetti BLE): i file vecchi restano leggibili
  //  perche' l'app mappa per NOME colonna, non per posizione.
  {
    char p[64]; snprintf(p, sizeof(p), "%s/shots.csv", s_sessDir);
    File f = SD.open(p, FILE_WRITE);
    if (!f) { DBG("SD: creazione shots.csv fallita"); return 0; }
    f.printf("# ArchBB CSV v%u  fw=%s\n", CSV_FORMAT_VER, ARCHBB_FW_VERSION);
    // Colonne. cant/alzo/hold in centesimi di grado (interi), jerk in m/s^3.
    // I flag *_valid non li scriviamo come colonna: usiamo la SENTINELLA nel
    // valore stesso (campo vuoto) per "non calcolato", coerente con elev.
    f.println(CSV_HEADER);
    f.close();
  }

  g_session_open = true;   // s_sessionId/s_shotCount impostati sopra
  DBG("SD: sessione %u aperta (%s)", id, s_sessDir);
  return id;
}

// ----------------------------------------------------------------------------
//  sd_session_resume_last — RIAPRE l'ultima sessione esistente (accoda)
// ----------------------------------------------------------------------------
//  Riprende la cartella con numero piu' alto gia' su card e fa ripartire il
//  contatore tiri dal numero di righe gia' in shots.csv. I nuovi tiri si
//  ACCODANO: appendCsvRow scrive in fondo, e i burst_NNNN.bin riprendono da
//  dove eravamo (nessuna sovrascrittura). Ritorna il numero di sessione ripresa,
//  o 0 se non c'e' nessuna sessione da riprendere (allora il chiamante apre una
//  nuova). Non ricrea header ne' session.txt: la sessione esiste gia'.
uint16_t sd_session_resume_last() {
  if (!g_sd_ok) return 0;
  if (g_session_open) sd_session_stop();

  // Trova l'ultima cartella reale (datata o numerata) e riprendila.
  if (!lastExistingSessionDir(s_sessDir, sizeof(s_sessDir))) {
    return 0;   // niente da riprendere -> il chiamante fara' start
  }

  // Riparti dal conteggio reale delle righe gia' scritte.
  char csv[64]; snprintf(csv, sizeof(csv), "%s/shots.csv", s_sessDir);
  if (!SD.exists(csv)) {
    // Cartella esiste ma senza CSV (sessione corrotta/vuota): trattala come
    // nuova ripartendo l'header. Raro, ma non lasciamo lo stato ambiguo.
    File f = SD.open(csv, FILE_WRITE);
    if (f) {
      f.printf("# ArchBB CSV v%u  fw=%s\n", CSV_FORMAT_VER, ARCHBB_FW_VERSION);
      f.println(CSV_HEADER);
      f.close();
    }
    s_shotCount = 0;
  } else {
    s_shotCount = countCsvRows(csv);
  }

  // s_sessionId: manteniamo un id numerico interno (primo-libero) solo per i
  // log e i contatori; l'IDENTITA' della sessione ora e' il nome cartella.
  s_sessionId    = firstFreeSessionId();
  g_session_open = true;
  // Riallinea session.txt al conteggio reale appena riletto dal CSV: se la
  // sessione era stata chiusa, il file dichiara ancora il numero vecchio.
  s_sessionStartMs = millis();
  rtc_iso_string(s_sessionStartIso, sizeof(s_sessionStartIso));
  writeSessionTxt(false);

  DBG("SD: sessione RIPRESA %s (%u tiri gia' presenti)", s_sessDir, s_shotCount);
  return s_sessionId;
}

// ----------------------------------------------------------------------------
//  sd_session_stop — chiude la sessione corrente
// ----------------------------------------------------------------------------
void sd_session_stop() {
  if (!g_session_open) return;

  writeSessionTxt(true);
  DBG("SD: sessione %u chiusa (%u tiri)", s_sessionId, s_shotCount);
  g_session_open = false;
  s_sessionId    = 0;
  s_shotCount    = 0;
  s_sessDir[0]   = '\0';
}

uint16_t sd_session_id()         { return s_sessionId; }
uint16_t sd_session_shot_count() { return s_shotCount; }

// True se c'e' almeno una cartella SESS_* su card (da riprendere).
bool sd_has_previous_session() {
  if (!g_sd_ok) return false;
  char dir[48];
  return lastExistingSessionDir(dir, sizeof(dir));
}

// Info sull'ultima sessione (nome cartella + tiri gia' salvati) senza aprirla.
// name riceve il basename della cartella (es. "SESS_20260721_1430" o "SESS_0001").
void sd_last_session_info(char* name, size_t nameSz, uint16_t& shots) {
  if (nameSz) name[0] = '\0';
  shots = 0;
  if (!g_sd_ok) return;
  char dir[48];
  if (!lastExistingSessionDir(dir, sizeof(dir))) return;
  // basename dopo l'ultimo '/'
  const char* base = strrchr(dir, '/');
  base = base ? base + 1 : dir;
  snprintf(name, nameSz, "%s", base);
  char csv[64]; snprintf(csv, sizeof(csv), "%s/shots.csv", dir);
  shots = countCsvRows(csv);
}

// ----------------------------------------------------------------------------
//  FASE 7 — elenco sessioni per il BLE / companion app.
// ----------------------------------------------------------------------------
//  Riusa lo stesso principio di lastExistingSessionDir: enumerare il filesystem
//  (la SD *e'* lo stato), non indovinare i nomi. Qui pero' raccogliamo TUTTE le
//  cartelle SESS_*, non solo l'ultima. Per l'ordinamento decrescente sfruttiamo
//  che i nomi SESS_* ordinano correttamente come stringhe (sia datati sia
//  numerati): inseriamo ogni nome nella posizione giusta con un semplice
//  insertion-sort discendente, cosi' out[0] e' sempre la piu' recente.

// Conta le cartelle SESS_* senza scrivere nulla (per session_count nello STATUS).
uint16_t sd_session_total() {
  if (!g_sd_ok) return 0;
  File root = SD.open(ROOT_DIR);
  if (!root || !root.isDirectory()) { if (root) root.close(); return 0; }
  uint16_t count = 0;
  File e = root.openNextFile();
  while (e) {
    if (e.isDirectory()) {
      const char* nm = e.name();
      const char* base = strrchr(nm, '/'); base = base ? base + 1 : nm;
      if (strncmp(base, "SESS_", 5) == 0) count++;
    }
    e.close();
    e = root.openNextFile();
  }
  root.close();
  return count;
}

uint16_t sd_enumerate_sessions(SdSessionInfo* out, uint16_t maxN, uint16_t& total) {
  total = 0;
  if (!out || maxN == 0 || !g_sd_ok) return 0;

  File root = SD.open(ROOT_DIR);
  if (!root || !root.isDirectory()) { if (root) root.close(); return 0; }

  uint16_t filled = 0;   // record attualmente in out[]
  File e = root.openNextFile();
  while (e) {
    if (e.isDirectory()) {
      const char* nm = e.name();
      const char* base = strrchr(nm, '/'); base = base ? base + 1 : nm;
      if (strncmp(base, "SESS_", 5) == 0) {
        total++;   // conta SEMPRE, anche se out[] e' gia' pieno

        // Inseriamo in out[] mantenendo l'ordine DECRESCENTE (piu' recente in
        // testa). Se out[] e' pieno, il nuovo entra solo se e' piu' recente del
        // piu' vecchio in lista (out[filled-1]); poi si scarta l'ultimo.
        if (filled < maxN || strcmp(base, out[filled - 1].name) > 0) {
          // Trova la posizione di inserimento (primo elemento < base).
          int pos = filled < maxN ? filled : maxN - 1;   // slot di partenza
          if (filled < maxN) filled++;                    // c'e' spazio: cresce
          // Shift verso il basso finche' il precedente e' piu' piccolo di base.
          while (pos > 0 && strcmp(out[pos - 1].name, base) < 0) {
            out[pos] = out[pos - 1];
            pos--;
          }
          // Scrivi il nuovo record in posizione pos.
          snprintf(out[pos].name, sizeof(out[pos].name), "%s", base);
          char csv[64]; snprintf(csv, sizeof(csv), "%s/%s/shots.csv", ROOT_DIR, base);
          out[pos].shots = countCsvRows(csv);
        }
      }
    }
    e.close();
    e = root.openNextFile();
  }
  root.close();
  return filled;
}

// ============================================================================
//  FASE 10 — risoluzione indice→nome e LETTURA dei file di sessione
// ============================================================================
//  Serve al download BLE: l'app dice "mandami il file X della sessione #k" e
//  qui traduciamo k nel percorso reale. L'unica fonte dell'ordinamento e'
//  sd_enumerate_sessions: la stessa che genera l'elenco mandato all'app. Se
//  usassimo un secondo criterio di ordinamento (per data del filesystem, per
//  esempio) l'indice #3 dell'app e l'indice #3 del download potrebbero cadere
//  su sessioni diverse. Una sola enumerazione, un solo significato.
// ----------------------------------------------------------------------------

bool sd_session_name_by_index(uint16_t index, char* out, size_t outSz) {
  if (!out || outSz == 0) return false;
  out[0] = '\0';
  if (!g_sd_ok) return false;
  if (index >= SD_MAX_ENUM_SESSIONS) return false;   // fuori dalla finestra enumerata

  // static: l'array e' 64*28 = 1792 byte, troppo per lo stack di un task da
  // 4096. Sicuro perche' chiamato solo dal ble_task (un solo consumatore alla
  // volta, i task IMU sono sospesi in modo BLE).
  static SdSessionInfo s_enum[SD_MAX_ENUM_SESSIONS];
  uint16_t total = 0;
  uint16_t n = sd_enumerate_sessions(s_enum, SD_MAX_ENUM_SESSIONS, total);
  if (index >= n) return false;

  snprintf(out, outSz, "%s", s_enum[index].name);
  return true;
}

// ----------------------------------------------------------------------------
//  Reader di file di sessione — vedi il razionale in sd_storage.h
// ----------------------------------------------------------------------------
static File s_reader;              // handle aperto (falsy se chiuso)
static bool s_reader_open = false;

bool sd_reader_open(uint16_t index, const char* fname, uint32_t& size) {
  size = 0;
  sd_reader_close();               // l'ultimo vince: mai due handle appesi
  if (!g_sd_ok || !fname || !fname[0]) return false;

  // 1) indice -> nome cartella
  char sess[32];
  if (!sd_session_name_by_index(index, sess, sizeof(sess))) {
    DBG("SD: reader — sessione #%u inesistente", index);
    return false;
  }

  // 2) percorso completo. char[96]: "/ARCHBB/" (8) + nome (fino a 25) + "/" +
  //    nome file (fino a 20) = ~55; 96 e' margine abbondante. Lezione Fase 5:
  //    i buffer di percorso si dimensionano sulla variante PIU' LUNGA, mai
  //    sulla piu' corta che si ha sott'occhio.
  char path[96];
  snprintf(path, sizeof(path), "%s/%s/%s", ROOT_DIR, sess, fname);

  s_reader = SD.open(path, FILE_READ);
  if (!s_reader) { DBG("SD: reader — apertura %s fallita", path); return false; }
  if (s_reader.isDirectory()) { s_reader.close(); return false; }

  size = (uint32_t)s_reader.size();
  s_reader_open = true;
  DBG("SD: reader aperto %s (%lu byte)", path, (unsigned long)size);
  return true;
}

size_t sd_reader_read(uint8_t* buf, size_t n) {
  if (!s_reader_open || !buf || n == 0) return 0;
  // Arduino-ESP32 3.x: File::read(uint8_t*, size_t) ritorna size_t (0 = EOF o
  // errore). Niente cast a int: su un file grande un int firmato sarebbe una
  // trappola silenziosa.
  return s_reader.read(buf, n);
}

void sd_reader_close() {
  if (s_reader_open) { s_reader.close(); s_reader_open = false; }
}

// ----------------------------------------------------------------------------
//  writeBurstBin — scrive burst_NNNN.bin (header 16B + campioni grezzi)
// ----------------------------------------------------------------------------
//  Ritorna true se il file e' stato scritto per intero. Il chiamante usa il
//  ritorno solo per il log: il fallimento del .bin NON invalida il tiro (il CSV
//  e' il dato che conta). Nome file restituito in outName per la colonna CSV.
// ----------------------------------------------------------------------------
//  FASE 24b — FREQUENZA DI CAMPIONAMENTO MISURATA, non dichiarata
// ----------------------------------------------------------------------------
//  La sessione del 12/09 diceva odr_hz=224 in tutti e cinque i session.txt. I
//  timestamp dei 41 burst dicevano 200,12 Hz, stabilissimi (200,00-200,20).
//  odrToHz(ODR_250HZ) ritorna una COSTANTE DI COMPILAZIONE, e quella costante
//  dimensiona le finestre: pre = 448 campioni valevano 2,238 s invece di 2,000,
//  post 1,119 invece di 1,000. Il 12% di errore sistematico su ogni conversione
//  campioni->tempo, e una riga di metadati falsa in ogni cartella.
//
//  La correzione non e' aggiustare la costante — sarebbe rimediare a un numero
//  inventato con un altro numero inventato. E' SMETTERE DI DICHIARARE cio' che
//  non si e' misurato: i timestamp sono nel burst, la frequenza si calcola.
//
//  Mediana e non media: un singolo intervallo lungo (una scrittura SD che ha
//  ritardato un campione) sposterebbe la media e lascerebbe la mediana ferma.
//  Ritorna centi-hertz (20012 = 200,12 Hz), che sta in un uint16 fino a 655 Hz.
static uint16_t misuraOdrCentiHz(const ImuSample* burst, uint16_t n, uint16_t trigIdx) {
  // Si misura SOLO sul tratto POST-trigger. Due ragioni, entrambe dirimenti:
  //
  //  1) E' SEMPRE FRESCO. I campioni dopo il congelamento vengono scritti in
  //     quel momento, uno dopo l'altro: non possono contenere residui
  //     dell'anello. Il pre-trigger si', e un burst con un buco di 6 secondi
  //     (visto davvero, tiro 2 del 14/09) manderebbe a gambe all'aria
  //     qualunque media calcolata sull'intera finestra.
  //
  //  2) LO SPAN E' ESATTO. (campioni-1)/(durata) non e' una stima: e' la
  //     definizione. Nessun ordinamento, nessuna mediana, nessun parametro.
  //
  //  PERCHE' NON LA MEDIANA DEGLI INTERVALLI — l'errore della 24b, e vale la
  //  pena lasciarlo scritto. Il tick FreeRTOS e' a 1 ms e imu_task fa
  //  vTaskDelay(1): ogni campione viene quantizzato a 4000 o 5000 us attorno al
  //  periodo vero di ~4590 us. La distribuzione e' BIMODALE, e su una
  //  distribuzione bimodale la mediana non stima niente: sceglie il modo piu'
  //  numeroso. Sui burst del 12/09 dava 200,12 Hz contro i 217,8 reali — un
  //  errore dell'8%, con tanto di tabella costruita sopra. La media di medie
  //  locali della 24b smorzava la bimodalita' e arrivava a 217,39, quasi giusto
  //  ma per costruzione, non per principio. Questo e' giusto e basta.
  //
  //  Ritorna centi-hertz (21777 = 217,77 Hz), che sta in un uint16 fino a 655 Hz.
  if (!burst || n < 16 || trigIdx + 16 >= n) return 0;
  const uint32_t t0 = burst[trigIdx].timestamp_us;
  const uint32_t t1 = burst[n - 1].timestamp_us;
  if (t1 <= t0) return 0;
  const uint32_t campioni = (uint32_t)(n - 1 - trigIdx);
  const uint64_t chz = (uint64_t)campioni * 100000000ULL / (uint64_t)(t1 - t0);
  return (chz > 65535ULL) ? 0u : (uint16_t)chz;
}

static bool writeBurstBin(uint16_t shotNum, const ImuSample* burst, uint16_t n,
                          uint16_t trigIdx, OdrSetting odr,
                          uint16_t preValidi, uint16_t odrCentiHz,
                          char* outName, size_t outNameSz) {
  outName[0] = '\0';
  if (burst == nullptr || n == 0) return false;

  char path[64]; buildBurstPath(path, sizeof(path), shotNum);
  File f = SD.open(path, FILE_WRITE);
  if (!f) { DBG("SD: apertura %s fallita", path); return false; }

  // --- Header 16 byte, little-endian (nativo ESP32) -------------------------
  //   [0..3]  magic "ABB1"
  //   [4]     format version
  //   [5]     odr code (OdrSetting)
  //   [6..7]  n_campioni (uint16)
  //   [8..9]  trigger_idx (uint16)
  //   [10..11] shot_num (uint16)
  //   [12..13] pre_validi  (uint16)  <- FASE 24b
  //   [14..15] odr misurato in centi-Hz (uint16) <- FASE 24b
  //
  //  I quattro byte erano riservati proprio per questo: l'header resta di 16
  //  byte, i campioni restano a passo 30, e un lettore v1 che salta i riservati
  //  legge i file v2 senza accorgersene. Nessuna rottura di formato.
  //
  //  Perche' QUI e non in coda al CSV: sono proprieta' del BURST, non del tiro.
  //  Chi apre un .bin da solo — a mano, in Python, fra sei mesi — deve poter
  //  sapere quanti dei suoi campioni valgono e a che passo sono stati presi,
  //  senza avere il CSV accanto.
  uint8_t hdr[16] = {0};
  memcpy(hdr, BURST_MAGIC, 4);
  hdr[4] = BURST_FORMAT_VER;
  hdr[5] = (uint8_t)odr;
  hdr[6] = (uint8_t)(n & 0xFF);        hdr[7] = (uint8_t)(n >> 8);
  hdr[8] = (uint8_t)(trigIdx & 0xFF);  hdr[9] = (uint8_t)(trigIdx >> 8);
  hdr[10]= (uint8_t)(shotNum & 0xFF);  hdr[11]= (uint8_t)(shotNum >> 8);
  hdr[12]= (uint8_t)(preValidi & 0xFF);  hdr[13]= (uint8_t)(preValidi >> 8);
  hdr[14]= (uint8_t)(odrCentiHz & 0xFF); hdr[15]= (uint8_t)(odrCentiHz >> 8);

  size_t wrote = f.write(hdr, sizeof(hdr));
  // I campioni COSI' COME SONO IN RAM: ImuSample e' packed 30B, il file e'
  // rileggibile con uno struct a passo 30 senza conversioni.
  wrote += f.write((const uint8_t*)burst, (size_t)n * sizeof(ImuSample));
  f.close();

  size_t expected = sizeof(hdr) + (size_t)n * sizeof(ImuSample);
  if (wrote != expected) {
    DBG("SD: burst troncato (%u/%u byte)", (unsigned)wrote, (unsigned)expected);
    return false;
  }
  // Solo il nome-file (senza path) va nella colonna CSV: l'app lo cerca dentro
  // la cartella della sessione, quindi il path relativo basta.
  snprintf(outName, outNameSz, "burst_%04u.bin", shotNum);
  return true;
}

// ----------------------------------------------------------------------------
//  appendCsvRow — appende UNA riga a shots.csv e CHIUDE (flush)
// ----------------------------------------------------------------------------
//  Il campo vuoto (due virgole di fila) e' la SENTINELLA di "non calcolato",
//  coerente con la filosofia dei *_valid: un hold non valido non scrive uno 0
//  (che sarebbe "tenuta perfetta"), scrive NIENTE. Chi rilegge distingue.
static bool appendCsvRow(uint16_t shotNum, const ScoreResult& r,
                         const char* burstName, uint16_t preValidi) {
  char p[64]; snprintf(p, sizeof(p), "%s/shots.csv", s_sessDir);
  File f = SD.open(p, FILE_APPEND);      // append: non tocca cio' che c'e' gia'
  if (!f) { DBG("SD: append shots.csv fallito"); return false; }

  // Costruiamo la riga in un buffer e la scriviamo in un colpo solo.
  char row[256];
  int k = 0;

  // shot, ts_ms, colpito, zona, dist_m
  k += snprintf(row+k, sizeof(row)-k, "%u,%lu,%u,",
                shotNum, (unsigned long)millis(), r.colpito ? 1 : 0);
  // zona: derivata dall'impatto (F19). Vuota se ignota.
  if (r.zona != ZONA_IGNOTA) k += snprintf(row+k, sizeof(row)-k, "%u,", r.zona);
  else                       k += snprintf(row+k, sizeof(row)-k, ",");
  // dist_m: 0 = ignota -> campo vuoto
  if (r.distanza_m > 0) k += snprintf(row+k, sizeof(row)-k, "%u,", r.distanza_m);
  else                  k += snprintf(row+k, sizeof(row)-k, ",");
  // elev_deg: sentinella ELEV_NOT_SET -> campo vuoto
  if (elevIsSet(r.elev_deg)) k += snprintf(row+k, sizeof(row)-k, "%d,", r.elev_deg);
  else                       k += snprintf(row+k, sizeof(row)-k, ",");
  // arcs
  k += snprintf(row+k, sizeof(row)-k, "%d,", r.arcs);

  // cant_cdeg, alzo_cdeg, angoli_stabili — solo se angles_valid
  if (r.angles_valid) {
    k += snprintf(row+k, sizeof(row)-k, "%d,%d,%u,",
                  r.cant_cdeg, r.alzo_cdeg, r.angles_stable ? 1 : 0);
  } else {
    k += snprintf(row+k, sizeof(row)-k, ",,,");
  }

  // hold_cdeg, hold_class — solo se hold_valid
  if (r.hold_valid) k += snprintf(row+k, sizeof(row)-k, "%d,%u,", r.hold_cdeg, r.hold_class);
  else              k += snprintf(row+k, sizeof(row)-k, ",,");

  // release_jerk, release_class — solo se release_valid
  if (r.release_valid) k += snprintf(row+k, sizeof(row)-k, "%u,%u,", r.release_jerk, r.release_class);
  else                 k += snprintf(row+k, sizeof(row)-k, ",,");

  // burst_file: nome del .bin o vuoto se non salvato
  k += snprintf(row+k, sizeof(row)-k, "%s,", burstName ? burstName : "");

  // --- F19: coordinate d'impatto (ultime colonne) --------------------------
  //  IMP_NOT_SET -> campi vuoti, stessa sentinella-per-assenza di elev e delle
  //  metriche. Uno 0/0 scritto al posto di "non rilevato" significherebbe
  //  "centro esatto", che e' il valore piu' desiderabile e il piu' falso.
  if (impIsSet(r.imp_x_cm) && impIsSet(r.imp_y_cm)) {
    k += snprintf(row+k, sizeof(row)-k, "%d,%d,%u",
                  r.imp_x_cm, r.imp_y_cm, r.imp_raggio_cm);
  } else {
    k += snprintf(row+k, sizeof(row)-k, ",,%u", r.imp_raggio_cm);
  }

  // --- F20: tempi del gesto -------------------------------------------------
  //  Sentinella TEMPO_NOT_SET -> campi vuoti, come per tutto il resto.
  if (r.tempo_mira_ms != TEMPO_NOT_SET)
       k += snprintf(row+k, sizeof(row)-k, ",%u", r.tempo_mira_ms);
  else k += snprintf(row+k, sizeof(row)-k, ",");
  if (r.tempo_alzata_ms != TEMPO_NOT_SET)
       k += snprintf(row+k, sizeof(row)-k, ",%u", r.tempo_alzata_ms);
  else k += snprintf(row+k, sizeof(row)-k, ",");

  // --- F20b: plausibilita' dell'assetto ------------------------------------
  k += snprintf(row+k, sizeof(row)-k, ",%u", r.assetto_ok);

  // --- F21: picco d'urto ----------------------------------------------------
  //  Scritto in CENTESIMI di m/s^2, interi: nessun float nel CSV, nessuna
  //  ambiguita' di separatore decimale fra locale italiana e pandas. Chi legge
  //  divide per 100, ed e' la stessa convenzione gia' usata per i cdeg.
  //  Campo VUOTO se non calcolato: lo zero e' un picco legittimo (arco fermo,
  //  trigger manuale a banco) e non puo' fare da sentinella.
  if (r.picco_valid) k += snprintf(row+k, sizeof(row)-k, ",%u", r.picco_cms2);
  else               k += snprintf(row+k, sizeof(row)-k, ",");

  // --- F24b: scarto fra elevazione armata e alzo misurato -------------------
  //  Si scrive SOLO se entrambe esistono davvero. Se l'elevazione non e' stata
  //  armata (ELEV_NOT_SET) o gli angoli non sono validi, il campo resta vuoto:
  //  uno zero significherebbe "coincidono perfettamente", che e' l'esatto
  //  contrario di "non lo sappiamo". Stessa disciplina di tutti gli altri *_valid.
  if (r.angles_valid && elevIsSet(r.elev_deg)) {
    int32_t sc = (int32_t)r.alzo_cdeg - (int32_t)r.elev_deg * 100;
    if (sc < 0) sc = -sc;
    k += snprintf(row+k, sizeof(row)-k, ",%ld", (long)sc);
  } else {
    k += snprintf(row+k, sizeof(row)-k, ",");
  }

  // --- F24b: campioni pre-trigger davvero appartenenti a questo tiro --------
  k += snprintf(row+k, sizeof(row)-k, ",%u", (unsigned)preValidi);

  f.println(row);
  f.close();                              // <-- flush: la riga e' su card
  return true;
}

// ----------------------------------------------------------------------------
//  sd_save_shot — orchestrazione: burst prima, CSV dopo (o solo CSV)
// ----------------------------------------------------------------------------
bool sd_save_shot(const ScoreResult& res,
                  const ImuSample* burst, uint16_t n, uint16_t trigIdx,
                  OdrSetting odr, uint16_t preValidi) {
  if (!g_sd_ok) return false;

  // Auto-start: chi tira senza aver aperto una sessione non perde il dato.
  if (!g_session_open) {
    if (sd_session_start() == 0) return false;   // card piena/assente
  }

  uint16_t shotNum = s_shotCount + 1;   // numero 1-based del tiro nella sessione

  // 1) Burst grezzo (sacrificabile): se fallisce, si prosegue col CSV.
  char burstName[20] = "";
  // FASE 24b: la frequenza si MISURA sui timestamp appena ricevuti, e il primo
  // valore buono diventa quello dichiarato in session.txt. Prima lo era una
  // costante di compilazione, e per una sessione intera ha mentito del 12%.
  const uint16_t odrCentiHz = misuraOdrCentiHz(burst, n, trigIdx);
  if (odrCentiHz) s_odrRealeCentiHz = odrCentiHz;

  bool burstOk = writeBurstBin(shotNum, burst, n, trigIdx, odr,
                               preValidi, odrCentiHz,
                               burstName, sizeof(burstName));
  (void)burstOk;   // il ritorno serve solo al log; non blocca il CSV

  // 2) CSV (dato prezioso): se questo fallisce, il tiro NON e' salvato.
  if (!appendCsvRow(shotNum, res, burstName, preValidi)) return false;

  s_shotCount = shotNum;   // contabilizza il tiro solo dopo il CSV riuscito
  rtc_iso_string(s_lastShotIso, sizeof(s_lastShotIso));   // F24b: ora del TIRO

  // session.txt aggiornato SUBITO, non solo alla chiusura. Farlo solo alla
  // chiusura presuppone che la chiusura avvenga: a batteria scarica o spegnendo
  // a fine percorso non avviene, ed e' cosi' che la seduta del 28/07 e' rimasta
  // a dichiarare 39 tiri avendone 51. Costa 300 byte accanto ai 20 KB del burst
  // appena scritto.
  writeSessionTxt(false);
  DBG("SD: tiro %u salvato (burst=%s)", shotNum, burstName[0] ? burstName : "no");
  // Lo spazio libero e' cambiato (~20 KB): aggiorna la cache UNA volta, qui,
  // fuori dal loop di refresh della UI. Cosi' la barra di stato resta veritiera
  // senza che il refresh periodico debba mai riscandire la FAT.
  sd_free_refresh();
  return true;
}

// ----------------------------------------------------------------------------
//  Spazio libero/totale (per la barra di stato in ATTESA)
// ----------------------------------------------------------------------------
//  sd_free_refresh() FA il conto pesante (scansione FAT) e aggiorna la cache.
//  Chiamarlo SOLO quando lo spazio e' cambiato davvero: al mount e dopo un
//  salvataggio. sd_free_mb()/sd_total_mb() ritornano la cache: istantanei e
//  NON bloccanti, sicuri da chiamare a ogni refresh della UI.
void sd_free_refresh() {
  if (!g_sd_ok) { s_freeMbCache = 0; s_totalMbCache = 0; s_spaceValid = false; return; }
  uint64_t total = SD.totalBytes();          // costoso: scandisce la FAT
  uint64_t used  = SD.usedBytes();           // costoso: scandisce la FAT
  uint64_t freeB = (total > used) ? (total - used) : 0;
  s_totalMbCache = (uint32_t)(total / (1024ULL * 1024ULL));
  s_freeMbCache  = (uint32_t)(freeB / (1024ULL * 1024ULL));
  s_spaceValid   = true;
}

uint32_t sd_free_mb() {
  // Se non e' mai stato calcolato (es. chiamata molto precoce), lo facciamo una
  // volta qui; da li' in poi e' sempre cache. Se la card non c'e', 0.
  if (!g_sd_ok) return 0;
  if (!s_spaceValid) sd_free_refresh();
  return s_freeMbCache;
}

uint32_t sd_total_mb() {
  if (!g_sd_ok) return 0;
  if (!s_spaceValid) sd_free_refresh();
  return s_totalMbCache;
}

// ============================================================================
//  FASE 24a — log energetico /ARCHBB/ENERGIA.CSV
// ============================================================================
static const char* ENERGIA_CSV = "/ARCHBB/ENERGIA.CSV";

// Apre in append e, se il file non esisteva, ci mette prima l'intestazione.
// Nota: SD.exists() prima di aprire, e non "controllo se e' vuoto dopo": un
// file aperto in append riporta gia' la posizione in coda, e size() su un
// handle appena aperto in append e' una di quelle cose che su qualche core
// funziona e su qualche altro no. Meglio la domanda esplicita.
static const char* const ENERGIA_HEADER =
    "iso,ms,v_batt,v_sys,pct,carica,bl_pct,cpu_mhz,power,temp_c";

static File energiaOpen() {
  const bool nuovo = !SD.exists(ENERGIA_CSV);
  File f = SD.open(ENERGIA_CSV, FILE_APPEND);
  if (!f) return f;
  if (nuovo) { f.println(ENERGIA_HEADER); return f; }

  // --- F24b.1: L'INTESTAZIONE VECCHIA NON DEVE MENTIRE ---------------------
  //  ENERGIA.CSV vive per sempre nella radice, apposta: le sessioni ci passano
  //  dentro e la curva non si spezza con loro. Ma l'intestazione si scrive solo
  //  alla creazione, quindi quando la 24b ha aggiunto temp_c il file esistente
  //  ha continuato a dichiarare nove colonne mentre le righe nuove ne avevano
  //  dieci. Chi legge mappando per nome trova un disallineamento, e lo trova in
  //  silenzio: nessun errore, solo una colonna che scivola.
  //
  //  Al primo accesso dopo l'avvio si confronta la prima riga con quella attesa.
  //  Se differisce si annota il cambio, in forma di commento: il file resta
  //  leggibile da chi salta le righe '#', e chi rilegge trova scritto dove le
  //  colonne sono cambiate e quali sono da li' in poi.
  static bool giaControllato = false;
  if (!giaControllato) {
    giaControllato = true;
    File r = SD.open(ENERGIA_CSV, FILE_READ);
    if (r) {
      char prima[160]; size_t k = 0;
      while (r.available() && k < sizeof(prima) - 1) {
        char c = (char)r.read();
        if (c == '\n') break;
        if (c != '\r') prima[k++] = c;
      }
      prima[k] = '\0';
      r.close();
      if (strcmp(prima, ENERGIA_HEADER) != 0) {
        f.printf("# COLONNE CAMBIATE, da qui in poi: %s\n", ENERGIA_HEADER);
      }
    }
  }
  return f;
}

bool sd_energia_log(float vBatt, float vSys, int pct, bool inCarica,
                    uint8_t blPct, uint32_t cpuMhz, uint8_t statoPower,
                    float tempC) {
  if (!g_sd_ok) return false;
  File f = energiaOpen();
  if (!f) return false;

  char iso[24]; rtc_iso_string(iso, sizeof(iso));

  // Le tensioni non leggibili diventano CAMPO VUOTO, non 0. Uno zero in una
  // colonna di volt e' indistinguibile da "batteria a terra" quando si rilegge
  // il file sei mesi dopo: e' la stessa disciplina dei *_valid nel CSV dei tiri.
  char riga[128];
  int k = snprintf(riga, sizeof(riga), "%s,%lu,", iso, (unsigned long)millis());
  if (isnan(vBatt)) k += snprintf(riga+k, sizeof(riga)-k, ",");
  else              k += snprintf(riga+k, sizeof(riga)-k, "%.3f,", (double)vBatt);
  if (isnan(vSys))  k += snprintf(riga+k, sizeof(riga)-k, ",");
  else              k += snprintf(riga+k, sizeof(riga)-k, "%.3f,", (double)vSys);
  if (pct < 0)      k += snprintf(riga+k, sizeof(riga)-k, ",");
  else              k += snprintf(riga+k, sizeof(riga)-k, "%d,", pct);
  k += snprintf(riga+k, sizeof(riga)-k, "%u,%u,%lu,%u,",
                inCarica ? 1u : 0u, (unsigned)blPct,
                (unsigned long)cpuMhz, (unsigned)statoPower);
  // F24b — temperatura. Non e' ambiente: e' il die del QMI8658 dentro un case
  // chiuso, quindi un paio di gradi sopra l'aria per autoriscaldamento. Ma la
  // cella sta nella stessa scatola, quindi come PROXY della sua temperatura e'
  // il migliore che abbiamo, ed e' gratis: il sensore c'e' gia' e il valore e'
  // gia' a schermo. La capacita' di una LiPo dipende dalla temperatura in modo
  // pesante — senza questa colonna, una scarica di novembre e una di luglio
  // sarebbero due numeri incomparabili senza che il file lo dica.
  if (!isnan(tempC)) k += snprintf(riga+k, sizeof(riga)-k, "%.1f", (double)tempC);
  // NaN -> campo vuoto: la riga finisce con la virgola gia' scritta sopra.

  f.println(riga);
  f.close();                 // chiudere = flush: se si spegne adesso, il dato c'e'
  return true;
}

bool sd_energia_nota(const char* testo) {
  if (!g_sd_ok || !testo) return false;
  File f = energiaOpen();
  if (!f) return false;
  char iso[24]; rtc_iso_string(iso, sizeof(iso));
  char riga[192];
  snprintf(riga, sizeof(riga), "# %s  %lu  %s", iso, (unsigned long)millis(), testo);
  f.println(riga);
  f.close();
  return true;
}
