// ============================================================================
//  ArchBB 1.83 — power_mgr.h · FASE 24a (politica di risparmio energetico)
//  Cesare Pagura · Padova/Noale IT · 12 settembre 2026
// ----------------------------------------------------------------------------
//  DA DOVE NASCE
//    Sessione di campo del 12/09, percorso tecnico: la scheda si e' spenta dopo
//    due ore abbondanti, a percorso non finito. Un campagna a 24 piazzole dura
//    4-5 ore. L'autonomia non e' un dettaglio di rifinitura: e' la differenza
//    fra uno strumento e un prototipo.
//
//  COSA FA QUESTO MODULO — E COSA NON FA
//    Fa la POLITICA: decide QUANDO attenuare, QUANDO spegnere, QUANDO risvegliare.
//    Non tocca l'hardware: il backlight e il pannello li possiede display.cpp
//    (displayBacklightPercent / displayPanelSleep), il PMU lo possiede
//    battery.cpp. "Un modulo possiede una risorsa hardware" (§4.1) — qui la
//    risorsa posseduta e' il TEMPO DI INATTIVITA', che non e' hardware ma e'
//    altrettanto facile da duplicare in tre punti diversi e vederli divergere.
//
//  IL VINCOLO CHE NON SI NEGOZIA
//    Lo schermo spento NON disarma niente. imu_task e trigger_task continuano
//    a girare, il buffer circolare continua a riempirsi, la freccia che parte a
//    schermo nero viene registrata esattamente come le altre. Uno strumento di
//    misura che perde un dato per risparmiare corrente ha fallito il suo unico
//    compito. Il risveglio e' un fatto di INTERFACCIA, non di acquisizione.
//
//  TRE STATI, NON DUE
//    ATTIVO   -> livello base (config: ENERGIA_BL_BASE_PCT)
//    PENOMBRA -> livello ridotto. Non serve solo a risparmiare: e' il PREAVVISO.
//                Vedere lo schermo calare e' l'unico modo che l'arciere ha di
//                sapere che fra un minuto si spegne, senza doverlo imparare a
//                memoria. Un'interfaccia che si spegne di colpo sembra rotta.
//    SPENTO   -> retro a zero + pannello in sleep + (opzionale) CPU a 80 MHz.
//
//  TRE PROFILI, PERCHE' NON TUTTE LE SCHERMATE SONO UGUALI
//    ATTESA        e' dove si passano le ore: attenua presto, spegne presto.
//    INTERATTIVO   pre-tiro, scoring, riepilogo: si GUARDANO mentre si pensa.
//                  Attenua molto tardi e non spegne MAI — hanno gia' un timeout
//                  proprio che le chiude, quindi non restano accese per sbaglio.
//    TRASFERIMENTO modo BLE: comanda il telefono, lo schermo serve a poco.
//
//  IL TOCCO DI RISVEGLIO NON E' UN COMANDO
//    Toccare uno schermo spento deve accenderlo e basta. Se quel tocco venisse
//    anche interpretato, ogni risveglio in ATTESA aprirebbe o chiuderebbe una
//    sessione a caso — cioe' il difetto peggiore possibile in un registratore
//    di dati. power_consume_risveglio() esiste per questo: il chiamante chiede
//    "questo tocco era di risveglio?" e, se si', lo butta.
// ============================================================================
#pragma once
#include <Arduino.h>
#include "config.h"

// ----------------------------------------------------------------------------
//  Tipi
// ----------------------------------------------------------------------------
enum class PowerStato : uint8_t { ATTIVO = 0, PENOMBRA = 1, SPENTO = 2 };
enum class PowerProfilo : uint8_t { ATTESA = 0, INTERATTIVO = 1, TRASFERIMENTO = 2 };

// ----------------------------------------------------------------------------
//  API
// ----------------------------------------------------------------------------

// Avvia la politica: accende la retro al livello base e azzera il cronometro di
// inattivita'. Da chiamare in setup() DOPO displayInit().
void power_init();

// Da chiamare a ogni giro di loop(). Non blocca mai: la rampa di attenuazione
// avanza di un gradino per chiamata (a 15 ms di loop, ~120 ms in tutto).
void power_tick();

// "L'arciere ha fatto qualcosa": azzera il cronometro. Da chiamare su ogni
// tocco valido, sul tasto PWRKEY e a ogni cambio di schermata.
void power_attivita();

// Risveglio esplicito, immediato e completo (retro al livello base, pannello
// sveglio, CPU alta). Serve quando succede qualcosa che l'arciere DEVE vedere
// senza toccare niente: uno scocco rilevato, un salvataggio, un errore.
// Ritorna true se lo schermo era effettivamente spento (cioe' se il chiamante
// deve ridisegnare la schermata corrente).
bool power_risveglia();

// true se il tocco appena arrivato era quello che ha risvegliato lo schermo e
// quindi NON va interpretato come comando. Consuma il flag: la seconda chiamata
// per lo stesso tocco ritorna false.
bool power_consume_risveglio();

// false quando il pannello e' in sleep: i ridisegni periodici vanno saltati.
// Disegnare su un pannello spento non fa danni, ma costa SPI, CPU e corrente
// per un'immagine che nessuno vede — cioe' esattamente cio' che stiamo
// cercando di non fare.
bool power_schermo_acceso();

// Profilo corrente. Da impostare a ogni cambio di AppFlow.
void power_set_profilo(PowerProfilo p);

// Stato corrente (diagnostica e log).
PowerStato power_stato();

// Livello base regolabile a runtime (0..100), in RAM: al riavvio si torna a
// ENERGIA_BL_BASE_PCT. Serve per la regolazione al volo controluce.
void    power_set_livello_base(uint8_t pct);
uint8_t power_livello_base();

// Percentuale di retro effettivamente applicata adesso (per /ENERGIA.CSV).
uint8_t power_backlight_pct();

// Frequenza di CPU corrente in MHz (per /ENERGIA.CSV). Con ENERGIA_CPU_SCALING
// a 0 ritorna sempre ENERGIA_CPU_MHZ_ALTA.
uint32_t power_cpu_mhz();
