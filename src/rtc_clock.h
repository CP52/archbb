// ============================================================================
//  ArchBB 1.83 — rtc_clock.h · FASE 6 (orologio RTC PCF85063)
//  Cesare Pagura · Padova/Noale IT · 21 luglio 2026
// ----------------------------------------------------------------------------
//  PERCHE'
//    Fino a qui le sessioni avevano solo millis() (tempo da accensione, azzerato
//    a ogni boot): inutile come riferimento assoluto. La 1.83 ha un RTC hardware
//    (PCF85063) che tiene l'ora anche a dispositivo spento — VERIFICATO sul
//    campo: dopo spegni/riaccendi l'ora era mantenuta. Ora le sessioni possono
//    avere una data/ora vera.
//
//  BUS CONDIVISO (stessa disciplina di IMU/touch/batteria)
//    Il PCF85063 sta sul bus I2C 15/14, gia' attivo. rtc_init() NON chiama
//    Wire.begin(): passa a SensorLib l'istanza Wire viva. Un begin() ripetuto
//    romperebbe gli altri dispositivi sul bus.
//
//  LIBRERIA
//    SensorPCF85063 (SensorLib, lewisxhe) — la STESSA gia' usata per il QMI8658.
//    Nessuna dipendenza nuova. NB: in questa versione di SensorLib i campi di
//    RTC_DateTime sono privati: si leggono coi getter (getYear()/getHour()/...).
//
//  VALIDITA' (il pezzo che conta)
//    rtc_is_valid() dice se l'ora e' AFFIDABILE. Lo determina dal flag OS
//    (Oscillator Stop) del chip + un controllo di sanita' sull'anno. Finche' il
//    BLE non esiste, l'RTC non e' mai stato impostato -> invalido -> le sessioni
//    NON scrivono una data-spazzatura (2000-01-01), ma "non impostata", e
//    l'orologio a schermo NON compare. Sentinella onesta, come i *_valid ovunque.
//
//  SET (predisposto, inattivo)
//    rtc_set() c'e' ma oggi non e' chiamato da nessuno: l'impostazione dell'ora
//    arrivera' dallo smartphone via BLE. La firma e' pronta perche' il BLE la
//    chiami senza altri cambiamenti.
// ============================================================================
#pragma once
#include <Arduino.h>
#include "config.h"

// Data/ora in forma semplice (evita di esporre RTC_DateTime di SensorLib a
// tutto il firmware: chi legge l'ora usa questa struct leggera).
struct RtcTime {
  uint16_t year;   // es. 2026
  uint8_t  month;  // 1..12
  uint8_t  day;    // 1..31
  uint8_t  hour;   // 0..23
  uint8_t  minute; // 0..59
  uint8_t  second; // 0..59
  bool     valid;  // false = RTC non impostato/affidabile
};

// Stato globale (definito in rtc_clock.cpp).
extern volatile bool g_rtc_ok;   // true se il chip risponde sul bus

// Inizializza il PCF85063 sul bus I2C GIA' attivo. Ritorna true se il chip
// risponde. Da chiamare in setup() dopo touchInit() (che avvia Wire).
bool rtc_init();

// True se l'ora e' affidabile: il chip risponde, il flag OS e' pulito (non si e'
// fermato) e l'anno e' plausibile. False finche' l'RTC non e' stato impostato.
bool rtc_is_valid();

// Legge l'ora corrente. Se l'RTC non e' valido, ritorna una RtcTime con
// valid=false (i campi data sono indefiniti: non usarli). Non fallisce mai.
RtcTime rtc_now();

// ----------------------------------------------------------------------------
//  FASE 24b.1 — allinea l'OROLOGIO DI SISTEMA POSIX all'RTC
// ----------------------------------------------------------------------------
//  PERCHE' SERVE, visto che l'ora ce l'abbiamo gia'.
//    Ce l'abbiamo per NOI: rtc_iso_string costruisce le stringhe che finiscono
//    nei nomi delle cartelle e nei CSV. Ma la microSD non passa da li': FATFS
//    chiama get_fattime(), che legge time(NULL), cioe' l'orologio di SISTEMA —
//    che nessuno aveva mai impostato. Partiva da zero, FAT non sa rappresentare
//    date prima del 1980, e ogni file della card e' nato "1/1/1980" con
//    l'UPTIME al posto dell'ora (test del 14/09: 00:26, 00:28, 00:31...).
//
//    Il timestamp del filesystem e' un SECONDO registro, indipendente da
//    session.txt, di quando le cose sono successe. Serve esattamente quando il
//    primo manca — come SESS_1215 del 12/09, chiusa da uno spegnimento e
//    rimasta senza ended_ms. Averlo azzerato per due anni e' stata una perdita
//    silenziosa: nessun errore, solo una colonna "Data modifica" inutile.
//
//  Da chiamare dopo rtc_init() e di nuovo dopo ogni rtc_set() (sync BLE):
//  l'orologio di sistema non si riallinea da solo.
bool rtc_sync_system_clock();

// Imposta l'ora dell'RTC (lo fara' il BLE quando arrivera'). Dopo un set
// riuscito, rtc_is_valid() torna true. Ritorna true se la scrittura e' andata.
// PREDISPOSTA: oggi nessuno la chiama.
bool rtc_set(uint16_t year, uint8_t month, uint8_t day,
             uint8_t hour, uint8_t minute, uint8_t second);

// Riempie out con "YYYY-MM-DDTHH:MM:SS" se valido, oppure "non impostata".
// Comodita' per session.txt. buf deve essere >= 20 byte.
void rtc_iso_string(char* buf, size_t bufSize);

// Riempie out con "YYYYMMDD_HHMM" se valido, altrimenti buf[0]='\0' (stringa
// vuota). Usato per i nomi cartella datati. buf deve essere >= 14 byte.
void rtc_compact_string(char* buf, size_t bufSize);
