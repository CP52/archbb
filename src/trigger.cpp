// ============================================================================
//  ArchBB 1.83 — trigger.cpp · FASE 4a (rilevazione tiro reale) · v1
//  Cesare Pagura · Padova/Noale IT · 13 luglio 2026
// ----------------------------------------------------------------------------
//  Macchina a stati portata dalla 1.69 v2.12.6. In Fase 4b il FIRED congela il
//  circular buffer (circular_buffer_freeze). Ancora niente semaforo verso un
//  task BLE (non c'e' BLE): il semaforo g_trigger_fired_sem va al task UI.
//    - "armato" non e' legato a g_recording_enabled (BLE) ma a un flag
//      controllato dal main: armato SOLO in ATTESA (vedi trigger.h).
//
//  Il resto della logica e' invariato perche' e' quella COLLAUDATA: gate sul
//  seq del campione (anti doppio-conteggio, perche' il task gira piu' veloce di
//  quanto arrivino i campioni), confirm_n consecutivi, rearm, trigger manuale
//  con fire immediato.
// ============================================================================
#include "trigger.h"
#include "tempo.h"      // FASE 20: macchina dei tempi di alzata/mira
#include "config_store.h"   // g_config: trigger_init applica il config in coda
#include "imu.h"
#include "circular_buffer.h"
#include "shot_angles.h"    // SHOT_ANGLE_GRAVITY (costante unica, mai duplicata)

// ----------------------------------------------------------------------------
//  Variabili globali
// ----------------------------------------------------------------------------
SemaphoreHandle_t     g_trigger_fired_sem = nullptr;
volatile uint16_t     g_shot_id           = 0;
volatile float        g_trigger_az_peak   = 0.0f;
volatile TriggerPhase g_trigger_phase     = TRIG_IDLE;

// Config locale (dai default di config.h; in futuro modificabile via BLE).
static TriggerMode s_mode      = TRIG_DEFAULT_MODE;
static float       s_threshold = TRIG_DEFAULT_THRESHOLD;
static uint8_t     s_confirm_n = TRIG_DEFAULT_CONFIRM_N;
static uint16_t    s_rearm_ms  = TRIG_DEFAULT_REARM_MS;
static float       s_odr_hz    = 224.0f;

// Fase 4b: finestra del burst in campioni (calcolata dall'ODR reale in init).
static uint16_t    s_pre_samples  = 0;
static uint16_t    s_post_samples = 0;

// Stato interno.
static uint8_t       s_confirm_count  = 0;
static float         s_az_prev        = 0.0f;
static volatile bool s_armed          = false;    // controllato dal main
static volatile bool s_manual_trigger = false;

// Istante del fire (millis), scritto da trigger_feed_sample e letto dal
// trigger_task per il conteggio del REARM. Statica di file perche' ora il fire
// avviene nella valutazione (imu_task) mentre l'attesa del rearm resta nel task.
static volatile uint32_t s_fired_ms = 0;

// Stima della GRAVITA' (media esponenziale lenta del vettore accelerazione).
// Serve al gate di postura: dice come e' orientata la scheda a prescindere dal
// movimento istantaneo. Aggiornata a ogni campione da trigger_feed_sample.
static float s_grav_x = 0, s_grav_y = 0, s_grav_z = 0;
static bool  s_grav_primed = false;

// ----------------------------------------------------------------------------
void trigger_init() {
  tempo_init();
  g_trigger_fired_sem = xSemaphoreCreateBinary();
  if (!g_trigger_fired_sem) { DBG("ERRORE: trigger semaforo fallito"); }

  s_odr_hz = (float)odrToHz(ODR_250HZ);
  s_pre_samples  = calcPreSamples(ODR_250HZ);
  s_post_samples = calcPostSamples(ODR_250HZ);
  g_shot_id       = 0;
  g_trigger_phase = TRIG_IDLE;
  s_confirm_count = 0;
  s_az_prev       = 0.0f;
  s_armed         = false;
  s_manual_trigger = false;

  // IMPORTANTE (ordine di init): questa funzione azzera pre/post campioni ai
  // default di compilazione. Se config_apply() e' gia' stata chiamata (al boot
  // avviene PRIMA, per il montaggio), i suoi valori verrebbero persi. Per
  // renderlo a prova di ordine, applichiamo QUI il config corrente: da questo
  // momento il trigger e' sempre allineato a g_config, chiunque abbia chiamato
  // cosa e in che ordine.
  trigger_configure(g_config.trig_threshold_ms2,
                    g_config.trig_confirm_n,
                    g_config.trig_rearm_ms,
                    g_config.window_pre_ms,
                    g_config.window_post_ms);

  DBG("Trigger init OK — mode=%d soglia=%.1f confirm_n=%d rearm=%dms",
      (int)s_mode, s_threshold, s_confirm_n, s_rearm_ms);
}

void trigger_set_armed(bool armed) {
  s_armed = armed;
}

void trigger_manual() {
  s_manual_trigger = true;
}

// ----------------------------------------------------------------------------
//  trigger_task — Core 1.
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
//  trigger_configure — applica i parametri del config al trigger (handoff §2.3).
//  Vedi trigger.h per il bug che chiude. Con clamp difensivi: un config
//  malformato non deve rendere il trigger inutilizzabile.
// ----------------------------------------------------------------------------
void trigger_configure(float threshold_ms2, uint8_t confirm_n, uint16_t rearm_ms,
                       uint16_t window_pre_ms, uint16_t window_post_ms) {
  // Soglia: sotto 0.5 m/s^2 sarebbe rumore puro; sopra 100 non scatterebbe mai.
  if (threshold_ms2 < 0.5f)   threshold_ms2 = 0.5f;
  if (threshold_ms2 > 100.0f) threshold_ms2 = 100.0f;
  s_threshold = threshold_ms2;

  if (confirm_n < 1)  confirm_n = 1;
  if (confirm_n > 10) confirm_n = 10;
  s_confirm_n = confirm_n;

  if (rearm_ms < 200) rearm_ms = 200;
  s_rearm_ms = rearm_ms;

  // Finestre: ms -> campioni all'ODR reale, con clamp alla capienza del buffer.
  // Se pre+post sfora, si riduce il POST (il PRE contiene la fase di mira, piu'
  // preziosa per le metriche).
  uint32_t pre  = (uint32_t)((float)window_pre_ms  / 1000.0f * s_odr_hz);
  uint32_t post = (uint32_t)((float)window_post_ms / 1000.0f * s_odr_hz);
  if (pre > CIRCULAR_BUFFER_SIZE - 10) pre = CIRCULAR_BUFFER_SIZE - 10;
  if (pre + post > CIRCULAR_BUFFER_SIZE) post = CIRCULAR_BUFFER_SIZE - pre;
  s_pre_samples  = (uint16_t)pre;
  s_post_samples = (uint16_t)post;

  DBG("Trigger CONFIGURATO: soglia=%.1f confirm=%u rearm=%u pre=%u post=%u camp",
      s_threshold, s_confirm_n, s_rearm_ms, s_pre_samples, s_post_samples);
}

// Istante del campione in valutazione. Serve a doFire(), che non riceve il
// campione ma deve datare lo scocco sulla stessa base di tempo del burst.
// Dichiarata QUI e non accanto a feed_sample: doFire viene prima nel file, e
// un uso prima della dichiarazione in C++ non compila.
static uint32_t s_ultimo_scocco_us = 0;

// ----------------------------------------------------------------------------
//  doFire — azione di scocco COMPLETA, condivisa da automatico e manuale.
// ----------------------------------------------------------------------------
//  Va fatta tutta qui, nell'istante del campione: incrementa shot_id, congela
//  la finestra del buffer (cosi' cattura il PRE giusto) e sveglia il main.
//  ATTENZIONE: prima del 24/07 il fire viveva nel case TRIG_FIRED del task;
//  spostando la valutazione in feed_sample il percorso manuale era rimasto
//  scollegato (impostava TRIG_FIRED ma nessuno faceva piu' freeze/semaforo).
//  Ora c'e' UNA sola funzione di fire: impossibile che i due percorsi divergano.
static void doFire() {
  // FASE 20: i tempi si congelano PRIMA di qualunque altra cosa. doFire e' gia'
  // l'unico punto di scocco condiviso da automatico e manuale (vedi sopra):
  // agganciarsi qui significa che i due percorsi non possono divergere nemmeno
  // sui tempi. s_ultimo_scocco_us e' l'istante del campione che ha fatto
  // scattare il trigger, non millis() del task: la base dei tempi resta quella
  // dell'IMU.
  tempo_on_fire(s_ultimo_scocco_us);
  g_shot_id++;
  // Congela PRIMA di postare il semaforo: quando la UI reagisce, il buffer sta
  // gia' raccogliendo i campioni POST.
  circular_buffer_freeze(s_pre_samples, s_post_samples, g_shot_id);
  xSemaphoreGive(g_trigger_fired_sem);
  s_fired_ms        = millis();
  s_confirm_count   = 0;
  g_trigger_phase   = TRIG_REARM;
}

// ----------------------------------------------------------------------------
//  trigger_feed_sample — valuta UN campione (chiamata da imu_task, per OGNI
//  campione: e' il punto in cui NON se ne perde nessuno). Vedi trigger.h per il
//  perche' (bug scocchi veri non rilevati del 24/07).
// ----------------------------------------------------------------------------
void trigger_feed_sample(const ImuSample& s) {
  s_ultimo_scocco_us = s.timestamp_us;
  // FASE 20. Alimentata PRIMA del return per trigger disarmato: la trazione
  // avviene mentre il trigger e' armato, ma il ritorno alla quiete dopo uno
  // scoring va visto comunque, altrimenti la macchina resterebbe congelata
  // nello stato in cui l'ha lasciata il tiro precedente.
  tempo_feed_sample(s);
  // --- STIMA DELLA GRAVITA' (media esponenziale lenta) ----------------------
  //  Aggiornata SEMPRE, anche a trigger disarmato: cosi' quando l'arciere alza
  //  l'arco la stima e' gia' assestata. Tau ~1s: lo scocco (~50ms) la sposta in
  //  modo trascurabile, mentre segue i cambi di postura fra un tiro e l'altro.
  {
    const float a = 1.0f / (TRIG_GRAVITY_TAU_S * s_odr_hz);
    if (!s_grav_primed) {
      s_grav_x = s.ax; s_grav_y = s.ay; s_grav_z = s.az; s_grav_primed = true;
    } else {
      s_grav_x += a * (s.ax - s_grav_x);
      s_grav_y += a * (s.ay - s_grav_y);
      s_grav_z += a * (s.az - s_grav_z);
    }
  }

  // Valutiamo SOLO nelle fasi in cui ha senso cercare uno scocco. Le fasi
  // FIRED/REARM sono gestite dal trigger_task (transizioni temporali).
  if (!s_armed) return;
  if (g_trigger_phase != TRIG_ARMED && g_trigger_phase != TRIG_CONFIRMING) return;

  // Trigger manuale: fire immediato, bypassa soglia e confirm_n.
  if (s_manual_trigger) {
    s_manual_trigger  = false;
    g_trigger_az_peak = s.az;
    doFire();
    return;
  }

  // Condizione di scocco sul campione corrente.
  bool condition = false;
  float misura = 0.0f;      // la grandezza confrontata con la soglia (per il picco)

  if (s_mode == TRIG_ACCEL_DEV) {
    // MODO PREDEFINITO (dal 25/07). | |a| - g |: modulo del vettore
    // accelerazione meno la gravita'. A riposo vale ~0 QUALUNQUE sia
    // l'inclinazione della scheda -> immune ai falsi da inclinazione, che con
    // |az| facevano scattare il trigger quando l'arco veniva abbassato.
    // E' anche indipendente dall'ASSE su cui arriva l'energia: sulla 1.83 lo
    // scocco si manifesta prevalentemente su ax, non su az come sulla 1.69.
    float mag = sqrtf(s.ax*s.ax + s.ay*s.ay + s.az*s.az);
    misura    = fabsf(mag - SHOT_ANGLE_GRAVITY);
    condition = (misura > s_threshold);
  } else if (s_mode == TRIG_AZ_THRESHOLD) {
    // STORICA: |az| grezzo. Contiene la gravita' -> soggetta a falsi quando la
    // scheda si inclina. Mantenuta solo per confronto/compatibilita'.
    misura    = fabsf(s.az);
    condition = (misura > s_threshold);
  } else {  // TRIG_JERK — derivata su campioni ora davvero adiacenti
    float dt  = 1.0f / s_odr_hz;
    misura    = fabsf((fabsf(s.az) - fabsf(s_az_prev)) / dt);
    condition = (misura > s_threshold);
  }

  if (condition) {
    // --- GATE DI POSTURA: l'arco dev'essere in posizione di tiro -----------
    //  Movimento c'e', ma se la scheda e' molto inclinata NON e' uno scocco:
    //  e' maneggio (arco appoggiato, spostato, urtato). Misurato su 19 eventi:
    //  tiri veri entro 18.5 gradi dalla verticale, falsi da 25.5 a 91.3.
    //  Il confronto usa la gravita' STIMATA, quindi e' indipendente dal
    //  montaggio (mount_apply normalizza sempre ax = verticale).
    float gm = sqrtf(s_grav_x*s_grav_x + s_grav_y*s_grav_y + s_grav_z*s_grav_z);
    float cosTilt = (gm > 0.1f) ? (s_grav_x / gm) : 0.0f;
    if (cosTilt < TRIG_POSTURE_GATE) {
      // Fuori postura: azzera e resta armato (non e' un tiro).
      s_confirm_count   = 0;
      g_trigger_az_peak = 0.0f;
      g_trigger_phase   = TRIG_ARMED;
      s_az_prev = s.az;
      return;
    }

    s_confirm_count++;
    // Il "picco" registrato resta l'accelerazione sull'asse freccia (az): e'
    // quella che serve allo scoring e alle metriche, indipendentemente da quale
    // grandezza abbia fatto scattare il trigger.
    if (fabsf(s.az) > fabsf(g_trigger_az_peak)) g_trigger_az_peak = s.az;
    if (s_confirm_count >= s_confirm_n) {
      doFire();                      // SCOCCO CONFERMATO
    } else {
      g_trigger_phase = TRIG_CONFIRMING;
    }
  } else {
    s_confirm_count   = 0;
    g_trigger_az_peak = 0.0f;
    g_trigger_phase   = TRIG_ARMED;
  }
  s_az_prev = s.az;
}

// ----------------------------------------------------------------------------
//  trigger_task — ora gestisce SOLO le transizioni TEMPORALI del trigger.
// ----------------------------------------------------------------------------
//  La VALUTAZIONE dei campioni e' passata a trigger_feed_sample(), chiamata da
//  imu_task per ogni campione (vedi trigger.h: prima il polling di
//  g_latest_sample perdeva i campioni degli scocchi veri, che durano ~10ms).
//  Qui restano le sole cose che dipendono dal TEMPO e non dai dati:
//    - IDLE  -> ARMED    quando il main arma
//    - ARMED -> IDLE     quando il main disarma
//    - REARM -> ARMED    allo scadere di s_rearm_ms
//  Un periodo di 10ms e' piu' che sufficiente: nessuna di queste transizioni e'
//  time-critical al millisecondo (il fire, che lo e', avviene in feed_sample).
void trigger_task(void* param) {
  DBG("trigger_task avviato su Core %d (solo transizioni temporali)", xPortGetCoreID());

  while (!g_imu_ok) { vTaskDelay(pdMS_TO_TICKS(10)); }

  for (;;) {
    switch (g_trigger_phase) {

      case TRIG_IDLE:
        // Disarmato: azzera lo stato e passa ad ARMED solo quando il main arma.
        s_confirm_count = 0;
        if (s_armed) {
          g_trigger_phase = TRIG_ARMED;
          DBG("Trigger ARMED (soglia=%.1f)", s_threshold);
        }
        break;

      case TRIG_ARMED:
      case TRIG_CONFIRMING:
        // La valutazione la fa imu_task via trigger_feed_sample(). Qui
        // controlliamo solo se il main ha disarmato (scoring iniziato).
        if (!s_armed) {
          g_trigger_phase = TRIG_IDLE;
          s_confirm_count = 0;
        }
        break;

      case TRIG_FIRED:
        // Rete di sicurezza. Oggi nessuno imposta piu' questa fase: il fire
        // completo (shot_id + freeze + semaforo) avviene in doFire(), dentro
        // trigger_feed_sample. Se pero' un percorso futuro impostasse TRIG_FIRED,
        // qui il tiro viene COMPLETATO invece di svanire in REARM senza aver
        // congelato il buffer (era il difetto del trigger manuale, 24/07).
        DBG("TRIGGER (via fase FIRED) shot_id=%u az_peak=%.2f", g_shot_id, g_trigger_az_peak);
        doFire();
        break;

      case TRIG_REARM:
        if ((millis() - s_fired_ms) >= s_rearm_ms) {
          s_confirm_count   = 0;
          g_trigger_az_peak = 0.0f;
          s_manual_trigger  = false;   // scarta un manuale arrivato nel rearm
          // Dopo il rearm, torna ARMED solo se il main ci vuole ancora armati
          // (di norma NO: siamo entrati in scoring; il main riarmera' in ATTESA).
          g_trigger_phase = s_armed ? TRIG_ARMED : TRIG_IDLE;
          DBG("Trigger REARMED");
        }
        break;
    }

    // 10ms: nessuna di queste transizioni e' time-critical (il fire avviene in
    // trigger_feed_sample, sul campione). Meno risvegli = piu' CPU per l'IMU.
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  vTaskDelete(nullptr);
}
