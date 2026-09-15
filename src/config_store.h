// ============================================================================
//  ArchBB 1.83 — config_store.h · FASE 6 (NVS + CONFIG)
//  Cesare Pagura · Padova/Noale IT · 20 luglio 2026
// ----------------------------------------------------------------------------
//  COSA E' E PERCHE'
//    E' la FONTE DI VERITA' unica dei parametri configurabili. Fino alla 4c i
//    parametri erano costanti compilate (TRIG_DEFAULT_*, WINDOW_*_MS,
//    ARCHBB_MOUNT_DEFAULT): per cambiarli serviva ricompilare. Da qui vivono in
//    NVS, editabili senza reflash — oggi in sola lettura on-device, domani via
//    BLE/app.
//
//  IL PATTERN (lo stesso della 1.69, che l'handoff cita come modello)
//    Una struct Config vive in RAM ed e' L'UNICA cosa che il firmware legge a
//    runtime. L'NVS non si interroga a ogni tiro: si legge UNA VOLTA al boot in
//    RAM (config_load), e si riscrive solo quando qualcosa cambia (config_save).
//
//      boot ── config_load() ── NVS valido? ── SI ─> struct in RAM
//                                    │
//                                    NO (vuoto/versione diversa/checksum ko)
//                                    └─────────────> DEFAULT compilati in RAM
//      runtime ── tutti leggono g_config (RAM)
//      edit    ── modifica g_config ── config_save() ── scrive in NVS
//
//  ROBUSTEZZA — versione + checksum (la parte che conta davvero)
//    - config_version: se aggiungi un campo e riflashi, la versione salvata non
//      combacia -> NON leggi spazzatura, riparti dai default. Migrazione sicura.
//    - checksum in coda: se una scrittura si interrompe (batteria a meta'), al
//      load il checksum non torna -> scarti il blocco corrotto, usi i default.
//    Senza questi due, il primo campo aggiunto o la prima scrittura interrotta
//    danno un dispositivo dal comportamento inspiegabile. Con questi, il peggio
//    e' "tornato ai default" — sempre recuperabile.
//
//  PARAMETRI vs PROCEDURE (distinzione importante)
//    Il config contiene VALORI. La calibrazione (livella, G) e' una PROCEDURA
//    che PRODUCE valori. Qui ci sono i CAMPI offset (offset_cant/alzo/g), a 0 =
//    non calibrato; le procedure che li misurano verranno in un passo separato.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <stdint.h>
#include "config.h"   // per i DEFAULT compilati (TRIG_*, WINDOW_*, MOUNT_*)

// ----------------------------------------------------------------------------
//  Versione del formato config. INCREMENTARE ogni volta che si cambia la struct
//  Config (campo aggiunto/rimosso/ridimensionato). E' il numero che fa scattare
//  la migrazione ai default quando la struct in NVS e' di una versione vecchia.
// ----------------------------------------------------------------------------
// v7 (F20b): nessun campo nuovo — la versione sale perche' cambia il MODO in cui
// si migra. Vedi config_store.cpp §TARATURA.
static constexpr uint16_t CONFIG_VERSION = 7;   // v6 (F19): raggi del cerchio
                                                // d'impatto (imp_raggio_*_cm).
                                                // v5/v4: modo trigger TRIG_ACCEL_DEV
                                                // (| |a|-g |, invariante all'orientamento)
                                                // e soglia default 6. v3 era: soglia 10
                                                // (misurata sui tiri veri 1.83) e
                                                // trigger_configure ora OPERATIVA.
                                                // v2 era: mount_orientation 0..3
                                                // (flip-Z spostato in mount_apply,
                                                // eliminato MOUNT_0_FLIPZ=4). Il
                                                // bump scarta i config v1 col vecchio
                                                // default 4 -> riparte da MOUNT_0.

// ----------------------------------------------------------------------------
//  La struct Config — l'unica fonte di verita' a runtime.
// ----------------------------------------------------------------------------
//  packed per avere un layout NVS deterministico (il checksum copre byte esatti).
//  Campi NUOVI vanno SEMPRE in coda, prima del checksum: cosi' una versione
//  vecchia letta con una struct nuova non disallinea i campi esistenti (e
//  comunque la version-check li scarterebbe). Stessa disciplina di ScorePacket.
struct __attribute__((packed)) Config {
  uint16_t version;             // = CONFIG_VERSION al save; controllato al load

  // --- TRIGGER ---
  uint8_t  trig_mode;           // TriggerMode: 2=accel_dev | |a|-g | (DEFAULT dal 25/07,
                                //  invariante all'orientamento), 0=az_threshold (storica,
                                //  soggetta a falsi da inclinazione), 1=jerk
  float    trig_threshold_ms2;  // soglia |az| [m/s^2] (default 20.0)
  uint8_t  trig_confirm_n;      // campioni consecutivi di conferma (default 2)
  uint16_t trig_rearm_ms;       // riarmo minimo tra due trigger [ms] (default 2000)

  // --- FINESTRE BURST ---
  uint16_t window_pre_ms;       // finestra pre-scocco [ms] (default 2000)
  uint16_t window_post_ms;      // finestra post-scocco [ms] (default 1000)

  // --- MONTAGGIO ---
  //  Indice della enum MountOrientation (mount.h). Valori: 0=frontale,
  //  1=DX(-90), 2=SX(+90), 3=opposto(180). Default 0 (MOUNT_0).
  //  NB 1.83: il flip-Z hardware del chip (asse Z invertito) NON e' un
  //  orientamento: e' applicato a monte da mount_apply (ARCHBB_HW_FLIP_Z),
  //  sempre, prima della rotazione. Percio' qui il default e' lo 0 pulito e non
  //  serve piu' la vecchia quinta posizione MOUNT_0_FLIPZ.
  uint8_t  mount_orientation;

  // --- OFFSET DI CALIBRAZIONE (prodotti dalle procedure, che arrivano dopo) ---
  //  A 0 = non calibrato. La livella misura offset_cant/alzo (il micro-tilt del
  //  supporto: l'handoff cita cant~+4.6 alzo~-2.9 a riposo). La calibrazione G
  //  misura offset_g (scostamento di |a| da 9.81 a riposo). Oggi restano 0.
  float    offset_cant_deg;     // 0 = non calibrato
  float    offset_alzo_deg;     // 0 = non calibrato
  float    offset_g_ms2;        // 0 = non calibrato

  // --- FISICA ARCO ---
  float    bow_mass_kg;         // massa arco armato [kg] (0 = non impostata)

  // --- FASE 19: SCORING, cerchio d'impatto ---
  //  Raggio in CENTIMETRI rappresentato dal cerchio della schermata impatto.
  //  Due valori perche' le due situazioni hanno scale diverse di un fattore 3:
  //  un colpito sta dentro la sagoma, un mancato puo' essere a un metro e mezzo.
  //  Finiscono anche in session.txt: senza, i centimetri salvati non avrebbero
  //  scala per chi rilegge il file fra sei mesi.
  uint16_t imp_raggio_colpito_cm;   // default 50  -> 0,50 cm/px sul display
  uint16_t imp_raggio_mancato_cm;   // default 150 -> 1,50 cm/px sul display

  // --- coda: checksum di tutti i byte precedenti ---
  uint32_t checksum;
};

// ----------------------------------------------------------------------------
//  FASE 20b — LA TARATURA SOPRAVVIVE AI CAMBI DI VERSIONE
// ----------------------------------------------------------------------------
//  Il 15/08 le sedute di giardino sono uscite con mount=0 invece di 2 e con
//  entrambi gli offset azzerati. Nessuno aveva toccato niente: era il bump di
//  CONFIG_VERSION da 5 a 6 della Fase 19 che aveva riportato TUTTO ai default.
//
//  Il difetto non era il reset in se' — e' che trattava allo stesso modo due
//  categorie diverse:
//
//    PARAMETRI   soglia del trigger, durata delle finestre, raggi del cerchio.
//                Ricostruibili in dieci secondi da chiunque legga il manuale.
//                Un default sbagliato si nota subito.
//
//    TARATURA    orientamento del montaggio e offset degli angoli. Misurati al
//                banco con l'arco fisico, costati ore, e — soprattutto — un
//                default sbagliato NON si nota: il sistema continua a produrre
//                numeri plausibili, solo sull'asse sbagliato. Il "cant di 8
//                gradi costante" delle sedute del 15/08 era l'elevazione del
//                bersaglio travestita.
//
//  Questi tre campi vengono percio' scritti ANCHE come chiavi NVS singole, fuori
//  dal BLOB. Chiavi scalari non dipendono dal layout della struct: sopravvivono
//  a qualunque cambio futuro, non solo a questo. Alla migrazione si riparte dai
//  default e poi si RIMETTONO i valori di taratura salvati.
//
//  Ritorna true se una taratura precedente e' stata trovata e riapplicata.
bool config_taratura_ripristina();
// Scrive le chiavi singole. Chiamata da config_save(): la copia fuori dal blob
// non e' un backup manuale da ricordarsi, e' un effetto di ogni salvataggio.
void config_taratura_salva();

// ----------------------------------------------------------------------------
//  Istanza globale — la copia di lavoro in RAM. Definita in config_store.cpp.
//  Tutti i moduli leggono DA QUI a runtime, mai dall'NVS direttamente.
// ----------------------------------------------------------------------------
extern Config g_config;

// ----------------------------------------------------------------------------
//  API
// ----------------------------------------------------------------------------

// Riempie g_config coi DEFAULT compilati (i valori attuali del firmware). Non
// tocca l'NVS. Usata come base sia al primo avvio sia dal "ripristina default".
void config_set_defaults();

// Carica il config dall'NVS in g_config. Se l'NVS e' vuoto, di versione diversa,
// o col checksum errato, riempie g_config coi default (e NON riscrive: la
// scrittura la decide il chiamante). Ritorna true se ha letto un config VALIDO
// dall'NVS, false se ha dovuto usare i default. Da chiamare una volta al boot,
// prima di configurare trigger/mount/finestre.
bool config_load();

// Scrive g_config nell'NVS (ricalcolando version e checksum). Ritorna true se
// la scrittura e' riuscita. Da chiamare dopo ogni modifica a g_config.
bool config_save();

// Ripristina i default compilati IN g_config e li SCRIVE in NVS (config_save).
// E' il "ripristina di fabbrica": azzera tutto ai valori del firmware corrente.
bool config_reset();

// Applica i valori di g_config ai moduli che li usano a runtime (trigger, mount).
// Da chiamare al boot dopo config_load, e ogni volta che g_config cambia (dopo
// un futuro edit BLE). Centralizza il "come i parametri diventano operativi".
void config_apply();

// FASE 7 (BLE): calcola il checksum di UNA Config qualsiasi (non solo g_config).
// Serve al modulo BLE per validare l'INTEGRITA' di una Config ricevuta dall'app
// PRIMA di accettarla in RAM: se il checksum inviato non torna, il pacchetto e'
// corrotto e va scartato. Espone in forma pubblica il checksum interno (che
// resta la sola implementazione: nessuna duplicazione della formula).
uint32_t config_checksum_of(const Config& c);
