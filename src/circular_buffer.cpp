// ============================================================================
//  ArchBB 1.83 — circular_buffer.cpp · FASE 4b (buffer circolare PSRAM) · v1
//  Cesare Pagura · Padova/Noale IT · 13 luglio 2026
// ----------------------------------------------------------------------------
//  Implementazione portata dalla 1.69 v2.12.6. Differenze 1.83:
//    - PSRAM come default (la 1.83 ha la Octal R8 gia' abilitata). Manteniamo
//      il fallback automatico a heap interno se psramFound() fosse falso: 750
//      campioni x 30B = ~22KB, che stanno comodamente anche in DRAM interna
//      (~300KB liberi) -> il sistema non muore mai per assenza di PSRAM.
//    - Dimensione unica CIRCULAR_BUFFER_SIZE (750) da config.h.
//
//  AUTOMA (push):
//    RUNNING: scrive alla testa e avanza modulo BUF_SIZE (finestra mobile).
//    FROZEN : il trigger ha marcato s_trigger_write_idx; i campioni successivi
//             vanno negli slot POST finche' non se ne raccolgono post_samples,
//             poi -> READY.
//    READY  : ignora i nuovi campioni (il burst e' congelato, in attesa di
//             essere letto/consumato dalla UI).
//
//  ESTRAZIONE (get_burst):
//    Il burst e' [trigger - pre .. trigger + post). L'inizio si calcola
//    all'indietro dal punto di trigger, in modulo BUF_SIZE. trigger_idx_out
//    restituisce 'pre' cioe' la posizione dello scocco entro il burst copiato:
//    e' esattamente cio' che shot_angles_compute() (Fase 4c) si aspetta.
// ============================================================================
#include "circular_buffer.h"

static constexpr uint16_t BUF_SIZE = CIRCULAR_BUFFER_SIZE;

static ImuSample*           s_buf               = nullptr;
static volatile uint16_t    s_write_idx         = 0;
static volatile BufferState s_state             = BUFFER_RUNNING;
static uint16_t             s_pre_samples       = 0;
static uint16_t             s_post_samples      = 0;
static uint16_t             s_shot_id           = 0;
static uint16_t             s_trigger_write_idx = 0;
static volatile uint16_t    s_post_count        = 0;

// --- FASE 24b: CAMPIONI FRESCHI DAL RESET -----------------------------------
//  Il difetto che questo contatore chiude (sessione del 12/09, burst_0005):
//  circular_buffer_reset() rimetteva lo stato a RUNNING ma NON svuotava l'anello
//  e non teneva conto di quanti campioni nuovi fossero arrivati da allora. Se il
//  trigger scattava meno di 'pre' campioni dopo il riarmo — nel caso reale 745 ms
//  dopo un tap OK, cioe' un maneggio — get_burst() copiava comunque 448 campioni
//  di pre-trigger, e i primi 299 venivano da UN MINUTO PRIMA (il buffer era
//  rimasto congelato durante il riepilogo). shot_angles calcolava la finestra di
//  calma su quei campioni stantii e produceva un alzo di 6,88 gradi per un
//  bersaglio armato a 30: un numero plausibile, sbagliato, e salvato come buono.
//
//  Non serviva cancellare l'anello (costoso e inutile): serviva SAPERE quanti
//  campioni valgono. Il contatore satura a BUF_SIZE — oltre, "sono tutti freschi"
//  e continuare a incrementare servirebbe solo a farlo traboccare.
static volatile uint32_t    s_freschi           = 0;   // push dal reset, satura
static uint16_t             s_freschi_al_trigger = 0;  // istantanea al congelamento
static SemaphoreHandle_t    s_mutex             = nullptr;

// ----------------------------------------------------------------------------
bool circular_buffer_init() {
  if (psramFound()) {
    s_buf = (ImuSample*)ps_malloc(sizeof(ImuSample) * BUF_SIZE);
    DBG("Buffer in PSRAM — %u campioni x %uB = %uB",
        BUF_SIZE, (unsigned)sizeof(ImuSample), (unsigned)(BUF_SIZE * sizeof(ImuSample)));
  } else {
    // Fallback sicuro: 22KB in DRAM interna (abbondante). Non blocchiamo mai
    // il sistema per assenza di PSRAM; al piu' lo segnaliamo nel log.
    DBG("PSRAM non trovata — fallback heap interno");
    s_buf = (ImuSample*)malloc(sizeof(ImuSample) * BUF_SIZE);
  }
  if (!s_buf) { DBG("ERRORE: malloc buffer circolare fallito"); return false; }
  memset(s_buf, 0, sizeof(ImuSample) * BUF_SIZE);

  s_mutex = xSemaphoreCreateMutex();
  if (!s_mutex) { free(s_buf); s_buf = nullptr; return false; }

  s_write_idx  = 0;
  s_state      = BUFFER_RUNNING;
  s_post_count = 0;
  s_freschi    = 0;   // FASE 24b: da qui in poi si conta daccapo
  DBG("Circular buffer OK — free heap: %u", (unsigned)ESP.getFreeHeap());
  return true;
}

// ----------------------------------------------------------------------------
void circular_buffer_push(const ImuSample& sample) {
  if (!s_buf) return;
  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1)) != pdTRUE) return;

  switch (s_state) {
    case BUFFER_RUNNING:
      s_buf[s_write_idx] = sample;
      s_write_idx = (s_write_idx + 1) % BUF_SIZE;
      if (s_freschi < BUF_SIZE) s_freschi++;
      break;

    case BUFFER_FROZEN:
      if (s_post_count < s_post_samples) {
        uint16_t idx = (s_trigger_write_idx + s_post_count) % BUF_SIZE;
        s_buf[idx] = sample;
        s_post_count++;
        if (s_post_count >= s_post_samples) {
          s_state = BUFFER_READY;
          DBG("Buffer READY: pre=%u post=%u tot=%u",
              s_pre_samples, s_post_samples, s_pre_samples + s_post_samples);
        }
      }
      break;

    case BUFFER_READY:
      break;   // burst congelato: nuovi campioni ignorati fino al reset
  }
  xSemaphoreGive(s_mutex);
}

// ----------------------------------------------------------------------------
void circular_buffer_freeze(uint16_t pre, uint16_t post, uint16_t shot_id) {
  if (!s_buf) return;

  // PROTEZIONE (emersa dai test): a ODR alti (448Hz) la finestra 3000+350ms
  // diventa 1500 campioni e SFOREREBBE il buffer da 750. Se pre+post supera
  // BUF_SIZE, riduciamo proporzionalmente mantenendo il rapporto pre/post, cosi'
  // la finestra resta centrata sullo scocco invece di essere troncata a caso.
  // Al default 224Hz (pre=672,post=78=750) e' un no-op. Serve solo da rete di
  // sicurezza per quando la Fase 5 rendera' l'ODR configurabile.
  uint32_t tot = (uint32_t)pre + (uint32_t)post;
  if (tot > BUF_SIZE) {
    float k = (float)BUF_SIZE / (float)tot;
    pre  = (uint16_t)(pre  * k);
    post = (uint16_t)(post * k);
    DBG("ATTENZIONE: finestra %uc > buffer %uc -> ridotta a pre=%u post=%u",
        (unsigned)tot, BUF_SIZE, pre, post);
  }

  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;
  s_pre_samples       = pre;
  s_post_samples      = post;
  s_shot_id           = shot_id;
  s_post_count        = 0;
  s_trigger_write_idx = s_write_idx;   // lo scocco e' "adesso": qui inizia il POST
  // FASE 24b: istantanea dei campioni freschi NEL MOMENTO del congelamento.
  // Deve essere presa qui e non in get_burst(), perche' fra il congelamento e la
  // lettura arrivano i POST campioni, che sono freschi ma stanno DOPO il trigger
  // e non dicono niente su quanto pre-trigger sia buono.
  s_freschi_al_trigger = (s_freschi > BUF_SIZE) ? BUF_SIZE : (uint16_t)s_freschi;
  s_state             = BUFFER_FROZEN;
  DBG("Buffer FROZEN: shot=%u pre=%u post=%u", shot_id, pre, post);
  xSemaphoreGive(s_mutex);
}

// ----------------------------------------------------------------------------
uint16_t circular_buffer_get_burst(ImuSample* dst, uint16_t max_samples,
                                   uint16_t* trigger_idx_out,
                                   uint16_t* pre_validi_out) {
  if (!s_buf || !dst) return 0;
  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return 0;

  uint16_t total = s_pre_samples + s_post_samples;
  if (total > max_samples) total = max_samples;

  // Inizio del burst: pre campioni PRIMA del punto di trigger, in modulo.
  uint16_t start = (uint16_t)((s_trigger_write_idx - s_pre_samples + BUF_SIZE) % BUF_SIZE);
  for (uint16_t i = 0; i < total; i++) {
    dst[i] = s_buf[(start + i) % BUF_SIZE];
  }
  if (trigger_idx_out) *trigger_idx_out = s_pre_samples;  // scocco = indice 'pre'

  // FASE 24b — quanti dei campioni PRE-trigger sono davvero di questo tiro.
  //  Se al congelamento erano arrivati meno di 'pre' campioni dal reset, i
  //  restanti sono residui dell'anello: presenti nel file (non li cancelliamo,
  //  sono comunque un dato), ma DICHIARATI non validi. Chi calcola gli angoli
  //  deve poter rifiutare invece di produrre un numero verosimile e falso.
  if (pre_validi_out) {
    *pre_validi_out = (s_freschi_al_trigger >= s_pre_samples)
                        ? s_pre_samples : s_freschi_al_trigger;
  }

  xSemaphoreGive(s_mutex);
  return total;
}

// ----------------------------------------------------------------------------
void circular_buffer_reset() {
  if (!s_buf) return;
  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;
  s_state      = BUFFER_RUNNING;
  s_post_count = 0;
  s_freschi    = 0;   // FASE 24b: da qui in poi si conta daccapo
  xSemaphoreGive(s_mutex);
  DBG("Buffer RESET -> RUNNING");
}

BufferState circular_buffer_state()     { return s_state; }
uint16_t    circular_buffer_post_count(){ return s_post_count; }
