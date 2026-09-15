// ============================================================================
//  ArchBB 1.83 — imu.h · FASE 4a (driver QMI8658C, polling puro)
//  Cesare Pagura · Padova/Noale IT · 13 luglio 2026
// ----------------------------------------------------------------------------
//  Interfaccia del modulo IMU. Portato dalla 1.69 v2.12.6 (logica MATURA e
//  validata sul campo), adattato alla 1.83 con due semplificazioni DELIBERATE
//  per la Fase 4a:
//
//   1) POLLING PURO senza pin INT. Sulla 1.69 l'IMU aveva INT su GPIO14, ma
//      sulla 1.83 il 14 e' SCL (bus condiviso col touch). SensorLib sa leggere
//      il registro STATUS via I2C con getDataReady(): non serve alcun GPIO
//      dedicato. Un pin in meno da sbagliare, comportamento identico.
//
//   2) NIENTE circular buffer (arrivera' in Fase 4b). In Fase 4a l'imu_task si
//      limita a mantenere g_latest_sample aggiornato (protetto da mutex): e'
//      tutto cio' che serve al trigger per osservare lo spike su az. Il buffer
//      per la finestra pre-scocco (angoli) e' un pezzo successivo.
//
//  BUS I2C CONDIVISO (trappola): imu_init() NON chiama Wire.begin(). Il bus e'
//  gia' vivo (touchInit() in Fase 2). Chiamare di nuovo begin() lo re-inizializza
//  e rompe il touch. imu_init() usa solo Wire.setClock() (idempotente) e passa
//  l'istanza Wire gia' attiva a SensorLib.
//
//  THREADING (FreeRTOS): imu_task gira su Core 1. Aggiorna g_latest_sample sotto
//  g_imu_sample_mutex. Il trigger_task (anch'esso Core 1) lo legge sotto lo
//  stesso mutex. g_imu_ok e' volatile: scritto da imu_init (setup/Core1), letto
//  dagli altri task per attendere che l'IMU sia pronta.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config.h"

// ----------------------------------------------------------------------------
//  Handle globali (definiti in imu.cpp)
// ----------------------------------------------------------------------------
extern SemaphoreHandle_t g_imu_sample_mutex;  // protegge g_latest_sample

// ----------------------------------------------------------------------------
//  MUTEX DEL BUS I2C CONDIVISO (fix blocco/hang)
// ----------------------------------------------------------------------------
//  QMI8658 (imu_task, Core 1), CST816 (touch, letto dal loop), AXP2101 e RTC
//  stanno TUTTI sullo stesso bus Wire (SDA=15/SCL=14). Le transazioni sono
//  preemptable: FreeRTOS puo' interrompere il loop a META' di una lettura touch
//  e dare la CPU a imu_task, che inizia la SUA transazione sullo stesso bus ->
//  SDA/SCL restano in uno stato incoerente -> una delle due transazioni aspetta
//  in eterno un ACK che non arriva -> HANG (il dispositivo resta appeso, niente
//  refresh, nessun reset: esattamente il sintomo osservato).
//
//  g_i2c_mutex SERIALIZZA gli accessi al bus: chi vuole parlare sull'I2C prende
//  il mutex, fa la sua transazione, lo rilascia. Nessuna transazione puo' piu'
//  sovrapporsi a un'altra. Va preso attorno a OGNI accesso Wire (touch, IMU,
//  batteria, RTC). NB: e' il mutex del BUS, distinto da g_imu_sample_mutex che
//  protegge il DATO g_latest_sample.
//
//  Creato in touchInit() (il primo a toccare il bus, con Wire.begin). Prima che
//  esista, gli accessi di setup() sono sequenziali (un solo thread): l'helper
//  i2cLock/i2cUnlock e' un no-op se il mutex e' ancora nullptr.
extern SemaphoreHandle_t g_i2c_mutex;

// Helper: prende/rilascia il mutex del bus I2C in modo sicuro anche se non e'
// ancora stato creato (fase di setup mono-thread). Usare SEMPRE in coppia.
// Il mutex e' RICORSIVO: una funzione che prende il lock puo' chiamarne un'altra
// che lo riprende senza deadlock (es. rtc_now -> rtc_is_valid). Ogni Take va
// bilanciato da un Give.
inline void i2cLock()   { if (g_i2c_mutex) xSemaphoreTakeRecursive(g_i2c_mutex, portMAX_DELAY); }
inline void i2cUnlock() { if (g_i2c_mutex) xSemaphoreGiveRecursive(g_i2c_mutex); }
extern ImuSample         g_latest_sample;     // ultimo campione (frame canonico)
extern volatile bool     g_imu_ok;            // true quando l'IMU e' inizializzata

// ----------------------------------------------------------------------------
//  API pubblica
// ----------------------------------------------------------------------------

// Inizializza il QMI8658C sul bus I2C GIA' ATTIVO (non chiama Wire.begin()).
// Ritorna true se il chip risponde e la config va a buon fine.
//   odr            : frequenza di campionamento (default 224 Hz reale)
//   accel_range_g  : fondo scala accelerometro in g (2/4/8/16; default 8)
//   gyro_range_dps : fondo scala giroscopio in deg/s (default 512)
bool imu_init(OdrSetting odr           = ODR_250HZ,
              uint8_t    accel_range_g = 8,
              uint16_t   gyro_range_dps = 512);

// Task FreeRTOS (Core 1). Polling di getDataReady(): quando c'e' un campione
// fresco lo legge, converte g->m/s^2, applica il frame canonico e aggiorna
// g_latest_sample sotto mutex. Non ritorna.
void imu_task(void* param);

// Temperatura on-chip [deg C] (diagnostica).
float imu_read_temperature();

// ----------------------------------------------------------------------------
//  FASE 4c — lettura GREZZA singola (pre-mount), per il DIAG ANGOLI.
// ----------------------------------------------------------------------------
//  Legge una volta accelerometro + giroscopio e restituisce i valori in frame
//  SENSORE (NON canonico: mount_apply NON viene applicato). Serve al DIAG
//  ANGOLI, che deve mostrare cosa produrrebbe OGNI montaggio a partire dagli
//  stessi grezzi: se leggesse dati gia' rimappati, applicherebbe il mount due
//  volte. Accel in m/s^2, gyro in deg/s. Ritorna false se non c'e' un campione
//  pronto (l'IMU va gia' inizializzata). Da usare PRIMA di avviare imu_task
//  (nel menu d'avvio), quando g_latest_sample non e' ancora in movimento.
bool imu_read_raw(float& ax, float& ay, float& az,
                  float& gx, float& gy, float& gz);
