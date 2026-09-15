// ============================================================================
//  ArchBB 1.83 — imu.cpp · FASE 4a (driver QMI8658C, polling puro) · v1
//  Cesare Pagura · Padova/Noale IT · 13 luglio 2026
// ----------------------------------------------------------------------------
//  Implementazione portata dalla 1.69 v2.12.6, con le semplificazioni della
//  Fase 4a (vedi imu.h): polling puro senza pin INT, nessun circular buffer.
//
//  STRATEGIA polling vs interrupt (perche' polling):
//    SensorLib espone getDataReady(), che interroga il registro STATUS del
//    QMI8658 via I2C. Non richiede un GPIO di interrupt. Fare attachInterrupt +
//    lettura I2C nella ISR sarebbe anche SBAGLIATO: l'I2C non e' ISR-safe. Il
//    task fa polling con vTaskDelay(1ms): a 224 Hz il periodo e' ~4.5ms, quindi
//    il worst-case di latenza e' ~1ms, del tutto trascurabile per il trigger.
//
//  NOTE sul pragma SensorLib:
//    SensorQMI8658.hpp emette un #pragma message di deprecazione a compile time.
//    NON e' un errore: e' un avviso del preprocessore. Lo sopprimiamo nel blocco
//    diagnostic push/pop qui sotto per non sporcare il log di build.
// ============================================================================
#include "imu.h"
#include "circular_buffer.h"
#include "mount.h"          // FASE 4c: rimappa gli assi grezzi nel frame canonico
#include "trigger.h"        // FIX 24/07: trigger_feed_sample (valutazione su OGNI campione)

#ifdef ARCHBB_RAW_DIAG
// Build diagnostica: l'hook e' definito in main_raw_diag.cpp. Riceve OGNI
// campione (peak-hold + log continuo su SD) per catturare la firma di uno
// scocco VERO sulla 1.83, che finora non abbiamo mai registrato.
void raw_diag_feed(const ImuSample& s);
#endif

// --- Collisione di macro DBG: la nostra (config.h) vs quella di SensorLib ---
// SensorLib definisce a sua volta un DBG(...) vuoto. Includendola dopo config.h
// il preprocessore emette "DBG redefined" e, peggio, la SUA versione (no-op)
// resterebbe attiva silenziando i nostri log.
//
// L'#undef va PRIMA dell'include, non dopo: e' durante l'include che la libreria
// definisce la sua macro, ed e' li' che nasce il warning. Togliendo la nostra
// prima, SensorLib trova campo libero; subito dopo ripristiniamo la nostra.
#undef DBG
#include <SensorQMI8658.hpp>
#undef DBG

#ifdef ARCHBB_DEBUG
  #define DBG(fmt, ...)  Serial.printf("[ARCHBB] " fmt "\n", ##__VA_ARGS__)
#else
  #define DBG(fmt, ...)
#endif

#include <Wire.h>

// ----------------------------------------------------------------------------
//  Variabili globali
// ----------------------------------------------------------------------------
SemaphoreHandle_t g_imu_sample_mutex = nullptr;
SemaphoreHandle_t g_i2c_mutex        = nullptr;   // mutex del BUS I2C condiviso (fix hang)
ImuSample         g_latest_sample    = {};
volatile bool     g_imu_ok           = false;

static SensorQMI8658*    s_imu = nullptr;
static volatile uint16_t s_seq = 0;

// ----------------------------------------------------------------------------
//  imu_init — configura il QMI8658 sul bus I2C GIA' ATTIVO.
// ----------------------------------------------------------------------------
bool imu_init(OdrSetting odr, uint8_t accel_range_g, uint16_t gyro_range_dps) {
  DBG("IMU init — bus condiviso SDA=%d SCL=%d addr=0x%02X (polling puro)",
      IMU_I2C_SDA, IMU_I2C_SCL, IMU_I2C_ADDR);

  // Il bus e' gia' stato inizializzato da touchInit() (Fase 2). NON richiamiamo
  // Wire.begin(): lo re-inizializzerebbe rompendo il touch. setClock e'
  // idempotente e sicuro: assicura 400 kHz anche se il touch avesse impostato
  // altro.
  Wire.setClock(400000);

  // Scan preventivo: il chip risponde su 0x6B?
  Wire.beginTransmission(IMU_I2C_ADDR);
  if (Wire.endTransmission() != 0) {
    DBG("ERRORE: QMI8658 non trovato su 0x%02X", IMU_I2C_ADDR);
    g_imu_ok = false;
    return false;
  }
  DBG("QMI8658 trovato su 0x%02X", IMU_I2C_ADDR);

  s_imu = new SensorQMI8658();
  if (!s_imu) { DBG("ERRORE: malloc SensorQMI8658"); return false; }

  // begin(Wire, address, sda, scl): passiamo l'istanza Wire GIA' attiva. Non
  // usiamo setPins()+begin() separati (causerebbe "bus already initialized").
  if (!s_imu->begin(Wire, IMU_I2C_ADDR, IMU_I2C_SDA, IMU_I2C_SCL)) {
    DBG("ERRORE: SensorLib begin() fallito");
    delete s_imu; s_imu = nullptr;
    g_imu_ok = false;
    return false;
  }
  DBG("SensorLib begin() OK — chipID=0x%02X", s_imu->getChipID());

  // --- Accelerometro ---
  SensorQMI8658::AccelRange acc_range;
  switch (accel_range_g) {
    case 2:  acc_range = SensorQMI8658::ACC_RANGE_2G;  break;
    case 4:  acc_range = SensorQMI8658::ACC_RANGE_4G;  break;
    case 16: acc_range = SensorQMI8658::ACC_RANGE_16G; break;
    default: acc_range = SensorQMI8658::ACC_RANGE_8G;  break;
  }
  SensorQMI8658::AccelODR acc_odr;
  switch (odr) {
    case ODR_100HZ: acc_odr = SensorQMI8658::ACC_ODR_125Hz; break;
    case ODR_500HZ: acc_odr = SensorQMI8658::ACC_ODR_500Hz; break;
    default:        acc_odr = SensorQMI8658::ACC_ODR_250Hz; break;
  }
  s_imu->configAccelerometer(acc_range, acc_odr, SensorQMI8658::LPF_MODE_2);

  // --- Giroscopio ---
  SensorQMI8658::GyroRange gyr_range;
  switch (gyro_range_dps) {
    case 16:   gyr_range = SensorQMI8658::GYR_RANGE_16DPS;   break;
    case 32:   gyr_range = SensorQMI8658::GYR_RANGE_32DPS;   break;
    case 64:   gyr_range = SensorQMI8658::GYR_RANGE_64DPS;   break;
    case 128:  gyr_range = SensorQMI8658::GYR_RANGE_128DPS;  break;
    case 256:  gyr_range = SensorQMI8658::GYR_RANGE_256DPS;  break;
    case 1024: gyr_range = SensorQMI8658::GYR_RANGE_1024DPS; break;
    default:   gyr_range = SensorQMI8658::GYR_RANGE_512DPS;  break;
  }
  SensorQMI8658::GyroODR gyr_odr;
  switch (odr) {
    case ODR_100HZ: gyr_odr = SensorQMI8658::GYR_ODR_112_1Hz; break;
    case ODR_500HZ: gyr_odr = SensorQMI8658::GYR_ODR_448_4Hz; break;
    default:        gyr_odr = SensorQMI8658::GYR_ODR_224_2Hz; break;
  }
  s_imu->configGyroscope(gyr_range, gyr_odr, SensorQMI8658::LPF_MODE_2);

  s_imu->enableAccelerometer();
  s_imu->enableGyroscope();
  // NB: NON abilitiamo enableINT(): usiamo getDataReady() in polling.

  g_imu_sample_mutex = xSemaphoreCreateMutex();
  if (!g_imu_sample_mutex) { DBG("ERRORE: mutex IMU fallito"); return false; }

  g_imu_ok = true;
  DBG("IMU OK — accel=+-%dg gyro=+-%ddps odr~%dHz",
      accel_range_g, gyro_range_dps, odrToHz(odr));
  return true;
}

// ----------------------------------------------------------------------------
//  imu_task — Core 1. Polling getDataReady() -> g_latest_sample.
// ----------------------------------------------------------------------------
//  In Fase 4a il frame e' MOUNT_0 (frontale): la trasformazione di montaggio e'
//  l'identita', quindi non c'e' nulla da permutare. Quando in futuro servira'
//  il supporto ai 4 montaggi bastera' reintrodurre mount_apply() qui, ESATTA-
//  MENTE in questo punto (a monte di tutto), come sulla 1.69. Lo lasciamo
//  documentato per non dover ricercare "dove va messo" in seguito.
// ----------------------------------------------------------------------------
void imu_task(void* param) {
  DBG("imu_task avviato su Core %d", xPortGetCoreID());

  while (!g_imu_ok) { vTaskDelay(pdMS_TO_TICKS(10)); }

  for (;;) {
    // --- Lettura sensore SOTTO il mutex del bus I2C (fix hang) ---------------
    // getDataReady/getAccelerometer/getGyroscope sono tre transazioni I2C via
    // SensorLib: le racchiudiamo nel mutex del bus perche' non si sovrappongano
    // a una lettura touch dal loop (era la causa dell'hang). Fuori dal lock
    // teniamo solo la matematica e le scritture in RAM (nessun accesso al bus).
    bool haveData = false;
    float ax_l = 0, ay_l = 0, az_l = 0, gx_l = 0, gy_l = 0, gz_l = 0;

    i2cLock();
    if (s_imu->getDataReady()) {
      bool a = s_imu->getAccelerometer(ax_l, ay_l, az_l);
      bool g = s_imu->getGyroscope(gx_l, gy_l, gz_l);
      haveData = a && g;
    }
    i2cUnlock();

    if (!haveData) { vTaskDelay(pdMS_TO_TICKS(1)); continue; }

    float ax_g = ax_l, ay_g = ay_l, az_g = az_l;
    float gx_dps = gx_l, gy_dps = gy_l, gz_dps = gz_l;

    // Conversione g -> m/s^2 in variabili LOCALI (allineate a 4 byte).
    // I campi di ImuSample sono packed e possono cadere non allineati su Xtensa:
    // NON si possono legare a float&. Percio' trasformiamo le locali e le
    // assegniamo DOPO con una copia byte-wise sicura.
    float ax = ax_g * 9.80665f;
    float ay = ay_g * 9.80665f;
    float az = az_g * 9.80665f;

    // --- FASE 4c: frame di montaggio ------------------------------------------
    // Rimappa gli assi GREZZI del sensore nel frame CANONICO, ESATTAMENTE qui: a
    // monte di g_latest_sample, del trigger e del circular buffer. Da questo
    // punto in poi l'intero firmware vede il frame canonico (ax=gravita',
    // ay=laterale, az=freccia) e shot_angles/trigger girano invariati.
    //
    // Si opera sulle LOCALI (float allineati a 4 byte), MAI sui campi di
    // ImuSample: sono packed e su Xtensa non si possono legare a 'float&'
    // (errore di compilazione). Per questo mount_apply e' chiamata PRIMA di
    // riempire sample. La stessa disciplina della 1.69.
    //
    // g_mount_orientation e' 'volatile': oggi lo scrive solo setup() (dal
    // default di config), ma quando arrivera' il BLE/NVS potra' cambiarlo a
    // runtime senza toccare questo hot-loop.
    mount_apply(g_mount_orientation, ax, ay, az, gx_dps, gy_dps, gz_dps);

    ImuSample sample;
    sample.seq          = s_seq++;
    sample.timestamp_us = (uint32_t)esp_timer_get_time();
    sample.ax = ax;   sample.ay = ay;   sample.az = az;
    sample.gx = gx_dps; sample.gy = gy_dps; sample.gz = gz_dps;

    if (xSemaphoreTake(g_imu_sample_mutex, pdMS_TO_TICKS(1)) == pdTRUE) {
      g_latest_sample = sample;
      xSemaphoreGive(g_imu_sample_mutex);
    }

    // Fase 4b: alimenta il buffer circolare (finestra mobile in PSRAM).
    // Il buffer gestisce da solo lo stato RUNNING/FROZEN/READY: qui basta
    // spingere ogni campione, sempre.
    circular_buffer_push(sample);

    // FIX 24/07 — VALUTAZIONE DEL TRIGGER SU OGNI CAMPIONE.
    //  Prima il trigger_task faceva polling di g_latest_sample ogni ~3ms mentre    //  l'IMU produce un campione ogni ~4.5ms: i due ritmi non sono sincronizzati
    //  e il trigger SALTAVA campioni. Uno scuotimento a mano (40-60ms sopra
    //  soglia) veniva comunque preso; uno SCOCCO VERO (~10ms, 2-3 campioni) no:
    //  ecco perche' i tiri veri non venivano rilevati e abbassare la soglia non
    //  serviva. Qui la valutazione vede OGNI campione, per costruzione.
    //  La chiamiamo DOPO il push: cosi' quando la condizione scatta, il campione
    //  del picco e' gia' nel buffer e il freeze cattura la finestra completa.
    trigger_feed_sample(sample);

#ifdef ARCHBB_RAW_DIAG
    // Build DIAGNOSTICA (env archbb_183_raw_diag): consegna OGNI campione al
    // diagnostico raw (peak-hold + log continuo su SD). Definita nel suo main.
    // Fuori da quella build questa chiamata non esiste nemmeno.
    raw_diag_feed(sample);
#endif

    vTaskDelay(pdMS_TO_TICKS(1));
  }
  vTaskDelete(nullptr);
}

// ----------------------------------------------------------------------------
float imu_read_temperature() {
  if (!s_imu) return -273.0f;
  // Lettura I2C: sotto il mutex del bus (chiamata dal loop, concorrente con imu_task).
  i2cLock();
  float t = s_imu->getTemperature_C();
  i2cUnlock();
  return t;
}

// ----------------------------------------------------------------------------
//  imu_read_raw — lettura singola GREZZA (pre-mount) per il DIAG ANGOLI.
// ----------------------------------------------------------------------------
//  Legge accel+gyro una volta, converte l'accel g->m/s^2, e restituisce i
//  valori in frame SENSORE (mount NON applicato: e' il DIAG a mostrare cosa
//  farebbe ogni montaggio). Attende brevemente un campione fresco.
// ----------------------------------------------------------------------------
bool imu_read_raw(float& ax, float& ay, float& az,
                  float& gx, float& gy, float& gz) {
  if (!s_imu || !g_imu_ok) return false;

  // Attende un campione pronto (max ~20ms: a 224Hz un dato arriva ogni ~4.5ms).
  // Tutta la sequenza I2C sotto il mutex del bus (per coerenza: nel DIAG i task
  // non girano, ma proteggere non costa e previene sorprese future).
  i2cLock();
  bool ok = false;
  float axg = 0, ayg = 0, azg = 0;
  uint32_t t0 = millis();
  do {
    while (!s_imu->getDataReady()) {
      if (millis() - t0 > 20) break;
      delay(1);
    }
    if (millis() - t0 > 20) break;
    if (!s_imu->getAccelerometer(axg, ayg, azg)) break;
    if (!s_imu->getGyroscope(gx, gy, gz))        break;
    ok = true;
  } while (0);
  i2cUnlock();
  if (!ok) return false;
  ax = axg * 9.80665f;
  ay = ayg * 9.80665f;
  az = azg * 9.80665f;
  return true;
}
