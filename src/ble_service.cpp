// ============================================================================
//  ArchBB 1.83 — ble_service.cpp · FASE 7 (BLE modale)
//  Cesare Pagura · Padova/Noale IT · 21 luglio 2026
// ----------------------------------------------------------------------------
//  Vedi ble_service.h per l'architettura. Qui l'implementazione.
//
//  MAPPA DEL FILE
//    - crc/helper                : niente (usiamo il checksum del config_store)
//    - callbacks NimBLE          : connessione, CONFIG, TIME, CMD
//    - ble_notify_status()       : compone e notifica lo StatusPacket
//    - list-sessions             : scansione SD -> N notify su SESSION
//    - ble_start / ble_stop      : ciclo di vita (init/deinit NimBLE)
//    - ble_task                  : elabora la coda comandi su Core 0, con PM-lock
//
//  DISCIPLINA ANTI-CRASH EREDITATA DALLA 1.69 (handoff §4), in forma leggera:
//    - i comandi che generano notify (list-sessions) NON si eseguono nel
//      callback onWrite: si accodano e si elaborano nel ble_task, cosi' NimBLE
//      non e' rientrante mentre gia' processa la write;
//    - le notify dell'elenco sono sincronizzate sul completamento (onStatus)
//      con un semaforo: si manda il record successivo SOLO dopo che il
//      precedente e' uscito davvero (niente accumulo nei buffer mbuf);
//    - PM-lock ESP_PM_NO_LIGHT_SLEEP per tutta la vita del task (previene la
//      classe di crash esp_pm_impl_waiti);
//    - connection interval REALE letto in onConnect e usato come base del pacing.
// ============================================================================
#include "ble_service.h"
#include "config.h"          // ARCHBB_FW_VERSION, OdrSetting, calcPreSamples...
#include "config_store.h"    // g_config, config_checksum_of, config_save, config_reset
#include "mount.h"           // g_mount_orientation, mount_is_valid
#include "rtc_clock.h"        // rtc_set
#include "sd_storage.h"
#include "imu.h"
#include "mount.h"       // sd_enumerate_sessions, g_sd_ok
#include "battery.h"          // battery_percent/charging

#include <NimBLEDevice.h>
#include <freertos/semphr.h>
#include <esp_pm.h>
#include <string.h>

extern volatile bool g_shutdown;   // definito in main.cpp (pattern condiviso)

// ----------------------------------------------------------------------------
//  Stato globale
// ----------------------------------------------------------------------------
volatile bool g_ble_active    = false;
volatile bool g_ble_connected = false;

// Connection interval REALE (ms), letto in onConnect. Fallback prudente a 30ms.
static volatile float s_conn_interval_ms = 30.0f;

// Flag di "e' cambiato qualcosa": il main li interroga all'uscita.
static volatile bool s_config_edited = false;
static volatile bool s_time_set      = false;

// Handle NimBLE
static NimBLEServer*         s_server       = nullptr;
static NimBLECharacteristic* s_char_config  = nullptr;
static NimBLECharacteristic* s_char_time    = nullptr;
static NimBLECharacteristic* s_char_cmd     = nullptr;
static NimBLECharacteristic* s_char_status  = nullptr;
static NimBLECharacteristic* s_char_session = nullptr;
static NimBLECharacteristic* s_char_data    = nullptr;   // FASE 10: download file

// Handle della connessione corrente (0xFFFF = nessuna). Serve UNICAMENTE a
// chiedere allo stack l'MTU davvero negoziato con QUESTO client: la dimensione
// del chunk non e' una costante nostra, e' una proprieta' della connessione.
static volatile uint16_t s_conn_handle = 0xFFFF;

static Config* s_cfg = nullptr;   // = &g_config, la fonte di verita' in RAM

// Handle del task e flag di stop pulito
static TaskHandle_t s_ble_task = nullptr;

// PM-lock anti-light-sleep: a livello file cosi' ble_stop puo' rilasciarlo se
// deve terminare il task forzatamente (il task ucciso non farebbe la sua uscita
// pulita). Creato una volta nel ble_task, riusato.
static esp_pm_lock_handle_t s_ble_pm_lock = nullptr;

// ----------------------------------------------------------------------------
//  Coda comandi — i comandi "pesanti" (che generano notify) NON si eseguono nel
//  callback NimBLE ma nel ble_task. La coda e' il ponte fra i due contesti.
// ----------------------------------------------------------------------------
#define BLE_CMD_QUEUE_LEN 8
struct BleQueuedCmd { uint8_t cmd; uint8_t payload[8]; uint8_t len; };
static QueueHandle_t s_cmd_queue = nullptr;

// ----------------------------------------------------------------------------
//  Semaforo di completamento notify (per l'elenco sessioni) — vedi §4 handoff.
//  Creato "dato" cosi' la primissima notify non aspetta un completamento mai
//  avvenuto. onStatus() lo restituisce a ogni notify conclusa (successo o no).
// ----------------------------------------------------------------------------
static SemaphoreHandle_t s_notify_done = nullptr;

// ============================================================================
//  CALLBACKS NimBLE
// ============================================================================

// -- Server: connessione/disconnessione ------------------------------------
class CfgServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, ble_gap_conn_desc* d) override {
    g_ble_connected = true;
    s_conn_handle   = d->conn_handle;   // FASE 10: serve per getPeerMTU()
    // Connection interval reale (unita' 1.25ms, convenzione BLE/Mynewt).
    s_conn_interval_ms = d->conn_itvl * 1.25f;
    // Chiediamo un interval corto (15-30ms) invece di accettare quello che il
    // telefono propone (spesso lungo per risparmio energetico): rende reattivo
    // il colloquio e riduce l'accumulo di notify. min/max in multipli di 1.25ms,
    // latenza 0, supervision timeout 600ms (10x margine).
    s->updateConnParams(d->conn_handle, 12, 24, 0, 60);
    // Continuiamo ad advertisare: cosi' se il client cade puo' riconnettersi.
    NimBLEDevice::startAdvertising();
  }
  void onDisconnect(NimBLEServer* s) override {
    (void)s;
    g_ble_connected = false;
    s_conn_handle   = 0xFFFF;
    NimBLEDevice::startAdvertising();
  }
};

// -- CONFIG: read = manda g_config; write = valida e applica ----------------
//  La validazione e' la STESSA logica difensiva della 1.69: una Config
//  malformata (finestre troppo lunghe, confirm_n assurdo, montaggio invalido)
//  non deve entrare in RAM e propagarsi ai task. Prima il checksum (integrita'),
//  poi i range (sanita' dei valori).
class ConfigCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c) override {
    if (!s_cfg) return;
    s_cfg->checksum = config_checksum_of(*s_cfg);   // sempre coerente in lettura
    c->setValue((uint8_t*)s_cfg, sizeof(Config));
  }
  void onWrite(NimBLECharacteristic* c) override {
    if (!s_cfg) return;
    const uint8_t* data = c->getValue().data();
    size_t len = c->getValue().size();
    if (len < sizeof(Config)) return;               // pacchetto corto: ignora

    Config nc;
    memcpy(&nc, data, sizeof(Config));

    // 1) INTEGRITA': il checksum deve tornare, altrimenti e' spazzatura.
    if (nc.checksum != config_checksum_of(nc)) return;

    // 2) SANITA' DEI RANGE (clamp difensivo, come 1.69):
    //    - le finestre pre+post non devono sforare il circular buffer;
    //    - confirm_n in [1..10], rearm >= 200ms, soglia >= 0.5 m/s^2;
    //    - montaggio valido (altrimenti default).
    // La finestra in campioni all'ODR reale: usiamo lo stesso conto del firmware.
    {
      // Ricostruiamo pre/post samples dai ms del pacchetto, all'ODR di default
      // (ODR_250HZ = 224Hz reale, l'unico in uso). Se sfora, rifiuta l'intera
      // scrittura: meglio tenere il vecchio config valido che accettarne uno rotto.
      uint32_t preS  = (uint32_t)((float)nc.window_pre_ms  / 1000.0f * odrToHz(ODR_250HZ));
      uint32_t postS = (uint32_t)((float)nc.window_post_ms / 1000.0f * odrToHz(ODR_250HZ));
      if (preS + postS > CIRCULAR_BUFFER_SIZE) return;   // non ci sta: rifiuta
    }
    if (nc.trig_confirm_n < 1)  nc.trig_confirm_n = 1;
    if (nc.trig_confirm_n > 10) nc.trig_confirm_n = 10;
    if (nc.trig_rearm_ms  < 200) nc.trig_rearm_ms = 200;
    if (nc.trig_threshold_ms2 < 0.5f) nc.trig_threshold_ms2 = 0.5f;
    if (!mount_is_valid(nc.mount_orientation)) nc.mount_orientation = ARCHBB_MOUNT_DEFAULT;

    // 3) COMMIT in RAM. NON salviamo in NVS qui dentro (il callback deve essere
    //    veloce e non bloccante): salviamo nel ble_task quando raccoglie il
    //    flag. Ma aggiorniamo subito g_config e il montaggio (scrittura atomica
    //    di un uint8_t, letta coerente dall'imu_task... che pero' ORA e' sospeso:
    //    doppia sicurezza).
    *s_cfg = nc;
    g_mount_orientation = nc.mount_orientation;
    s_config_edited = true;   // il main fara' config_save()+config_apply() all'uscita
  }
};

// -- TIME: write di 7 byte -> imposta l'RTC ---------------------------------
//  Formato: [yearLo, yearHi, month, day, hour, minute, second].
//  L'anno e' little-endian a 16 bit (2026 = 0xEA 0x07). L'app manda l'ora dello
//  smartphone: da qui in poi le sessioni sono datate corrette.
class TimeCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    const uint8_t* d = c->getValue().data();
    size_t len = c->getValue().size();
    if (len < 7 || !d) return;
    uint16_t year = (uint16_t)d[0] | ((uint16_t)d[1] << 8);
    uint8_t  mon  = d[2], day = d[3], hh = d[4], mm = d[5], ss = d[6];
    // Sanita' minima: anni plausibili e campi in range. Un pacchetto sballato
    // non deve avvelenare l'RTC.
    if (year < 2024 || year > 2099) return;
    if (mon < 1 || mon > 12 || day < 1 || day > 31) return;
    if (hh > 23 || mm > 59 || ss > 59) return;
    // --- F24b: IL SYNC DELL'ORA DEVE LASCIARE TRACCIA ---------------------
    //  Il 13/09 un sync da smartphone ha spostato l'RTC di +198 s a meta' di
    //  una scarica, e la colonna iso di ENERGIA.CSV e' saltata senza dirlo:
    //  mezz'ora persa a cercare un guasto hardware che non c'era. Se fosse
    //  successo durante una sessione di tiro, i timestamp prima e dopo il sync
    //  apparterrebbero a due orologi diversi e nessuno potrebbe accorgersene
    //  rileggendo il CSV — una discontinuita' invisibile dentro una sequenza
    //  monotona e plausibile.
    //
    //  Si scrive il PRIMA e il DELTA: da li' la deriva dell'RTC si calcola da
    //  sola, sync dopo sync, senza dover fare un esperimento apposta.
    char prima[24]; rtc_iso_string(prima, sizeof(prima));
    if (rtc_set(year, mon, day, hh, mm, ss)) {
        s_time_set = true;
        rtc_sync_system_clock();   // F24b.1: anche l'orologio di sistema, o i
                                   // file nati dopo il sync resterebbero indietro
        char dopo[24]; rtc_iso_string(dopo, sizeof(dopo));
        char nota[128];
        snprintf(nota, sizeof(nota), "SYNC ORA da BLE: era %s ora %s", prima, dopo);
        sd_energia_nota(nota);
    }
  }
};

// -- CMD: write di [cmd][payload] -------------------------------------------
//  Solo accodamento: l'elaborazione (soprattutto list-sessions, che notifica)
//  avviene nel ble_task per non chiamare NimBLE in modo rientrante.
class CmdCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    if (!s_cmd_queue) return;
    const uint8_t* d = c->getValue().data();
    size_t len = c->getValue().size();
    if (len == 0 || !d) return;
    BleQueuedCmd q = {};
    q.cmd = d[0];
    q.len = (uint8_t)(len - 1 > sizeof(q.payload) ? sizeof(q.payload) : len - 1);
    if (q.len) memcpy(q.payload, d + 1, q.len);
    xQueueSend(s_cmd_queue, &q, 0);
  }
};

// -- Callback di completamento notify — il PACING, cuore dell'anti-crash ----
//  Firma per NimBLE-Arduino 1.4.x: (characteristic, Status, code). Se sul tuo
//  ambiente il compilatore protesta, la 1.69 documenta le alternative (2 param
//  o NimBLEConnInfo): la logica interna resta identica (dare il semaforo).
//
//  FASE 10 — la STESSA istanza serve SESSION e DATA. E' corretto perche' i due
//  flussi non si sovrappongono mai: entrambi vengono eseguiti dal ble_task, che
//  e' uno solo e processa un comando alla volta. Un semaforo unico e' quindi la
//  rappresentazione fedele della realta' ("c'e' al massimo una notify in volo"),
//  non una semplificazione. Due semafori separati darebbero l'illusione di un
//  parallelismo che non esiste.
//
//  s_notify_err viene alzato quando lo stack segnala un errore di invio: e'
//  cosi' che do_send_file scopre di dover rallentare (backoff), invece di
//  scoprirlo dal reset.
static volatile bool s_notify_err = false;

class PacedNotifyCallbacks : public NimBLECharacteristicCallbacks {
  void onStatus(NimBLECharacteristic* c, Status status, int code) override {
    (void)c;   // non logghiamo dentro il callback: alterare il ritmo qui e' il
               // modo piu' rapido per falsare la misura del ritmo stesso.
    (void)code;
    // Status e' l'enum di NimBLECharacteristicCallbacks: tutto cio' che non e'
    // un successo (notify disabilitata, ERROR_GATT, nessun client...) e' un
    // segnale di rallentare.
    if (status != SUCCESS_NOTIFY && status != SUCCESS_INDICATE) s_notify_err = true;
    if (s_notify_done) xSemaphoreGive(s_notify_done);
  }
};

// Istanze statiche dei callback (vivono per tutta la sessione BLE).
static CfgServerCallbacks s_server_cb;
static ConfigCallbacks    s_config_cb;
static TimeCallbacks      s_time_cb;
static CmdCallbacks       s_cmd_cb;
static PacedNotifyCallbacks s_paced_cb;   // condiviso da SESSION e DATA

// ============================================================================
//  ble_notify_status — compone lo StatusPacket e lo notifica (o solo setValue).
// ============================================================================
static void ble_notify_status() {
  if (!s_char_status) return;

  BleStatusPacket p = {};
  p.pkt_type      = 0x01;
  p.ble_mode      = 1;
  int bp          = battery_percent();  // ritorna int; clamp difensivo 0..100
  p.batt_pct      = (bp < 0) ? 255 : (bp > 100 ? 100 : (uint8_t)bp);  // 255 = ignoto
  p.batt_charging = battery_charging() ? 1 : 0;
  p.sd_ok         = g_sd_ok ? 1 : 0;
  p.rtc_valid     = rtc_is_valid() ? 1 : 0;
  p.session_count = g_sd_ok ? sd_session_total() : 0;
  p.sd_free_mb    = g_sd_ok ? sd_free_mb() : 0;
  strncpy(p.fw_version, ARCHBB_FW_VERSION, sizeof(p.fw_version) - 1);
  p.fw_version[sizeof(p.fw_version) - 1] = '\0';

  s_char_status->setValue((uint8_t*)&p, sizeof(p));
  if (s_server && s_server->getConnectedCount() > 0) s_char_status->notify();
}

// ============================================================================
//  do_list_sessions — scandisce la SD e notifica N SessionRecord su SESSION.
// ----------------------------------------------------------------------------
//  Eseguita NEL ble_task (contesto sicuro). Manda prima un record-header
//  (index=0xFFFF, shots=totale) cosi' l'app sa quanti record aspettare, poi un
//  record per sessione. Ogni notify aspetta il completamento della precedente
//  (semaforo su onStatus): niente accumulo mbuf, la lezione della 1.69.
// ============================================================================
// ----------------------------------------------------------------------------
//  pacedNotify — UNA notify, col ritmo giusto. La regola §4.1 applicata al
//  COMPORTAMENTO e non solo alle costanti: elenco sessioni e download file
//  usano LA STESSA funzione, quindi non possono divergere. Sulla 1.69 il crash
//  nacque proprio da due percorsi di invio con discipline diverse.
// ----------------------------------------------------------------------------
//  LA DISCIPLINA, in ordine di importanza:
//    1) ASPETTA il completamento della notify precedente (semaforo su onStatus).
//       Questo, e non il delay, e' il vero freno: il delay e' una stima, il
//       semaforo e' un fatto.
//    2) RITARDO MINIMO ancorato al connection interval REALE + 5 ms. La radio
//       non puo' trasmettere piu' spesso di un pacchetto per intervallo: dare
//       allo stack piu' notify di quante ne puo' spedire riempie il pool mbuf
//       -> BLE_HS_ENOMEM -> rst:0xc. E' la diagnosi della 1.69, per intero.
//    3) BACKOFF 2.5x se lo stack ha segnalato un errore sull'ultima notify.
//       Il ritardo maggiorato resta per il resto del trasferimento: se il
//       canale e' degradato, tornare subito a correre significa ricascarci.
//
//  Ritorna false se il client e' sparito a meta' (il chiamante interrompe).
static uint32_t s_pace_delay_ms = 0;    // ritardo corrente, ricalcolato a ogni avvio

static void pacedResetDelay() {
  s_pace_delay_ms = (uint32_t)s_conn_interval_ms + 5;
  s_notify_err    = false;
}

static bool pacedNotify(NimBLECharacteristic* ch, const uint8_t* data, size_t len) {
  if (!ch || !s_server || s_server->getConnectedCount() == 0) return false;

  // 1) attesa del completamento precedente (guardia 500 ms: non restiamo
  //    appesi in eterno se un onStatus non arrivasse mai).
  if (s_notify_done) xSemaphoreTake(s_notify_done, pdMS_TO_TICKS(500));

  // 3) backoff: se l'invio precedente ha segnalato errore, allarghiamo il passo.
  if (s_notify_err) {
    s_pace_delay_ms = (uint32_t)(s_pace_delay_ms * 2.5f);
    if (s_pace_delay_ms > 500) s_pace_delay_ms = 500;   // tetto di ragionevolezza
    s_notify_err = false;
  }

  ch->setValue((uint8_t*)data, len);
  ch->notify();

  // 2) pavimento del ritardo (cintura+bretelle rispetto al semaforo).
  vTaskDelay(pdMS_TO_TICKS(s_pace_delay_ms));
  return true;
}

static void do_list_sessions() {
  if (!s_char_session || !s_server || s_server->getConnectedCount() == 0) return;

  // Raccogliamo i record dalla SD. Il tetto e' SD_MAX_ENUM_SESSIONS, la
  // costante di sd_storage.h: la STESSA che usa il risolutore indice→nome del
  // download. Prima era un 64 letterale qui dentro — e un indice che significa
  // due cose diverse in due punti e' esattamente il bug piu' costoso di questo
  // progetto (§4.1).
  static SdSessionInfo sess[SD_MAX_ENUM_SESSIONS];
  uint16_t total = 0;
  uint16_t n = sd_enumerate_sessions(sess, SD_MAX_ENUM_SESSIONS, total);

  pacedResetDelay();

  // 1) record-header: index=0xFFFF, shots=totale reale su card.
  BleSessionRecord hdr = {};
  hdr.index = 0xFFFF;
  hdr.shots = total;
  strncpy(hdr.name, "HEADER", sizeof(hdr.name) - 1);
  pacedNotify(s_char_session, (const uint8_t*)&hdr, sizeof(hdr));

  // 2) un record per sessione (dalla piu' recente).
  for (uint16_t i = 0; i < n; i++) {
    BleSessionRecord r = {};
    r.index = i;
    r.shots = sess[i].shots;
    strncpy(r.name, sess[i].name, sizeof(r.name) - 1);
    if (!pacedNotify(s_char_session, (const uint8_t*)&r, sizeof(r))) return;
  }
}

// ============================================================================
//  FASE 10 — DOWNLOAD DI UN FILE DI SESSIONE
// ============================================================================

// -- CRC32 (IEEE 802.3, riflesso) — versione senza tabella -------------------
//  Perche' senza tabella: la tabella costa 1 KB di RAM/flash per risparmiare
//  microsecondi su un file da 800 byte. Il bit-by-bit fa 8 iterazioni per byte:
//  su 20 KB (il caso peggiore immaginabile) sono ~160k operazioni, qualche
//  millisecondo. Irrilevante rispetto ai secondi del trasferimento radio.
//  Lo stesso algoritmo e' implementato in JavaScript nell'app: se un giorno
//  cambia qui, DEVE cambiare anche li' (sono due implementazioni della stessa
//  specifica, non due copie dello stesso codice — la specifica e' "CRC-32/ISO-HDLC").
static uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int k = 0; k < 8; k++) {
      crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
  }
  return crc;
}

// Manda un frame ERROR e basta. Un solo punto di uscita per gli errori: chi
// legge il codice vede subito TUTTI i modi in cui il download puo' fallire.
static void send_data_error(uint8_t code) {
  uint8_t f[2] = { BLE_DATA_ERROR, code };
  pacedResetDelay();
  pacedNotify(s_char_data, f, sizeof(f));
}

// Quanti byte di payload stanno in un chunk, DATA LA CONNESSIONE CORRENTE.
// Non e' una costante: dipende dall'MTU che il telefono ha negoziato. Un iPhone
// e un Android danno numeri diversi, e usare un valore fisso significherebbe
// o sprecare banda (troppo piccolo) o farsi troncare le notify (troppo grande).
static size_t chunk_payload_size() {
  uint16_t mtu = 0;
  if (s_server && s_conn_handle != 0xFFFF) mtu = s_server->getPeerMTU(s_conn_handle);
  if (mtu < 23) mtu = 23;                       // default BLE: sempre valido
  size_t p = (size_t)mtu - 3 - BLE_CHUNK_OVERHEAD;   // 3 = header ATT
  if (p < BLE_CHUNK_PAYLOAD_MIN) p = BLE_CHUNK_PAYLOAD_MIN;
  if (p > BLE_CHUNK_BUF_MAX)     p = BLE_CHUNK_BUF_MAX;
  return p;
}

// ----------------------------------------------------------------------------
//  do_mount_cal — "l'arco e' a piombo adesso": registra l'assetto come offset.
// ----------------------------------------------------------------------------
//  Legge BLE_MOUNT_CAL_N campioni GREZZI, li media, applica la trasformazione
//  di montaggio corrente e ricava cant/alzo. Quei due numeri diventano gli
//  offset: da li' in poi vengono sottratti a ogni tiro.
//
//  Si media PRIMA di calcolare gli angoli, non dopo: mediare gli angoli
//  significherebbe mediare due atan2, che non e' lineare. Mediare il vettore
//  gravita' e poi prendere l'atan2 una volta sola e' la cosa giusta, ed e' la
//  stessa scelta di shot_angles_compute nella finestra calma.
static void do_mount_cal() {
    float sx = 0, sy = 0, sz = 0;
    int n = 0;
    for (int i = 0; i < BLE_MOUNT_CAL_N; i++) {
        float ax, ay, az, gx, gy, gz;
        if (imu_read_raw(ax, ay, az, gx, gy, gz)) { sx += ax; sy += ay; sz += az; n++; }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (n < BLE_MOUNT_CAL_MIN) {
        DBG("BLE: taratura montaggio FALLITA (%d/%d letture)", n, BLE_MOUNT_CAL_N);
        return;                       // meglio nessuna taratura che una sbagliata
    }

    float ax = sx / n, ay = sy / n, az = sz / n, gx = 0, gy = 0, gz = 0;
    mount_apply(g_mount_orientation, ax, ay, az, gx, gy, gz);

    // Controllo di sanita': se il modulo non e' quello della gravita', l'arco
    // si stava muovendo e la posa non vale niente.
    float mod = sqrtf(ax*ax + ay*ay + az*az);
    if (fabsf(mod - 9.81f) > 1.5f) {
        DBG("BLE: taratura montaggio SCARTATA (|a| = %.2f, arco in movimento?)", mod);
        return;
    }

    g_config.offset_cant_deg = atan2f(ay, ax) * 180.0f / (float)M_PI;
    g_config.offset_alzo_deg = atan2f(az, ax) * 180.0f / (float)M_PI;
    config_save();

    DBG("BLE: taratura montaggio -> cant %.2f  alzo %.2f  (%d letture, |a| %.2f)",
        g_config.offset_cant_deg, g_config.offset_alzo_deg, n, mod);
}

static void do_mount_cal_reset() {
    g_config.offset_cant_deg = 0.0f;
    g_config.offset_alzo_deg = 0.0f;
    config_save();
    DBG("BLE: offset di montaggio azzerati");
}

// -- do_send_file: HEADER -> N CHUNK -> END ---------------------------------
//  Eseguita nel ble_task (mai in un callback). Apre il file, lo strema, chiude.
//  Il reader di sd_storage vive interamente dentro questa funzione: nessuno
//  stato sopravvive all'uscita, nemmeno sui percorsi d'errore.
static void do_send_file(uint16_t index, uint8_t kind) {
  if (!s_char_data || !s_server || s_server->getConnectedCount() == 0) return;

  // 1) kind -> nome file. Una sola mappa, qui.
  const char* fname = nullptr;
  switch (kind) {
    case BLE_FILE_SHOTS_CSV:   fname = "shots.csv";   break;
    case BLE_FILE_SESSION_TXT: fname = "session.txt"; break;
    default: send_data_error(BLE_ERR_BAD_KIND); return;
  }

  if (!g_sd_ok) { send_data_error(BLE_ERR_NO_CARD); return; }

  // 2) apertura. sd_reader_open distingue "sessione inesistente" da "file
  //    mancante"? No: ritorna solo false. Per dare all'app un errore utile
  //    chiediamo prima il nome della sessione — se quello manca, l'indice e'
  //    sbagliato; se c'e' ma l'open fallisce, e' il file a mancare.
  char sessName[32];
  if (!sd_session_name_by_index(index, sessName, sizeof(sessName))) {
    send_data_error(BLE_ERR_NO_SESSION); return;
  }
  uint32_t total = 0;
  if (!sd_reader_open(index, fname, total)) {
    send_data_error(BLE_ERR_NO_FILE); return;
  }

  pacedResetDelay();

  // 3) HEADER: l'app ora sa quanti byte aspettare e puo' disegnare la barra.
  BleFileHeader h = {};
  h.frame_type = BLE_DATA_HEADER;
  h.kind       = kind;
  h.index      = index;
  h.size       = total;
  if (!pacedNotify(s_char_data, (const uint8_t*)&h, sizeof(h))) {
    sd_reader_close(); return;
  }

  // 4) CHUNK. Buffer statico: 1 byte tipo + 2 seq + payload.
  static uint8_t frame[1 + 2 + BLE_CHUNK_BUF_MAX];
  const size_t payloadSz = chunk_payload_size();

  uint32_t crc  = 0xFFFFFFFFu;
  uint32_t sent = 0;
  uint16_t seq  = 0;

  while (sent < total) {
    size_t want = (size_t)(total - sent);
    if (want > payloadSz) want = payloadSz;

    size_t got = sd_reader_read(frame + 3, want);
    if (got == 0) {                       // lettura interrotta: card estratta?
      sd_reader_close();
      send_data_error(BLE_ERR_READ);
      return;
    }

    frame[0] = BLE_DATA_CHUNK;
    frame[1] = (uint8_t)(seq & 0xFF);
    frame[2] = (uint8_t)(seq >> 8);
    crc = crc32_update(crc, frame + 3, got);

    if (!pacedNotify(s_char_data, frame, 3 + got)) {   // client sparito
      sd_reader_close();
      return;
    }
    sent += got;
    seq++;

    // Uscita di cortesia: se il main sta chiudendo il modo BLE, molliamo il
    // trasferimento invece di tenere il task occupato mentre ble_stop attende.
    if (!g_ble_active) { sd_reader_close(); return; }
  }

  sd_reader_close();

  // 5) END con il CRC finalizzato (complemento, come da specifica CRC-32).
  BleFileEnd e = {};
  e.frame_type = BLE_DATA_END;
  e.chunks     = seq;
  e.crc32      = crc ^ 0xFFFFFFFFu;
  pacedNotify(s_char_data, (const uint8_t*)&e, sizeof(e));

  DBG("BLE: inviato %s di %s (%lu byte, %u chunk)",
      fname, sessName, (unsigned long)total, (unsigned)seq);
}

// ============================================================================
//  ble_start — avvia advertising + task. Inizializza NimBLE/GATT UNA SOLA VOLTA
//  (prima chiamata); le volte successive riusa lo stack gia' costruito.
// ============================================================================
//  PERCHE' non re-inizializzare ogni volta: NimBLEDevice::init()/deinit(true)
//  ripetuti su ESP32-S3 (NimBLE 1.4.1) sono instabili e causavano un PANIC
//  all'USCITA dal modo BLE (deinit mentre callback/task erano ancora in volo).
//  La 1.69, che sul campo non crashava, NON chiamava mai deinit: il BLE era un
//  demone permanente. Adottiamo lo stesso principio: lo stack BLE si costruisce
//  una volta e resta vivo; entrare/uscire dal modo BLE = solo avviare/fermare
//  advertising e il task di servizio. Costo: ~40KB restano occupati dopo la
//  prima entrata BLE (accettabile; i task IMU girano comunque).
static bool s_ble_initialized = false;

void ble_start(const char* advertising_name, Config* cfg_ptr) {
  if (g_ble_active) return;   // idempotente
  s_cfg = cfg_ptr;
  s_config_edited = false;
  s_time_set      = false;
  s_conn_interval_ms = 30.0f;

  // Coda comandi + semaforo notify (creati una sola volta, riusati).
  if (!s_cmd_queue) s_cmd_queue = xQueueCreate(BLE_CMD_QUEUE_LEN, sizeof(BleQueuedCmd));
  if (!s_notify_done) { s_notify_done = xSemaphoreCreateBinary(); xSemaphoreGive(s_notify_done); }

  // --- Costruzione NimBLE + GATT: SOLO la prima volta -----------------------
  if (!s_ble_initialized) {
    NimBLEDevice::init(advertising_name);
    NimBLEDevice::setMTU(247);                 // MTU ampio: Config in un solo pacchetto
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);    // potenza max (raggio/robustezza)

    s_server = NimBLEDevice::createServer();
    s_server->setCallbacks(&s_server_cb);

    NimBLEService* svc = s_server->createService(ARCHBB_CFG_SERVICE_UUID);

    // CONFIG (read/write della struct)
    s_char_config = svc->createCharacteristic(ARCHBB_CHAR_CONFIG_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
    s_char_config->setCallbacks(&s_config_cb);

    // TIME (write -> RTC)
    s_char_time = svc->createCharacteristic(ARCHBB_CHAR_TIME_UUID,
        NIMBLE_PROPERTY::WRITE);
    s_char_time->setCallbacks(&s_time_cb);

    // CMD (write -> coda)
    s_char_cmd = svc->createCharacteristic(ARCHBB_CHAR_CMD_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    s_char_cmd->setCallbacks(&s_cmd_cb);

    // STATUS (read/notify)
    s_char_status = svc->createCharacteristic(ARCHBB_CHAR_STATUS_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

    // SESSION (notify) — elenco sessioni, un record per notify
    s_char_session = svc->createCharacteristic(ARCHBB_CHAR_SESSION_UUID,
        NIMBLE_PROPERTY::NOTIFY);
    s_char_session->setCallbacks(&s_paced_cb);

    // DATA (notify) — FASE 10: flusso di byte del download file.
    //  Stesso callback di pacing di SESSION: i due canali non si sovrappongono
    //  mai (un solo ble_task, un comando alla volta), quindi un semaforo solo.
    s_char_data = svc->createCharacteristic(ARCHBB_CHAR_DATA_UUID,
        NIMBLE_PROPERTY::NOTIFY);
    s_char_data->setCallbacks(&s_paced_cb);

    svc->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(ARCHBB_CFG_SERVICE_UUID);
    adv->setScanResponse(true);

    s_ble_initialized = true;
  }

  // Aggiorna il valore corrente del config nella caratteristica (ogni entrata).
  if (s_cfg && s_char_config) {
    s_cfg->checksum = config_checksum_of(*s_cfg);
    s_char_config->setValue((uint8_t*)s_cfg, sizeof(Config));
  }

  // Avvia advertising (ogni entrata nel modo BLE).
  NimBLEDevice::startAdvertising();

  g_ble_active = true;

  // --- Lancia il ble_task su Core 0 ---------------------------------------
  //  Stack 4096 come sulla 1.69. Priorita' 4 (sotto i task IMU quando esistono,
  //  ma qui sono sospesi: il BLE ha Core 1 libero e Core 0 per se').
  xTaskCreatePinnedToCore(ble_task, "ble", 4096, nullptr, 4, &s_ble_task, 0);
}

// ============================================================================
//  ble_stop — ferma advertising e il task, MA lascia vivo lo stack NimBLE.
// ----------------------------------------------------------------------------
//  NON chiama deinit(): re-init/deinit ripetuti su ESP32-S3 causavano il PANIC
//  all'uscita. Lo stack resta inizializzato (~40KB) e pronto per la prossima
//  entrata, che rifara' solo startAdvertising(). Qui: fermiamo l'advertising,
//  disconnettiamo eventuali client, e chiudiamo il ble_task in modo pulito.
// ============================================================================
void ble_stop() {
  if (!g_ble_active) return;

  // 1) Ferma l'advertising: il device non e' piu' visibile/agganciabile.
  if (s_ble_initialized) NimBLEDevice::stopAdvertising();

  // 2) Sblocca un eventuale do_list_sessions in attesa del semaforo notify,
  //    cosi' il task non resta appeso mentre gli chiediamo di uscire.
  if (s_notify_done) xSemaphoreGive(s_notify_done);

  // 3) Chiedi al task di uscire e ATTENDI che termini DAVVERO. g_ble_active=false
  //    fa uscire il while del ble_task. Se entro ~1.5s non e' morto da solo, lo
  //    eliminiamo esplicitamente (meglio un task morto che uno zombie sul BLE).
  g_ble_active = false;
  bool ended = false;
  for (int i = 0; i < 150; i++) {
    if (s_ble_task == nullptr) { ended = true; break; }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (!ended && s_ble_task != nullptr) {
    TaskHandle_t h = s_ble_task;
    s_ble_task = nullptr;
    vTaskDelete(h);
    // Il task ucciso non ha rilasciato il suo PM-lock: lo facciamo qui noi.
    if (s_ble_pm_lock) { esp_pm_lock_release(s_ble_pm_lock); }
  }

  // NB: NON chiamiamo deinit(): lo stack NimBLE resta vivo (s_server e le
  // caratteristiche restano validi) e la prossima ble_start rifara' solo
  // startAdvertising(). Niente client disconnessi a mano: se un client era
  // agganciato, cadra' per timeout; lo stack lo gestisce senza nostro intervento.
  g_ble_connected = false;
}

// ============================================================================
//  Flag interrogati dal main all'uscita.
// ============================================================================
bool ble_config_was_edited() { return s_config_edited; }
bool ble_time_was_set()      { return s_time_set; }

// ============================================================================
//  ble_task — Core 0. PM-lock + loop di servizio della coda comandi.
// ============================================================================
void ble_task(void* param) {
  (void)param;

  // --- PM-lock anti-light-sleep (lezione 1.69 v2.10.4) --------------------
  //  Impedisce l'automatic light-sleep mentre il BLE e' attivo: in light-sleep
  //  le periferiche sono clock-gated e gli interrupt radio non vengono serviti
  //  in tempo -> lo stack BLE si blocca -> watchdog -> reset. Il lock lavora a
  //  runtime, indipendente da sdkconfig. Se il PM non e' attivo, e' un no-op.
  //  s_ble_pm_lock e' a livello file: creato una volta (riusato a ogni entrata),
  //  cosi' anche ble_stop puo' rilasciarlo se deve terminare il task a forza.
  if (!s_ble_pm_lock) {
    esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "archbb_ble", &s_ble_pm_lock);
  }
  if (s_ble_pm_lock) esp_pm_lock_acquire(s_ble_pm_lock);

  // Notifica lo stato iniziale appena qualcuno si connette (utile all'app).
  uint32_t lastStatusMs = 0;

  while (g_ble_active) {
    // 1) Elabora la coda comandi (contesto sicuro, fuori dai callback NimBLE).
    BleQueuedCmd q;
    if (s_cmd_queue && xQueueReceive(s_cmd_queue, &q, pdMS_TO_TICKS(50)) == pdTRUE) {
      switch (q.cmd) {
        case BLE_CMD_PING:
          ble_notify_status();
          break;
        case BLE_CMD_LIST_SESSIONS:
          do_list_sessions();
          break;
        case BLE_CMD_MOUNT_CAL:
          do_mount_cal();
          ble_notify_status();
          break;
        case BLE_CMD_MOUNT_CAL_RST:
          do_mount_cal_reset();
          ble_notify_status();
          break;
        case BLE_CMD_GET_FILE: {
          // payload atteso: [index_lo][index_hi][kind]. Se e' corto, e' un
          // comando malformato: rispondiamo con un errore esplicito invece di
          // ignorarlo in silenzio (un'app che non riceve NULLA non sa se il
          // device e' occupato, rotto o sordo).
          if (q.len < 3) { send_data_error(BLE_ERR_BAD_KIND); break; }
          uint16_t idx  = (uint16_t)q.payload[0] | ((uint16_t)q.payload[1] << 8);
          uint8_t  kind = q.payload[2];
          do_send_file(idx, kind);
          break;
        }
        case BLE_CMD_CONFIG_RESET:
          // Ripristina i default e salva; il main rifara' config_apply all'uscita.
          config_reset();
          if (s_cfg) {
            s_cfg->checksum = config_checksum_of(*s_cfg);
            if (s_char_config) s_char_config->setValue((uint8_t*)s_cfg, sizeof(Config));
          }
          g_mount_orientation = s_cfg ? s_cfg->mount_orientation : ARCHBB_MOUNT_DEFAULT;
          s_config_edited = true;
          ble_notify_status();
          break;
        default:
          break;
      }
    }

    // 2) STATUS periodico ogni ~2s se connesso (batteria, SD, ora vivi nell'app).
    if (g_ble_connected && millis() - lastStatusMs > 2000) {
      lastStatusMs = millis();
      ble_notify_status();
    }
  }

  // --- Uscita pulita -------------------------------------------------------
  // Rilascia il PM-lock (NON lo distrugge: e' file-level, riusato alla prossima
  // entrata). Poi segnala a ble_stop che il task e' finito e si auto-elimina.
  if (s_ble_pm_lock) esp_pm_lock_release(s_ble_pm_lock);
  s_ble_task = nullptr;      // segnala a ble_stop che il task e' finito
  vTaskDelete(nullptr);      // il task si auto-elimina
}
