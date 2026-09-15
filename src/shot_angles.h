// =============================================================================
// shot_angles.h — ArchBB: calcolo angoli biomeccanici dal burst (v1.4)
// Firmware 2.15.0 — scoring + HOLD (follow-through) + RELEASE (pulizia rilascio)
// =============================================================================
//
// SCOPO
//   Estrarre dal burst IMU quattro grandezze biomeccaniche, ognuna su una
//   finestra temporale diversa e con un significato distinto:
//
//     [ ...mira... | SCOCCO |--40ms--|........900ms........]
//        CANT/ALZO            RELEASE          HOLD
//        (come miri)          (come rilasci)   (come tieni)
//
//     - CANT    : rollio laterale dell'arco (dx/sx)              [pre-scocco]
//     - ALZO    : elevazione dell'asse freccia                   [pre-scocco]
//     - RELEASE : brutalita' del rilascio (jerk laterale)        [0..40ms]   <-- v1.4
//     - HOLD    : tenuta del braccio dopo il rilascio            [0..900ms]  <-- v1.3
//
//   Le tre fasi sono errori DISTINTI e misurabilmente indipendenti:
//   rho(release, hold) = -0.024 su 42 tiri. Un tiro puo' avere rilascio sporco
//   e tenuta perfetta, o viceversa. Vanno letti insieme, non uno al posto
//   dell'altro.
//
// PERCHE' PRE-SCOCCO PER GLI ANGOLI D'ASSETTO
//   All'istante dello scocco l'accelerometro e' dominato dallo spike dinamico
//   dello strappo (az balza a ~+34 m/s^2), non dalla gravita': l'assetto
//   calcolato li' sarebbe falsato (un alzo reale di 12 deg verrebbe letto come
//   ~73 deg). Nella finestra pre-trigger, ad arco fermo, l'accelerometro e'
//   dominato dalla gravita' -> assetto pulito.
//
// GEOMETRIA DELLE FINESTRE
//   Non si prendono gli ULTIMISSIMI campioni prima del trigger: negli ~50ms che
//   precedono il rilascio l'arciere sta gia' "partendo" (clicker scattato, mano
//   di corda in apertura), c'e' micro-movimento. Si arretra di un GUARD GAP e si
//   media una finestra di STABILITA' precedente:
//
//     [ ...... mira stabile ...... | guard | SCOCCO |  post-scocco  ]
//              ^--- finestra ---^    ~50ms  trigger  ^--- HOLD 900ms ---^
//              (cant, alzo, bias gy)                 (integrazione gy)
//
//   Default: guard gap 50ms, finestra 200ms (a 224Hz: ~11 e ~44 campioni).
//   Entrambi ricavati dall'ODR reale, non assunti.
//
// FRAME DI RIFERIMENTO
//   I campioni del burst sono GIA' in frame CANONICO 0 deg (la trasformazione di
//   montaggio e' applicata a monte in imu.cpp). Quindi il calcolo e' IDENTICO
//   per tutti e 4 i montaggi. Convenzioni (da mount.h, ancorate a tiri reali):
//     ax = verticale / gravita'  (+ ad arco verticale)
//     ay = laterale dx/sx        (base del cant)
//     az = asse freccia / scocco
//   cant = atan2(ay, ax)   ->  +dx / -sx  ;  0 ad arco verticale
//   alzo = atan2(az, ax)   ->  +su / -giu ;  0 ad asse freccia orizzontale
//
// QUALITA' DELLA MISURA
//   La funzione restituisce anche la DISPERSIONE (deviazione standard del modulo
//   dell'accelerazione) nella finestra: bassa = arco fermo, misura affidabile;
//   alta = arciere in movimento, angolo meno attendibile.
//
// =============================================================================
// v1.3 — METRICA HOLD (follow-through)   [validata su 42 tiri, sessione 14JUL]
// =============================================================================
//
// COS'E'
//   hold_deg = variazione dell'angolo di ELEVAZIONE a +900ms dallo scocco,
//              rispetto all'assetto di mira.
//     negativo = il braccio si e' ABBASSATO (follow-through mollato)
//     ~zero    = assetto TENUTO
//
// PERCHE' SI INTEGRA IL GIROSCOPIO E NON SI USA atan2(az,ax)
//   Dopo il rilascio l'accelerometro e' sporcato dal rinculo del riser
//   (oscillazione di ampiezza >> g): atan2 diventa inutilizzabile nel post.
//   Il giroscopio misura la velocita' angolare: il rinculo la perturba ma non la
//   satura (picco misurato |gy| ~50 dps nei primi 40ms, fondo scala 512).
//   Integrando gy si ricostruisce l'angolo relativo. L'integrazione e' l'UNICA
//   strada percorribile nel post-scocco.
//
// IL BIAS DEL GIROSCOPIO E LA FINESTRA CALMA (principio ZUPT)
//   Un giroscopio MEMS ha un offset a riposo (bias) che, integrato, produce
//   deriva. Lo si stima nella FINESTRA CALMA pre-scocco: li' l'arco e' fermo,
//   quindi la velocita' angolare VERA e' zero e tutto cio' che il sensore legge
//   E' il bias. Lo si sottrae prima di integrare.
//     -> E' il pattern ZUPT (Zero-velocity UPdaTe) della letteratura inerziale:
//        sfruttare ogni fase di stasi NOTA per riazzerare l'integratore.
//     -> Vantaggio su una calibrazione all'avvio (es. il COD del QMI8658): la
//        stima e' rifatta a OGNI tiro, quindi compensa automaticamente la
//        deriva termica. Il bias MEMS dipende dalla temperatura: calibrare a
//        freddo e poi tirare al sole lascerebbe una calibrazione stale.
//        (In piu' il COD del QMI8658 calibra il GAIN, non l'offset: non
//        risolverebbe comunque questo problema.)
//     -> Costo zero: la finestra calma e' GIA' cercata per cant/alzo, e i
//        campioni sono GIA' nel circular buffer.
//
//   MISURA sull'esemplare in uso (42 tiri, finestra calma, |g|=10.14 sd=0.175):
//     bias gx = +4.27 dps | bias gy = +0.42 dps | bias gz = +4.18 dps
//   gx e gz hanno un offset di fabbrica ~10x quello di gy (t-test gx vs gz:
//   p=0.82, indistinguibili tra loro). NON deriva nel tempo (45 min di
//   sessione: p=0.26/0.57/0.19, tutti stabili) -> e' offset costante, non
//   termico. Impatta gx/gz, NON gy: hold_deg (che usa gy) non ne soffre.
//
// IL dt VA PRESO DAL TIMESTAMP REALE, NON DALL'ODR NOMINALE
//   MISURA (sessione 14JUL, 44 burst): il campionamento NON e' a passo costante,
//   alterna 4005 e 5005 us. E' il tick FreeRTOS da 1ms che batte contro il
//   periodo ideale di 4448us: il polling di getDataReady() non puo' cadere a
//   meta' tick. La MEDIA e' esattamente l'ODR nominale (224.91 Hz; sd della
//   durata burst 0.5ms su 44 tiri -> nessuna deriva accumulata), ma il singolo
//   dt oscilla del +-11%. Integrare con dt costante accumulerebbe errore.
//   -> si usa timestamp_us campione per campione. E' gia' dentro ImuSample:
//      costo zero.
//
// SOGLIE — TARATE SUL RUMORE MISURATO, NON SCELTE A TAVOLINO
//   Il rumore di integrazione e' stato QUANTIFICATO con un test a vuoto sul dato
//   reale: si integra gy per ~1s DENTRO la finestra pre-scocco (arco fermo ->
//   angolo vero = 0), e tutto cio' che si legge E' l'errore.
//     RISULTATO (42 tiri): |deriva| media 1.03 deg, p95 2.10 deg, max 2.45 deg.
//   Una soglia "tenuto" a 3 deg (proposta iniziale) starebbe DENTRO il rumore:
//   il 70% del budget se lo mangerebbe lo strumento. Le soglie sono quindi
//   ancorate al p95 misurato:
//     TENUTO    |hold| <= 4 deg   (~2x p95)
//     LIEVE     4 < |hold| <= 8   (zona grigia)
//     ABBASSATO |hold| > 8 deg    (~4x p95 -> zero falsi positivi)
//
//   NOTA: allungare la finestra di stima del bias NON aiuta. Misurato:
//     bias su 249ms -> p95 2.81 | su 996ms -> p95 2.10 | bias globale di
//     sessione (42 tiri, stima teoricamente imbattibile) -> p95 1.89.
//   Cioe': il bias NON e' il collo di bottiglia, il RUMORE lo e' (sd intra-
//   finestra ~1.9 dps). ~2 deg e' il floor fisico del QMI8658 a 512dps su 1s.
//   Rimuovere il bias guadagna solo 1.4x. Non c'e' trucco di calibrazione che
//   scenda sotto: e' fisica del MEMS.
//
//   *** QUANTO SONO SOLIDE LE DUE SOGLIE (misurato sui 42 tiri) ***
//   Le due soglie NON hanno la stessa affidabilita', e la differenza va capita
//   prima di fidarsi di una classificazione:
//
//     soglia 8 (LIEVE|ABBASSATO) -- SOLIDA
//       il gap fra il "tenuto" peggiore (-3.83) e l'"abbassato" migliore
//       (-8.85) e' 5.02 deg = 2.4x il p95 del rumore. Nessuna sovrapposizione.
//       La distinzione TENUTO vs ABBASSATO -- l'unica che conta per la
//       diagnosi -- e' affidabile.
//
//     soglia 4 (TENUTO|LIEVE) -- FRAGILE PER COSTRUZIONE
//       21 tiri su 42 cadono nella fascia [1.9 .. 6.1], cioe' entro +-p95 dalla
//       soglia: per quei tiri il rumore PUO' cambiare la classe. Non e' un
//       difetto da correggere, e' una proprieta' del dato: la distribuzione ha
//       il picco proprio li' (24 tiri fra 0 e 4 deg).
//       -> LIEVE va letto come "zona grigia", non come un verdetto. Un tiro a
//          -4.2 e uno a -3.8 sono lo stesso tiro, misurato due volte.
//       -> Per confronti fini fra tiri della stessa classe, usare il VALORE
//          numerico, non l'etichetta. Per questo il display mostra entrambi.
//
//   COROLLARIO PRATICO: se due esecuzioni della stessa analisi (es. firmware vs
//   script offline) classificano diversamente un tiro vicino ai 4 deg, NON e'
//   necessariamente un bug: e' il rumore. Verificare sul GAP, non sui confini.
//
// PERCHE' hold_deg E NON LA PENDENZA
//   La firma temporale osservata dice che lo strappo iniziale NON discrimina
//   (+2/+3 deg nei primi 30-40ms in TUTTI i tiri: e' il riser che reagisce al
//   rilascio, fisiologico), mentre discrimina cio' che accade DOPO i ~150ms.
//   Confermato: Mann-Whitney tenuti vs abbassati -> strappo a 30ms p=0.086
//   (non discrimina), pendenza 150-900ms p=0.0001 (discrimina).
//   Verrebbe quindi da misurare la PENDENZA. Ma il dato dice di no:
//     hold_deg : tenuti [-3.9 .. +2.4] | abbassati <= -8.5  -> GAP PULITO
//     pendenza : tenuti fino a -13.2   | abbassati fino a -3.8 -> SOVRAPPONE
//   Le due correlano r=0.892: la pendenza non aggiunge informazione, aggiunge
//   falsi positivi. Esempio concreto: il tiro 15 aveva pendenza -13.2 dps
//   (peggiore di 4 "abbassati") ma hold_deg -2.7: era sceso e RIENTRATO.
//   Conta dove sei alla fine, non come ci sei arrivato.
//   -> si implementa hold_deg. La pendenza resta diagnostica secondaria.
//
// IL QUADRANTE DIAGNOSTICO (perche' hold serve PRIMA dello scoring)
//   Avere hold a display PRIMA di dichiarare l'esito da' la diagnosi:
//     mancato + TENUTO     -> errore di stima distanza / drop  (gesto ok)
//     mancato + ABBASSATO  -> errore di esecuzione             (gesto no)
//     colpito + ABBASSATO  -> fortuna / compensazione          (non replicabile)
//     colpito + TENUTO     -> tiro buono                       (replicabile)
//
// STATO DI VALIDAZIONE
//   SNR misurato 6.1x (gap tenuti/abbassati 12.85 deg contro rumore p95 2.10).
//   Separazione delle classi pulita, zero sovrapposizione.
//   Incrocio con esito: 5/5 TENUTI -> colpito, 1/1 ABBASSATO -> mancato, ma su
//   soli 11 tiri con esito registrato e 2 soli mancati: la direzione e' quella
//   attesa, ma NON e' statisticamente conclusivo. Serve una sessione con esito
//   su TUTTI i tiri. I dati comandano.
// =============================================================================

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include "config.h"   // ImuSample, odrToHz, OdrSetting

// Parametri di default della finestra (in ms); convertiti in campioni con l'ODR.
static constexpr uint16_t SHOT_ANGLE_GUARD_MS  = 50;   // gap minimo prima del trigger
static constexpr uint16_t SHOT_ANGLE_WINDOW_MS = 200;  // ampiezza finestra di media

// ----------------------------------------------------------------------------
//  FASE 24b.1 — FRESCHEZZA MINIMA DEL PRE-TRIGGER
// ----------------------------------------------------------------------------
//  Sotto questa durata di campioni freschi, gli angoli NON si calcolano affatto.
//
//  IL RAGIONAMENTO, che non e' una percentuale scelta a occhio.
//    La finestra adattiva esiste per un motivo preciso, documentato quando fu
//    introdotta: negli ultimi ~200 ms prima del trigger l'arciere STA GIA'
//    PARTENDO, e una finestra fissa li' dentro dava errori di 13-40 gradi. La
//    soluzione fu poter scorrere all'indietro fino a due secondi, per trovare
//    un momento in cui l'arco era davvero fermo.
//
//    Quando la parte fresca e' corta, il problema non e' che la ricerca ha meno
//    scelte: e' che le scelte rimaste stanno TUTTE nella zona che la ricerca
//    doveva evitare. La parte fresca e' per costruzione adiacente al trigger.
//    Con 91 campioni (0,42 s, tiro 2 del 14/09) si cerca la calma esattamente
//    dove l'arciere e' gia' in movimento — e si trova sempre qualcosa, perche'
//    il minimo di una funzione esiste comunque. Verrebbe fuori un numero, e
//    sarebbe il numero sbagliato.
//
//    Un secondo: meta' della finestra di progetto (2 s) e cinque volte la zona
//    "sta gia' partendo". E' in MILLISECONDI e non in campioni ne' in frazione
//    di trigger_idx, cosi' resta valida se cambiano l'ODR o le finestre.
//
//  IL PREZZO, dichiarato. Un tiro VERO scoccato entro un secondo dalla conferma
//  del riepilogo precedente perde gli angoli (il burst resta, intero: si
//  rinuncia a interpretarlo, non a registrarlo). In campo, fra un tiro e il
//  successivo si cammina fino alla piazzola dopo: scoccare entro un secondo dal
//  tap OK non e' un tiro, e' la scheda che viene rimessa a posto. Costo reale
//  vicino a zero, in cambio dell'eliminazione di una classe di falsificazione
//  silenziosa.
static constexpr uint16_t SHOT_ANGLE_PRE_MIN_MS = 1000;

// Soglie di STABILITA' della finestra (tarate sul test statico 13/07f).
// La finestra adattiva sceglie, fra tutte le posizioni pre-trigger, quella col
// vettore accelerazione piu' vicino alla sola gravita' (|a|~g, dispersione bassa):
// li' l'accelerometro misura l'assetto, non il movimento. Se nemmeno la migliore
// e' abbastanza pulita, l'angolo e' marcato non-stabile (stable=false).
//   Dai dati: finestre buone |g| in [9.9..10.3] sd<0.5 ; finestre nel movimento
//   |g|>11 sd>2. Soglie con margine:
static constexpr float SHOT_ANGLE_G_TOL_MS2  = 1.5f;   // |mean|a| - g| ammesso
static constexpr float SHOT_ANGLE_SD_MAX_MS2 = 1.0f;   // dispersione |a| ammessa
static constexpr float SHOT_ANGLE_GRAVITY    = 9.81f;  // g di riferimento

// -----------------------------------------------------------------------------
// v1.3 — PARAMETRI METRICA HOLD
// -----------------------------------------------------------------------------
// Orizzonte di misura del follow-through: 900ms dopo lo scocco. Scelto perche':
//   - la finestra post del burst e' 1000ms (misurata: 224 campioni @224.9Hz),
//     quindi 900 sta dentro con margine per il jitter di campionamento;
//   - a 900ms la coda smorzata del riser e' esaurita e l'assetto e' quello
//     "finale" del braccio.
static constexpr uint16_t SHOT_HOLD_MS = 900;

// Soglie di classificazione [deg], ancorate al p95 del rumore misurato (2.10):
static constexpr float SHOT_HOLD_TENUTO_DEG    = 4.0f;   // ~2x p95
static constexpr float SHOT_HOLD_ABBASSATO_DEG = 8.0f;   // ~4x p95

// Numero minimo di campioni post-scocco perche' hold sia calcolabile.
// Sotto questa soglia l'integrazione e' troppo corta per significare qualcosa.
static constexpr uint16_t SHOT_HOLD_MIN_SAMPLES = 20;

// -----------------------------------------------------------------------------
// v1.4 — PARAMETRI METRICA RELEASE (pulizia del rilascio)
// -----------------------------------------------------------------------------
// COS'E'
//   release_jerk = massimo |d(ay)/dt| nei ~40ms in cui la freccia e' ancora
//                  sulla corda. Misura quanto BRUSCAMENTE la mano di corda
//                  spinge lateralmente il riser al rilascio ("plucking").
//   Unita': m/s^3. Alto = strappo, basso = rilascio pulito.
//
// PERCHE' 40ms E NON DI PIU'
//   La freccia lascia la corda dopo ~40ms (9 campioni @224Hz). Solo cio' che
//   accade in QUELLA finestra puo' influenzare il volo. Dopo, il movimento e'
//   ancora diagnostico del gesto ma non causa piu' nulla: e' territorio di
//   hold_deg, non del rilascio.
//
// PERCHE' IL JERK (LA DERIVATA) E NON L'AMPIEZZA -- QUESTO E' IL PUNTO
//   La scelta ovvia sarebbe l'ampiezza: quanto ay si scosta dalla baseline.
//   MISURATO sui 42 tiri: l'ampiezza e' un CONFONDENTE GEOMETRICO
//   (rho = -0.389 con l'alzo, p=0.011). Cioe' misura l'inclinazione del
//   bersaglio, non il gesto: tirando in salita o in discesa la gravita' si
//   proietta diversamente sugli assi e ay cambia da sola.
//
//   La DERIVATA no, e c'e' una ragione fisica esatta:
//     ay_misurato = ay_moto + g*sin(theta)
//     d/dt[ay_misurato] = d/dt[ay_moto] + d/dt[g*sin(theta)]
//   Se l'assetto theta e' ~costante nella finestra, il secondo termine sparisce.
//   E LO E': misurato, l'assetto varia di appena 0.24 deg nei 40ms, quindi
//   g*sin(theta) cambia di ~0.041 m/s^2 contro un jerk medio di 451 m/s^3 *
//   0.04s = 18 m/s^2. Il termine gravitazionale e' lo 0.2% del segnale.
//   -> il jerk vede il MOTO e non l'ASSETTO. E' pulito PER COSTRUZIONE.
//
//   Verifica statistica indipendente (correlazione parziale, 42 tiri):
//     jerk <-> alzo, controllando l'ampiezza : rho=+0.041 p=0.796 (pulito)
//     ampiezza <-> alzo, controllando il jerk: rho=-0.291 p=0.061 (sporco)
//   Il confondente sta nell'AMPIEZZA, non nella RAPIDITA'.
//
// VALIDAZIONE (42 tiri, sessione 14JUL)
//   SNR 3.9x: jerk medio nel rilascio 451 m/s^3 contro un floor (stessa misura
//     su finestra equivalente PRE-scocco, arco fermo) di p95 = 115 m/s^3.
//   Ripetibile: split-half pari vs dispari p=0.218 (coerente).
//   Nessun outlier IQR; distribuzione continua e ben distribuita (108..934).
//   INDIPENDENTE da hold_deg: rho = -0.024, p=0.879. E' un'informazione NUOVA,
//     non una riscrittura del follow-through. Coerente: non discrimina gli
//     "abbassati" (p=0.685), perche' rilascio e follow-through sono due errori
//     DISTINTI. Un tiro puo' avere rilascio sporco e tenuta perfetta.
//
// *** LIMITE DA DICHIARARE: LE SOGLIE SONO RELATIVE, NON ASSOLUTE ***
//   330/510 sono i terzili di QUESTA sessione (un arciere, un arco, un giorno):
//   ripartiscono esattamente 14/14/14 tiri. Dicono "pulito PER TE OGGI", non
//   "pulito in assoluto". Servono piu' sessioni (e idealmente piu' arcieri) per
//   sapere se 330 e' una soglia universale. Fino ad allora release_jerk va usato
//   per confrontare tiri fra loro, non per un giudizio assoluto.
//   La soglia PULITO sta comunque 2.9x sopra il rumore (p95 = 115 m/s^3): la
//   distinzione e' reale, e' la sua TARATURA a essere provvisoria.
//
//   NB: le soglie sono tarate sui valori prodotti da QUESTA implementazione
//   (derivata centrata a 2 punti). Una formula diversa (es. np.gradient a 3
//   punti pesati) produce valori leggermente diversi: sui 42 tiri la differenza
//   media e' 17 m/s^3, ma bastava a spostare 5 tiri di classe. Se si cambia la
//   formula, VANNO RITARATE le soglie.
// ----------------------------------------------------------------------------
//  FASE 21 — PICCO D'URTO: la finestra, e perche' e' asimmetrica
// ----------------------------------------------------------------------------
//  La finestra e' [trigger - 60 ms, trigger + 150 ms]. Non e' centrata, e la
//  ragione e' fisica: il trigger scatta DOPO confirm_n campioni sopra soglia,
//  cioe' quando l'impulso e' gia' iniziato da ~9 ms. I 60 ms prima servono a
//  non tagliare il fronte di salita; i 150 dopo coprono l'urto e la prima
//  oscillazione di rinculo, dove sui dati del 15/08 il massimo cadeva sempre.
//
//  Perche' 150 e non 900 (come hold): oltre i ~200 ms quello che si misura non
//  e' piu' l'urto ma il movimento del braccio, che ha ampiezze confrontabili e
//  natura completamente diversa. Una finestra lunga darebbe un numero piu'
//  grande e meno significativo — il modo piu' comune di rovinare una metrica.
//
//  ENTRAMBI i limiti sono in MILLISECONDI e vengono confrontati coi timestamp
//  REALI dei campioni, non con un dt costante: il tick FreeRTOS alternato
//  4005/5005 us rende il dt nominale una bugia (v. nota su integrate_gy).
static constexpr uint16_t SHOT_PICCO_PRE_MS  = 60;
static constexpr uint16_t SHOT_PICCO_POST_MS = 150;
// Sotto questa soglia la finestra e' troppo rada per contenere il massimo vero:
// meglio dichiarare "non calcolato" che pubblicare il massimo di quattro
// campioni. A 224 Hz, 210 ms sono ~47 campioni: 20 e' un minimo prudente che
// tollera un burst troncato ma rifiuta un frammento.
static constexpr uint16_t SHOT_PICCO_MIN_SAMPLES = 20;

static constexpr uint16_t SHOT_RELEASE_MS = 40;      // finestra influenza freccia
static constexpr uint16_t SHOT_RELEASE_MIN_SAMPLES = 5;
static constexpr float SHOT_RELEASE_PULITO_MS3  = 330.0f;   // <= : pulito
static constexpr float SHOT_RELEASE_STRAPPO_MS3 = 510.0f;   // >  : strappo

// Classificazione della pulizia del rilascio.
enum ReleaseClass : uint8_t {
    RELEASE_PULITO  = 0,   // jerk <= 330 m/s^3
    RELEASE_MEDIO   = 1,   // 330 < jerk <= 510
    RELEASE_STRAPPO = 2,   // jerk > 510
    RELEASE_NA      = 3,   // non calcolabile
};

// Classificazione del follow-through.
enum HoldClass : uint8_t {
    HOLD_TENUTO    = 0,   // |hold| <= 4 deg
    HOLD_LIEVE     = 1,   // 4 < |hold| <= 8 deg
    HOLD_ABBASSATO = 2,   // hold < -8 deg  (braccio caduto)
    HOLD_ALZATO    = 3,   // hold > +8 deg  (raro: braccio sollevato)
    HOLD_NA        = 4,   // non calcolabile (finestra post insufficiente)
};

// Risultato del calcolo angoli.
struct ShotAngles {
    bool  valid;      // true se la finestra aveva abbastanza campioni validi
    float cant_deg;   // rollio laterale [deg]  (+dx / -sx)
    float alzo_deg;   // elevazione asse freccia [deg] (+su punta alto / -giu)
    float disp_ms2;   // dispersione |a| nella finestra [m/s^2] (qualita': basso=fermo)
    int   n_used;     // numero di campioni effettivamente mediati
    bool  stable;     // true se la finestra scelta e' STABILE (|g|~9.8, sd bassa):
                      //   assetto affidabile. false = finestra migliore comunque
                      //   dentro il movimento -> angolo poco attendibile.
    float g_mean;     // |a| medio della finestra scelta [m/s^2] (diagnostica)

    // --- v1.3: metrica HOLD (follow-through) ---
    bool     hold_valid;   // true se c'erano abbastanza campioni post-scocco.
                           //   NB: indipendente da 'valid'. Se la finestra calma
                           //   e' scarsa il bias e' stimato male e hold ne
                           //   risente: guardare anche 'stable'.
    float    hold_deg;     // variazione elevazione a +900ms [deg]; negativo = abbassato
    uint8_t  hold_class;   // HoldClass
    float    gy_bias_dps;  // bias gy stimato nella finestra calma [dps] (diagnostica:
                           //   se |gy_bias| > ~2 dps la finestra non era ferma)

    // --- v1.4: metrica RELEASE (pulizia del rilascio) ---
    // Ortogonale a hold: hold dice cosa succede DOPO (900ms), release dice cosa
    // succede MENTRE la freccia e' ancora sulla corda (40ms). Misurato: rho fra
    // le due = -0.024. Sono due errori distinti e vanno letti insieme.
    bool     release_valid;   // true se c'erano abbastanza campioni nella finestra
    float    release_jerk;    // max |d(ay)/dt| nei 40ms [m/s^3]; alto = strappo
    uint8_t  release_class;   // ReleaseClass

    // --- FASE 21: PICCO D'URTO (ampiezza dell'impulso di rilascio) ---
    // NESSUNA CLASSE, e non e' una dimenticanza. Una classe verde/gialla/rossa
    // dichiarerebbe che si sa quale valore e' "buono": non si sa. Il picco e'
    // una grandezza da mettere in serie storica, non da giudicare a colpo
    // d'occhio, finche' il cronografo non avra' detto a cosa corrisponde.
    bool     picco_valid;     // true se la finestra aveva abbastanza campioni
    float    picco_ms2;       // max | |a| - g | nella finestra [m/s^2]
    int      picco_n;         // campioni effettivamente esaminati (diagnostica)
};

// Calcola cant, alzo e hold dal burst.
//   samples     : array del burst (gia' in frame canonico)
//   total       : numero totale di campioni nel burst
//   trigger_idx : indice del campione di scocco entro l'array
//   odr         : impostazione ODR (usata solo per dimensionare le finestre in
//                 campioni; l'integrazione usa i timestamp REALI)
// Ritorna ShotAngles con valid=false se non ci sono abbastanza campioni pre-scocco.
// hold_valid=false se il post-scocco e' troppo corto; cant/alzo restano validi.
// off_cant_deg / off_alzo_deg — TARATURA DI MONTAGGIO.
//   Vengono SOTTRATTI agli angoli calcolati. Sono l'assetto che la scheda legge
//   quando l'arco e' a piombo: la somma dell'inclinazione del supporto e di
//   qualunque disallineamento meccanico. Azzerarli riporta gli angoli
//   all'assoluto grezzo.
//
//   PERCHE' PARAMETRI E NON LETTURA DIRETTA DI g_config
//     Questa funzione e' replicata in Python (archbb_metrics.py) e in
//     JavaScript per la ri-analisi offline. Se leggesse una variabile globale,
//     le repliche dovrebbero indovinarne il valore. Passandola come parametro
//     resta una funzione pura: stessi ingressi, stessa uscita, ovunque.
//     Gli offset vengono scritti in session.txt proprio per questo.
//  FASE 24b — pre_validi: quanti campioni PRIMA del trigger appartengono davvero
//    a questo tiro (v. circular_buffer_get_burst). La finestra di calma viene
//    cercata SOLO dentro quella parte: se il trigger e' scattato poco dopo il
//    riarmo, il resto del pre-trigger e' residuo dell'anello e usarlo produce un
//    angolo verosimile e falso — e' successo davvero (12/09, tiro 5 della
//    sessione 1028: alzo 6,88 gradi su un bersaglio armato a 30, calcolato su
//    campioni di un minuto prima, e salvato come buono).
//    Il default 0xFFFF significa "non dichiarato, tutto valido" e conserva il
//    comportamento storico per le repliche Python/JavaScript che non lo passano.
//    Se non c'e' spazio per una finestra dentro la parte fresca, la funzione
//    ritorna angoli NON validi: campo vuoto nel CSV, mai un numero inventato.
ShotAngles shot_angles_compute(const ImuSample* samples, uint16_t total,
                               uint16_t trigger_idx, OdrSetting odr,
                               float off_cant_deg = 0.0f,
                               float off_alzo_deg = 0.0f,
                               uint16_t pre_validi = 0xFFFF);

// Converte un angolo in gradi a centesimi di grado (int16) per lo ScorePacket.
// Satura a +-180.00 deg (+-18000) per sicurezza.
int16_t shot_angle_to_cdeg(float deg);

// Etichetta breve della classe di hold (per display, max 9 caratteri + NUL).
const char* hold_class_name(uint8_t c);

// Etichetta breve della classe di rilascio (per display, max 7 caratteri + NUL).
const char* release_class_name(uint8_t c);
