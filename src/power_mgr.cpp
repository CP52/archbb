// ============================================================================
//  ArchBB 1.83 — power_mgr.cpp · FASE 24a
//  Cesare Pagura · Padova/Noale IT · 12 settembre 2026
// ----------------------------------------------------------------------------
//  Vedi power_mgr.h per il perche'. Qui c'e' il come, e il come e' volutamente
//  noioso: una macchina a tre stati pilotata da un solo cronometro. Niente
//  euristiche "intelligenti" — la lezione generale del progetto e' che un
//  filtro furbo che si comporta in modo strano e' quasi sempre il filtro, non
//  un fenomeno nuovo.
//
//  PERCHE' NON SI RISVEGLIA SUL MOVIMENTO DELL'IMU
//    Sembrerebbe elegante: l'arco si alza, lo schermo si accende. In campo e'
//    il contrario di quel che serve — la scheda sta su un riser che viene
//    portato a spalla per chilometri, e ogni passo e' movimento. Lo schermo non
//    si spegnerebbe MAI, cioe' il risparmio sarebbe nullo proprio nel momento
//    in cui serve. Il risveglio e' TOCCO o TASTO: due gesti deliberati.
// ============================================================================
#include "power_mgr.h"
#include "display.h"      // displayBacklightPercent / displayPanelSleep

// ----------------------------------------------------------------------------
//  Stato interno (tutto static: nessuno fuori da qui puo' falsarlo)
// ----------------------------------------------------------------------------
static PowerStato   s_stato      = PowerStato::ATTIVO;
static PowerProfilo s_profilo    = PowerProfilo::ATTESA;
static uint32_t     s_ultimaAtt  = 0;      // millis dell'ultima attivita'
static uint8_t      s_base       = ENERGIA_BL_BASE_PCT;
static uint8_t      s_pctAttuale = ENERGIA_BL_BASE_PCT;
static uint8_t      s_pctTarget  = ENERGIA_BL_BASE_PCT;
static bool         s_risveglio  = false;  // il prossimo tocco e' da buttare
static uint32_t     s_risveglioMs = 0;     // quando e' stato armato
static uint32_t     s_cpuMhz     = ENERGIA_CPU_MHZ_ALTA;

// Passo della rampa di attenuazione, in punti percentuali per tick. A 15 ms di
// loop, 6 punti/tick portano da 60 a 25 in ~90 ms: si vede che sta calando
// (e' il preavviso) senza sembrare un guasto.
static constexpr uint8_t RAMPA_PASSO = 6;

// ----------------------------------------------------------------------------
//  Soglie secondo il profilo. Sono qui e da nessun'altra parte.
// ----------------------------------------------------------------------------
static uint32_t sogliaDim() {
  switch (s_profilo) {
    case PowerProfilo::ATTESA:        return ENERGIA_T_DIM_ATTESA_MS;
    case PowerProfilo::INTERATTIVO:   return ENERGIA_T_DIM_INTER_MS;
    case PowerProfilo::TRASFERIMENTO: return ENERGIA_T_DIM_ATTESA_MS;
  }
  return ENERGIA_T_DIM_ATTESA_MS;
}

// 0 = "questo profilo non spegne mai". Vale per INTERATTIVO: quelle schermate
// si chiudono da sole per timeout proprio, quindi non restano accese a vuoto,
// e spegnerle mentre l'arciere decide dove ha colpito sarebbe solo un modo per
// fargli perdere il filo.
static uint32_t sogliaOff() {
  switch (s_profilo) {
    case PowerProfilo::ATTESA:        return ENERGIA_T_OFF_ATTESA_MS;
    case PowerProfilo::INTERATTIVO:   return 0;
    case PowerProfilo::TRASFERIMENTO: return ENERGIA_T_OFF_ATTESA_MS;
  }
  return ENERGIA_T_OFF_ATTESA_MS;
}

// ----------------------------------------------------------------------------
//  Frequenza di CPU — predisposta e spenta, v. ENERGIA_CPU_SCALING in config.h
// ----------------------------------------------------------------------------
static void cpuSet(uint32_t mhz) {
#if ENERGIA_CPU_SCALING
  if (mhz == s_cpuMhz) return;
  setCpuFrequencyMhz(mhz);
  s_cpuMhz = mhz;
  DBG("POWER: CPU -> %lu MHz", (unsigned long)mhz);
#else
  (void)mhz;   // flag spento: si resta a 240 MHz e si paga la corrente
#endif
}

// ----------------------------------------------------------------------------
//  Applicazione del livello, con rampa
// ----------------------------------------------------------------------------
//  Il target si imposta di colpo; il valore reale lo insegue di RAMPA_PASSO per
//  tick SOLO scendendo. Salendo si va istantaneamente: il risveglio deve essere
//  immediato, o si tocca due volte credendo che il primo tocco non sia passato.
static void applicaRampa() {
  if (s_pctAttuale == s_pctTarget) return;

  if (s_pctAttuale < s_pctTarget) {
    s_pctAttuale = s_pctTarget;                       // salita: istantanea
  } else {
    uint8_t delta = (uint8_t)(s_pctAttuale - s_pctTarget);
    s_pctAttuale = (delta <= RAMPA_PASSO) ? s_pctTarget
                                          : (uint8_t)(s_pctAttuale - RAMPA_PASSO);
  }
  displayBacklightPercent(s_pctAttuale);
}

// ----------------------------------------------------------------------------
//  API
// ----------------------------------------------------------------------------
void power_init() {
  s_base       = ENERGIA_BL_BASE_PCT;
  s_pctTarget  = s_base;
  s_pctAttuale = s_base;
  s_stato      = PowerStato::ATTIVO;
  s_profilo    = PowerProfilo::ATTESA;
  s_ultimaAtt  = millis();
  s_risveglio  = false;
  s_risveglioMs = 0;
  s_cpuMhz     = ENERGIA_CPU_MHZ_ALTA;
  displayBacklightPercent(s_base);
  DBG("POWER: avviato (base %u%%, dim %lu ms, off %lu ms)",
      (unsigned)s_base, (unsigned long)sogliaDim(), (unsigned long)sogliaOff());
}

// Risalita ad ATTIVO, senza toccare il flag di risveglio. E' il pezzo comune
// fra i tre modi di svegliarsi (tocco, evento, cambio di profilo): scriverlo
// una volta sola evita che i tre divergano — che e' il modo in cui, in questo
// progetto, i difetti nascono tutte le volte.
static bool risalita() {
  s_ultimaAtt = millis();
  if (s_stato == PowerStato::ATTIVO) return false;

  const bool eraSpento = (s_stato == PowerStato::SPENTO);
  if (eraSpento) displayPanelSleep(false);
  cpuSet(ENERGIA_CPU_MHZ_ALTA);
  s_stato      = PowerStato::ATTIVO;
  s_pctTarget  = s_base;
  s_pctAttuale = s_base;
  displayBacklightPercent(s_base);
  return eraSpento;
}

void power_attivita() {
  // Il flag si arma QUI e non allo spegnimento: si arma solo se questo gesto
  // ha davvero riacceso uno schermo spento. Armarlo allo spegnimento voleva
  // dire che un risveglio per evento (uno scocco) lo lasciava carico, pronto a
  // mangiarsi il primo tocco legittimo dell'arciere mezz'ora dopo.
  if (risalita()) {
    s_risveglio   = true;
    s_risveglioMs = millis();
  }
}

bool power_risveglia() {
  const bool eraSpento = risalita();
  s_risveglio = false;    // non c'e' nessun tocco da buttare: e' un evento
  return eraSpento;
}

bool power_consume_risveglio() {
  if (!s_risveglio) return false;
  s_risveglio = false;
  return true;
}

bool power_schermo_acceso() { return s_stato != PowerStato::SPENTO; }

void power_set_profilo(PowerProfilo p) {
  if (p == s_profilo) return;
  s_profilo = p;
  // Cambiare schermata E' attivita': entrare nello scoring non deve trovare lo
  // schermo gia' a meta' strada verso la penombra per colpa dell'attesa di prima.
  // risalita() e non power_attivita(): un cambio di schermata non e' un tocco,
  // quindi non c'e' niente da buttare.
  risalita();
}

PowerStato power_stato() { return s_stato; }

void power_set_livello_base(uint8_t pct) {
  if (pct > 100) pct = 100;
  if (pct < 5)   pct = 5;      // sotto il 5% e' indistinguibile da spento:
                               // un utente non deve poter "perdere" lo schermo
                               // con una regolazione e credere di averlo rotto.
  s_base = pct;
  risalita();                  // la regolazione si vede subito, ovviamente
  if (s_stato == PowerStato::ATTIVO) {
    s_pctTarget = s_pctAttuale = s_base;
    displayBacklightPercent(s_base);
  }
}

uint8_t power_livello_base()  { return s_base; }
uint8_t power_backlight_pct() { return s_pctAttuale; }
uint32_t power_cpu_mhz()      { return s_cpuMhz; }

// ----------------------------------------------------------------------------
//  power_tick — la macchina a stati, chiamata a ogni giro di loop
// ----------------------------------------------------------------------------
void power_tick() {
  applicaRampa();

  // SCADENZA DEL FLAG DI RISVEGLIO. Il tocco che riaccende lo schermo viene
  // buttato da chi gestisce il rilascio — ma se quel rilascio non arriva mai
  // (dito appoggiato fuori da ogni zona attiva, tocco sporco scartato dal
  // debounce) il flag resterebbe carico per sempre e si mangerebbe il PRIMO
  // tocco buono successivo. Un secondo e mezzo: piu' di qualunque tocco umano,
  // meno di qualunque pausa fra due gesti distinti.
  if (s_risveglio && (millis() - s_risveglioMs) > 1500) s_risveglio = false;

  const uint32_t inattivo = millis() - s_ultimaAtt;
  const uint32_t tDim     = sogliaDim();
  const uint32_t tOff     = sogliaOff();

  switch (s_stato) {

    case PowerStato::ATTIVO:
      if (inattivo >= tDim) {
        s_stato     = PowerStato::PENOMBRA;
        s_pctTarget = ENERGIA_BL_DIM_PCT;   // la rampa ci arriva nei prossimi tick
        DBG("POWER: -> PENOMBRA");
      }
      break;

    case PowerStato::PENOMBRA:
      if (tOff != 0 && inattivo >= tOff) {
        // Ordine: prima la luce, poi il pannello, poi la CPU. Spegnere la retro
        // per ultima lascerebbe vedere per un istante il pannello che si spegne
        // — brutto, e indistinguibile da un guasto.
        s_pctTarget  = 0;
        s_pctAttuale = 0;
        displayBacklightPercent(0);
        displayPanelSleep(true);
        cpuSet(ENERGIA_CPU_MHZ_BASSA);
        s_stato = PowerStato::SPENTO;
        DBG("POWER: -> SPENTO");
      }
      break;

    case PowerStato::SPENTO:
      // Si esce solo per power_attivita() / power_risveglia(). Nessuna uscita
      // automatica: un risveglio a tempo sarebbe corrente bruciata per nessuno.
      break;
  }
}
