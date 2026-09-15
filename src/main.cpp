// ============================================================================
//  ArchBB 1.83 — main.cpp · FASE 4c (angoli cant/alzo dal burst) · v1
//  Cesare Pagura · Padova/Noale IT · 19 luglio 2026
// ----------------------------------------------------------------------------
//  COSA AGGIUNGE LA FASE 4c (rispetto alla 4a/4b)
//    La catena IMU era pronta fino al BUFFER CONGELATO: al FIRED il circular
//    buffer cattura la finestra PRE+POST attorno allo scocco e diventa READY.
//    Mancava l'ultimo anello: LEGGERE quel burst e calcolarne gli ANGOLI
//    biomeccanici. Ora, al DONE dello scoring:
//
//      circular_buffer_get_burst() -> shot_angles_compute() -> ScoreResult
//                                   -> riepilogo esteso (cant/alzo/hold/release)
//
//  IL PUNTO CRITICO: IL FRAME DI MONTAGGIO (perche' c'e' il CLINOMETRO)
//    shot_angles fu validato sul frame CANONICO della 1.69. La 1.83 e' un'altra
//    scheda: gli assi grezzi del suo QMI8658 potrebbero non essere canonici.
//    Percio' imu_task ora applica mount_apply() a monte (frame canonico a
//    valle), MA quale montaggio sia quello giusto NON si decide a tavolino: si
//    misura col CLINOMETRO (menu d'avvio), test statico ad angolo noto. Il
//    default e' MOUNT_DX_M90 (ipotesi \"come 1.69 a destra\"), da CONFERMARE.
//
//  ALLOCAZIONE DEL BURST: STATIC, MAI SULLO STACK
//    Il burst e' CIRCULAR_BUFFER_SIZE * sizeof(ImuSample) = 750*30 = ~22 KB.
//    Su stack (locale del loop) farebbe saltare la UI task. Lo teniamo STATIC:
//    vive una volta sola nel .bss, azzerato all'avvio, riusato a ogni tiro.
// ============================================================================
// ============================================================================
//  ArchBB 1.83 — main.cpp (FIRMWARE DI PRODUZIONE)
// ----------------------------------------------------------------------------
//  Questo file e' attivo SOLO quando NON si compila il test montaggi. L'env
//  archbb_183_mount_test definisce -D ARCHBB_MOUNT_TEST=1, che disattiva tutto
//  questo file e attiva main_mount_test.cpp al suo posto. Cosi' entrambi i main
//  vivono in src/ senza conflitto (un solo setup()/loop() compilato), senza
//  bisogno di src_filter fragili con path relativi.
// ============================================================================
#if !defined(ARCHBB_MOUNT_TEST) && !defined(ARCHBB_RAW_DIAG) && !defined(ARCHBB_DIAG_ASSI)

#include <Arduino.h>
#include "display.h"
#include "clino.h"      // FASE 18: clinometro di banco (sostituisce runAngleDiag)
#include "touch.h"
#include "scoring.h"
#include "config.h"
#include "imu.h"
#include "trigger.h"
#include "tempo.h"        // FASE 20: tempi di alzata e di mira
#include "circular_buffer.h"
#include "touch_cal.h"
#include "mount.h"          // FASE 4c: frame di montaggio (g_mount_orientation)
#include "shot_angles.h"    // FASE 4c: shot_angles_compute, shot_angle_to_cdeg
#include "sd_storage.h"     // FASE 5: persistenza microSD (sessioni, CSV+burst)
#include "battery.h"        // FASE 5: stato batteria via AXP2101
#include "config_store.h"   // FASE 6: config NVS (fonte di verita' unica)
#include "config_ui.h"      // FASE 6: schermata CONFIG on-device (lettura + reset)
#include "rtc_clock.h"      // FASE 6: orologio RTC PCF85063 (data sessioni)
#include "ble_service.h"    // FASE 7: modo BLE modale (colloquio con smartphone)
#include "power_mgr.h"      // FASE 24a: politica di risparmio energetico
#include <esp_mac.h>        // FASE 7: esp_read_mac (suffisso MAC nel nome device)
#include <esp_system.h>     // FIX-DIAG: esp_reset_reason (motivo dell'ultimo reset)
#include <math.h>            // F24b: isnan() sulle soglie di preavviso batteria

// Flag di shutdown pulito (predisposto: i task lo controllano nella 1.69 e
// teniamo il pattern anche se in Fase 4 non lo attiviamo mai).
volatile bool g_shutdown = false;

// ----------------------------------------------------------------------------
//  FASE 7 — INGRESSO nel modo BLE (via PWRKEY del PMU, NON GPIO0).
// ----------------------------------------------------------------------------
//  Il pulsante utente della 1.83 e' il PWRKEY dell'AXP2101, letto via I2C dal
//  PMU (funzione power_key_short_pressed() in battery.cpp). GPIO0 e' stato
//  BANDITO: e' uno strapping pin di boot e usarlo a runtime causava blocchi
//  (falsi LOW -> enterBleMode spurio -> hang -> reset). Il PWRKEY non ha questi
//  problemi: e' un ingresso del PMU pensato apposta come tasto utente.
//
//  ARCHBB_BLE_ENTRY:
//    2 = PWRKEY (default, corretto e stabile)
//    1 = GPIO0  (SOLO test storico, sconsigliato: causava blocchi)
//    0 = nessun ingresso (BLE non attivabile)
#define ARCHBB_BLE_ENTRY 2
static constexpr int PIN_BLE_BUTTON = 0;   // GPIO0 = BOOT (usato solo se ENTRY==1)

// Gestione degli HANDLE dei task IMU/trigger: servono per SOSPENDERLI quando si
// entra in modo BLE (Core 1 libero, niente polling I2C) e RIPRENDERLI all'uscita.
// Prima erano creati con handle nullptr (non serviva riferirli); ora li teniamo.
static TaskHandle_t s_imuTaskHandle     = nullptr;
static TaskHandle_t s_triggerTaskHandle = nullptr;

enum class AppFlow : uint8_t { ATTESA, PRETIRO, SCORING, RIEPILOGO, SESSION_CHOICE,
                               BLE_CONFIRM, BLE_MODE, SPEGNIMENTO };
static AppFlow     s_flow = AppFlow::ATTESA;

// --- F24b: modale di conferma BLE ------------------------------------------
//  Cinque secondi: piu' di quanto serve a leggere due pulsanti, meno di quanto
//  serve a dimenticarsi perche' lo schermo e' cambiato.
static constexpr uint8_t ARCHBB_BLE_CONFIRM_S = 5;

// F24b — intervallo minimo fra due commutazioni di sessione (ms).
static constexpr uint32_t ARCHBB_SESSION_COOLDOWN_MS = 2000;
static uint32_t s_bleConfirmMs   = 0;   // quando e' comparsa

// --- F24b.2: stato dello spegnimento in corso ------------------------------
//  Il PMU stacca dopo 10 s di pressione continua, ma la IRQ che ce lo dice
//  scatta molto prima. Se dopo ARCHBB_SPEGN_MS siamo ancora vivi, il tasto e'
//  stato rilasciato: 12 s, cioe' oltre la soglia del PMU con due secondi di
//  margine per non dichiarare un annullamento che non c'e' stato.
static constexpr uint32_t ARCHBB_SPEGN_MS = 12000;
static uint32_t s_spegnMs        = 0;
static bool     s_sessEraAperta  = false;   // per poterla riprendere se annulla
static uint8_t  s_bleConfirmLast = 0;   // ultimo secondo disegnato

// ============================================================================
//  FASE 22 — IL BERSAGLIO ARMATO
// ============================================================================
//  Distanza ed elevazione si dichiarano PRIMA dello scocco e restano armate
//  fino a che non le si cambia. La sequenza di campo diventa:
//      telemetro -> tocco sulla card -> distanza, elevazione, OK -> attesa
//  invece di scoccare e poi ricostruire a memoria a che bersaglio si stava
//  tirando. E' anche il presupposto di ogni futuro ausilio balistico: senza
//  sapere la distanza PRIMA, non c'e' niente da calcolare prima.
//
//  SOLO IN RAM, per scelta. All'accensione si riparte dai default e non dagli
//  ultimi valori usati: dopo uno spegnimento non c'e' nessuna ragione di
//  credere di essere ancora alla stessa piazzola, e un valore ereditato da
//  ieri sarebbe piu' pericoloso di un default palesemente generico. Zero
//  scritture NVS, zero usura, zero migrazioni da gestire ai bump di versione.
static constexpr uint8_t PRE_DIST_DEFAULT = 30;
static constexpr int8_t  PRE_ELEV_DEFAULT = 0;
static uint8_t s_preDist  = PRE_DIST_DEFAULT;
static int8_t  s_preElev  = PRE_ELEV_DEFAULT;
// true = confermata nel pre-tiro DOPO l'ultimo scocco; false = ereditata dal
// tiro precedente. Non entra nel dato salvato: e' solo il colore del bordo
// della card, cioe' un promemoria visivo. Se un giorno servisse anche
// nell'analisi, diventerebbe una colonna in coda al CSV.
static bool    s_preConf  = false;
static ScoreResult s_lastResult;
static uint32_t    s_lastTempMs = 0;   // per rinfrescare la temperatura in attesa

// ----------------------------------------------------------------------------
//  BURST STATIC (Fase 4c) — ~22 KB, MAI sullo stack.
// ----------------------------------------------------------------------------
//  Buffer di lavoro dove copiamo il burst congelato per darlo a
//  shot_angles_compute. static -> vive nel .bss, allocato una volta, riusato a
//  ogni tiro. In PSRAM ci sarebbe stato bene, ma il .bss interno basta e avanza
//  (22 KB su ~320 KB di RAM interna) ed evita una dipendenza da ps_malloc qui.
static ImuSample s_burst[CIRCULAR_BUFFER_SIZE];

// ----------------------------------------------------------------------------
//  FASE 5 — metadati del burst appena letto, per la scrittura SD DIFFERITA.
// ----------------------------------------------------------------------------
//  computeShotAngles() copia il burst in s_burst al DONE. La scrittura su SD
//  avviene invece piu' tardi, al tap OK del riepilogo (hit==2), PRIMA di
//  enterAttesa() che resetta il buffer. Fra i due momenti dobbiamo ricordare
//  QUANTI campioni e DOVE sta il trigger: li salviamo qui quando leggiamo il
//  burst, e li riusiamo alla scrittura. s_burstN=0 -> nessun burst valido da
//  salvare (tiro manuale senza IMU): si salvera' il solo CSV.
static uint16_t s_burstN     = 0;   // campioni validi in s_burst
static uint16_t s_burstTrig  = 0;   // indice di scocco entro s_burst
// F24b: quanti campioni PRE-trigger appartengono davvero a questo tiro. Meno di
// s_burstTrig significa che il trigger e' scattato poco dopo il riarmo e parte
// della finestra e' residuo dell'anello precedente.
static uint16_t s_burstPreOk = 0xFFFF;

// ----------------------------------------------------------------------------
//  FASE 5 — gestione SESSIONE via tocco (ridisegnato: niente piu' tap-lungo).
// ----------------------------------------------------------------------------
//  In ATTESA, con IMU viva, il tocco apre/chiude la sessione. Usiamo il fronte
//  di RILASCIO gia' debounced (tapReleased): un tocco pulito = un toggle. Non
//  serve piu' distinguere breve/lungo -> il bug del cronometro fragile sparisce
//  perche' non c'e' piu' nulla da cronometrare.

// StatusInfo condiviso per la barra di stato (Fase 5): ricostruito a ogni
// ingresso in ATTESA. Vive qui per non riallocarlo a ogni refresh.
static StatusInfo s_status;

// ----------------------------------------------------------------------------
//  Debounce/cooldown per i tap di RIEPILOGO (identico alla Fase 4a).
// ----------------------------------------------------------------------------
static constexpr uint8_t  REL_STABLE_N = 2;
static constexpr uint32_t COOLDOWN_MS  = 300;
static bool     s_stable       = false;
static uint8_t  s_relCnt       = 0;
static uint32_t s_lastActionMs = 0;

// FASE 22 — DOVE e' stato toccato, non solo SE.
//  Finora in ATTESA il tocco aveva un solo significato e la posizione non
//  serviva. Con la card serve, e va presa MENTRE il dito e' giu': al rilascio
//  il CST816 non riporta piu' coordinate, e l'ultimo campione valido e' l'unico
//  che dice dove si e' premuto davvero.
static uint16_t s_pressX = 0;
static uint16_t s_pressY = 0;

static bool tapReleased(const TouchData& td) {
  bool raw = td.valid && (td.fingers > 0);
  if (raw) { s_pressX = td.x; s_pressY = td.y;
             s_relCnt = 0; s_stable = true; return false; }
  if (s_stable && ++s_relCnt >= REL_STABLE_N) {
    s_stable = false; s_relCnt = 0;
    uint32_t now = millis();
    if (now - s_lastActionMs >= COOLDOWN_MS) { s_lastActionMs = now; return true; }
  }
  return false;
}

// ----------------------------------------------------------------------------
//  computeShotAngles — legge il burst congelato e popola le metriche 4c.
// ----------------------------------------------------------------------------
//  Chiamata al DONE dello scoring, PRIMA di enterAttesa() (che resetta il
//  buffer). Se il buffer non e' READY (tiro manuale a banco senza IMU, o burst
//  non congelato) lascia i campi a \"non valido\": il riepilogo lo mostra come
//  n/d. cant/alzo e hold/release restano indipendenti fra loro (v. shot_angles).
// ----------------------------------------------------------------------------
static void computeShotAngles(ScoreResult& res) {
  // --- FASE 20: i tempi NON dipendono dal burst ----------------------------
  //  Vengono da una macchina a stati che gira sui campioni in tempo reale e
  //  che ha gia' congelato il risultato in doFire(). Si copiano PRIMA di ogni
  //  altra cosa e fuori da tutti i return anticipati sotto: un tiro senza
  //  burst valido (buffer non pronto, IMU appena riavviata) ha comunque dei
  //  tempi validi, e perderli per un motivo che non li riguarda sarebbe un
  //  accoppiamento inventato.
  {
    TempoTiro tt = tempo_ultimo();
    res.tempo_mira_ms   = tt.mira_ms;
    res.tempo_alzata_ms = tt.alzata_ms;
  }
  res.assetto_ok = 2;   // finche' gli angoli non sono calcolati

  // Default: tutto \"non calcolato\". Se qualcosa manca, il riepilogo dira' n/d.
  res.angles_valid = res.angles_stable = false;
  res.hold_valid   = res.release_valid = false;
  res.cant_cdeg = res.alzo_cdeg = 0;
  res.hold_cdeg = 0; res.release_jerk = 0;
  res.hold_class = HOLD_NA; res.release_class = RELEASE_NA;
  // F21: il picco segue la stessa disciplina. Lo zero e' un valore legittimo
  // (arco fermo), quindi non puo' significare "non misurato": serve il flag.
  res.picco_valid = false; res.picco_cms2 = 0;

  // FASE 5: nessun burst valido finche' non lo leggiamo davvero (sotto). Cosi'
  // un tiro manuale senza IMU non eredita i metadati del burst precedente.
  s_burstN = 0; s_burstTrig = 0; s_burstPreOk = 0xFFFF;

  // Il burst ha senso solo se l'IMU e' viva e il buffer ha catturato la finestra.
  if (!g_imu_ok) return;
  if (circular_buffer_state() != BUFFER_READY) return;   // niente da leggere

  uint16_t trig_idx = 0;
  uint16_t preOk = 0xFFFF;
  uint16_t n = circular_buffer_get_burst(s_burst, CIRCULAR_BUFFER_SIZE, &trig_idx, &preOk);
  if (n == 0) return;

  // FASE 5: memorizza i metadati del burst per la scrittura SD differita (al
  // tap OK del riepilogo). Il burst e' gia' in s_burst e ci resta fino ad allora.
  s_burstN     = n;
  s_burstTrig  = trig_idx;
  s_burstPreOk = preOk;
  if (preOk < trig_idx) {
    // Non e' un errore: e' un tiro partito troppo presto dopo il riarmo. Lo
    // diciamo nel log e lo scriviamo nel file; gli angoli si arrangeranno da
    // soli a dichiararsi non validi se la finestra di calma non ci sta.
    DBG("BURST: solo %u/%u campioni pre-trigger freschi (riarmo recente)",
        (unsigned)preOk, (unsigned)trig_idx);
  }

  // ODR_250HZ serve solo a dimensionare le finestre in campioni; l'integrazione
  // usa i timestamp reali del burst.
  //  F24b — ATTENZIONE al commento che c'era qui: diceva "= 224 Hz reale". Non
  //  lo era: i 41 burst del 12/09 misurano 200,12 Hz, e quel 224 rendeva ogni
  //  finestra il 12% piu' lunga di quanto dichiarasse. Adesso la frequenza vera
  //  viene misurata e scritta nel burst e in session.txt; qui resta il codice di
  //  configurazione perche' e' quello che dimensiona l'array, ma nessuno deve
  //  piu' credere che 224 sia una misura.
  ShotAngles a = shot_angles_compute(s_burst, n, trig_idx, ODR_250HZ,
                                     g_config.offset_cant_deg,
                                     g_config.offset_alzo_deg,
                                     preOk);

  if (a.valid) {
    res.angles_valid  = true;
    res.angles_stable = a.stable;
    res.cant_cdeg = shot_angle_to_cdeg(a.cant_deg);
    res.alzo_cdeg = shot_angle_to_cdeg(a.alzo_deg);
    // F20b: plausibilita'. Si marca, non si scarta — vedi scoring.h.
    const bool ok = (abs((int)res.cant_cdeg) <= ASSETTO_LIMITE_CDEG) &&
                    (abs((int)res.alzo_cdeg) <= ASSETTO_LIMITE_CDEG);
    res.assetto_ok = ok ? 0 : 1;
  }
  if (a.hold_valid) {
    res.hold_valid = true;
    res.hold_cdeg  = shot_angle_to_cdeg(a.hold_deg);
    res.hold_class = a.hold_class;
  }
  if (a.release_valid) {
    res.release_valid = true;
    // jerk e' un modulo >=0; lo saturiamo a 65535 per il campo uint16.
    float j = a.release_jerk;
    res.release_jerk  = (j > 65535.0f) ? 65535 : (uint16_t)(j + 0.5f);
    res.release_class = a.release_class;
  }
  if (a.picco_valid) {
    res.picco_valid = true;
    // m/s^2 -> centesimi, con saturazione. Il fondo scala dell'accelerometro
    // sta molto sotto i 655 m/s^2, quindi la saturazione non scatta mai in
    // pratica: c'e' perche' un uint16 che va in overflow silenzioso produce un
    // numero piccolo e credibile, che e' il modo peggiore di sbagliare.
    float c = a.picco_ms2 * 100.0f;
    if (c < 0.0f)       c = 0.0f;
    if (c > 65535.0f)   c = 65535.0f;
    res.picco_cms2 = (uint16_t)(c + 0.5f);
  }
}

// ----------------------------------------------------------------------------
//  FASE 5 — refreshStatus: ricostruisce lo StatusInfo per la barra di stato.
// ----------------------------------------------------------------------------
//  Legge batteria (AXP2101, I2C singola), spazio SD (dalla CACHE, non piu' dalla
//  FAT) e stato sessione (variabili RAM). Tutte letture non-bloccanti: sicura da
//  chiamare a ogni refresh in ATTESA (ogni 3s).
//
//  FIX BLOCCO: prima qui si chiamava sd_free_mb() quando ancora scandiva la FAT
//  a ogni invocazione -> una scansione lenta/incagliata bloccava la UI dopo un
//  paio di refresh. Ora sd_free_mb() ritorna la cache aggiornata solo al mount e
//  dopo un salvataggio: il refresh non tocca piu' il filesystem.
static void refreshStatus() {
  s_status.battPct      = battery_percent();
  s_status.battCharging = battery_charging();
  s_status.sdOk         = g_sd_ok;
  s_status.sdFreeMb     = g_sd_ok ? sd_free_mb() : 0;
  s_status.sdDiag       = g_sd_diag;     // diagnosi (mostrata se !sdOk)
  s_status.sessionOpen  = g_session_open;
  s_status.sessionId    = sd_session_id();
  s_status.sessionShots = sd_session_shot_count();
  // FASE 6: orologio dalla RTC, solo se valida (impostata).
  RtcTime rt = rtc_now();
  s_status.hasTime = rt.valid;
  if (rt.valid) snprintf(s_status.clock, sizeof(s_status.clock), "%02u:%02u", rt.hour, rt.minute);
  else          s_status.clock[0] = '\0';
}

// ----------------------------------------------------------------------------
//  Entra in ATTESA: disegna la schermata IMU-armata e ARMA il trigger.
// ----------------------------------------------------------------------------
static void enterAttesa() {
  // Il tiro precedente e' concluso -> libera il buffer per il prossimo.
  // In 4c il burst e' gia' stato LETTO (computeShotAngles) al DONE: qui si
  // resetta soltanto, tornando in RUNNING per la finestra mobile del prossimo.
  circular_buffer_reset();
  // FASE 22: azzera il debounce del tocco di ATTESA. Si rientra qui da quattro
  // strade diverse (riepilogo, pre-tiro, BLE, scelta sessione) e uno stato di
  // pressione rimasto appeso in una di quelle produrrebbe un tocco fantasma
  // sulla card appena disegnata.
  s_stable = false; s_relCnt = 0; s_lastActionMs = millis();
  float tC = g_imu_ok ? imu_read_temperature() : NAN;
  refreshStatus();                                       // FASE 5: aggiorna barra
  displayAttesaTiroIMU_v5((uint16_t)(g_shot_id + 1), tC, s_status,
                          s_preDist, s_preElev, s_preConf);
  s_lastTempMs = millis();
  // Svuota un eventuale semaforo residuo (un trigger arrivato mentre eravamo
  // ancora nel riepilogo non deve far partire subito il tiro dopo).
  if (g_trigger_fired_sem) xSemaphoreTake(g_trigger_fired_sem, 0);
  trigger_set_armed(true);
  s_flow = AppFlow::ATTESA;
}

// ----------------------------------------------------------------------------
//  Avvia lo scoring del tiro corrente: DISARMA il trigger e mostra il flash.
// ----------------------------------------------------------------------------
static void startScoringForShot(float azPeak) {
  trigger_set_armed(false);               // niente tiri fantasma durante lo score
  displayTriggerFlash(g_shot_id, azPeak); // feedback visivo istantaneo
  // FASE 22: lo scoring riceve il bersaglio armato e parte da ESITO.
  scoringStart(g_shot_id, s_preDist, s_preElev);
  // Il tiro ha "consumato" la conferma: da qui in avanti i valori sono
  // EREDITATI, e la card lo dira' col bordo ambra al rientro in ATTESA.
  s_preConf = false;
  // Reset debounce del main: il tap dato dentro lo scoring non deve trapelare.
  s_stable = false; s_relCnt = 0; s_lastActionMs = millis();
  s_flow = AppFlow::SCORING;
}

// ----------------------------------------------------------------------------
//  FASE 7 — bleButtonPressed(): fronte di pressione del tasto BOOT, debounced.
// ----------------------------------------------------------------------------
//  GPIO0 e' attivo-basso (pull-up). Rileviamo il FRONTE di pressione (HIGH->LOW)
//  una volta sola per pressione, con un debounce temporale semplice: dopo un
//  evento valido ignoriamo il tasto per DEBOUNCE_MS, cosi' un rimbalzo meccanico
//  non genera due toggle. Non usiamo il rilascio: il fronte di pressione da'
//  una risposta immediata (l'utente vuole "entro/esco" al tocco, non al lascia).
static bool bleButtonPressed() {
#if ARCHBB_BLE_ENTRY == 2
  // PWRKEY del PMU (via I2C): il tasto utente vero della 1.83. Non-bloccante,
  // ritorna true una volta per pressione breve. Nessun problema di strapping.
  // THROTTLE: interroghiamo il PMU al massimo ogni ~120ms invece che a ogni giro
  // di loop (~15ms). Una pressione dura ben piu' di 120ms, quindi non se ne
  // perde nessuna, e alleggeriamo il bus I2C condiviso con IMU/touch.
  static uint32_t lastPollMs = 0;
  uint32_t now = millis();
  if (now - lastPollMs < 120) return false;
  lastPollMs = now;
  return power_key_short_pressed();
#elif ARCHBB_BLE_ENTRY == 1
  // GPIO0 (SOLO test storico: causava blocchi, non usare sul campo).
  static bool     wasLow        = false;
  static uint32_t lastEdgeMs    = 0;
  constexpr uint32_t DEBOUNCE_MS = 250;
  bool low = (digitalRead(PIN_BLE_BUTTON) == LOW);
  uint32_t now = millis();
  if (low && !wasLow && (now - lastEdgeMs >= DEBOUNCE_MS)) {
    wasLow = true; lastEdgeMs = now; return true;
  }
  if (!low) wasLow = false;
  return false;
#else
  // Nessun ingresso: BLE non attivabile.
  return false;
#endif
}

// ----------------------------------------------------------------------------
//  FASE 7 — enterBleMode(): sospende i task IMU e avvia il colloquio BLE.
// ----------------------------------------------------------------------------
//  L'ordine conta:
//    1) DISARMA il trigger e SOSPENDI imu_task/trigger_task: da qui Core 1 e'
//       libero e nessuno tocca piu' l'I2C in polling (il rischio di contesa col
//       BLE sparisce per costruzione — vedi ble_service.h).
//    2) ble_start(): NimBLE su Core 0, advertising col nome device.
//    3) Disegna la schermata "in ascolto".
//  Il nome advertising porta le ultime 2 cifre esadecimali del MAC cosi' due
//  ArchBB vicini si distinguono nello scanner. Lo componiamo una volta.
static char s_bleDevName[24] = "ArchBB-183";
static bool s_bleLastConnected = false;   // per ridisegnare solo al cambio stato

static void enterBleMode() {
  // 1) Ferma la catena IMU. trigger_set_armed(false) prima, poi sospendi: cosi'
  //    non resta un semaforo di trigger appeso da raccogliere all'uscita.
  trigger_set_armed(false);

  // --- SOSPENSIONE SICURA DEI TASK (fix deadlock PWRKEY) -------------------
  //  ATTENZIONE: sospendere un task MENTRE detiene il mutex del bus I2C lascia
  //  il mutex preso per sempre -> il primo i2cLock() successivo (refreshStatus,
  //  ble_start) si blocca in eterno. Era esattamente il blocco al PWRKEY.
  //
  //  SOLUZIONE: prendiamo NOI il mutex I2C prima di sospendere. Se imu_task e' a
  //  meta' di una transazione (dentro i2cLock), questa Take aspetta che la
  //  rilasci; solo allora sospendiamo, a bus LIBERO. Poi rilasciamo: da qui in
  //  poi nel modo BLE il bus e' usato solo dal loop UI (i task sono fermi), che
  //  prende/rilascia il mutex normalmente.
  i2cLock();                                     // attende che i task lascino il bus
  if (s_imuTaskHandle)     vTaskSuspend(s_imuTaskHandle);
  if (s_triggerTaskHandle) vTaskSuspend(s_triggerTaskHandle);
  i2cUnlock();                                   // bus libero, task fermi a bus pulito

  // Svuota un eventuale trigger residuo (un tiro proprio mentre premevi PWRKEY).
  if (g_trigger_fired_sem) xSemaphoreTake(g_trigger_fired_sem, 0);

  // 2) Componi il nome device col suffisso MAC (una volta) e avvia il BLE.
  {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);   // MAC del radio Bluetooth
    snprintf(s_bleDevName, sizeof(s_bleDevName), "ArchBB-183-%02X%02X", mac[4], mac[5]);
  }
  ble_start(s_bleDevName, &g_config);

  // 3) Schermata "in ascolto" (non connesso). refreshStatus riempie batt/SD/ora.
  refreshStatus();
  s_bleLastConnected = false;
  displayBleMode(false, s_bleDevName, s_status);

  s_flow = AppFlow::BLE_MODE;
}

// ----------------------------------------------------------------------------
//  FASE 7 — exitBleMode(): chiude il BLE, applica il config, torna in ATTESA.
// ----------------------------------------------------------------------------
//  L'ordine speculare all'ingresso:
//    1) ble_stop(): disconnette, ferma NimBLE, libera lo stack (~40KB).
//    2) Se l'app ha EDITATO il config, config_save()+config_apply() lo rendono
//       operativo (oggi: il montaggio; trigger/finestre quando arriveranno i
//       loro setter runtime — vedi README §BLE).
//    3) RIPRENDI i task IMU e rientra in ATTESA (enterAttesa riarma il trigger).
static void exitBleMode() {
  bool cfgEdited = ble_config_was_edited();
  bool timeSet   = ble_time_was_set();

  ble_stop();

  // Persisti + applica se qualcosa e' cambiato. config_reset (dall'app) ha gia'
  // scritto in NVS; una WRITE del config invece ha solo aggiornato la RAM: qui
  // la salviamo. config_save e' idempotente e poco costoso, lo chiamiamo se il
  // flag "edited" e' attivo.
  if (cfgEdited) {
    config_save();
    config_apply();   // rende operativo il montaggio (unico campo cablato oggi)
  }

  // Riprendi la catena IMU (se esiste: potrebbe non esserci a banco senza IMU).
  if (s_imuTaskHandle)     vTaskResume(s_imuTaskHandle);
  if (s_triggerTaskHandle) vTaskResume(s_triggerTaskHandle);

  // Feedback breve se l'ora e' stata sincronizzata (utile: ora le sessioni si
  // datano corrette). Poi torna in ATTESA, che riarma il trigger.
  if (timeSet) {
    RtcTime rt = rtc_now();
    if (rt.valid) {
      tft.fillScreen(ArchColor::BG);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(ArchColor::GOOD, ArchColor::BG);
      tft.drawString("ORA SINCRONIZZATA", ARCHBB_W/2, ARCHBB_H/2 - 12, 4);
      char t[24]; snprintf(t, sizeof(t), "%02u:%02u %02u/%02u/%04u",
                           rt.hour, rt.minute, rt.day, rt.month, rt.year);
      tft.setTextColor(ArchColor::TEXT2, ArchColor::BG);
      tft.drawString(t, ARCHBB_W/2, ARCHBB_H/2 + 20, 2);
      delay(1200);
    }
  }

  enterAttesa();   // ridisegna ATTESA e riarma il trigger
}

// ============================================================================
//  setup()
// ============================================================================
void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);   // Serial.printf non blocca col monitor chiuso

  // FASE 7: ingresso modo BLE. Col PWRKEY (ENTRY==2, default) NON serve alcun
  // pinMode: il tasto e' letto via I2C dal PMU (init in battery_init). GPIO0 si
  // configura solo nel caso di test storico ENTRY==1.
#if ARCHBB_BLE_ENTRY == 1
  pinMode(PIN_BLE_BUTTON, INPUT_PULLUP);
#endif

  // --- FASE 6: CONFIG da NVS (fonte di verita' unica) ----------------------
  //  Carica il config dall'NVS in g_config (o default se assente/corrotto), poi
  //  lo rende operativo. config_apply() imposta g_mount_orientation dal config:
  //  sostituisce l'assegnazione diretta di prima. Da qui il montaggio (e in
  //  futuro trigger/finestre via BLE) vengono dal config, non da costanti fisse.
  bool cfgFromNvs = config_load();
  config_apply();

  // --- Init hardware, nell'ordine giusto -----------------------------------
  displayInit();

  // FASE 24a: la politica energetica parte QUI, subito dopo il display e prima
  // di qualunque schermata. Prima della 24a la retro restava al 100% dal boot
  // allo spegnimento; adesso il livello base lo decide power_init(), e ogni
  // schermata successiva — compreso l'avviso di reset qui sotto — lo eredita.
  power_init();

  // --- FIX-DIAG: MOTIVO DELL'ULTIMO RESET a schermo ------------------------
  //  "I dati comandano": invece di indovinare perche' si blocca, facciamo dire
  //  al firmware perche' e' ripartito. Se l'ultimo boot e' finito in watchdog o
  //  panic (blocco -> reset automatico), qui lo vediamo scritto. Se e' un reset
  //  pulito (accensione, tasto RESET) non ci fermiamo. Resta 2s solo se il reset
  //  precedente era ANOMALO, cosi' non rallenta l'uso normale.
  static const char* s_resetTxt = "RESET: normale";
  {
    esp_reset_reason_t rr = esp_reset_reason();
    const char* txt = nullptr;
    switch (rr) {
      case ESP_RST_PANIC:    txt = "RESET: PANIC (crash SW)";        break;
      case ESP_RST_TASK_WDT: txt = "RESET: TASK WATCHDOG";           break;
      case ESP_RST_INT_WDT:  txt = "RESET: INT WATCHDOG";            break;
      case ESP_RST_WDT:      txt = "RESET: WATCHDOG (altro)";        break;
      case ESP_RST_BROWNOUT: txt = "RESET: BROWNOUT (tensione)";     break;
      default: break;   // POWERON / EXT / SW: reset normale, non mostrare nulla
    }
    // FASE 24a: il motivo va anche in /ARCHBB/ENERGIA.CSV. A schermo si vede per
    // due secondi e poi e' perso; nel log resta, datato, accanto alla tensione
    // di quel momento — ed e' esattamente cio' che serve per distinguere uno
    // spegnimento pulito del PMU da un brownout.
    if (txt) s_resetTxt = txt;
    if (txt) {
      tft.fillScreen(ArchColor::BG);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(ArchColor::BAD, ArchColor::BG);
      tft.drawString("ULTIMO RIAVVIO", ARCHBB_W/2, ARCHBB_H/2 - 24, 2);
      tft.setTextColor(ArchColor::AMBER, ArchColor::BG);
      tft.drawString(txt, ARCHBB_W/2, ARCHBB_H/2, 2);
      tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
      tft.drawString("(diagnostica blocco)", ARCHBB_W/2, ARCHBB_H/2 + 26, 1);
      delay(2500);
    }
  }

  touchInit();     // avvia Wire (bus I2C condiviso) e configura il CST816

  // --- IMU inizializzata PRIMA del menu (serve al CLINOMETRO) ---------------
  // In 4a l'IMU si inizializzava dopo il menu touch; in 4c la anticipiamo,
  // perche' il CLINOMETRO (test statico) deve poter leggere il sensore mentre
  // e' nel menu. I task NON partono ancora: g_latest_sample resta fermo, il
  // DIAG legge coi propri imu_read_raw() senza conflitti.
  bool imuOk = imu_init(ODR_250HZ, 8, 512);   // usa il bus GIA' attivo

  // --- FASE 5: batteria (AXP2101) sul bus I2C GIA' attivo -------------------
  // Dopo touchInit (che ha avviato Wire): battery_init NON re-inizializza il bus.
  bool battOk = battery_init();
  if (battOk) battery_power_tuning();   // FASE 24a: LED di carica spento

  // --- FASE 6: RTC PCF85063 sul bus I2C GIA' attivo ------------------------
  // Stesso bus di IMU/touch/batteria. rtc_init NON chiama Wire.begin(). Serve a
  // datare le sessioni. Impostazione dell'ora via BLE (futuro).
  bool rtcOk = rtc_init();
  // F24b.1 — l'orologio di SISTEMA segue l'RTC, o la microSD data tutto al 1980.
  // Prima di sd_init(): i file della card vanno timbrati fin dal primo.
  if (rtcOk) rtc_sync_system_clock();

  // --- FASE 5: microSD sul suo bus SPI dedicato (FSPI, pin 1/2/3/42) --------
  // Bus separato dal display: nessuna contesa. Se la card e' assente, sd_init
  // torna false e il firmware prosegue senza salvare (g_sd_ok resta false).
  bool sdOk = sd_init();

  // --- FASE 24a: le NOTE D'AVVIO nel log energetico -------------------------
  //  Tre righe scritte una volta sola, che rendono leggibile tutto il resto del
  //  file: perche' siamo ripartiti, com'e' messa la cella adesso, e quali rail
  //  del PMU sono accesi. Senza queste, /ENERGIA.CSV e' una colonna di numeri
  //  senza storia; con queste, la curva di scarica ha un inizio e una causa.
  if (sdOk) {
    sd_energia_nota(s_resetTxt);
    if (battOk) {
      char nota[224];
      snprintf(nota, sizeof(nota), "AVVIO batt=%.3fV sys=%.3fV pct=%d voff=%umV bl=%u%%",
               (double)battery_voltage(), (double)battery_voltage_sys(),
               battery_percent(), (unsigned)battery_voff_mv(),
               (unsigned)power_livello_base());
      sd_energia_nota(nota);

      char rail[224];
      battery_rail_report(rail, sizeof(rail));
      char nota2[256];
      snprintf(nota2, sizeof(nota2), "RAIL %s", rail);
      sd_energia_nota(nota2);
    }
  }

  // --- MENU D'AVVIO (OPT-IN): CALIBRA / DIAG touch / CLINOMETRO -------------
  // Subito dopo il touch e prima di avviare i task. E' l'unico momento in cui
  // bloccare e' innocuo (nessun task, nessun tiro). Default = salta dopo 3s.
  {
    int scelta = touch_cal_offer(tft, 3000);
    if (scelta == 1) {
      TouchCalResult r = touch_cal_run(tft);
      (void)r;   // i coefficienti si ricopiano a mano in config.h (voluto)
    } else if (scelta == 2) {
      touch_cal_diag(tft);
    } else if (scelta == 3) {
      // FASE 18 - CLINOMETRO. Sostituisce il vecchio DIAG ANGOLI, che serviva a
      // SCEGLIERE il montaggio fra quattro candidati: scelta ormai fatta e
      // congelata in config. La domanda aperta non e' piu' quale montaggio, ma:
      // i segni sono quelli attesi, e il giroscopio racconta la stessa storia
      // dell'accelerometro? A differenza di runAngleDiag, clino_run RITORNA:
      // si misura al banco, si esce, si va a tirare senza spegnere.
      if (imuOk) clino_run(tft);
      // Se l'IMU non c'e', il menu non offre nemmeno la voce (v. touch_cal_offer).
    } else if (scelta == 4) {
      // CONFIG (Fase 6): lettura parametri NVS + ripristina default. Ritorna
      // all'uscita, come il clinometro. Dopo un eventuale reset,
      // riapplichiamo il config cosi' il montaggio torna operativo dal valore
      // ripristinato.
      config_ui_show(tft);
      config_apply();
    }
  }

  bool bufOk = circular_buffer_init();        // buffer in PSRAM (Fase 4b)
  trigger_init();

  // --- Task FreeRTOS su Core 1 ----------------------------------------------
  // Priorita': IMU la piu' alta (non deve perdere campioni), trigger appena
  // sotto, la UI (loop) gira alla priorita' di default di Arduino.
  if (imuOk) {
    // FASE 7: salviamo gli handle per poter sospendere/riprendere i task quando
    // si entra/esce dal modo BLE.
    xTaskCreatePinnedToCore(imu_task,     "imu",     4096, nullptr, 5, &s_imuTaskHandle,     1);
    xTaskCreatePinnedToCore(trigger_task, "trigger", 4096, nullptr, 4, &s_triggerTaskHandle, 1);
  }

  // --- Schermata iniziale + arma il trigger ---------------------------------
  enterAttesa();

  if ((bool)Serial && Serial.availableForWrite() > 0) {
    Serial.printf("=== ArchBB 1.83 — Fase 5 (microSD) — IMU %s, buffer %s, mount %s ===\n",
                  imuOk ? "OK" : "ASSENTE", bufOk ? "OK" : "FALLITO",
                  mount_name(g_mount_orientation));
    Serial.printf("    BATT %s | SD %s",
                  battOk ? "OK" : "ASSENTE", sdOk ? "OK" : "ASSENTE");
    if (sdOk) Serial.printf(" (%lu MB liberi / %lu MB)",
                            (unsigned long)sd_free_mb(), (unsigned long)sd_total_mb());
    Serial.println();
    Serial.printf("    CONFIG %s (v%u)\n",
                  cfgFromNvs ? "da NVS" : "default", g_config.version);
    { char iso[24]; rtc_iso_string(iso, sizeof(iso));
      Serial.printf("    RTC %s (%s)\n", rtcOk ? "OK" : "ASSENTE", iso); }
  }
}

// ============================================================================
//  loop()  =  TASK UI (touch + scoring + display)
// ============================================================================
// ----------------------------------------------------------------------------
//  FASE 24a — dal flusso applicativo al profilo energetico, in un posto solo
// ----------------------------------------------------------------------------
//  La tentazione era spargere power_set_profilo() nei sei punti in cui s_flow
//  cambia. Sarebbero sei punti da ricordare, e il settimo — quello nuovo, fra
//  tre mesi — sarebbe stato dimenticato, lasciando lo scoring col profilo
//  dell'attesa e uno schermo che si spegne mentre l'arciere decide dove ha
//  colpito. Qui la corrispondenza e' una funzione pura, chiamata a ogni giro:
//  aggiungere un AppFlow costringe il compilatore a passare di qui.
static PowerProfilo profiloPer(AppFlow f) {
  switch (f) {
    case AppFlow::ATTESA:         return PowerProfilo::ATTESA;
    case AppFlow::BLE_MODE:       return PowerProfilo::TRASFERIMENTO;
    case AppFlow::PRETIRO:
    case AppFlow::SCORING:
    case AppFlow::RIEPILOGO:
    case AppFlow::SESSION_CHOICE:
    case AppFlow::BLE_CONFIRM:
    case AppFlow::SPEGNIMENTO:    return PowerProfilo::INTERATTIVO;
  }
  return PowerProfilo::ATTESA;
}

void loop() {
  TouchData td = touchRead();

  // --- FASE 24a: politica energetica, in testa al giro ----------------------
  //  1) un dito appoggiato E' attivita', sempre, in qualunque schermata;
  //  2) il profilo segue il flusso;
  //  3) power_tick() fa avanzare la macchina a stati e la rampa.
  //  Nessuno di questi tre blocca: il costo per giro e' qualche confronto.
  if (td.valid && td.fingers > 0) power_attivita();
  power_set_profilo(profiloPer(s_flow));
  power_tick();

  // --- F24b: PRESSIONE LUNGA = SPEGNIMENTO IN ARRIVO ------------------------
  //  Il PMU stacca da solo dopo 10 s, e non glielo togliamo: uno spegnimento
  //  hardware deve restare hardware. Ma la IRQ di pressione lunga scatta molto
  //  prima, e in quella finestra si fa l'unica cosa che il PMU non puo' fare —
  //  lasciare scritto perche'.
  //
  //  Il 12/09 uno spegnimento accidentale ha lasciato SESS_1215 senza ended_ms,
  //  e per capire cosa fosse successo ho dovuto ricostruire la timeline da due
  //  ancoraggi indipendenti. Due righe qui valgono quella mezz'ora.
  //
  //  Vale in QUALUNQUE schermata, per questo sta prima dello switch: il tasto si
  //  puo' sfiorare anche a meta' scoring.
  if (s_flow != AppFlow::SPEGNIMENTO && power_key_long_pressed()) {
    DBG("PWRKEY: pressione lunga -> spegnimento imminente");
    s_sessEraAperta = g_session_open;
    if (g_sd_ok) {
      char nota[96];
      snprintf(nota, sizeof(nota), "SPEGNIMENTO DA TASTO batt=%.3fV sessione=%s",
               (double)battery_voltage(), g_session_open ? "aperta" : "chiusa");
      sd_energia_nota(nota);
      if (g_session_open) sd_session_stop();   // ended_ms/ended_datetime veri
    }
    power_risveglia();
    s_spegnMs = millis();
    displaySpegnimento(0, false);
    s_flow = AppFlow::SPEGNIMENTO;
  }

  // --- F24b: PREAVVISO DI BATTERIA, sulle soglie in volt --------------------
  //  Agganciato al campionamento del log (una lettura ogni 30 s) e non al giro
  //  di loop: la tensione non cambia in 15 ms, e leggere il PMU a raffica
  //  occuperebbe l'I2C condiviso con IMU e touch per niente.
  //
  //  Una volta sola per soglia, mai un ritorno indietro: s_avvisoBatt sale e non
  //  scende. Senza questo, una tensione che oscilla attorno a 3,70 sotto carico
  //  farebbe comparire l'avviso ogni mezzo minuto — e un avviso che si ripete e'
  //  un avviso che si impara a ignorare.
  //
  //  Non interrompe niente: si mostra solo da ATTESA. Comparire in mezzo allo
  //  scoring farebbe perdere il filo su un tiro gia' scoccato, e la batteria
  //  puo' aspettare quindici secondi.
  {
    static uint8_t s_avvisoBatt = 0;    // 0 nessuno, 1 dato, 2 critico dato
    static uint32_t ultimoCheck = 0;
    if (millis() - ultimoCheck >= ENERGIA_LOG_MS) {
      ultimoCheck = millis();
      const float v = battery_voltage();
      if (!isnan(v) && !battery_charging() && s_flow == AppFlow::ATTESA) {
        if (v <= ENERGIA_V_CRITICO && s_avvisoBatt < 2) {
          s_avvisoBatt = 2;
          power_risveglia();
          displayAvvisoBatteria("~20 minuti", true);
          delay(2500);
          s_lastTempMs = 0;
        } else if (v <= ENERGIA_V_AVVISO && s_avvisoBatt < 1) {
          s_avvisoBatt = 1;
          power_risveglia();
          displayAvvisoBatteria("~1 ora", false);
          delay(2000);
          s_lastTempMs = 0;
        }
      }
    }
  }

  // --- FASE 24a: campionamento del log energetico ---------------------------
  //  Il cronometro sta QUI e non dentro sd_storage: la cadenza e' una decisione
  //  di politica, e si legge dove le decisioni si prendono. Il periodo viene da
  //  config.h (ENERGIA_LOG_MS), non da un 30000 scritto a mano in mezzo al loop.
  {
    static uint32_t ultimoLog = 0;
    if (g_sd_ok && (millis() - ultimoLog) >= ENERGIA_LOG_MS) {
      ultimoLog = millis();
      sd_energia_log(battery_voltage(), battery_voltage_sys(), battery_percent(),
                     battery_charging(), power_backlight_pct(), power_cpu_mhz(),
                     (uint8_t)power_stato(),
                     g_imu_ok ? imu_read_temperature() : NAN);
    }
  }

  switch (s_flow) {

    // ---------------------------------------------------------------------
    case AppFlow::ATTESA: {
      // FASE 20: riga di stato dal vivo. Chiamata a ogni giro; la funzione
      // ridisegna solo quando il testo cambia davvero.
      displayTempoRiga((uint8_t)tempo_stato(), tempo_ms_nello_stato(), false);

      // 0) FASE 7: tasto BOOT (ingranaggio) = entra nel modo BLE. Solo da ATTESA
      //    (non durante scoring/riepilogo). enterBleMode sospende i task IMU.
      // F24b — la pressione breve non entra piu' in BLE: CHIEDE.
      if (bleButtonPressed()) {
        s_bleConfirmMs = millis();
        displayBleConfirm(g_session_open, ARCHBB_BLE_CONFIRM_S);
        s_bleConfirmLast = ARCHBB_BLE_CONFIRM_S;
        s_stable = false; s_relCnt = 0;
        s_flow = AppFlow::BLE_CONFIRM;
        break;
      }

      // 1) Tiro REALE: il trigger IMU ha sparato? E' l'UNICO modo di tirare
      //    quando l'IMU e' viva (lo scoring e' chiuso: la freccia comanda).
      if (g_trigger_fired_sem &&
          xSemaphoreTake(g_trigger_fired_sem, 0) == pdTRUE) {
        // FASE 24a — LO SCHERMO SPENTO NON HA DISARMATO NIENTE.
        //  La freccia e' partita mentre il pannello dormiva: imu_task e
        //  trigger_task non si sono mai fermati, il buffer circolare era pieno,
        //  il trigger ha sparato. Adesso — e solo adesso — si riaccende, perche'
        //  la prossima schermata l'arciere DEVE vederla. Questo e' il punto in
        //  cui il risparmio energetico dimostra di non aver toccato la misura.
        power_risveglia();
        startScoringForShot(g_trigger_az_peak);
        break;
      }

      // 2) TOCCO in ATTESA = gestione SESSIONE (Fase 5, ridisegnato).
      //    Non piu' tap-lungo: qualunque tocco pulito apre/chiude la sessione.
      //    Niente cronometro fragile da distinguere dal tap-breve -> il bug
      //    "parte sempre come breve" sparisce alla radice (non c'e' piu' un
      //    breve da cui distinguere). tapReleased() da' il fronte di rilascio
      //    gia' debounced e col cooldown: un tocco = un toggle, pulito.
      //
      //    ECCEZIONE: se l'IMU e' ASSENTE, il tocco resta il tiro manuale
      //    (unica ancora di salvezza a banco). In campo l'IMU c'e' sempre,
      //    quindi questo ramo non si vede mai.
      if (tapReleased(td)) {
        // FASE 24a — IL TOCCO CHE HA RIACCESO LO SCHERMO NON E' UN COMANDO.
        //  Senza questo controllo, ogni risveglio in ATTESA aprirebbe o
        //  chiuderebbe una sessione a caso (o armerebbe il bersaglio, se il
        //  dito e' caduto sulla card): il difetto peggiore possibile in un
        //  registratore di dati, perche' agisce da solo e non lo si vede.
        //  Il tocco si e' gia' preso il suo effetto — accendere — e finisce li'.
        if (power_consume_risveglio()) {
          // La GRAM del pannello ha conservato l'immagine di prima del sonno, ma
          // ora e batteria sono vecchie di minuti: forziamo il rinfresco al giro
          // successivo azzerando il cronometro invece di ridisegnare qui. Cosi'
          // il ridisegno resta in UN posto solo (il ramo 3), e non c'e' modo che
          // le due copie divergano.
          s_lastTempMs = 0;
          break;
        }

        // FASE 22 — la CARD ha la precedenza su tutto il resto.
        //  Distinzione SPAZIALE, non temporale: qui non si misura la durata di
        //  niente. Il tocco cade sulla card oppure no, e la risposta e' la
        //  stessa a ogni tentativo. Un doppio tap avrebbe richiesto di attendere
        //  ~300 ms prima di sapere se un tocco era singolo, ritardando ogni
        //  apertura di sessione e riaprendo la classe di bug che il tap-lungo
        //  era gia' costata.
        //
        //  Vale anche senza IMU: a banco si arma il bersaglio e poi si tocca
        //  fuori dalla card per il tiro manuale.
        if (displayAttesaHitCard(s_pressX, s_pressY)) {
          scoringStartPretiro(s_preDist, s_preElev);
          s_flow = AppFlow::PRETIRO;
          break;
        }
        if (!g_imu_ok) {
          // IMU assente: il tocco e' l'unico trigger disponibile.
          g_shot_id++;
          startScoringForShot(0.0f);
          break;
        }
        // IMU viva: il tocco gestisce la sessione.
        //  F24b — COOLDOWN. Il 12/09 sono nate due sessioni vuote: SESS_1134
        //  chiusa 3 secondi dopo l'apertura e SESS_1135 dopo 18. Tre tocchi
        //  fuori dalla card nel giro di venti secondi, mentre si chiudeva quella
        //  vera. Due tap ravvicinati non possono voler dire "apri e chiudi":
        //  e' un gesto che nessuno fa apposta.
        //  Non e' un debounce (quello e' sul tocco): e' una regola sul
        //  SIGNIFICATO, e sta qui dove il significato viene deciso.
        static uint32_t s_ultimoToggleMs = 0;
        if (millis() - s_ultimoToggleMs < ARCHBB_SESSION_COOLDOWN_MS) {
          DBG("Sessione: toggle ignorato (cooldown)");
          break;
        }
        if (g_sd_ok) {
          s_ultimoToggleMs = millis();
          if (g_session_open) {
            // Sessione aperta -> chiudi (comportamento invariato).
            sd_session_stop();
            refreshStatus();
            displayAttesaTiroIMU_v5((uint16_t)(g_shot_id + 1),
                                    imu_read_temperature(), s_status,
                                    s_preDist, s_preElev, s_preConf);
            s_lastTempMs = millis();
          } else if (sd_has_previous_session()) {
            // Nessuna sessione aperta ma ce n'e' una su card -> CHIEDI:
            // riprendere l'ultima (accoda) o iniziarne una nuova?
            char lastName[24] = ""; uint16_t lastShots = 0;
            sd_last_session_info(lastName, sizeof(lastName), lastShots);
            displaySessionChoice(lastName, lastShots);
            s_stable = false; s_relCnt = 0; s_lastActionMs = millis();
            s_flow = AppFlow::SESSION_CHOICE;
          } else {
            // Card vuota (nessuna sessione precedente) -> apri direttamente.
            sd_session_start();
            refreshStatus();
            displayAttesaTiroIMU_v5((uint16_t)(g_shot_id + 1),
                                    imu_read_temperature(), s_status,
                                    s_preDist, s_preElev, s_preConf);
            s_lastTempMs = millis();
          }
        } else {
          // Nessuna SD: il tocco non ha effetto utile. Lampeggia un avviso
          // breve invece di fingere che sia successo qualcosa.
          displaySessionNoSd();
          s_lastTempMs = 0;   // forza il refresh alla prossima iterazione
        }
        break;
      }

      // 3) Rinfresca temperatura + barra di stato ogni 3s (feedback "IMU viva").
      //    FASE 24a: non a pannello spento. Ridisegnare una schermata che
      //    nessuno vede costa SPI, CPU e corrente per niente — cioe' esattamente
      //    cio' che stiamo cercando di non fare. Al risveglio si ridisegna
      //    comunque tutto, e con dati piu' freschi di questi.
      if (power_schermo_acceso() && millis() - s_lastTempMs > 3000) {
        s_lastTempMs = millis();
        refreshStatus();
        displayAttesaTiroIMU_v5((uint16_t)(g_shot_id + 1),
                                g_imu_ok ? imu_read_temperature() : NAN, s_status,
                                s_preDist, s_preElev, s_preConf);
      }
      break;
    }

    // ---------------------------------------------------------------------
    //  FASE 22 — PRE-TIRO: le due schermate di slider prima dello scocco.
    //  Il trigger NON viene armato qui: enterAttesa() lo arma, e ci si torna
    //  solo all'uscita. Mentre si sposta uno slider l'arco e' in mano e viene
    //  mosso, ed e' esattamente la situazione che produceva le 26 catture di
    //  maneggio su 66 del 28/07.
    case AppFlow::PRETIRO: {
      ScoringState st = scoringUpdate(td);
      if (st == ScoringState::DONE) {
        if (scoringPretiroConfermato()) {
          scoringGetPretiro(s_preDist, s_preElev);
          s_preConf = true;          // confermato per il PROSSIMO tiro
        }
        // ANNULLA (o timeout): i valori armati restano quelli di prima, e anche
        // s_preConf resta com'era. Annullare non e' confermare.
        enterAttesa();
      }
      break;
    }

    // ---------------------------------------------------------------------
    case AppFlow::SCORING: {
      ScoringState st = scoringUpdate(td);
      if (st == ScoringState::DONE) {
        s_lastResult = scoringGetResult();

        // --- F13: le tre uscite dello scoring -----------------------------
        //  SCARTATO  (anche per timeout): non era un tiro -> NULLA su microSD,
        //            niente riepilogo, si torna subito in attesa. E' la difesa
        //            contro il maneggio: sui dati del 28/07 erano 26 catture su
        //            66, e finivano tutte sulla card.
        //  SALTATO   era un tiro ma non lo valuto -> riga CSV + burst SENZA
        //            punteggio, e via diretti senza riepilogo (si salta per
        //            fare in fretta: chiedere una conferma vanificherebbe).
        //  VALUTATO  percorso normale -> riepilogo, e la scrittura avviene al
        //            tap OK come prima.
        if (s_lastResult.esito == EsitoScoring::SCARTATO) {
          displayScartato();
          enterAttesa();
          break;
        }
        if (s_lastResult.esito == EsitoScoring::SALTATO) {
          computeShotAngles(s_lastResult);   // le metriche si salvano comunque
          if (g_sd_ok) {
            const ImuSample* bp = (s_burstN > 0) ? s_burst : nullptr;
            if (sd_save_shot(s_lastResult, bp, s_burstN, s_burstTrig, ODR_250HZ,
                             s_burstPreOk)) {
              displaySavedFlash(sd_session_shot_count());
            }
          }
          enterAttesa();
          break;
        }

        // --- FASE 4c: leggi il burst congelato e calcola gli angoli ---------
        // QUI, prima del riepilogo e prima di enterAttesa (che resetta il
        // buffer). Se il buffer non e' READY (tiro manuale senza IMU) le
        // metriche restano \"non valide\" -> il riepilogo mostra n/d.
        computeShotAngles(s_lastResult);

        s_stable = false; s_relCnt = 0; s_lastActionMs = millis();
        displayScoreRiepilogo(s_lastResult,
                              zonaLabel(s_lastResult.colpito, s_lastResult.zona));
        s_flow = AppFlow::RIEPILOGO;
      }
      break;
    }

    // ---------------------------------------------------------------------
    case AppFlow::SESSION_CHOICE: {
      static uint16_t cx = 0, cy = 0;
      if (td.valid && td.fingers > 0) { cx = td.x; cy = td.y; }
      if (tapReleased(td)) {
        uint8_t hit = hitSessionChoice(cx, cy);
        if (hit == 1) {                     // RIPRENDI -> accoda all'ultima
          sd_session_resume_last();
          refreshStatus();
          displayAttesaTiroIMU_v5((uint16_t)(g_shot_id + 1),
                                  g_imu_ok ? imu_read_temperature() : NAN, s_status,
                                  s_preDist, s_preElev, s_preConf);
          s_lastTempMs = millis();
          s_flow = AppFlow::ATTESA;
        } else if (hit == 2) {              // NUOVA -> sessione vergine
          sd_session_start();
          refreshStatus();
          displayAttesaTiroIMU_v5((uint16_t)(g_shot_id + 1),
                                  g_imu_ok ? imu_read_temperature() : NAN, s_status,
                                  s_preDist, s_preElev, s_preConf);
          s_lastTempMs = millis();
          s_flow = AppFlow::ATTESA;
        }
        // tocco fuori dai pulsanti: resta nella schermata di scelta.
      }
      break;
    }

    // ---------------------------------------------------------------------
    case AppFlow::RIEPILOGO: {
      static uint16_t rx = 0, ry = 0;
      if (td.valid && td.fingers > 0) { rx = td.x; ry = td.y; }
      if (tapReleased(td)) {
        uint8_t hit = hitRiepilogo(rx, ry);
        if (hit == 2) {                  // OK -> cementa e torna in attesa
          // --- FASE 5: SCRITTURA SU microSD --------------------------------
          //  Qui, PRIMA di enterAttesa() (che resetta il buffer). Il burst e'
          //  gia' in s_burst (letto da computeShotAngles al DONE) e i suoi
          //  metadati in s_burstN/s_burstTrig. Passiamo il puntatore solo se il
          //  burst e' valido; altrimenti nullptr -> si salva il solo CSV.
          //  sd_save_shot auto-apre una sessione se non ce n'e' una (l'arciere
          //  che tira senza aver premuto "start" non perde il dato).
          if (g_sd_ok) {
            const ImuSample* bp = (s_burstN > 0) ? s_burst : nullptr;
            bool saved = sd_save_shot(s_lastResult, bp, s_burstN, s_burstTrig, ODR_250HZ,
                                      s_burstPreOk);
            if (saved) {
              // Feedback di scrittura riuscita: doppio flash verde col numero
              // del tiro appena salvato (= conteggio attuale della sessione).
              displaySavedFlash(sd_session_shot_count());
            }
          }
          // --- FASE 22: il bersaglio segue le correzioni --------------------
          //  Se durante lo scoring l'arciere e' tornato indietro fino a
          //  DISTANZA e ha corretto (accorgersi DOPO il tiro che era rimasta
          //  armata la piazzola precedente e' il caso che rende necessario quel
          //  percorso all'indietro), la correzione vale anche per i tiri
          //  successivi: e' lo stesso bersaglio. Ripescare il valore dal
          //  risultato invece di tenerne una copia a parte garantisce che
          //  quello salvato e quello armato non possano divergere.
          if (s_lastResult.esito != EsitoScoring::SCARTATO) {
            s_preDist = s_lastResult.distanza_m;
            if (s_lastResult.elev_deg != ELEV_NOT_SET) s_preElev = s_lastResult.elev_deg;
          }
          enterAttesa();                 // riarma il trigger per il prossimo tiro
        } else if (hit == 1) {           // CORREGGI -> riapre lo scoring corrente
          scoringResume();
          s_flow = AppFlow::SCORING;
        }
        // tocco fuori dai pulsanti: ignorato.
      }
      break;
    }

    // ---------------------------------------------------------------------
    // ------------------------------------------------------------------
    //  F24b — BLE_CONFIRM: la domanda, col trigger ancora armato
    // ------------------------------------------------------------------
    case AppFlow::BLE_CONFIRM: {
      // 1) UNA FRECCIA HA LA PRECEDENZA SULLA DOMANDA.
      //    Se l'arciere scocca mentre la modale e' a schermo, il tiro va
      //    registrato e la domanda decade. Sospendere la misura per aspettare
      //    una risposta sarebbe esattamente il difetto che questa schermata
      //    esiste per impedire.
      if (g_trigger_fired_sem &&
          xSemaphoreTake(g_trigger_fired_sem, 0) == pdTRUE) {
        startScoringForShot(g_trigger_az_peak);
        break;
      }

      const uint32_t trascorsi = millis() - s_bleConfirmMs;
      const uint32_t totMs     = (uint32_t)ARCHBB_BLE_CONFIRM_S * 1000u;

      // 2) Timeout -> NO. Il verso della sicurezza: lasciando fare, si tira.
      if (trascorsi >= totMs) {
        refreshStatus();
        displayAttesaTiroIMU_v5((uint16_t)(g_shot_id + 1),
                                g_imu_ok ? imu_read_temperature() : NAN, s_status,
                                s_preDist, s_preElev, s_preConf);
        s_lastTempMs = millis();
        s_flow = AppFlow::ATTESA;
        break;
      }

      // 3) Conto alla rovescia: si ridisegna solo la riga che cambia.
      const uint8_t rimasti = (uint8_t)((totMs - trascorsi + 999u) / 1000u);
      if (rimasti != s_bleConfirmLast) {
        s_bleConfirmLast = rimasti;
        displayBleConfirmTick(rimasti);
      }

      static uint16_t cx = 0, cy = 0;
      if (td.valid && td.fingers > 0) { cx = td.x; cy = td.y; }
      if (tapReleased(td)) {
        const uint8_t hit = hitBleConfirm(cx, cy);
        if (hit == 2) {
          enterBleMode();
          break;
        }
        // hit == 1 (RESTA A TIRARE) e hit == 0 (fuori dai pulsanti) fanno la
        // STESSA cosa. Non e' pigrizia: un tocco a caso su questa schermata,
        // come quello che l'ha fatta comparire, non deve poter entrare in BLE.
        refreshStatus();
        displayAttesaTiroIMU_v5((uint16_t)(g_shot_id + 1),
                                g_imu_ok ? imu_read_temperature() : NAN, s_status,
                                s_preDist, s_preElev, s_preConf);
        s_lastTempMs = millis();
        s_flow = AppFlow::ATTESA;
      }
      break;
    }

    // ------------------------------------------------------------------
    //  F24b.2 — SPEGNIMENTO: si tiene lo schermo e si aspetta il PMU
    // ------------------------------------------------------------------
    //  PERCHE' SERVE UNO STATO E NON BASTAVA DISEGNARE.
    //   Nella 24b la schermata veniva disegnata e poi il flusso tornava in
    //   ATTESA, dove il rinfresco periodico dei 3 secondi la cancellava e
    //   rimetteva la schermata di attesa. Sembrava che lo spegnimento fosse
    //   stato annullato — e a quel punto chiunque molla il tasto, quindi il PMU
    //   non arrivava mai ai suoi dieci secondi. In modo BLE non succedeva,
    //   perche' li' lo schermo si ridisegna solo al cambio di connessione: e'
    //   il motivo per cui da BLE lo spegnimento "funzionava".
    //   Il difetto non era nel PMU ne' nella soglia: era il nostro rinfresco.
    case AppFlow::SPEGNIMENTO: {
      const uint32_t t = millis() - s_spegnMs;

      if (t < ARCHBB_SPEGN_MS) {
        // Barra di avanzamento come guida al dito. Se il PMU stacca, questa
        // riga semplicemente non viene mai piu' eseguita.
        displaySpegnimento((uint8_t)(t * 100u / ARCHBB_SPEGN_MS), false);
        break;
      }

      // Siamo ancora vivi: il tasto e' stato rilasciato prima della soglia.
      //  La sessione era gia' stata chiusa — a ragione, perche' al momento
      //  della IRQ non potevamo sapere se ci sarebbe stata un'altra occasione.
      //  Adesso che sappiamo, la si riprende: stessa cartella, stessa
      //  numerazione, ended_ms che verra' riscritto alla chiusura vera. Un
      //  annullamento non deve lasciare effetti collaterali sui dati.
      if (s_sessEraAperta && g_sd_ok && !g_session_open) {
        sd_session_resume_last();
        DBG("Spegnimento annullato: sessione ripresa");
      }
      displaySpegnimento(0, true);
      delay(1200);
      refreshStatus();
      displayAttesaTiroIMU_v5((uint16_t)(g_shot_id + 1),
                              g_imu_ok ? imu_read_temperature() : NAN, s_status,
                              s_preDist, s_preElev, s_preConf);
      s_lastTempMs = millis();
      s_flow = AppFlow::ATTESA;
      break;
    }

    case AppFlow::BLE_MODE: {
      // 1) Tasto BOOT di nuovo = ESCI dal modo BLE (chiude tutto, applica config,
      //    riprende i task, torna in ATTESA).
      if (bleButtonPressed()) {
        exitBleMode();
        break;
      }

      // 2) Ridisegna la schermata SOLO quando cambia lo stato di connessione
      //    (app agganciata / staccata): niente refresh nel loop stretto. Lo
      //    STATUS periodico (batteria/SD/ora) lo notifica il ble_task all'app;
      //    a schermo aggiorniamo il colpo d'occhio solo sul cambio connessione.
      bool nowConnected = g_ble_connected;
      if (nowConnected != s_bleLastConnected) {
        s_bleLastConnected = nowConnected;
        refreshStatus();
        displayBleMode(nowConnected, s_bleDevName, s_status);
      }
      break;
    }
  }

  // Cede il Core 1 agli altri task. 15ms: UI fluida (~66 Hz touch), IMU/trigger
  // hanno CPU in abbondanza.
  delay(15);
}

#endif // firmware di produzione (escluso nei build di test)
