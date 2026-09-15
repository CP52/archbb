// ============================================================================
//  ArchBB 1.83 — config_store.cpp · FASE 6 (NVS + CONFIG)
//  Cesare Pagura · Padova/Noale IT · 20 luglio 2026
// ----------------------------------------------------------------------------
//  Vedi config_store.h per l'architettura. Qui l'implementazione su NVS via
//  Preferences (wrapper standard Arduino-ESP32). La struct Config viaggia come
//  BLOB unico: un solo putBytes/getBytes, atomico quanto basta, e il checksum
//  in coda cattura le scritture interrotte.
// ============================================================================
#include "config_store.h"
#include "mount.h"       // g_mount_orientation
#include "trigger.h"     // trigger_configure: rende operativi i parametri (§2.3)
#include <Preferences.h>

// Istanza globale in RAM: la copia di lavoro che tutti leggono a runtime.
Config g_config;

// Namespace e chiave NVS. Nomi corti (l'NVS ha un limite di 15 char per chiave).
static const char* NVS_NS  = "archbb";
static const char* NVS_KEY = "config";


// ----------------------------------------------------------------------------
//  checksum — somma dei byte di Config ESCLUSO il campo checksum stesso.
// ----------------------------------------------------------------------------
//  Semplice ma efficace per il caso d'uso (scrittura interrotta): una FNV-1a a
//  32 bit sui byte che precedono il checksum. Non e' crittografia, e' un
//  rilevatore di corruzione — e per quello basta e avanza.
static uint32_t configChecksum(const Config& c) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&c);
  size_t n = sizeof(Config) - sizeof(c.checksum);   // tutto tranne il checksum finale
  uint32_t h = 2166136261u;                         // offset FNV-1a
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
  return h;
}

// FASE 7 (BLE) — wrapper pubblico del checksum interno. Vedi config_store.h: il
// modulo BLE lo usa per validare una Config ricevuta dall'app. Deleghiamo alla
// stessa funzione statica: UNA sola formula, mai duplicata (la lezione piu'
// costosa della 1.69 e' proprio "costanti/formule duplicate = bug garantito").
uint32_t config_checksum_of(const Config& c) { return configChecksum(c); }

// ----------------------------------------------------------------------------
//  §TARATURA — chiavi scalari fuori dal BLOB (F20b)
// ----------------------------------------------------------------------------
//  Vedi config_store.h per il perche'. Qui il come: tre chiavi corte (limite
//  NVS 15 caratteri) nello stesso namespace. Non sono una copia di riserva da
//  ricordarsi di fare — sono scritte da config_save() a ogni salvataggio, cosi'
//  non esiste uno stato in cui il blob e' aggiornato e loro no.
static const char* K_MOUNT = "t_mount";
static const char* K_OFFC  = "t_offcant";
static const char* K_OFFA  = "t_offalzo";

void config_taratura_salva() {
  Preferences p;
  if (!p.begin(NVS_NS, /*readOnly=*/false)) return;
  p.putUChar(K_MOUNT, g_config.mount_orientation);
  p.putFloat(K_OFFC,  g_config.offset_cant_deg);
  p.putFloat(K_OFFA,  g_config.offset_alzo_deg);
  p.end();
}

bool config_taratura_ripristina() {
  Preferences p;
  if (!p.begin(NVS_NS, /*readOnly=*/true)) return false;
  // isKey distingue "mai salvata" da "salvata a zero". Senza, un offset
  // legittimamente nullo sarebbe indistinguibile da un'assenza — lo stesso
  // errore dello zero-come-sentinella, in un altro vestito.
  const bool c_e = p.isKey(K_MOUNT);
  uint8_t m = c_e ? p.getUChar(K_MOUNT, g_config.mount_orientation) : 0;
  float oc = p.isKey(K_OFFC) ? p.getFloat(K_OFFC, 0.0f) : 0.0f;
  float oa = p.isKey(K_OFFA) ? p.getFloat(K_OFFA, 0.0f) : 0.0f;
  p.end();
  if (!c_e) return false;
  if (!mount_is_valid(m)) return false;      // NVS sporca: meglio il default
  g_config.mount_orientation = m;
  g_config.offset_cant_deg   = oc;
  g_config.offset_alzo_deg   = oa;
  g_config.checksum          = configChecksum(g_config);
  DBG("CFG: taratura ripristinata (mount=%u off_cant=%.2f off_alzo=%.2f)",
      m, oc, oa);
  return true;
}

// ----------------------------------------------------------------------------
//  config_set_defaults — riempie g_config coi DEFAULT compilati.
// ----------------------------------------------------------------------------
//  Sono ESATTAMENTE i valori attuali del firmware (le costanti di config.h):
//  cosi', config vuoto = comportamento identico a prima dell'NVS.
void config_set_defaults() {
  g_config.version            = CONFIG_VERSION;
  // trigger
  g_config.trig_mode          = (uint8_t)TRIG_DEFAULT_MODE;       // 2 = accel_dev
  g_config.trig_threshold_ms2 = TRIG_DEFAULT_THRESHOLD;          // 20.0
  g_config.trig_confirm_n     = TRIG_DEFAULT_CONFIRM_N;          // 2
  g_config.trig_rearm_ms      = TRIG_DEFAULT_REARM_MS;           // 2000
  // finestre
  g_config.window_pre_ms      = WINDOW_PRE_MS;                    // 2000
  g_config.window_post_ms     = WINDOW_POST_MS;                   // 1000
  // montaggio (1.83 = FLIPZ)
  g_config.mount_orientation  = ARCHBB_MOUNT_DEFAULT;            // 0 (MOUNT_0; flip-Z in mount_apply)
  // offset di calibrazione: 0 = non calibrato (procedure future)
  g_config.offset_cant_deg    = 0.0f;
  g_config.offset_alzo_deg    = 0.0f;
  g_config.offset_g_ms2       = 0.0f;
  // fisica arco
  g_config.bow_mass_kg        = 0.0f;                            // non impostata
  // scoring: cerchio d'impatto (F19)
  g_config.imp_raggio_colpito_cm = IMP_RAGGIO_COLPITO_CM_DEFAULT;   // 50
  g_config.imp_raggio_mancato_cm = IMP_RAGGIO_MANCATO_CM_DEFAULT;   // 150
  // checksum coerente
  g_config.checksum           = configChecksum(g_config);
}

// ----------------------------------------------------------------------------
//  config_load — legge da NVS in g_config, con fallback ai default.
// ----------------------------------------------------------------------------
bool config_load() {
  Preferences prefs;
  // readOnly: apre il namespace senza crearlo se non serve scrivere.
  if (!prefs.begin(NVS_NS, /*readOnly=*/true)) {
    DBG("CFG: NVS namespace assente -> default");
    config_set_defaults();
    return false;
  }

  // Il blob deve esistere ed avere ESATTAMENTE la dimensione della struct.
  size_t len = prefs.getBytesLength(NVS_KEY);
  if (len != sizeof(Config)) {
    DBG("CFG: blob assente o dimensione errata (%u vs %u) -> default",
        (unsigned)len, (unsigned)sizeof(Config));
    prefs.end();
    config_set_defaults();
    return false;
  }

  Config tmp;
  size_t got = prefs.getBytes(NVS_KEY, &tmp, sizeof(Config));
  prefs.end();
  if (got != sizeof(Config)) {
    DBG("CFG: lettura incompleta -> default");
    config_set_defaults();
    return false;
  }

  // Versione: se non combacia, la struct in NVS e' di un altro firmware ->
  // migrazione ai default (non interpretiamo byte di un layout diverso).
  if (tmp.version != CONFIG_VERSION) {
    // I PARAMETRI ripartono dai default: interpretare byte di un layout diverso
    // e' peggio che perderli. La TARATURA no: viaggia su chiavi scalari, che il
    // layout non lo conoscono nemmeno. Vedi §TARATURA e config_store.h.
    DBG("CFG: versione NVS %u != %u -> default parametri", tmp.version, CONFIG_VERSION);
    config_set_defaults();
    if (config_taratura_ripristina()) {
      DBG("CFG: ... ma la taratura e' sopravvissuta");
      config_save();     // riallinea il blob nuovo con la taratura recuperata
    }
    return false;
  }

  // Checksum: se non torna, il blocco e' corrotto (scrittura interrotta) ->
  // default. Meglio ripartire puliti che lavorare su valori spazzatura.
  uint32_t expect = configChecksum(tmp);
  if (tmp.checksum != expect) {
    DBG("CFG: checksum errato (%08x vs %08x) -> default", tmp.checksum, expect);
    config_set_defaults();
    return false;
  }

  // Tutto valido: e' la nostra fonte di verita'.
  g_config = tmp;
  DBG("CFG: caricato da NVS (v%u ok)", tmp.version);
  return true;
}

// ----------------------------------------------------------------------------
//  config_save — scrive g_config in NVS (aggiorna version + checksum).
// ----------------------------------------------------------------------------
bool config_save() {
  g_config.version  = CONFIG_VERSION;
  g_config.checksum = configChecksum(g_config);
  config_taratura_salva();   // F20b: la taratura esce anche dal blob

  Preferences prefs;
  if (!prefs.begin(NVS_NS, /*readOnly=*/false)) {
    DBG("CFG: apertura NVS in scrittura fallita");
    return false;
  }
  size_t wrote = prefs.putBytes(NVS_KEY, &g_config, sizeof(Config));
  prefs.end();

  bool ok = (wrote == sizeof(Config));
  DBG("CFG: salvato in NVS (%u byte) -> %s", (unsigned)wrote, ok ? "OK" : "FALLITO");
  return ok;
}

// ----------------------------------------------------------------------------
//  config_reset — default + scrittura (ripristino di fabbrica).
// ----------------------------------------------------------------------------
bool config_reset() {
  config_set_defaults();
  DBG("CFG: ripristino default di fabbrica");
  return config_save();
}

// ----------------------------------------------------------------------------
//  config_apply — rende OPERATIVI i valori di g_config.
// ----------------------------------------------------------------------------
//  Centralizza il "come i parametri diventano attivi". Oggi cabla montaggio e
//  trigger (che hanno gia' variabili runtime). Le finestre pre/post restano
//  legate alle costanti compilate in questo passo (parametrizzare il circular
//  buffer e' un cambiamento piu' profondo, da fare a parte senza rischiare
//  regressioni): se un domani si vorranno editabili, e' qui che si applicheranno.
//
//  NB: il trigger espone i suoi setter runtime tramite variabili static in
//  trigger.cpp; finche' non c'e' una trigger_configure() pubblica, qui usiamo
//  g_mount_orientation (gia' extern) e lasciamo il trigger ai suoi default —
//  che COINCIDONO coi default del config. Quando aggiungeremo trigger_configure,
//  questa funzione la chiamera'. Per ora l'unico effetto vivo e' il montaggio.
void config_apply() {
  // Montaggio: g_mount_orientation e' letto dall'imu_task.
  g_mount_orientation = g_config.mount_orientation;

  // TRIGGER (handoff §2.3 — chiuso il 25/07): fino a ieri questa riga NON
  // c'era, e la soglia del config restava lettera morta: il trigger usava
  // sempre il default di compilazione. Con i tiri veri della 1.83 che picchiano
  // a 11.6-16.4 m/s^2 e il default a 20, NESSUN tiro veniva rilevato — mentre
  // gli scuotimenti a mano (fino a 29.9) scattavano sempre. Ora i parametri del
  // config arrivano davvero al trigger, e cambiarli sortisce effetto.
  trigger_configure(g_config.trig_threshold_ms2,
                    g_config.trig_confirm_n,
                    g_config.trig_rearm_ms,
                    g_config.window_pre_ms,
                    g_config.window_post_ms);

  DBG("CFG applicato: mount=%u trig_thr=%.1f confirm=%u rearm=%u pre=%u post=%u",
      g_config.mount_orientation, g_config.trig_threshold_ms2,
      g_config.trig_confirm_n, g_config.trig_rearm_ms,
      g_config.window_pre_ms, g_config.window_post_ms);
}
