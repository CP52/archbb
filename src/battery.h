// ============================================================================
//  ArchBB 1.83 — battery.h · FASE 5 (stato batteria via AXP2101)
//  Cesare Pagura · Padova/Noale IT · 20 luglio 2026
// ----------------------------------------------------------------------------
//  PERCHE' E' DIVERSO DALLA 1.69
//    Sulla 1.69 la batteria si leggeva con un partitore resistivo su un ADC:
//    misura grezza di tensione (OCV), rumorosa, da correggere a mano, e con la
//    trappola nota che collegare l'USB falsa la lettura (parte la carica).
//
//    La 1.83 monta il PMU AXP2101, un chip di gestione alimentazione che sta
//    sul bus I2C GIA' CONDIVISO (SDA=15/SCL=14, lo stesso di IMU e touch). Il
//    PMU calcola LUI la percentuale di carica, la tensione di batteria e lo
//    stato (in carica / a batteria), gia' filtrati. Leggiamo valori puliti via
//    I2C, niente ADC, niente partitore, nessun pin nuovo.
//
//  BUS CONDIVISO (stessa disciplina di IMU e touch)
//    battery_init() NON chiama Wire.begin(): il bus e' gia' vivo (touchInit in
//    Fase 2). Passa a XPowersLib l'istanza Wire attiva. Un begin() ripetuto
//    romperebbe touch e IMU. E' la trappola gia' imparata sulla 1.83.
//
//  LIBRERIA
//    XPowersLib (lewisxhe) — stesso autore di SensorLib che gia' usi per il
//    QMI8658. API pulita per l'AXP2101. La aggiungiamo a lib_deps.
//
//  ROBUSTEZZA
//    Se il PMU non risponde (raro: e' saldato, ma teniamo il pattern difensivo
//    di tutto il progetto), g_batt_ok resta false e le letture tornano
//    sentinelle (-1 percentuale, NAN tensione): la barra di stato mostrera'
//    "batt n/d" invece di un numero inventato. Coerente con i *_valid ovunque.
// ============================================================================
#pragma once
#include <Arduino.h>
#include "config.h"

// ----------------------------------------------------------------------------
//  Stato globale (definito in battery.cpp)
// ----------------------------------------------------------------------------
extern volatile bool g_batt_ok;   // true se l'AXP2101 risponde

// ----------------------------------------------------------------------------
//  API
// ----------------------------------------------------------------------------

// Inizializza l'AXP2101 sul bus I2C GIA' ATTIVO (non chiama Wire.begin()).
// Ritorna true se il chip risponde. Da chiamare in setup(), dopo touchInit()
// (che ha avviato il bus) e senza vincoli d'ordine con imu_init().
bool battery_init();

// Percentuale di carica 0..100, oppure -1 se non disponibile (PMU assente o
// percentuale non ancora calcolata dal chip). L'AXP2101 la stima internamente:
// e' il valore da mostrare all'arciere.
int battery_percent();

// Tensione di batteria in volt, oppure NAN se non disponibile. Utile come
// dettaglio diagnostico (una batteria "100%" ma a 3.3V sta per spegnersi).
float battery_voltage();

// True se in questo momento la batteria si sta caricando (USB collegato e
// carica attiva). Serve a mostrare l'icona "in carica" ed evitare allarmi di
// batteria scarica mentre e' attaccata.
bool battery_charging();

// ----------------------------------------------------------------------------
//  FASE 7 — tasto PWRKEY del PMU (ingresso del modo BLE)
// ----------------------------------------------------------------------------
//  Il PWRKEY dell'AXP2101 e' il pulsante utente VERO della 1.83: letto via I2C
//  dal PMU (non un GPIO), immune ai problemi di GPIO0 che causavano i blocchi.
//  L'IRQ di pressione breve e' abilitato in battery_init(). Questa funzione fa
//  polling non-bloccante nel loop e ritorna true UNA volta per pressione breve.
bool power_key_short_pressed();

// ============================================================================
//  FASE 24a — GESTIONE DELL'ENERGIA LATO PMU
// ============================================================================
//  Tutto cio' che riguarda l'AXP2101 vive qui e solo qui: power_mgr possiede la
//  politica (quando attenuare/spegnere), battery possiede il chip. Mischiarli
//  significherebbe due moduli che parlano allo stesso I2C con due mutex e due
//  idee diverse di chi comanda.
// ----------------------------------------------------------------------------

// Applica le impostazioni di risparmio "una volta e per sempre" al PMU.
// Da chiamare in setup(), subito dopo battery_init().
//
// COSA FA, e perche' solo questo:
//   1) SPEGNE IL LED DI CARICA. In campo e' acceso o lampeggiante per nessuno:
//      la scheda sta sul riser, il LED guarda il terreno. Vale qualche mA, che
//      su sei ore non sono pochi, e non toglie nessuna informazione (lo stato
//      di carica e' gia' nella barra di stato, con la percentuale).
//   2) NIENT'ALTRO. In particolare NON tocca ne' la corrente di carica ne' la
//      misura sul pin TS (termistore): su una cella al litio quelli sono i due
//      parametri che, sbagliati, trasformano un risparmio in un incendio. Non
//      si toccano senza schematico alla mano e una ragione misurata.
void battery_power_tuning();

// Tensione di SISTEMA in volt (VSYS), o NAN. E' diversa dalla tensione di
// batteria: e' quella che il PMU consegna davvero al resto della scheda, e il
// suo crollo sotto carico e' il segnale che precede lo spegnimento.
float battery_voltage_sys();

// ----------------------------------------------------------------------------
//  Soglia di spegnimento di sistema (VOFF) — l'ipotesi da falsificare per prima
// ----------------------------------------------------------------------------
//  L'AXP2101 stima la percentuale DALLA TENSIONE (lo dichiara Waveshare stessa),
//  e la tensione di una cella al litio crolla sotto carico. Con ~130 mA di
//  assorbimento il sag puo' valere 150-250 mV. Se la soglia di power-down e'
//  tarata alta, il PMU stacca mentre nella cella c'e' ancora il 20-25% di
//  carica: lo spegnimento del 12/09 potrebbe essere questo, e non l'esaurimento.
//
//  Si legge PRIMA di cambiarla. Se risultasse gia' al minimo, l'ipotesi e'
//  falsificata e si smette di pensarci — che e' il punto di misurare.
uint16_t battery_voff_mv();            // 0 se non leggibile
bool     battery_set_voff_mv(uint16_t mv);   // range utile AXP2101: 2600..3300

// ----------------------------------------------------------------------------
//  Censimento dei rail — "cosa sta alimentando questa scheda, adesso?"
// ----------------------------------------------------------------------------
//  Sulla 1.83 ci sono due chip audio (ES8311, ES7210) e un amplificatore che il
//  firmware di produzione non usa MAI: il tang acustico e' un ambiente di banco
//  separato. Se il loro rail e' acceso dal boot, stiamo alimentando tre chip
//  per niente, tutto il giorno.
//
//  NON SPEGNIAMO NIENTE ALLA CIECA. Prima si guarda cosa c'e' acceso e a che
//  tensione, si scrive in /ENERGIA.CSV, e solo dopo — schematico alla mano — si
//  decide cosa e' spegnibile. Spegnere un rail a caso su questa scheda vuol
//  dire, nel migliore dei casi, un touch che non risponde piu'.
//
//  Riempie out con una riga compatta tipo:
//    "DC1=3300 DC3=off ALDO1=3300 ALDO2=off BLDO1=off ..."
void battery_rail_report(char* out, size_t outSz);

// ----------------------------------------------------------------------------
//  F24b — pressione LUNGA del PWRKEY
// ----------------------------------------------------------------------------
//  Ritorna true una volta sola quando il PMU segnala una pressione lunga. Non
//  impedisce lo spegnimento: quello e' hardware e resta al PMU. Serve ad
//  ACCORGERSENE in tempo, nella finestra fra la soglia della IRQ e i 10 secondi
//  del power-off, per chiudere la sessione e lasciare scritto perche'.
bool power_key_long_pressed();
