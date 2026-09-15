// ============================================================================
//  ArchBB 1.83 — ble_service.h · FASE 7 (BLE modale: colloquio con smartphone)
//  Cesare Pagura · Padova/Noale IT · 21 luglio 2026
// ----------------------------------------------------------------------------
//  COSA E' E PERCHE' E' DIVERSO DAL BLE DELLA 1.69
//
//    Il BLE della 1.69 era un DEMONE SEMPRE ATTIVO: girava in parallelo allo
//    scoring, "beccava i burst al volo" e faceva LIVE streaming. Questo lo
//    costringeva a convivere con l'IMU (contesa I2C, back-pressure adattivo su
//    decine di notify per burst, PM-lock permanente, race cross-core).
//
//    Sulla 1.83 il BLE e' un MODO MODALE, un "capolinea" come CALIBRA e DIAG:
//      - ci si entra con una pressione breve del tasto BOOT (GPIO0, ingranaggio);
//      - il dispositivo SOSPENDE imu_task + trigger_task (Core 1 si libera);
//      - NimBLE gira su Core 0 e ha la scena tutta per se';
//      - l'app smartphone legge/scrive il Config, imposta l'RTC, chiede l'elenco
//        sessioni;
//      - si esce con un'altra pressione breve di BOOT -> config_apply() rende
//        operativi i nuovi parametri, i task IMU riprendono, si torna in ATTESA.
//
//    PERCHE' E' PIU' SICURO (e piu' semplice):
//      1) IMU sospesa durante il BLE = il rischio latente del bus I2C condiviso
//         (handoff §5) e' annullato PER COSTRUZIONE: nessuno martella l'I2C in
//         polling mentre il BLE e' attivo. Niente mutex di bus "per sicurezza"
//         (non c'e' contesa da proteggere: l'abbiamo eliminata alla radice).
//      2) Niente stream di burst = niente back-pressure complicata. Le notify
//         di questa fase sono singole e rade (una risposta config, un record
//         di elenco per volta): la disciplina onStatus+connection-interval
//         della 1.69 si riusa in forma MOLTO piu' leggera.
//      3) BLE su Core 0 (roadmap + lezione 1.69) con PM-lock anti-light-sleep:
//         previene la classe di crash esp_pm_impl_waiti gia' diagnosticata.
//
//  IL GATT (UUID base "bb1c..." = "BB 1.83 config", diverso dalla 1.69 "bb4a"
//  per non confondere l'app se entrambe fossero in giro):
//
//    Servizio ARCHBB_CFG_SERVICE
//      CONFIG   (READ/WRITE)   struct Config (35B, con version+checksum)
//      TIME     (WRITE)        7B: [yearLo,yearHi,mon,day,hour,min,sec] -> RTC
//      CMD      (WRITE)        [cmd][payload...]  (ping, list-sessions, reset)
//      STATUS   (READ/NOTIFY)  StatusPacket: stato+fw+batt+RTC+n.sessioni
//      SESSION  (NOTIFY)       SessionRecord, uno per notify (elenco sessioni)
//      DATA     (NOTIFY)       FASE 10: flusso di byte per il download file
//
//  METODO: file completo, commenti italiani, un pezzo alla volta validato sul
//  campo. Fase 7 ha portato config+RTC+elenco. Questa (Fase 10) aggiunge il
//  DOWNLOAD dei file leggeri di sessione — shots.csv e session.txt — sulla
//  caratteristica DATA. I burst grezzi (20 KB a tiro) restano DELIBERATAMENTE
//  fuori dal BLE: si leggono estraendo la card. Il perche' e' nel README di
//  fase e nel blocco §PROTOCOLLO qui sotto.
// ============================================================================
#pragma once
#include <Arduino.h>
#include "config_store.h"   // struct Config (la caratteristica CONFIG la espone)

// ----------------------------------------------------------------------------
//  UUID del servizio e delle caratteristiche
// ----------------------------------------------------------------------------
//  Base 128-bit "bb1c0000-0001-0000-0000-0000000000NN". Il suffisso NN
//  distingue le caratteristiche. Usare una base tutta nostra (non i 16-bit
//  standard) evita collisioni con servizi di sistema e rende il device
//  riconoscibile a colpo d'occhio nello scanner dell'app.
#define ARCHBB_CFG_SERVICE_UUID   "bb1c0000-0001-0000-0000-000000000001"
#define ARCHBB_CHAR_CONFIG_UUID   "bb1c0000-0001-0000-0000-000000000010"
#define ARCHBB_CHAR_TIME_UUID     "bb1c0000-0001-0000-0000-000000000020"
#define ARCHBB_CHAR_CMD_UUID      "bb1c0000-0001-0000-0000-000000000030"
#define ARCHBB_CHAR_STATUS_UUID   "bb1c0000-0001-0000-0000-000000000040"
#define ARCHBB_CHAR_SESSION_UUID  "bb1c0000-0001-0000-0000-000000000050"
#define ARCHBB_CHAR_DATA_UUID     "bb1c0000-0001-0000-0000-000000000060"

// ----------------------------------------------------------------------------
//  Comandi accettati dalla caratteristica CMD (primo byte del write)
// ----------------------------------------------------------------------------
enum BleCmdCode : uint8_t {
  BLE_CMD_PING          = 0x01,  // health-check: il device notifica STATUS
  BLE_CMD_LIST_SESSIONS = 0x02,  // il device notifica N SessionRecord su SESSION
  BLE_CMD_CONFIG_RESET  = 0x03,  // ripristina i default del config (poi salva+apply)
  BLE_CMD_GET_FILE      = 0x04,  // FASE 10: scarica un file di sessione su DATA
                                 // payload: [index u16][kind u8]
  BLE_CMD_MOUNT_CAL     = 0x05,  // taratura montaggio: "sono a piombo, azzera"
  BLE_CMD_MOUNT_CAL_RST = 0x06,  // azzera gli offset di montaggio
};

// ============================================================================
//  TARATURA DI MONTAGGIO (comandi 0x05 / 0x06)
// ============================================================================
//  COSA FA
//    L'arciere mette l'arco a piombo (in morsa, su cavalletto, o appoggiato a
//    uno stipite) e preme un tasto nell'app. Il device legge l'assetto e lo
//    salva come OFFSET in NVS. Da quel momento cant e alzo sono riferiti
//    all'arco a piombo, non al chip: l'inclinazione del supporto e il
//    disallineamento meccanico della staffa spariscono dai numeri.
//
//  PERCHE' SERVE
//    Sulla 1.69 c'era, sulla 1.83 no: i 17 gradi letti il 25/07 erano alzo
//    VERO piu' inclinazione della staffa, e senza taratura i due non si
//    separano. Le VARIAZIONI restano affidabili comunque (l'offset e' costante
//    e sparisce nelle differenze), ma il valore assoluto no — e l'alzo assoluto
//    serve, per esempio, a confrontarlo col simulatore balistico.
//
//  COME FUNZIONA CON I TASK SOSPESI
//    In modo BLE i task IMU sono fermi, quindi g_latest_sample non si aggiorna.
//    Si usa imu_read_raw(), che parla direttamente col sensore prendendo il
//    mutex del bus I2C: e' nata per questo (menu d'avvio) ed e' esattamente il
//    caso d'uso.
//
//  MEDIA SU PIU' LETTURE
//    Un solo campione porterebbe dentro il rumore di quel campione. Si media su
//    BLE_MOUNT_CAL_N letture; se troppe falliscono, la taratura NON viene
//    scritta. Meglio nessuna taratura che una sbagliata: la prima si rifa', la
//    seconda falsa ogni tiro successivo.
static constexpr int BLE_MOUNT_CAL_N   = 48;   // ~0,25 s a 218 Hz
static constexpr int BLE_MOUNT_CAL_MIN = 24;   // sotto questa soglia si rinuncia

// ============================================================================
//  FASE 10 — PROTOCOLLO DI TRASFERIMENTO FILE (caratteristica DATA)
// ============================================================================
//  COME FUNZIONA, IN UNA FRASE
//    L'app scrive su CMD [0x04][index_lo][index_hi][kind]; il device risponde
//    su DATA con un HEADER, poi N CHUNK, poi un END con il CRC32 dell'intero
//    file. Se qualcosa non torna, un solo frame ERROR e finisce li'.
//
//  PERCHE' UNA CARATTERISTICA NUOVA E NON SESSION
//    SESSION ha un payload a record FISSO di 30 byte (BleSessionRecord). Usarla
//    anche come flusso di byte significherebbe due grammatiche diverse sullo
//    stesso canale, distinte solo dal contesto: l'app dovrebbe "ricordarsi" in
//    che modalita' si trova. Un canale, una grammatica.
//
//  PERCHE' NIENTE RITRASMISSIONE PER BLOCCO
//    Un shots.csv di 9 tiri sta in ~800 byte: 4 chunk. A MTU 247 e connection
//    interval reale 15-30 ms il trasferimento intero dura meno di 200 ms.
//    In questo regime un protocollo con NAK per blocco costa piu' righe di
//    codice di quanti byte trasferisce. Il CRC32 end-to-end nel frame END dice
//    se il file e' arrivato integro; se non torna, l'app rimanda il comando e
//    ripaga 200 ms. Quando (e SE) arrivera' il download dei burst da 20 KB,
//    quella scelta andra' rimessa in discussione CON I DATI del caso d'uso —
//    non prima.
//
//  PERCHE' IL CRC STA NELL'END E NON NELL'HEADER
//    Per metterlo nell'header dovremmo leggere il file DUE volte (una per
//    calcolare il CRC, una per spedirlo). Nell'END il CRC si accumula mentre i
//    byte scorrono: una sola lettura dalla card.
// ----------------------------------------------------------------------------

// Quale file della sessione si vuole (byte "kind" del comando 0x04).
enum BleFileKind : uint8_t {
  BLE_FILE_SHOTS_CSV   = 0,   // shots.csv    — le metriche, il dato prezioso
  BLE_FILE_SESSION_TXT = 1,   // session.txt  — i metadati (fw, ODR, data fine)
  // 2 = burst_NNNN.bin: NON implementato. Vedi §"perche' niente burst via BLE"
  // nel README della fase: 20 KB a tiro sono 2-3 s ciascuno, e la card si
  // estrae. La costante non esiste apposta: niente segnaposto che sembrino
  // funzionalita'.
};

// Primo byte di ogni frame notificato su DATA: dice come leggere il resto.
enum BleDataFrameType : uint8_t {
  BLE_DATA_HEADER = 0x01,  // [0x01][kind u8][index u16][size u32]        = 8 B
  BLE_DATA_CHUNK  = 0x02,  // [0x02][seq u16][payload...]                 ≤ MTU-3
  BLE_DATA_END    = 0x03,  // [0x03][chunks u16][crc32 u32]               = 7 B
  BLE_DATA_ERROR  = 0xFF,  // [0xFF][code u8]                             = 2 B
};

// Codici del frame ERROR. Sono pochi e specifici apposta: "errore generico" non
// aiuta nessuno a capire se il problema e' la card, l'indice o il file.
enum BleDataError : uint8_t {
  BLE_ERR_NO_CARD    = 1,   // microSD non montata
  BLE_ERR_NO_SESSION = 2,   // indice di sessione inesistente
  BLE_ERR_NO_FILE    = 3,   // la sessione c'e' ma quel file no
  BLE_ERR_BAD_KIND   = 4,   // kind non riconosciuto (o non implementato)
  BLE_ERR_READ       = 5,   // errore di lettura a meta' trasferimento
};

// Overhead dei nostri frame CHUNK: 1 byte tipo + 2 byte numero di sequenza.
// Il payload utile per chunk e' (MTU_negoziato - 3_ATT - BLE_CHUNK_OVERHEAD).
static constexpr size_t BLE_CHUNK_OVERHEAD = 3;

// Tetto del buffer di trasferimento. MTU massimo richiesto = 247 -> payload
// utile 241; 244 lascia margine se un giorno l'MTU salisse. Un solo buffer
// statico nel modulo: niente malloc nel percorso caldo.
static constexpr size_t BLE_CHUNK_BUF_MAX = 244;

// Payload minimo garantito se l'MTU negoziato non e' noto o e' quello di
// default BLE (23 byte): 23 - 3 ATT - 3 nostri = 17. Lento ma corretto.
static constexpr size_t BLE_CHUNK_PAYLOAD_MIN = 17;

// Header del trasferimento (frame BLE_DATA_HEADER), packed: l'app lo rilegge
// con un DataView a offset fissi.
struct __attribute__((packed)) BleFileHeader {
  uint8_t  frame_type;   // = BLE_DATA_HEADER
  uint8_t  kind;         // BleFileKind
  uint16_t index;        // indice di sessione (lo stesso dell'elenco)
  uint32_t size;         // dimensione totale del file in byte
};

// Coda del trasferimento (frame BLE_DATA_END), packed.
struct __attribute__((packed)) BleFileEnd {
  uint8_t  frame_type;   // = BLE_DATA_END
  uint16_t chunks;       // quanti CHUNK sono stati mandati
  uint32_t crc32;        // CRC32 (IEEE 802.3, init 0xFFFFFFFF, riflesso) del file
};

// ----------------------------------------------------------------------------
//  StatusPacket — lo "stato del device" leggibile/notificabile dall'app.
// ----------------------------------------------------------------------------
//  packed: layout deterministico, l'app lo rilegge con un DataView a offset
//  fissi. Campi in coda per compatibilita' futura (stessa disciplina di Config).
struct __attribute__((packed)) BleStatusPacket {
  uint8_t  pkt_type;        // = 0x01 (marcatore di tipo)
  uint8_t  ble_mode;        // 1 = in modo BLE (sempre 1 quando notifichiamo)
  uint8_t  batt_pct;        // 0..100 (255 = sconosciuto)
  uint8_t  batt_charging;   // 0/1
  uint8_t  sd_ok;           // 0/1 microSD montata
  uint8_t  rtc_valid;       // 0/1 orologio impostato/affidabile
  uint16_t session_count;   // quante sessioni ci sono su card
  uint32_t sd_free_mb;      // MB liberi (0 se no card)
  char     fw_version[16];  // stringa versione firmware (ARCHBB_FW_VERSION)
};

// ----------------------------------------------------------------------------
//  SessionRecord — un elemento dell'elenco sessioni (una notify a testa).
// ----------------------------------------------------------------------------
//  L'app riceve prima uno "header" (index=0xFFFF con total nel campo shots) e
//  poi un record per sessione. Nome cartella + n. tiri: basta a popolare la
//  lista; il DOWNLOAD del contenuto arrivera' dopo.
struct __attribute__((packed)) BleSessionRecord {
  uint16_t index;           // 0-based; 0xFFFF = record-header (total in shots)
  uint16_t shots;           // n. tiri (o TOTALE sessioni se index==0xFFFF)
  char     name[26];        // basename cartella, es "SESS_20260721_1430"
};

// ----------------------------------------------------------------------------
//  Stato globale del modulo (definito in ble_service.cpp)
// ----------------------------------------------------------------------------
extern volatile bool g_ble_active;      // true mentre siamo in modo BLE
extern volatile bool g_ble_connected;   // true se un client e' connesso

// ----------------------------------------------------------------------------
//  API — ciclo di vita del modo BLE
// ----------------------------------------------------------------------------

// Avvia il modo BLE: inizializza NimBLE, crea il servizio GATT, parte in
// advertising col nome dato, e lancia il ble_task su Core 0. cfg_ptr e' la
// Config di lavoro (g_config): la caratteristica CONFIG legge/scrive DA LI'.
// Da chiamare DOPO aver sospeso imu_task/trigger_task. Idempotente: una seconda
// chiamata mentre e' gia' attivo e' un no-op.
void ble_start(const char* advertising_name, Config* cfg_ptr);

// Ferma il modo BLE: disconnette l'eventuale client, ferma l'advertising,
// termina il ble_task e deinizializza NimBLE (libera lo stack, ~40KB). Da
// chiamare PRIMA di riprendere i task IMU. Dopo questa, g_ble_active=false.
// NON chiama config_apply(): quello lo fa il main (per centralizzare il "come
// i parametri diventano operativi", coerente con config_store).
void ble_stop();

// True se, dall'ultima ble_start(), l'app ha SCRITTO il config (caratteristica
// CONFIG) o ha resettato/impostato qualcosa che richiede config_apply() e un
// eventuale refresh dello scoring all'uscita. Il main la interroga in ble_stop
// per decidere se rifare config_apply(). Si azzera a ogni ble_start().
bool ble_config_was_edited();

// True se l'app ha impostato l'RTC in questa sessione BLE (per un feedback
// "ora sincronizzata" all'uscita). Si azzera a ogni ble_start().
bool ble_time_was_set();

// ----------------------------------------------------------------------------
//  API — task e notifiche (uso interno + status on-demand)
// ----------------------------------------------------------------------------

// Il task BLE (Core 0). Gestisce la coda comandi (list-sessions, reset) fuori
// dal contesto callback NimBLE, con la disciplina anti-crash della 1.69. Non
// ritorna finche' g_ble_active resta true; esce pulito quando ble_stop lo
// abbassa. Lanciato da ble_start: non chiamarlo a mano.
void ble_task(void* param);
