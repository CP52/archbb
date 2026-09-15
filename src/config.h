// ============================================================================
//  ArchBB 1.83 — config.h · FASE 4a (IMU QMI8658 + trigger reale)
//  Cesare Pagura · Padova/Noale IT · 13 luglio 2026
// ----------------------------------------------------------------------------
//  Questo file NON e' il config.h monolitico della 1.69 (che porta con se' UUID
//  BLE, ScorePacket, buffer, ecc.). E' un config.h VOLUTAMENTE MINIMALE per la
//  1.83, che contiene SOLO cio' che la Fase 4a usa davvero:
//    - ImuSample     : il campione IMU (identico byte-per-byte alla 1.69, cosi'
//                      quando arrivera' il BLE il pacchetto sara' gia' compatibile)
//    - OdrSetting    : le tre frequenze di campionamento e odrToHz()
//    - i pin/indirizzi I2C dell'IMU
//    - i parametri di default del trigger
//
//  FILOSOFIA "un passo alla volta": aggiungiamo qui i pezzi delle fasi 4b/4c
//  (circular buffer, angoli) SOLO quando serviranno. Tenere il config leggero
//  evita di trascinare dipendenze morte e rende chiaro cosa e' realmente attivo.
//
//  TRAPPOLA HARDWARE 1.83 (bus I2C CONDIVISO):
//    L'IMU QMI8658 (0x6B) e il touch CST816 (0x15) stanno sullo STESSO bus I2C
//    SDA=15/SCL=14, gia' inizializzato da touchInit() in Fase 2. In imu_init()
//    NON richiamiamo Wire.begin() (romperebbe il touch): usiamo il bus attivo.
//    Sulla 1.69 il bus era 11/10 e l'IMU aveva un pin INT su GPIO14; QUI il 14 e'
//    gia' SCL, quindi NON esiste un IMU_INT_PIN. Scelta: POLLING PURO di
//    getDataReady() (SensorLib legge il registro STATUS via I2C, nessun GPIO
//    dedicato). Piu' semplice e piu' robusto: un pin in meno da sbagliare.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ┌─ LUNGHEZZA MASSIMA: 15 CARATTERI ──────────────────────────────────────┐
// │ Questa stringa finisce in BleStatusPacket.fw_version, che e' char[16]: │
// │ 15 caratteri utili + terminatore. "1.83-Fase9-soglia4" (18) arrivava   │
// │ all'app TRONCATO a "1.83-Fase9-sogl" — non un bug, ma un'informazione  │
// │ persa proprio quando serve (capire quale firmware ha prodotto un file).│
// │ Da qui in avanti i nomi restano corti. Su session.txt ci va per intero,│
// │ quindi la forma breve deve bastare a identificare la build.            │
// └────────────────────────────────────────────────────────────────────────┘
#define ARCHBB_FW_VERSION "1.83-F22b-pretiro"

// ----------------------------------------------------------------------------
//  I2C dell'IMU (bus CONDIVISO col touch — vedi trappola sopra)
// ----------------------------------------------------------------------------
//  Questi valori coincidono con ARCHBB_I2C_SDA/SCL dei build_flags (Fase 2). Li
//  ridefiniamo qui come costanti C++ per chiarezza nel codice IMU, ma NON
//  reinizializziamo il bus: e' gia' vivo. L'indirizzo 0x6B e' il default della
//  SensorLib (QMI8658_L_SLAVE_ADDRESS) per il pin SA0 a massa, come sul modulo
//  Waveshare integrato.
static constexpr int     IMU_I2C_SDA  = 15;    // = ARCHBB_I2C_SDA (Fase 2)
static constexpr int     IMU_I2C_SCL  = 14;    // = ARCHBB_I2C_SCL (Fase 2)
static constexpr uint8_t IMU_I2C_ADDR = 0x6B;  // QMI8658_L_SLAVE_ADDRESS

// ----------------------------------------------------------------------------
//  ODR — Output Data Rate del QMI8658
// ----------------------------------------------------------------------------
//  Il QMI8658 NON ha ODR esatti 100/250/500 Hz per il giroscopio: i valori reali
//  piu' vicini sono 112.1 / 224.2 / 448.4 Hz. Manteniamo i nomi "logici" della
//  1.69 (ODR_100/250/500) ma odrToHz() ritorna i valori REALI, perche' tutti i
//  calcoli tempo->campioni (finestra pre-scocco, jerk) devono usare la frequenza
//  vera, non quella nominale. Default 224 Hz: buon compromesso banda/rumore per
//  cogliere l'impulso di rilascio (~40ms di finestra utile della freccia).
enum OdrSetting : uint8_t {
  ODR_100HZ = 0,   // ACC_ODR_125Hz / GYR_ODR_112_1Hz
  ODR_250HZ = 1,   // ACC_ODR_250Hz / GYR_ODR_224_2Hz  (DEFAULT)
  ODR_500HZ = 2,   // ACC_ODR_500Hz / GYR_ODR_448_4Hz
};

inline uint16_t odrToHz(OdrSetting odr) {
  switch (odr) {
    case ODR_100HZ: return 112;
    case ODR_250HZ: return 224;
    case ODR_500HZ: return 448;
    default:        return 224;
  }
}

// ----------------------------------------------------------------------------
//  ImuSample — IDENTICO alla 1.69 (30 byte, packed)
// ----------------------------------------------------------------------------
//  Lo teniamo bit-per-bit uguale alla 1.69 anche se in Fase 4a non lo
//  trasmettiamo via BLE: quando il BLE arrivera' sulla 1.83, il pacchetto burst
//  sara' gia' compatibile con l'app senza toccare nulla. I campi sono gia' nel
//  FRAME CANONICO (la trasformazione di montaggio e' applicata a monte
//  nell'imu_task, come sulla 1.69): ax=verticale/gravita', ay=laterale,
//  az=asse freccia/scocco.
struct __attribute__((packed)) ImuSample {
  uint16_t seq;           // numero sequenziale (per anti-doppio-conteggio nel trigger)
  uint32_t timestamp_us;  // microsecondi da boot (esp_timer_get_time())
  float    ax;            // [m/s^2] verticale / gravita'
  float    ay;            // [m/s^2] laterale (dx/sx)
  float    az;            // [m/s^2] asse freccia / scocco
  float    gx;            // [deg/s]
  float    gy;            // [deg/s]
  float    gz;            // [deg/s]
};
static_assert(sizeof(ImuSample) == 30, "ImuSample deve essere 30 byte (compat 1.69)");

// ----------------------------------------------------------------------------
//  Parametri di default del TRIGGER (dal campo, 1.69 v2.12.6)
// ----------------------------------------------------------------------------
//  Questi valori sono quelli COLLAUDATI sulla 1.69, non inventati. Il trigger
//  cerca lo spike dell'impulso di rilascio sull'asse freccia (az). Vedi
//  trigger.h per il razionale fisico completo.
enum TriggerMode : uint8_t {
  TRIG_AZ_THRESHOLD = 0,   // |az| oltre soglia per N campioni (STORICA — vedi nota)
  TRIG_JERK         = 1,   // derivata di |az| oltre soglia (piu' selettivo)
  TRIG_ACCEL_DEV    = 2,   // | |a| - g | oltre soglia — DEFAULT dal 25/07
};

// ----------------------------------------------------------------------------
//  PERCHE' TRIG_ACCEL_DEV e non piu' |az| (misurato il 25/07, sessione reale)
// ----------------------------------------------------------------------------
//  DIFETTO DI |az|: contiene la GRAVITA'. Se la scheda si inclina (arco
//  abbassato fra un tiro e l'altro), la gravita' finisce su az e a RIPOSO |az|
//  vale gia' 7-8.5 m/s^2: basta un fremito da 2 per superare la soglia.
//  Il trigger finiva per misurare l'INCLINAZIONE, non lo scocco.
//
//  Prova dalla sessione SESS_20260725_1133 (2 tiri veri + 5 falsi allarmi):
//     falsi:  az a riposo -7.5..-8.5 (gravita' su az)  movimento reale 1.2-3.0
//     veri:   az a riposo -2.5       (scheda dritta)   movimento reale 16-17.4
//  I 5 falsi non avevano QUASI MOVIMENTO: solo inclinazione.
//
//  SOLUZIONE: | |a| - g |, il modulo del vettore accelerazione meno la
//  gravita'. A riposo vale ~0 QUALUNQUE sia l'inclinazione: e' invariante
//  all'orientamento. Ed e' anche invariante all'ASSE, quindi non importa se
//  l'energia dello scocco finisce su ax, ay o az (sulla 1.83 finisce
//  prevalentemente su ax, non su az come sulla 1.69: altro motivo per cui la
//  soglia su |az| non funzionava).
//
//  SEPARAZIONE MISURATA (5 tiri veri + 5 falsi, due sessioni indipendenti):
//     tiri veri : 8.5 · 8.6 · 14.3 · 16.0 · 17.4
//     falsi     : 1.3 · 2.2 · 2.8 · 2.9 · 3.0
//  Soglia 6 -> 5/5 veri presi, 0/5 falsi, zero eventi spuri sui log continui.
//  Margine 2x sopra i falsi e 1.4x sotto il tiro piu' debole.
static constexpr TriggerMode TRIG_DEFAULT_MODE      = TRIG_ACCEL_DEV;

// ----------------------------------------------------------------------------
//  GATE DI POSTURA — l'arco dev'essere in posizione di tiro
// ----------------------------------------------------------------------------
//  Il criterio | |a|-g | elimina i falsi da INCLINAZIONE STATICA, ma non quelli
//  da MANEGGIO: appoggiare l'arco, spostarlo, urtarlo produce movimento vero e
//  farebbe scattare il trigger. Qui entra la postura.
//
//  MISURA (19 eventi, 2 sessioni del 25/07, montaggi frontale E -90):
//     11 tiri veri : inclinazione 13.0 - 18.5 gradi  (arco impugnato e in mira)
//      8 falsi     : 25.5 · 46.3 · 53.9 · 54.0 · 57.5 · 65.3 · 80.2 · 91.3
//  L'arco in posizione di tiro sta entro ~20 gradi dalla verticale; quando lo si
//  maneggia e' molto piu' inclinato.
//
//  IMPLEMENTAZIONE: si confronta la direzione della GRAVITA' STIMATA (media
//  esponenziale lenta del vettore accelerazione, tau ~1s) con l'asse ax
//  (verticale nel frame canonico). Il coseno dell'angolo dev'essere sopra
//  questa soglia. NB: funziona con QUALSIASI montaggio, perche' mount_apply
//  normalizza sempre ax = verticale (verificato: frontale e -90 danno entrambi
//  ax a riposo ~+9.6).
//
//  VALORE: 0.85 = 31.8 gradi. Margine ampio sui tiri reali (max 18.5) per
//  coprire alzi maggiori a distanze lunghe. Con 0.88 (28.4 gradi) sarebbe stato
//  respinto anche l'ultimo falso residuo (29.6 gradi), ma il margine sui tiri
//  in salita si assottiglierebbe: si puo' alzare se i falsi da maneggio
//  dovessero dare fastidio.
static constexpr float TRIG_POSTURE_GATE = 0.85f;   // cos(angolo max dalla verticale)

// Costante di tempo della stima di gravita' (media esponenziale). 1 secondo:
// abbastanza lenta da non essere "trascinata" dallo scocco (~50ms), abbastanza
// veloce da seguire l'arciere che alza e abbassa l'arco fra un tiro e l'altro.
static constexpr float TRIG_GRAVITY_TAU_S = 1.0f;
static constexpr float       TRIG_DEFAULT_THRESHOLD = 4.0f;   // m/s^2 su | |a|-g | — vedi nota
// ----------------------------------------------------------------------------
//  SOGLIA 4.0: perche' proprio li' (misurato il 25/07, sessione 12:07)
// ----------------------------------------------------------------------------
//  La misura decisiva e' il movimento durante TRAZIONE e MIRA — l'unica fase in
//  cui il gate di postura non protegge (l'arco e' gia' in posizione di tiro):
//
//     rumore in trazione/mira (9 tiri) : 0.95 - 1.40 m/s^2
//     scocco piu' debole misurato      : 6.1 m/s^2   (montaggio -90)
//     scocco tipico                    : 9.8 - 15.7  (frontale)
//
//  Fra i due c'e' un divario di 4.4x. La soglia 4.0 sta 2.9x sopra il rumore di
//  trazione e 1.5x sotto il tiro piu' debole: margine su entrambi i lati.
//
//  PERCHE' NON 6.0 (valore precedente): un tiro reale a -90 ha misurato ESATTAMENTE
//  6.1, cioe' margine +0.1. Un tiro appena piu' debole finiva sotto e veniva
//  perso — ed e' con ogni probabilita' quel che e' successo al tiro mancante.
//
//  NB MONTAGGIO: il frontale da un segnale piu' forte del -90 (mediana 11.8
//  contro 9.5, cioe' 1.2x). Se si vuole margine massimo, il frontale e' meglio.
// ----------------------------------------------------------------------------
//  SOGLIA: perche' 10 e non piu' 20 (misurato sul campo il 25/07)
// ----------------------------------------------------------------------------
//  Il log continuo della 1.83 (firmware raw_diag, nessun trigger di mezzo) ha
//  misurato la firma REALE di tiri veri e scuotimenti su QUESTA scheda:
//
//    tiri veri (3):   picco |az| = 11.6 · 12.8 · 16.4 m/s^2
//    scuotimenti (9): picco |az| = 15.2 ... 29.9 m/s^2
//    rumore di fondo: p99 = 9.6 m/s^2
//
//  Con la vecchia soglia di 20, NESSUN tiro vero la superava (max 16.4) mentre
//  gli scuotimenti si': ecco perche' "a mano funziona, con la freccia no".
//
//  PERCHE' la 1.83 vede meno della 1.69 (che aveva picchi |az| ~38): il chip
//  della 1.83 e' piu' vicino al perno di rotazione dell'arco. Prova nei dati:
//  a parita' di gesto la 1.83 misura META' accelerazione lineare ma PIU'
//  rotazione (gy 67 dps contro 42). Su un corpo rigido la velocita' angolare e'
//  la stessa ovunque, l'accelerazione lineare cresce con la distanza dal perno.
//
//  MARGINE: 10 prende tutti e 3 i tiri misurati (il piu' debole a 11.6) con 3
//  falsi positivi in ~5 minuti di manipolazione intensa. E' un margine STRETTO
//  (16% sul tiro piu' debole) e basato su soli 3 tiri: da riconfermare con una
//  sessione piu' ampia. Un montaggio piu' rigido o piu' lontano dal perno
//  alzerebbe il segnale e restituirebbe margine.
//
//  NB: dal 25/07 questo valore e' DAVVERO operativo: config_apply() chiama
//  trigger_configure(). Prima la soglia del config non arrivava al trigger.
static constexpr uint8_t     TRIG_DEFAULT_CONFIRM_N = 2;      // campioni consecutivi
static constexpr uint16_t    TRIG_DEFAULT_REARM_MS  = 2000;   // ~30 colpi/min max

// ----------------------------------------------------------------------------
//  FASE 4b — FINESTRA DEL BURST e CIRCULAR BUFFER
// ----------------------------------------------------------------------------
//  Il buffer circolare in PSRAM tiene sempre in memoria gli ultimi N campioni.
//  Sul trigger "congela" una finestra PRE (prima dello scocco) + POST (dopo),
//  che diventa il burst da cui la Fase 4c estrarra' gli angoli e che in futuro
//  finira' su microSD / BLE.
//
//  VALORI REALI DAL CAMPO (CSV BBarch_13JULf, ODR misurato 224.9 Hz):
//    pre  = 2000 ms  -> 448 campioni   (trigger_idx=448 in ogni tiro)
//    post = 1000 ms  -> 224 campioni
//    tot  = 672 campioni  (< CIRCULAR_BUFFER_SIZE=750, margine 78)
//
//  NOTA: i configDefaults() hard-coded nel sorgente 1.69 dicono 3000/350, ma
//  quei default sono SOVRASCRITTI: l'app HTML in fase di config scrive nella
//  Config (BLE->NVS) i valori reali 2000/1000, ed e' quello che il firmware usa.
//  Il CSV di un tiro vero e' l'arbitro: 448 pre / 224 post = 2000/1000 ms. Ho
//  corretto qui di conseguenza ("i dati comandano").
//
//  PERCHE' pre=2000 ms: la Fase 4c cerca una FINESTRA DI STABILITA' pre-scocco
//  (arco fermo in mira) facendola scorrere nell'intervallo pre-trigger. 2s
//  coprono ampiamente il tempo di mira di un barebow.
//
//  PERCHE' post=1000 ms (non 350!): le metriche post-scocco (follow-through,
//  recoil, vib_hz, drift) si calcolano sull'OSCILLAZIONE del riser DOPO il
//  rilascio. Con 350 ms si taglierebbe la coda smorzata; 1000 ms la cattura
//  tutta. Il post lungo NON e' spreco: e' il dato biomeccanico del rilascio.
//
//  NB: pre_ms + post_ms (in campioni all'ODR reale) NON devono superare
//  CIRCULAR_BUFFER_SIZE. A 224Hz: 672 < 750 (ok). A 448Hz sforerebbe (1344):
//  il clamp difensivo in circular_buffer_freeze() lo gestisce.
static constexpr uint16_t WINDOW_PRE_MS  = 2000;   // finestra pre-scocco [ms]
static constexpr uint16_t WINDOW_POST_MS = 1000;   // finestra post-scocco [ms]

// ----------------------------------------------------------------------------
//  FASE 19 — raggi del cerchio d'impatto, in centimetri sul bersaglio
// ----------------------------------------------------------------------------
//  Sono PARAMETRI e non costanti compilate per una ragione precisa: il valore
//  giusto e' una stima "a occhio" che dipende dai gruppi di sagome che si
//  frequentano, e cambiera'. Se cambiasse ricompilando, le sedute vecchie
//  diventerebbero illeggibili — perche' un numero in centimetri senza il raggio
//  con cui e' stato prodotto non ha scala. Stando in config finiscono in
//  session.txt e ogni seduta resta interpretabile per sempre.
//
//  50 cm di raggio (100 di diametro) copre grosso modo sagoma+spot delle
//  sagome 3D piu' comuni; 150 cm di raggio e' la zona utile dei mancati.
static constexpr uint16_t IMP_RAGGIO_COLPITO_CM_DEFAULT = 50;
static constexpr uint16_t IMP_RAGGIO_MANCATO_CM_DEFAULT = 150;

static constexpr uint16_t CIRCULAR_BUFFER_SIZE = 750;  // campioni in PSRAM (~22KB)

// Conversione ms->campioni all'ODR REALE (non nominale): i calcoli tempo->indice
// devono usare la frequenza vera del QMI8658, altrimenti la finestra e' storta.
inline uint16_t calcPreSamples(OdrSetting odr)  {
  return (uint16_t)((float)WINDOW_PRE_MS  / 1000.0f * odrToHz(odr));
}
inline uint16_t calcPostSamples(OdrSetting odr) {
  return (uint16_t)((float)WINDOW_POST_MS / 1000.0f * odrToHz(odr));
}

// ----------------------------------------------------------------------------
//  MONTAGGIO DELL'IMU sul riser (FASE 4c)
// ----------------------------------------------------------------------------
//  La 1.83 e' una scheda FISICAMENTE diversa dalla 1.69: stesso QMI8658 ma
//  altro PCB, altro verso di saldatura, pannello gia' ruotato via software
//  (setRotation(0)+offset). Quindi NON e' garantito che gli assi elettronici
//  del sensore coincidano col frame canonico su cui shot_angles e' stato
//  validato (ax=gravita', ay=laterale, az=freccia).
//
//  mount_apply() (modulo mount.h, portato dalla 1.69) rimappa gli assi grezzi
//  del sensore nel frame canonico A MONTE (in imu_task, prima di trigger,
//  buffer e angoli): da li' in poi TUTTO il firmware vede il frame canonico e
//  shot_angles gira invariato. E' l'architettura della 1.69.
//
//  QUALE MONTAGGIO? — MISURATO col DIAG angoli il 19/07 (test statico, arco
//  fermo su supporto, |g|~9.98). Risultato:
//    - cant a destra -> ay POSITIVO  (gia' corretto in frame canonico)
//    - punta su      -> az NEGATIVO  (INVERTITO: dovrebbe essere positivo)
//
//  CARATTERIZZAZIONE FISICA (22/07, pose di gravita' note) — conclusione:
//    In posizione 0 gli assi hanno i RUOLI GIUSTI (X=verticale/grav,
//    Y=laterale/cant, Z=asse-freccia/alzo): NESSUNO scambio di assi. L'unico
//    problema e' che l'asse Z del chip 1.83 e' orientato all'OPPOSTO della
//    convenzione (punta su -> az negativo). Non e' un montaggio a se': e' una
//    proprieta' HARDWARE della scheda. Percio' il flip-Z e' ora un PRE-PASSO in
//    mount_apply (ARCHBB_HW_FLIP_Z, mount.h), applicato SEMPRE prima della
//    rotazione. Le 4 rotazioni tornano quelle PURE della 1.69, e il default e'
//    di nuovo MOUNT_0 (il flip lo mette mount_apply, non l'orientamento).
//    VERIFICATO equivalente al vecchio MOUNT_0_FLIPZ: stessi angoli, nessuna
//    regressione sui dati gia' raccolti.
//
//  REGOLA (§4 handoff): i segni si validano SOLO su test statico ad angolo noto.
static constexpr uint8_t ARCHBB_MOUNT_DEFAULT = 0;  // MOUNT_0 (il flip-Z lo applica mount_apply)

// ----------------------------------------------------------------------------
//  GEOMETRIA DEL PANNELLO (fatto hardware)
// ----------------------------------------------------------------------------
//  Sta QUI e non in display.h perche' e' un fatto HARDWARE, come i pin e gli
//  indirizzi I2C: non e' proprieta' del driver di disegno. Anche il driver del
//  TOUCH deve conoscerlo (per il clamp delle coordinate corrette), e non ha
//  senso che per saperlo debba includere display.h — e con esso tutto TFT_eSPI.
//
//  display.h include config.h ed eredita queste costanti: un solo posto dove
//  sono scritte, come per KEY_Y/BTN_TOP nello scoring. Stessa lezione (§4.1):
//  se un fatto e' scritto in due posti, prima o poi divergono.
//
//  1.83 = 240x284 (la 1.69 era 240x280). L'offset Y=20 e' il rowstart del
//  ST7789V2 su questo pannello, applicato via CGRAM_OFFSET+setRotation().
static constexpr int16_t ARCHBB_W        = 240;   // larghezza visibile
static constexpr int16_t ARCHBB_H        = 284;   // altezza visibile (era 280 su 1.69)
static constexpr int16_t ARCHBB_OFFSET_Y = 20;    // ritaglio verticale (rowstart)
static constexpr int16_t ARCHBB_OFFSET_X = 0;     // nessun offset orizzontale

// ============================================================================
//  CALIBRAZIONE DEL TOUCH (1.83)
// ============================================================================
//  IL SINTOMO (osservato sul campo con lo scoring DIST+ELEV)
//    I tasti "-" e "+" (48px) rispondono male, e solo sul bordo verso il centro.
//    Gli slider (192px) funzionano benissimo. Il RIEPILOGO e la ZONA (bersagli
//    grandi) non hanno mai dato problemi.
//
//    Questo NON e' un bug della schermata nuova: e' una discrepanza fra le
//    coordinate del CST816 e quelle del display, presente da sempre e MASCHERATA
//    dai bersagli grandi. I tasti piccoli l'hanno solo resa visibile. E' la
//    stessa identica diagnosi della 1.69 (§4.5 del compendio), dove le celle
//    ZONA da 66px nascondevano cio' che i tasti da 32px rivelavano.
//
//  DOVE SI APPLICA LA CORREZIONE (la decisione importante)
//    Nel DRIVER, dentro touchRead(), UNA VOLTA SOLA. Da li' in poi TUTTO il
//    firmware vede coordinate-display e nient'altro.
//
//    L'alternativa — correggere in ogni handler — sarebbe un disastro: N punti
//    da tenere allineati a mano, e ogni schermata futura una nuova occasione di
//    dimenticarsene. Il bug e' latente proprio perche' le soglie sono empiriche
//    e sparse: la cura non puo' essere aggiungerne altre. Un solo punto di
//    verita', il piu' a monte possibile.
//    (NB: la 1.69 applica la correzione in scoring_ui.cpp, NON nel driver. La
//    sua stessa documentazione dice che il posto giusto e' il driver: qui
//    seguiamo la lezione, non l'implementazione.)
//
//  COME SI OTTENGONO QUESTI NUMERI — e come NON si ottengono
//    Con touch_cal.h: 5 crocette a coordinata NOTA, si misura DI QUANTO SBAGLIA
//    la catena completa, si fitta l'errore ai minimi quadrati.
//
//    NON si deducono a tavolino. Sulla 1.69 questo e' costato SEI versioni:
//    era stato dedotto "il sensore e' piu' stretto del 28%" dagli span rx
//    31..203 letti in diagnostica -> AX=1.38953. Ma quegli span non erano i
//    bordi del sensore: erano DOVE L'UTENTE AVEVA TOCCATO. Il coefficiente
//    inventato ha CAUSATO il disallineamento che poi si cercava di correggere.
//    Misurato per bene, l'asse X era gia' allineato (AX=1.006).
//
//    REGOLA: gli span misurano l'utente, non il chip. I bersagli a coordinata
//    nota misurano il chip. Si calibra solo cosi'.
//
//  VALORI INIZIALI = IDENTITA'
//    Partiamo con la trasformazione neutra (a=1, b=0): il firmware si comporta
//    esattamente come adesso. NON mettiamo i coefficienti della 1.69: sono di
//    un ALTRO pannello, con altri pin e altra meccanica. Copiarli sarebbe
//    ripetere l'errore di usare un numero non misurato su questo hardware.
//    Si esegue la calibrazione e si ricopiano qui i numeri che escono.
// COEFFICIENTI MISURATI da Cesare con la calibrazione a 5 punti (2 passate, stilo).
// Verificati: applicati ai centri dei tre tasti li riportano tutti in zona
// (- -> 40, ? -> 114, + -> 193). La X e' espansa ~1.34x (AX=0.746), la Y ~1.16x.
// Il "non ripetibile" che la calibrazione segnalava (spread 3.1%) era un FALSO
// ALLARME: soglia troppo severa (3%), ora alzata a 5% - per un tocco a mano
// libera con lo stilo un residuo del 3% e' del tutto normale.
static constexpr float TOUCH_CAL_AX = 0.74591f;
static constexpr float TOUCH_CAL_BX = 21.84f;
static constexpr float TOUCH_CAL_AY = 0.86208f;
static constexpr float TOUCH_CAL_BY = 21.68f;

//  Se un domani si volesse tornare all'identita' (touch grezzo, per ri-diagnosi):
//    static constexpr float TOUCH_CAL_AX = 1.0f;
//    static constexpr float TOUCH_CAL_BX = 0.0f;
//    static constexpr float TOUCH_CAL_AY = 1.0f;
//    static constexpr float TOUCH_CAL_BY = 0.0f;

// ----------------------------------------------------------------------------
//  DEBUG — coerente con la disciplina 1.83: mai Serial nel loop/hot-path.
// ----------------------------------------------------------------------------
//  Su ESP32-S3 con USB CDC nativo (HWCDC) il seriale col monitor chiuso puo'
//  bloccare i task (bug noto del core). Percio' i DBG sono attivi SOLO se
//  ARCHBB_DEBUG e' definito, e comunque best-effort (setTxTimeoutMs(0) nel main).
//  In produzione (ARCHBB_DEBUG assente) diventano no-op a costo zero.
#ifdef ARCHBB_DEBUG
  #define DBG(fmt, ...)  Serial.printf("[ARCHBB] " fmt "\n", ##__VA_ARGS__)
#else
  #define DBG(fmt, ...)
#endif

// ============================================================================
//  FASE 24a — ENERGIA (risparmio) e SEGNO DELL'ELEVAZIONE
// ============================================================================
//  PERCHE' ESISTE QUESTA SEZIONE
//    Sessione di campo del 12/09: percorso tecnico, oltre due ore, la scheda si
//    e' spenta prima della fine. Un percorso di campagna a 24 piazzole dura
//    4-5 ore: l'autonomia di progetto non e' "piu' di adesso", e' SEI ORE.
//
//  DOVE VA L'ENERGIA (stime da datasheet, DA MISURARE — v. ENERGIA.CSV)
//    retroilluminazione 100%   ~40-60 mA   <- il candidato numero uno
//    ESP32-S3 @240MHz attivo   ~35-45 mA
//    logica pannello ST7789     ~5 mA
//    IMU QMI8658 a+g 250Hz      ~1.5 mA
//    microSD a riposo           ~0.3 mA (a raffiche in scrittura)
//    LED di carica del PMU       ~2-5 mA  <- acceso per niente, in campo
//    Il totale stimato (~100-130 mA) su una cella da 400 mAh da' 3-4 ore
//    TEORICHE, che diventano due reali per il sag di tensione sotto carico.
//
//  I PARAMETRI SONO COSTANTI DI COMPILAZIONE, NON NVS — per ora.
//    Mettere subito in NVS dei valori che non sappiamo ancora se sono giusti
//    significherebbe un bump di CONFIG_VERSION (che azzera i parametri, non la
//    calibrazione) per ogni ripensamento. Prima si misura in campo, poi si
//    promuovono i valori vincenti a parametro persistente. Stessa disciplina
//    del bersaglio armato: in RAM finche' non e' chiaro che serve altrove.
// ----------------------------------------------------------------------------

// Livello base della retroilluminazione in percentuale (0..100). Non e' un
// "quanto ti piace": e' l'unico numero che pesa davvero sull'autonomia. 60 e'
// il punto di partenza — leggibile all'ombra e in penombra, da rialzare a mano
// (schermata ENERGIA) quando si tira controluce.
static constexpr uint8_t ENERGIA_BL_BASE_PCT = 60;

// Livello in PENOMBRA: la schermata resta visibile ma si e' capito che sta per
// spegnersi. E' il preavviso, non un risparmio in se'.
static constexpr uint8_t ENERGIA_BL_DIM_PCT = 25;

// Frequenza PWM della retroilluminazione.
//  20 kHz e' scelta contro lo SFARFALLIO, non a caso: la IEEE 1789-2015
//  colloca il "nessun effetto osservabile" sopra ~1.25 kHz per qualunque
//  profondita' di modulazione. A 200 Hz (valore tipico di tanti esempi) un
//  display che si muove nel campo visivo mentre si incocca produce l'effetto
//  stroboscopico proprio sul gesto che stiamo misurando. 20 kHz e' anche sopra
//  la banda udibile: niente fischio dall'induttore.
static constexpr uint32_t ENERGIA_BL_PWM_HZ  = 20000;
static constexpr uint8_t  ENERGIA_BL_PWM_BIT = 10;      // 0..1023

// Tempi di inattivita' (ms). ATTESA e' lo stato in cui si passano le ore.
static constexpr uint32_t ENERGIA_T_DIM_ATTESA_MS = 40000;    // 40 s -> penombra
static constexpr uint32_t ENERGIA_T_OFF_ATTESA_MS = 100000;   // 100 s -> spento

// Negli stati INTERATTIVI (pre-tiro, scoring, riepilogo, scelta sessione) non
// si spegne mai e si attenua molto piu' tardi: sono schermate che si GUARDANO
// mentre si pensa, e hanno gia' un timeout proprio che le chiude.
static constexpr uint32_t ENERGIA_T_DIM_INTER_MS  = 90000;    // 90 s -> penombra

// Il pannello va in sleep (comando 0x10 dello ST7789) quando la retro e' spenta.
// Vale qualche mA e toglie il fantasma grigio dello schermo acceso a nero.
// Se un giorno il risveglio dovesse dare artefatti, si azzera questo flag e
// resta solo la retro spenta (il 90% del risparmio e' li' comunque).
#define ENERGIA_PANEL_SLEEP 1

// --- Scalatura della frequenza di CPU: PREDISPOSTA, NON ATTIVA --------------
//  Scendere da 240 a 80 MHz vale ~20 mA, ed e' il secondo risparmio in ordine
//  di grandezza. NON lo attivo alla cieca: su S3 con PSRAM OCTAL il cambio di
//  frequenza a runtime tocca la temporizzazione della PSRAM, e in PSRAM ci vive
//  il buffer circolare che imu_task riempie a 224 Hz. Un burst corrotto e' il
//  danno peggiore che questo strumento possa fare — falsifica il dato senza
//  dichiararlo.
//
//  IL TEST CHE SBLOCCA QUESTO FLAG (banco, mezz'ora):
//    1) env archbb_183_shotrec con ENERGIA_CPU_SCALING=1
//    2) si registra a 80 MHz una serie di scuotimenti noti
//    3) si confronta il burst con la stessa serie a 240 MHz: se la forma
//       d'onda e' la stessa e nessun campione e' spazzatura, il flag si accende.
//  Finche' quel confronto non esiste, 240 MHz fissi e 20 mA in piu'.
//
//  MAI SOTTO GLI 80 MHz: a 40 MHz l'APB scende con la CPU e cambiano di colpo
//  le frequenze reali di SPI (display) e I2C (IMU, touch, PMU, RTC). A 80, 160
//  e 240 l'APB resta 80 MHz e non cambia niente.
#define ENERGIA_CPU_SCALING 0
static constexpr uint32_t ENERGIA_CPU_MHZ_ALTA  = 240;
static constexpr uint32_t ENERGIA_CPU_MHZ_BASSA = 80;

// Periodo di campionamento del log energetico su microSD (/ENERGIA.CSV).
// 30 s -> 120 righe/ora, ~8 KB per una giornata: irrilevante sulla card e
// sufficiente a ricostruire la curva di scarica e a datare lo spegnimento.
static constexpr uint32_t ENERGIA_LOG_MS = 30000;

// ----------------------------------------------------------------------------
//  COLORE DEL SEGNO DELL'ELEVAZIONE
// ----------------------------------------------------------------------------
//  ELEV_PALETTE = 0  ciano (giu') / ambra (su')   <- default, consigliata
//  ELEV_PALETTE = 1  verde (su') / rosso (giu')
//
//  PERCHE' NON VERDE/ROSSO DI DEFAULT — due ragioni indipendenti:
//
//   1) VISIONE DEI COLORI. Il deficit rosso-verde riguarda circa l'8% dei
//      maschi di origine europea (Birch 2012): in un gruppo di venti arcieri,
//      uno o due non distinguono quella coppia. Ciano e ambra restano separati
//      per protanopi e deuteranopi, e hanno anche luminanze diverse — cioe'
//      restano distinguibili persino in bianco e nero. E' il principio della
//      CODIFICA RIDONDANTE: il colore non deve MAI essere l'unico canale.
//      Per questo, accanto al colore, il segno c'e' comunque, disegnato, e
//      sopra c'e' una freccia: tre canali per la stessa informazione.
//
//   2) COERENZA INTERNA, che qui pesa piu' della prima. In ArchBB il verde e
//      il rosso hanno gia' un significato: GOOD = centro, BAD = fuori. Un
//      angolo di -20 gradi non e' un errore, e' una piazzola in discesa.
//      Colorarlo di rosso vuol dire insegnare all'occhio che rosso significa
//      due cose diverse a seconda di dove guarda — ed e' esattamente cosi' che
//      un colpo d'occhio smette di funzionare.
//
//  Se dopo una sessione di campo verde/rosso risultasse comunque piu' immediato,
//  si mette 1 qui e si riflasha: l'informazione resta comunque ridondata.
#define ELEV_PALETTE 0

// ----------------------------------------------------------------------------
//  FASE 24b — SOGLIE DI PREAVVISO BATTERIA (in VOLT, non in percentuale)
// ----------------------------------------------------------------------------
//  Ricavate dalla scarica completa misurata il 13/09 (4,201 V -> 3,032 V in
//  3h29m): la curva ha un plateau lungo e poi un ginocchio netto.
//     da 3,70 V restano ~1h25m
//     da 3,60 V restano ~20 min
//  Sotto il ginocchio il crollo e' rapidissimo, quindi la seconda soglia non e'
//  "fra poco": e' "adesso".
//
//  PERCHE' NON LA PERCENTUALE. L'AXP2101 la stima dalla tensione e sotto carico
//  fluttua: nello stesso file si vedono rimbalzi di parecchi punti fra righe
//  consecutive. Una soglia sulla percentuale scatterebbe e rientrerebbe a caso.
//  La tensione grezza, letta a cadenza lenta, e' l'unico segnale onesto che
//  abbiamo — e va confrontata con isteresi, mai con un semplice "<".
static constexpr float ENERGIA_V_AVVISO   = 3.70f;   // "circa un'ora"
static constexpr float ENERGIA_V_CRITICO  = 3.60f;   // "venti minuti"
