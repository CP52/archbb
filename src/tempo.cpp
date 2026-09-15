// ============================================================================
//  ArchBB 1.83 — tempo.cpp · FASE 20
// ----------------------------------------------------------------------------
//  Vedi tempo.h per il razionale e per la definizione onesta di cio' che si
//  misura. Qui c'e' solo la macchina.
//
//  BASE DEI TEMPI: ImuSample::timestamp_us, non millis(). E' la STESSA base
//  del burst, quindi i tempi calcolati qui e gli istanti letti nel burst
//  dall'analizzatore sono confrontabili senza conversioni. Due basi dei tempi
//  in un sistema che misura tempi sono una sorgente di errore gratuita.
// ============================================================================
#include "tempo.h"
#include "shot_angles.h"   // SHOT_ANGLE_GRAVITY
#include <math.h>

// ODR reale misurata sul campo: 217,72 Hz (briefing 31/07), non i 224 nominali
// ne' i 249 apparenti dal battimento dei tick FreeRTOS. Qui serve solo per la
// costante di tempo della stima di gravita', dove un 3% non cambia niente — ma
// scriverci il numero vero costa zero e toglie una domanda a chi legge.
static constexpr float TEMPO_ODR_HZ = 217.7f;

// Oltre questa deviazione dal modulo di g il campione e' moto, non postura, e
// non deve entrare nella stima della verticale.
static constexpr float TEMPO_GRAVITA_DEV_MAX = 2.0f;

static TempoStato s_stato      = TEMPO_RIPOSO;
static uint32_t   s_t_stato_us = 0;     // quando siamo entrati nello stato
static uint32_t   s_t_alzata_us = 0;    // inizio dell'ultimo movimento
static uint32_t   s_t_ancora_us = 0;    // inizio dell'ultima quiete confermata
static bool       s_ha_alzata  = false;
static bool       s_ha_ancora  = false;

// Candidata ad ancora: quando la quiete comincia, ma non e' ancora confermata.
// Serve perche' t_ancora dev'essere l'istante in cui la quiete E' COMINCIATA,
// non quello in cui l'abbiamo creduta. Confondere i due accorcerebbe ogni
// misura di TEMPO_QUIETE_MS in modo sistematico e invisibile.
static uint32_t   s_t_quiete_us = 0;
static bool       s_in_quiete   = false;

static uint32_t   s_ultimo_us   = 0;

// Stima lenta della gravita', per il gate di postura. Il trigger ne ha una sua:
// duplicarla e' voluto e va detto. Condividerla significherebbe che il modulo
// tempo smette di funzionare se un giorno il trigger cambia costante di tempo,
// e sono due domande diverse — "e' uno scocco?" e "l'arco e' su?" — che non
// devono condividere uno stato mutevole. La costante di tempo qui e' piu'
// lunga: non ci interessa seguire lo scocco, ci interessa la postura.
static float s_gx = 0, s_gy = 0, s_gz = 0;
static bool  s_g_primed = false;
//  TAU e' 0,3 s e NON 2 s, e il valore lungo era un errore trovato al banco.
//  Con tau lungo la stima impiega secondi a seguire il passaggio da "arco
//  abbassato" a "arco in postura di tiro": il gate di postura restava falso
//  per il primo secondo di mira, e in quel secondo la quiete candidata veniva
//  azzerata a ogni campione. Risultato: mira misurata 1492 ms invece di 2500,
//  con l'alzata persa del tutto. Un filtro troppo lento non produce un numero
//  rumoroso, ne produce uno pulito e sbagliato.
static constexpr float TEMPO_GRAVITA_TAU_S = 0.3f;

// --- F20b: l'ancora CONCLUSA -----------------------------------------------
//  Quando ANCORA finisce (per movimento, per uscita di postura o per timeout)
//  la sua durata viene congelata QUI. E' questo che doFire() legge, non lo
//  stato vivo — che al momento del trigger e' gia' MOTO da 30-80 ms. Vedi la
//  CORREZIONE DEL 15/08 in tempo.h.
static uint16_t s_ancora_durata_ms = TEMPO_NOT_SET;
static uint16_t s_ancora_alzata_ms = TEMPO_NOT_SET;
static uint32_t s_ancora_fine_us   = 0;
static bool     s_ancora_conclusa  = false;

static TempoTiro s_ultimo = { TEMPO_NOT_SET, TEMPO_NOT_SET, TEMPO_NO_ANCORA };

void tempo_init() {
  s_stato = TEMPO_RIPOSO;
  s_t_stato_us = s_t_alzata_us = s_t_ancora_us = s_t_quiete_us = 0;
  s_ha_alzata = s_ha_ancora = s_in_quiete = s_g_primed = false;
  s_ancora_durata_ms = s_ancora_alzata_ms = TEMPO_NOT_SET;
  s_ancora_fine_us = 0; s_ancora_conclusa = false;
  s_ultimo = { TEMPO_NOT_SET, TEMPO_NOT_SET, TEMPO_NO_ANCORA };
}

// Millisecondi fra due timestamp in microsecondi, con saturazione a uint16.
// L'aritmetica su uint32 gestisce da sola il rollover di esp_timer (che
// comunque arriva dopo 71 minuti e mezzo, ma scriverlo costa zero).
static uint16_t delta_ms(uint32_t da_us, uint32_t a_us) {
  uint32_t d = (a_us - da_us) / 1000u;
  return (d >= TEMPO_NOT_SET) ? (uint16_t)(TEMPO_NOT_SET - 1) : (uint16_t)d;
}

// Chiude l'ancora in corso, se c'e', congelandone la durata. UN SOLO punto:
// le uscite da ANCORA sono tre (movimento, fuori postura, timeout) e ognuna
// che se la sbrigasse da sola sarebbe un'occasione di dimenticarsene una.
static void chiudiAncora(uint32_t t_us) {
  if (!s_ha_ancora) return;
  s_ancora_durata_ms = delta_ms(s_t_ancora_us, t_us);
  s_ancora_alzata_ms = (s_ha_alzata && (int32_t)(s_t_ancora_us - s_t_alzata_us) > 0)
                       ? delta_ms(s_t_alzata_us, s_t_ancora_us) : TEMPO_NOT_SET;
  s_ancora_fine_us  = t_us;
  s_ancora_conclusa = true;
  s_ha_ancora       = false;
}

void tempo_feed_sample(const ImuSample& s) {
  s_ultimo_us = s.timestamp_us;

  // --- le due grandezze di quiete ------------------------------------------
  const float omega = sqrtf(s.gx*s.gx + s.gy*s.gy + s.gz*s.gz);
  const float dev   = fabsf(sqrtf(s.ax*s.ax + s.ay*s.ay + s.az*s.az)
                            - SHOT_ANGLE_GRAVITY);

  // --- gravita' stimata, per la postura ------------------------------------
  //  Aggiornata SOLO quando il campione e' plausibilmente gravita' e non moto.
  //  Durante lo scocco l'accelerazione arriva a decine di m/s^2: lasciarla
  //  entrare nella stima farebbe "ruotare" la verticale per un paio di decimi
  //  di secondo proprio nell'istante in cui serve ferma.
  {
    const float a = 1.0f / (TEMPO_GRAVITA_TAU_S * TEMPO_ODR_HZ);
    if (!s_g_primed) { s_gx = s.ax; s_gy = s.ay; s_gz = s.az; s_g_primed = true; }
    else if (dev < TEMPO_GRAVITA_DEV_MAX) {
      s_gx += a * (s.ax - s_gx);
      s_gy += a * (s.ay - s_gy);
      s_gz += a * (s.az - s_gz);
    }
  }
  const float gm = sqrtf(s_gx*s_gx + s_gy*s_gy + s_gz*s_gz);
  const bool  in_postura = (gm > 0.1f) && ((s_gx / gm) > TEMPO_POSTURA_COS);

  const bool fermo  = (omega < TEMPO_QUIETE_DPS) && (dev < TEMPO_QUIETE_MS2);
  const bool muove  = (omega > TEMPO_MOTO_DPS);

  // Fuori postura non si misura niente: l'arco e' abbassato, appoggiato o in
  // mano lungo il fianco. E' anche cio' che impedisce di scambiare per "mira"
  // il fermo-immobile di chi aspetta il proprio turno con l'arco a terra.
  if (!in_postura) {
    chiudiAncora(s.timestamp_us);
    if (s_stato != TEMPO_RIPOSO) { s_stato = TEMPO_RIPOSO; s_t_stato_us = s.timestamp_us; }
    s_in_quiete = false;
    s_ha_alzata = false;
    return;
  }

  // Una mira che dura piu' di TEMPO_MAX_MS non e' una mira: e' l'arco tenuto
  // su fermo. Si torna a riposo e si aspetta un movimento vero.
  if (s_stato == TEMPO_ANCORA &&
      (s.timestamp_us - s_t_ancora_us) / 1000u > TEMPO_MAX_MS) {
    // Non si chiude l'ancora: una mira di piu' di un minuto non e' una mira, e
    // congelarne la durata significherebbe offrirla al prossimo tiro.
    s_stato = TEMPO_RIPOSO; s_t_stato_us = s.timestamp_us;
    s_ha_ancora = false; s_in_quiete = false;
    return;
  }

  if (muove) {
    // Movimento. Se veniamo da ANCORA, QUESTO E' IL RILASCIO (o un
    // riassestamento): la durata si congela adesso, che e' l'istante giusto —
    // 30-80 ms prima che il trigger se ne accorga.
    chiudiAncora(s.timestamp_us);
    if (s_stato != TEMPO_MOTO) {
      s_stato = TEMPO_MOTO; s_t_stato_us = s.timestamp_us;
      s_t_alzata_us = s.timestamp_us; s_ha_alzata = true;
    }
    s_in_quiete = false;
    return;
  }

  if (fermo) {
    if (!s_in_quiete) { s_in_quiete = true; s_t_quiete_us = s.timestamp_us; }
    else if (s_stato != TEMPO_ANCORA &&
             (s.timestamp_us - s_t_quiete_us) / 1000u >= TEMPO_QUIETE_MS) {
      // Quiete confermata. L'ancora e' quando la quiete E' COMINCIATA.
      s_stato = TEMPO_ANCORA;
      s_t_ancora_us = s_t_quiete_us;
      s_t_stato_us  = s_t_quiete_us;
      s_ha_ancora   = true;
    }
    return;
  }

  // Zona grigia fra le due soglie (isteresi): non si cambia stato, ma la
  // quiete candidata decade. Senza questo, un tremore appena sopra la soglia
  // di quiete e sotto quella di moto lascerebbe correre un cronometro che non
  // sta piu' misurando niente.
  s_in_quiete = false;
}

void tempo_on_fire(uint32_t t_scocco_us) {
  TempoTiro r = { TEMPO_NOT_SET, TEMPO_NOT_SET, TEMPO_NO_ANCORA };

  // Caso raro ma legittimo: il trigger scatta mentre siamo ANCORA in ancora
  // (il movimento non ha ancora superato la soglia). Si chiude adesso.
  if (s_ha_ancora) chiudiAncora(t_scocco_us);

  if (s_ancora_conclusa) {
    uint32_t eta_ms = (t_scocco_us - s_ancora_fine_us) / 1000u;
    if (eta_ms <= TEMPO_LATCH_MAX_MS) {
      r.mira_ms   = s_ancora_durata_ms;
      r.alzata_ms = s_ancora_alzata_ms;
      r.motivo    = TEMPO_OK;
    } else {
      r.motivo = TEMPO_ANCORA_VECCHIA;
    }
  }

  s_ultimo = r;
  // Dopo lo scocco si riparte da capo: il follow-through non e' una mira, e
  // l'ancora congelata e' stata consumata da questo tiro.
  s_stato = TEMPO_RIPOSO; s_t_stato_us = t_scocco_us;
  s_ha_alzata = s_ha_ancora = s_in_quiete = false;
  s_ancora_conclusa = false;
}

TempoTiro  tempo_ultimo()          { return s_ultimo; }
TempoStato tempo_stato()           { return s_stato; }
uint32_t   tempo_ms_nello_stato()  {
  if (s_t_stato_us == 0) return 0;
  return (s_ultimo_us - s_t_stato_us) / 1000u;
}
