// ============================================================================
//  ArchBB 1.83 — trigger.h · FASE 4a (rilevazione tiro reale)
//  Cesare Pagura · Padova/Noale IT · 13 luglio 2026
// ----------------------------------------------------------------------------
//  RAZIONALE FISICO (dalla 1.69, invariato):
//    Quando l'arciere apre le dita (rilascio), la corda trasmette un impulso di
//    accelerazione al riser attraverso il punto di ancoraggio. La componente az
//    (asse di trazione della freccia, nel frame canonico) mostra un picco netto
//    seguito da oscillazioni smorzate. Il trigger deve riconoscere questo evento
//    e distinguerlo da: passi/vento, ricerca lenta della mira (<2 m/s^2),
//    respiro (0.2-0.5 m/s^2, <1 Hz).
//
//  RUOLO NELLA FASE 4a:
//    Il trigger SOSTITUISCE il tocco simulato di ATTESA. Quando spara, posta il
//    semaforo g_trigger_fired_sem; il loop del main lo raccoglie e avvia lo
//    scoring (scoringStart) esattamente come prima faceva col tap. Niente
//    circular buffer, niente BLE: solo "e' partita una freccia -> apri lo score".
//
//  DUE MODALITA' (default AZ_THRESHOLD):
//    1) AZ_THRESHOLD : |az| > soglia per N campioni consecutivi. Semplice,
//       robusto, bassa latenza. Soglia default 20 m/s^2 (su barebow reale
//       l'impulso e' 30-80 m/s^2).
//    2) JERK : |d|az|/dt| > soglia. Piu' selettivo (un abbassamento lento del
//       braccio ha az alto ma jerk basso; il rilascio ha jerk altissimo).
//
//  CONFIRM_N + REARM:
//    Il trigger scatta solo se la condizione e' vera per N campioni consecutivi
//    (default 2 -> ~9ms @224Hz: filtra i singoli picchi spuri). Dopo lo sparo,
//    attende rearm_ms (default 2000) prima di riarmarsi: evita trigger multipli
//    sulla stessa onda di vibrazione dello scocco.
//
//  DIFFERENZA CHIAVE dalla 1.69: il trigger 1.69 e' sempre "armato" quando
//  recording e' attivo. QUI, poiche' lo scoring blocca il flusso (l'utente
//  compila 3 schermate), il trigger dev'essere ARMATO SOLO in ATTESA: mentre si
//  compila lo score o si guarda il riepilogo NON deve sparare. Il main controlla
//  l'arma con trigger_set_armed(true/false).
// ============================================================================
#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config.h"

// ----------------------------------------------------------------------------
//  Handle globali (definiti in trigger.cpp)
// ----------------------------------------------------------------------------
// Semaforo binario: postato dal trigger_task quando un tiro e' confermato.
// Il loop del main fa xSemaphoreTake (non bloccante) per avviare lo scoring.
extern SemaphoreHandle_t g_trigger_fired_sem;

// Shot id: incrementato a ogni tiro reale. Sostituisce il contatore manuale.
extern volatile uint16_t g_shot_id;

// Picco di az al momento del trigger [m/s^2] (diagnostica: "trigger sharpness").
extern volatile float g_trigger_az_peak;

// Fasi interne (per il display diagnostico).
enum TriggerPhase : uint8_t {
  TRIG_IDLE,        // disarmato (scoring/riepilogo in corso)
  TRIG_ARMED,       // armato, in attesa dell'evento
  TRIG_CONFIRMING,  // condizione vera per K<N campioni
  TRIG_FIRED,       // appena sparato (transitorio)
  TRIG_REARM,       // in attesa di rearm_ms
};
extern volatile TriggerPhase g_trigger_phase;

// ----------------------------------------------------------------------------
//  API pubblica
// ----------------------------------------------------------------------------

// Crea il semaforo e carica i default. Da chiamare in setup() dopo imu_init().
void trigger_init();

// Arma / disarma il trigger. Il main arma in ATTESA, disarma appena parte lo
// scoring, riarma quando torna in ATTESA. Thread-safe (flag volatile).
void trigger_set_armed(bool armed);

// Forza un tiro manuale (utile per test a banco senza tirare). Thread-safe.
void trigger_manual();

// Task FreeRTOS (Core 1). Legge g_latest_sample, applica la macchina a stati,
// su FIRED incrementa g_shot_id e posta g_trigger_fired_sem. Non ritorna.
void trigger_task(void* param);

// ----------------------------------------------------------------------------
//  trigger_feed_sample() — valuta UN campione per la condizione di scocco.
// ----------------------------------------------------------------------------
//  DA CHIAMARE DA imu_task, per OGNI campione prodotto, subito dopo aver
//  applicato il frame di montaggio e PRIMA/INSIEME al push nel buffer circolare.
//
//  PERCHE' ESISTE (bug del 24/07, scocchi veri non rilevati):
//    Prima la valutazione stava dentro trigger_task, che faceva polling di
//    g_latest_sample ogni ~3ms. Ma l'IMU produce un campione ogni ~4.5ms
//    (224Hz) e i due ritmi NON sono sincronizzati: il trigger vedeva solo
//    ALCUNI campioni, saltandone una parte.
//      - Uno SCUOTIMENTO a mano resta sopra soglia per 40-60ms (9-13 campioni):
//        anche saltandone meta', il trigger ne raccoglie abbastanza -> SCATTA.
//      - Uno SCOCCO VERO dura ~10ms (2-3 campioni): se ne perde uno, non
//        raggiunge confirm_n campioni consecutivi -> NON SCATTA MAI.
//    Ecco perche' abbassare la soglia non serviva a nulla: il problema non era
//    l'ampiezza, era che i campioni del picco non venivano proprio guardati.
//
//  SOLUZIONE: valutare il campione DOVE PASSA, cioe' in imu_task, che li vede
//  TUTTI per costruzione. Nessun campione perso, nessuna coda da dimensionare,
//  e il trigger scatta nello stesso istante in cui il campione esiste (utile per
//  il freeze del buffer). trigger_task resta per le sole transizioni TEMPORALI
//  (FIRED -> REARM -> ARMED), che dipendono da millis() e non dai campioni.
//
//  Thread-safety: chiamata dal solo imu_task (Core 1). Le variabili di stato del
//  trigger sono toccate qui e nelle transizioni temporali del trigger_task: le
//  fasi sono disgiunte (la valutazione avviene solo in ARMED/CONFIRMING, le
//  transizioni temporali solo in FIRED/REARM), quindi non si pestano i piedi.
void trigger_feed_sample(const ImuSample& s);

// ----------------------------------------------------------------------------
//  trigger_configure() — rende OPERATIVI i parametri del config (handoff §2.3).
// ----------------------------------------------------------------------------
//  BUG STORICO CHE QUESTA FUNZIONE CHIUDE (diagnosticato il 25/07 sui dati):
//    config_apply() cablava SOLO il montaggio. La soglia scritta nel config
//    (da menu o via BLE) finiva in NVS ma NON arrivava MAI al trigger, che
//    continuava a usare TRIG_DEFAULT_THRESHOLD. Per questo abbassare la soglia
//    "non serviva a niente": il valore non veniva applicato.
//    Effetto sul campo: tiri veri con picco |az| 11.6-16.4 m/s^2 sempre SOTTO
//    la soglia reale di 20 -> mai rilevati; scuotimenti a mano fino a 29.9 ->
//    sopra 20 -> sempre rilevati. Esattamente il sintomo osservato.
//
//  Da chiamare da config_apply(), al boot e a ogni modifica del config.
//  Le finestre sono in millisecondi e vengono convertite in campioni all'ODR
//  reale, con clamp alla capienza del buffer circolare.
void trigger_configure(float threshold_ms2, uint8_t confirm_n, uint16_t rearm_ms,
                       uint16_t window_pre_ms, uint16_t window_post_ms);
