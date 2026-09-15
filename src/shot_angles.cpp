// =============================================================================
// shot_angles.cpp — ArchBB: calcolo angoli biomeccanici dal burst (v1.3)
// Firmware 2.14.0 — finestra adattiva + flag qualita + metrica HOLD
// =============================================================================
// Vedi shot_angles.h per la documentazione completa della strategia, delle
// misure che hanno tarato le soglie e del perche' di ogni scelta.
// =============================================================================

#include "shot_angles.h"
#include <math.h>

// -----------------------------------------------------------------------------
// shot_angle_to_cdeg()
//   gradi -> centesimi di grado (int16), con saturazione a +-18000 (+-180.00 deg).
// -----------------------------------------------------------------------------
int16_t shot_angle_to_cdeg(float deg) {
    float cd = deg * 100.0f;
    if (cd >  18000.0f) cd =  18000.0f;
    if (cd < -18000.0f) cd = -18000.0f;
    return (int16_t)lroundf(cd);
}

// -----------------------------------------------------------------------------
// hold_class_name()
//   Etichetta breve per il display. Max 9 caratteri: deve stare nella riga
//   della schermata pre-scoring senza andare a capo (font 4, 240px).
// -----------------------------------------------------------------------------
const char* hold_class_name(uint8_t c) {
    switch (c) {
        case HOLD_TENUTO:    return "TENUTO";
        case HOLD_LIEVE:     return "LIEVE";
        case HOLD_ABBASSATO: return "ABBASSATO";
        case HOLD_ALZATO:    return "ALZATO";
        default:             return "N/D";
    }
}

// -----------------------------------------------------------------------------
// release_class_name()   [v1.4]
//   Max 7 caratteri: sta nella banda del display accanto a HOLD.
// -----------------------------------------------------------------------------
const char* release_class_name(uint8_t c) {
    switch (c) {
        case RELEASE_PULITO:  return "PULITO";
        case RELEASE_MEDIO:   return "MEDIO";
        case RELEASE_STRAPPO: return "STRAPPO";
        default:              return "N/D";
    }
}

// -----------------------------------------------------------------------------
// windowStats()
//   Statistiche di una finestra [a,b): media vettoriale dell'accelerazione +
//   media/sd del suo modulo.
//   La media VETTORIALE (mediare le componenti e POI calcolare l'angolo) e' piu'
//   corretta che mediare gli angoli campione per campione: evita gli artefatti
//   di wrap-around di atan2 e pesa naturalmente i campioni per l'intensita' del
//   vettore gravita'.
// -----------------------------------------------------------------------------
static void windowStats(const ImuSample* s, int32_t a, int32_t b,
                        double& mx, double& my, double& mz,
                        double& gmean, double& gsd) {
    int32_t n = b - a;
    double sx=0, sy=0, sz=0;
    for (int32_t i=a; i<b; i++) { sx+=s[i].ax; sy+=s[i].ay; sz+=s[i].az; }
    mx = sx/n; my = sy/n; mz = sz/n;
    double sm=0;
    for (int32_t i=a; i<b; i++)
        sm += sqrt((double)s[i].ax*s[i].ax + (double)s[i].ay*s[i].ay + (double)s[i].az*s[i].az);
    gmean = sm/n;
    double var=0;
    for (int32_t i=a; i<b; i++) {
        double m = sqrt((double)s[i].ax*s[i].ax + (double)s[i].ay*s[i].ay + (double)s[i].az*s[i].az);
        double d = m - gmean; var += d*d;
    }
    gsd = (n>=2) ? sqrt(var/(n-1)) : 0.0;
}

// -----------------------------------------------------------------------------
// integrate_gy()
//   Integrazione TRAPEZOIDALE di gy (bias-corretto) da 'start' per 'span_ms',
//   usando i TIMESTAMP REALI di ogni campione.
//
//   PERCHE' TRAPEZOIDALE E NON RETTANGOLARE
//     Il metodo rettangolare (somma di w[i]*dt) assume la velocita' costante
//     nell'intervallo: su un segnale che varia rapidamente come il rinculo
//     introduce un errore sistematico proporzionale alla derivata. Il
//     trapezoidale media i due estremi ((w[i]+w[i+1])/2 * dt): l'errore scende
//     da O(dt) a O(dt^2). Costa una addizione in piu' per campione: nulla.
//
//   PERCHE' dt REALE E NON 1/ODR
//     Misurato: il dt alterna 4005/5005us (tick FreeRTOS 1ms vs periodo ideale
//     4448us). Con dt costante l'errore si accumula lungo i 900ms. Il timestamp
//     e' gia' in ImuSample: usarlo costa una sottrazione.
//
//   PROTEZIONE ANTI-WRAP
//     timestamp_us e' uint32: va in overflow ogni ~71.6 minuti di uptime. La
//     differenza fra campioni consecutivi calcolata in uint32 e' comunque
//     CORRETTA anche a cavallo del wrap (aritmetica modulare), purche' si
//     sottragga in uint32 e solo DOPO si converta. Campioni non monotoni o con
//     dt assurdo (>50ms, cioe' >11x il nominale) vengono saltati: sarebbero
//     buchi di campionamento, non dati.
//
//   Ritorna l'angolo cumulato in gradi; n_used riceve i campioni effettivamente
//   integrati.
// -----------------------------------------------------------------------------
static double integrate_gy(const ImuSample* s, int32_t start, int32_t end,
                           double bias_dps, uint16_t span_ms, int32_t& n_used) {
    n_used = 0;
    if (end - start < 2) return 0.0;

    const uint32_t t0     = s[start].timestamp_us;
    const uint32_t span_us = (uint32_t)span_ms * 1000UL;
    const uint32_t DT_MAX_US = 50000UL;   // 50ms: oltre e' un buco, non un campione

    double acc = 0.0;
    for (int32_t i = start; i < end - 1; i++) {
        // Aritmetica modulare in uint32: corretta anche a cavallo dell'overflow.
        uint32_t elapsed = s[i].timestamp_us - t0;
        if (elapsed > span_us) break;                 // superato l'orizzonte

        uint32_t dt_us = s[i+1].timestamp_us - s[i].timestamp_us;
        if (dt_us == 0 || dt_us > DT_MAX_US) continue; // campione anomalo: salta

        double w0 = (double)s[i].gy   - bias_dps;
        double w1 = (double)s[i+1].gy - bias_dps;
        acc += 0.5 * (w0 + w1) * ((double)dt_us / 1e6);   // deg/s * s = deg
        n_used++;
    }
    // SEGNO — corretto in F16 dopo l'ispezione fisica del chip.
    //  Assi reali del QMI8658 nel montaggio attuale (scheda ruotata di 90 a
    //  destra): X = giu, Y = asse freccia verso la punta, Z = destra. L'alzo e'
    //  la rotazione attorno al LATERALE, cioe' attorno a Z, e per la regola
    //  della mano destra una omega_z positiva porta la punta in SU.
    //  La trasformazione di montaggio da cgy = -rgz: l'integrale di gy e'
    //  quindi l'alzo COL SEGNO ROVESCIATO.
    //
    //  Conseguenza sui dati precedenti: hold usciva POSITIVO quando il braccio
    //  CALAVA. Misurato sulle sedute del 27 e 28 luglio, 55 tiri veri: media
    //  +1,78 gradi, 34 positivi su 55 — cioe' il dispositivo diceva che il
    //  braccio saliva nella maggioranza dei tiri. L'arciere conferma che cala.
    //  Da qui la negazione: adesso NEGATIVO = abbassato, come dice HoldClass.
    return -acc;
}

// -----------------------------------------------------------------------------
// compute_picco() — FASE 21: ampiezza massima dell'impulso di rilascio.
// -----------------------------------------------------------------------------
//   La grandezza misurata e' | |a| - g |, IDENTICA a quella che trigger.cpp
//   confronta con la soglia in modo TRIG_ACCEL_DEV. Non e' una coincidenza da
//   preservare a memoria: e' la stessa costante SHOT_ANGLE_GRAVITY letta dagli
//   stessi due file, cosi' come pxToCm/cmToPx sono una sola coppia. Se qui si
//   usasse |az| e la' | |a|-g |, il picco mostrato nel riepilogo e la soglia
//   scritta nel config direbbero due cose diverse chiamandosi allo stesso modo.
//
//   La finestra e' definita in TEMPO, non in campioni, e camminata coi
//   timestamp reali in aritmetica modulare uint32 (stessa disciplina di
//   integrate_gy): il tick alternato 4005/5005 us rende il dt nominale falso.
//
//   Ritorna il massimo in m/s^2; n_used riceve i campioni esaminati.
static float compute_picco(const ImuSample* s, int32_t total, int32_t trig,
                           uint16_t pre_ms, uint16_t post_ms, int32_t& n_used) {
    n_used = 0;
    if (!s || total <= 0 || trig < 0 || trig >= total) return 0.0f;

    const uint32_t t0      = s[trig].timestamp_us;
    const uint32_t pre_us  = (uint32_t)pre_ms  * 1000UL;
    const uint32_t post_us = (uint32_t)post_ms * 1000UL;

    float pk = 0.0f;

    // Una sola espressione per la deviazione: scritta due volte (indietro e
    // avanti) potrebbe divergere in un ramo solo, ed e' l'errore che non si
    // vede perche' il numero resta plausibile.
    auto dev = [&](int32_t i) -> float {
        const float mag = sqrtf(s[i].ax*s[i].ax + s[i].ay*s[i].ay + s[i].az*s[i].az);
        return fabsf(mag - SHOT_ANGLE_GRAVITY);
    };

    // Indietro dal trigger, fino a pre_ms. Il campione del trigger e' incluso
    // qui e NON viene ricontato nel giro in avanti (che parte da trig+1).
    for (int32_t i = trig; i >= 0; i--) {
        uint32_t back = t0 - s[i].timestamp_us;   // modulare: corretta anche a cavallo
        if (back > pre_us) break;
        float d = dev(i);
        if (d > pk) pk = d;
        n_used++;
    }
    // Avanti dal trigger, fino a post_ms.
    for (int32_t i = trig + 1; i < total; i++) {
        uint32_t fwd = s[i].timestamp_us - t0;
        if (fwd > post_us) break;
        float d = dev(i);
        if (d > pk) pk = d;
        n_used++;
    }
    return pk;
}

// -----------------------------------------------------------------------------
// classify_hold()
//   Soglie ancorate al p95 del rumore di integrazione MISURATO (2.10 deg).
//   Vedi shot_angles.h per la derivazione.
// -----------------------------------------------------------------------------
static uint8_t classify_hold(float hold_deg) {
    float a = fabsf(hold_deg);
    if (a <= SHOT_HOLD_TENUTO_DEG)    return HOLD_TENUTO;
    if (a <= SHOT_HOLD_ABBASSATO_DEG) return HOLD_LIEVE;
    return (hold_deg < 0.0f) ? HOLD_ABBASSATO : HOLD_ALZATO;
}

// -----------------------------------------------------------------------------
// compute_release_jerk()   [v1.4]
//   Massimo |d(ay)/dt| nella finestra di influenza della freccia (~40ms).
//
//   PERCHE' LA DERIVATA E NON L'AMPIEZZA: la derivata cancella il termine
//   gravitazionale g*sin(theta), che e' ~costante nei 40ms (misurato: l'assetto
//   varia di 0.24 deg, cioe' lo 0.2% del segnale). L'ampiezza grezza invece e'
//   un confondente geometrico (rho=-0.389 con l'alzo, p=0.011): misurerebbe
//   l'inclinazione del bersaglio, non il gesto. Dettagli in shot_angles.h.
//
//   DERIVATA CENTRATA: si usa (ay[i+1] - ay[i-1]) / (t[i+1] - t[i-1]) invece
//   della differenza in avanti. Costa uguale ma ha errore O(dt^2) invece di
//   O(dt), e soprattutto e' simmetrica: non introduce ritardo di fase, quindi
//   il picco cade dove il jerk e' davvero massimo e non mezzo campione dopo.
//   Serve perche' il campionamento e' jitterato (4005/5005us alternati): una
//   differenza in avanti pesarebbe diversamente i campioni "corti" e "lunghi".
//
//   Ritorna il massimo in m/s^3; n_used riceve i campioni valutati.
// -----------------------------------------------------------------------------
static float compute_release_jerk(const ImuSample* s, int32_t start, int32_t end,
                                  uint16_t span_ms, int32_t& n_used) {
    n_used = 0;
    if (end - start < 3) return 0.0f;

    const uint32_t t0 = s[start].timestamp_us;
    const uint32_t span_us = (uint32_t)span_ms * 1000UL;
    const uint32_t DT_MAX_US = 50000UL;

    float jmax = 0.0f;
    // parte da start+1 e arriva a end-2: la derivata centrata ha bisogno di un
    // campione per lato.
    for (int32_t i = start + 1; i < end - 1; i++) {
        uint32_t elapsed = s[i].timestamp_us - t0;
        if (elapsed > span_us) break;

        uint32_t dt2 = s[i+1].timestamp_us - s[i-1].timestamp_us;
        if (dt2 == 0 || dt2 > 2 * DT_MAX_US) continue;   // buco di campionamento

        float j = fabsf((s[i+1].ay - s[i-1].ay) / ((float)dt2 / 1e6f));
        if (j > jmax) jmax = j;
        n_used++;
    }
    return jmax;
}

// -----------------------------------------------------------------------------
// classify_release()   [v1.4]
//   ATTENZIONE: soglie RELATIVE alla sessione 14JUL (terzili: 36/33/31%), non
//   assolute. Dicono "pulito per questo arciere", non "pulito in assoluto".
//   Vedi il limite dichiarato in shot_angles.h.
// -----------------------------------------------------------------------------
static uint8_t classify_release(float jerk) {
    if (jerk <= SHOT_RELEASE_PULITO_MS3)  return RELEASE_PULITO;
    if (jerk <= SHOT_RELEASE_STRAPPO_MS3) return RELEASE_MEDIO;
    return RELEASE_STRAPPO;
}

// -----------------------------------------------------------------------------
// shot_angles_compute()
// -----------------------------------------------------------------------------
ShotAngles shot_angles_compute(const ImuSample* samples, uint16_t total,
                               uint16_t trigger_idx, OdrSetting odr,
                               float off_cant_deg, float off_alzo_deg,
                               uint16_t pre_validi) {
    // Inizializzazione esplicita di TUTTI i campi (incluso il blocco hold v1.3):
    // se una via d'uscita anticipata scatta, il chiamante non legge spazzatura.
    ShotAngles r = {false, 0.0f, 0.0f, 0.0f, 0, false, 0.0f,
                    false, 0.0f, HOLD_NA, 0.0f,
                    false, 0.0f, RELEASE_NA,
                    false, 0.0f, 0};          // F21: picco_valid, picco_ms2, picco_n

    if (!samples || total == 0) return r;

    // =========================================================================
    // F21 — PICCO D'URTO: calcolato SUBITO, prima di ogni uscita anticipata.
    // =========================================================================
    //  Tutte le uscite qui sotto riguardano la FINESTRA CALMA pre-scocco: se
    //  l'arciere non e' mai stato fermo, cant e alzo non si possono misurare.
    //  Il picco non c'entra nulla con quella finestra — sta dall'altra parte
    //  del trigger — e farlo dipendere da essa sarebbe un accoppiamento
    //  inventato, lo stesso che in main.cpp aveva rischiato di far perdere i
    //  tempi del gesto su un tiro senza burst valido. Un tiro con angoli non
    //  calcolabili ha comunque un picco perfettamente valido.
    {
        int32_t n_pk = 0;
        float pk = compute_picco(samples, (int32_t)total, (int32_t)trigger_idx,
                                 SHOT_PICCO_PRE_MS, SHOT_PICCO_POST_MS, n_pk);
        r.picco_n = (int)n_pk;
        if (n_pk >= (int32_t)SHOT_PICCO_MIN_SAMPLES) {
            r.picco_ms2   = pk;
            r.picco_valid = true;
        }
    }

    // Converte le durate della finestra in numero di campioni all'ODR reale.
    const uint16_t hz    = odrToHz(odr);
    const uint16_t guard = (uint16_t)((float)SHOT_ANGLE_GUARD_MS  / 1000.0f * hz);
    const uint16_t win   = (uint16_t)((float)SHOT_ANGLE_WINDOW_MS / 1000.0f * hz);

    // --- FINESTRA ADATTIVA (validata sul test statico 13/07f) ---
    // PROBLEMA della finestra a offset fisso: negli ~200ms prima del trigger
    // l'arciere sta gia' partendo (clicker, apertura), e la finestra pescava
    // DENTRO il movimento -> |a| lontano da g, angoli sballati (err 13-40 deg sui
    // dati reali). SOLUZIONE: far scorrere la finestra su tutto l'intervallo
    // pre-trigger [0, trigger-guard) e scegliere quella col vettore piu' vicino
    // alla SOLA gravita' (min |mean|a|-g| + sd). Li' l'accelerometro misura
    // l'assetto, non il movimento. Sul test statico questo ha portato l'errore
    // medio da ~13 deg (fisso) a ~3.7 deg (adattivo).
    if (trigger_idx <= guard) return r;

    // F24b.1 — FRESCHEZZA MINIMA (v. SHOT_ANGLE_PRE_MIN_MS in shot_angles.h).
    //  Sotto un secondo di campioni di questo tiro si rinuncia: la parte fresca
    //  e' adiacente al trigger, quindi cercare la calma li' dentro significa
    //  cercarla dove l'arciere e' gia' in movimento. Un minimo lo si trova
    //  sempre — ed e' proprio questo il problema.
    if (pre_validi != 0xFFFF) {
        const uint16_t minFreschi = (uint16_t)((float)SHOT_ANGLE_PRE_MIN_MS / 1000.0f * hz);
        if (pre_validi < minFreschi) return r;   // angoli NON validi: campo vuoto
    }

    int32_t we_max = (int32_t)trigger_idx - (int32_t)guard;   // fine massima (esclusiva)

    // FASE 24b — PAVIMENTO DELLA RICERCA: solo campioni di QUESTO tiro.
    //  Tutto cio' che sta prima di (trigger_idx - pre_validi) e' residuo
    //  dell'anello, lasciato li' da un congelamento precedente. La finestra di
    //  calma non puo' pescarci dentro: l'arco era fermo, si', ma un minuto prima
    //  e in un'altra posizione. E' il punto esatto in cui nasceva il numero
    //  falso — e la correzione e' una sola riga di pavimento.
    int32_t ws_min = (pre_validi == 0xFFFF)
                       ? 0
                       : (int32_t)trigger_idx - (int32_t)pre_validi;
    if (ws_min < 0) ws_min = 0;

    if (we_max < ws_min + (int32_t)win) return r;   // niente spazio: angoli NON validi

    int32_t best_start = -1, best_end = -1;
    double  best_score = 1e18, best_gmean = 0, best_gsd = 0;
    double  best_mx = 0, best_my = 0, best_mz = 0;
    for (int32_t we = ws_min + (int32_t)win; we <= we_max; we++) {
        int32_t ws = we - (int32_t)win;
        double mx, my, mz, gmean, gsd;
        windowStats(samples, ws, we, mx, my, mz, gmean, gsd);
        double score = fabs(gmean - SHOT_ANGLE_GRAVITY) + gsd;   // piu' basso = piu' fermo
        if (score < best_score) {
            best_score = score;
            best_start = ws; best_end = we;
            best_gmean = gmean; best_gsd = gsd;
            best_mx = mx; best_my = my; best_mz = mz;
        }
    }
    if (best_start < 0) return r;
    int32_t n = best_end - best_start;
    if (n < 3) return r;

    double mx = best_mx, my = best_my, mz = best_mz;

    // Flag di STABILITA': la finestra scelta e' affidabile solo se il suo |a|
    // medio e' vicino a g e la dispersione e' bassa. Altrimenti l'angolo va
    // preso con cautela (arciere mai fermo nel burst).
    r.stable = (fabs(best_gmean - SHOT_ANGLE_GRAVITY) < SHOT_ANGLE_G_TOL_MS2)
               && (best_gsd < SHOT_ANGLE_SD_MAX_MS2);
    r.g_mean = (float)best_gmean;

    // --- Angoli dal vettore gravita' medio (frame canonico) ---
    // cant = atan2(ay, ax): rollio laterale. Positivo = inclinato a DESTRA.
    //   Validato sul test statico 13/07: tiro cant-antiorario -> +20, orario -> -28.
    //
    // alzo = atan2(az, ax): elevazione dell'asse di mira.
    //   CONVENZIONE (fissata col test statico ad angoli noti, 13/07f):
    //     positivo = PUNTA della freccia verso l'ALTO (mira in su)
    //     negativo = punta verso il BASSO (mira in giu)
    //
    //   STORIA DEL SEGNO (per non ripetere l'errore):
    //   - v2.12.3 aveva messo atan2(-az,ax), tarato sul CSV 13JUL (primo). Ma
    //     quel CSV erano "tocchi simulati" con l'arco maneggiato a caso (|g|
    //     ballerino, assetti non fisici): un ground-truth inaffidabile.
    //   - Il test STATICO controllato (arco fermo su supporto, angoli noti,
    //     |g|~9.8 sd<0.15) ha smentito quel segno: col gesto dichiarato
    //     "punta in alto" la formula col meno dava NEGATIVO. Verifica diretta:
    //       tiro "alto"  (az=+3.0) -> atan2(+az,ax) = +17.3  (giusto)
    //       tiro "basso" (az=-2.5) -> atan2(+az,ax) = -14.4  (giusto)
    //     Quindi il segno corretto e' SENZA meno: atan2(az, ax).
    //   Lezione: tarare i segni solo su dati statici ad angolo noto, mai su
    //   gesti ambigui.
    //
    //   NOTA offset di montaggio: nel test il "piatto" dava alzo ~+9, cant ~-5
    //   costanti -> tilt residuo del supporto, non errore di formula (le
    //   VARIAZIONI hanno verso corretto). Confermato sul campo (sessione 14JUL,
    //   44 tiri): cant medio -4.8 contro -5.1 del test statico. Un arciere non
    //   tiene 5 deg di cant sistematico: e' montaggio. Una calibrazione di
    //   offset mount resta un miglioramento futuro separato.
    // Angoli grezzi dal vettore gravita' medio, poi TARATURA DI MONTAGGIO.
    //   La sottrazione avviene DOPO l'atan2, non sulle componenti: gli offset
    //   sono angoli, e sottrarre angoli e' l'unica operazione che conserva il
    //   loro significato. Sottrarli dalle componenti sarebbe una rotazione
    //   diversa, e per angoli grandi darebbe un risultato diverso.
    r.cant_deg = (float)(atan2(my, mx) * 180.0 / M_PI) - off_cant_deg;
    r.alzo_deg = (float)(atan2(mz, mx) * 180.0 / M_PI) - off_alzo_deg;

    // Dispersione |a| della finestra scelta (gia' calcolata nella ricerca):
    // basso = arco fermo, misura affidabile; e' la stessa grandezza usata per il
    // flag 'stable'.
    r.disp_ms2 = (float)best_gsd;
    r.n_used   = (int)n;
    r.valid    = true;

    // =========================================================================
    // v1.3 — METRICA HOLD (follow-through)
    // =========================================================================
    // Riusa la STESSA finestra calma appena trovata: li' l'arco e' fermo, quindi
    // la media di gy E' il bias del giroscopio (principio ZUPT). Nessuna ricerca
    // aggiuntiva, nessuna memoria extra: i campioni sono gia' qui.
    double sgy = 0.0;
    for (int32_t i = best_start; i < best_end; i++) sgy += samples[i].gy;
    const double bias_gy = sgy / (double)n;
    r.gy_bias_dps = (float)bias_gy;

    // Integrazione da trigger_idx per SHOT_HOLD_MS, con dt reale dai timestamp.
    if (trigger_idx < total) {
        int32_t n_int = 0;
        double hold = integrate_gy(samples, (int32_t)trigger_idx, (int32_t)total,
                                   bias_gy, SHOT_HOLD_MS, n_int);
        if (n_int >= (int32_t)SHOT_HOLD_MIN_SAMPLES) {
            r.hold_deg   = (float)hold;
            r.hold_class = classify_hold(r.hold_deg);
            r.hold_valid = true;
        } else {
            // Post-scocco troppo corto (burst troncato?): hold non calcolabile.
            // cant/alzo restano validi: sono indipendenti.
            r.hold_class = HOLD_NA;
            r.hold_valid = false;
        }

        // =====================================================================
        // v1.4 — METRICA RELEASE (pulizia del rilascio)
        // =====================================================================
        // Finestra CORTISSIMA (40ms) rispetto a hold (900ms): qui la freccia e'
        // ancora sulla corda, quindi e' l'unico intervallo in cui il gesto puo'
        // ancora influenzare il volo. Nessuna baseline da sottrarre: la derivata
        // elimina da sola il termine gravitazionale costante (v. shot_angles.h).
        int32_t n_rel = 0;
        float jerk = compute_release_jerk(samples, (int32_t)trigger_idx,
                                          (int32_t)total, SHOT_RELEASE_MS, n_rel);
        if (n_rel >= (int32_t)SHOT_RELEASE_MIN_SAMPLES) {
            r.release_jerk  = jerk;
            r.release_class = classify_release(jerk);
            r.release_valid = true;
        } else {
            r.release_class = RELEASE_NA;
            r.release_valid = false;
        }
    }

    return r;
}
