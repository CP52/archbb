// =============================================================================
// mount.h — ArchBB: trasformazione assi per orientamento di montaggio
// Versione 1.0 (introdotta in firmware v2.9)
// =============================================================================
//
// SCOPO
//   La scheda Waveshare puo' essere montata sul riser in 4 orientamenti, per
//   adattarsi ai fori liberi dei riser moderni (olimpici o compound):
//     - 0 gradi  : frontale, schermo verso l'arciere (montaggio di riferimento)
//     - -90 gradi: lato DESTRO del riser
//     - +90 gradi: lato SINISTRO del riser
//     - 180 gradi: lato OPPOSTO
//   I 4 montaggi sono rotazioni attorno all'ASSE GRAVITA' (ax): "l'alto resta
//   l'alto". Cio' che cambia e' come l'asse FRECCIA (az) e l'asse LATERALE (ay)
//   si distribuiscono sugli assi elettronici del sensore.
//
// PRINCIPIO: TRASFORMAZIONE A MONTE
//   La trasformazione viene applicata in imu.cpp SUBITO dopo la lettura del
//   sensore e PRIMA di ogni elaborazione a valle (trigger, circular buffer,
//   burst BLE, metriche app). Cosi' l'INTERO sistema a valle vede sempre dati
//   nel frame CANONICO 0 gradi, e NESSUN altro codice va modificato: il trigger
//   continua a cercare lo spike su az, il cant continua a usare atan2(ay,ax),
//   ecc. Questo e' l'approccio raccomandato in letteratura per il problema
//   "sensor-to-body alignment" (cfr. AllAboutCircuits, "How to Interpret IMU
//   Sensor Data for Dead-Reckoning: Rotation Matrix Creation", 2019; brevetto
//   US11886245 "runtime-frame velocity of wearable device", trasformazione dal
//   frame di montaggio a un frame di training/canonico).
//
// FRAME CANONICO (0 gradi) — ANCORATO A TIRI VERI (sessione 02JUL, 5 scocchi)
//   ax = asse verticale / GRAVITA'  (+ax ad arco verticale, gravita' media +10.2)
//   ay = asse laterale (dx/sx)      (base del cant)
//   az = asse FRECCIA / SCOCCO      (spike +az allo scocco, media +34 m/s^2)
//
// MATRICI DI ROTAZIONE R  (v_canonico = R . v_sensore)
//   Determinate empiricamente e validate (ortonormali, det=+1). Poiche' sono
//   rotazioni di multipli di 90 gradi attorno ad ax, R contiene solo 0 e +-1:
//   la trasformazione e' una semplice permutazione con cambi di segno, ZERO
//   trigonometria, ZERO errori di arrotondamento, costo computazionale nullo.
//
//     0 gradi  (MOUNT_0)      : identita'
//                cax= ax  cay= ay  caz= az      cgx= gx  cgy= gy  cgz= gz
//     -90 (MOUNT_DX_M90)      :
//                cax= ax  cay=-az  caz= ay      cgx= gx  cgy=-gz  cgz= gy
//     +90 (MOUNT_SX_P90)      :
//                cax= ax  cay= az  caz=-ay      cgx= gx  cgy= gz  cgz=-gy
//     180 (MOUNT_180)         :
//                cax= ax  cay=-ay  caz=-az      cgx= gx  cgy=-gy  cgz=-gz
//
//   NOTA FISICA: in tutti i montaggi cax=ax (gravita' invariante) e caz
//   ricostruisce sempre l'asse freccia canonico -> il trigger vede lo spike
//   su az qualunque sia il montaggio. Il giroscopio usa la STESSA matrice
//   dell'accelerometro: sono rigidamente solidali sullo stesso PCB, quindi
//   condividono un'unica trasformazione rigida (cfr. Frosio et al., calibrazione
//   MEMS: basta l'allineamento dell'accelerometro, il gyro segue).
// =============================================================================

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include "config.h"

// -----------------------------------------------------------------------------
// ENUM ORIENTAMENTO DI MONTAGGIO
//   Valori STABILI (persistiti in NVS nel byte Config::mount_orientation).
//   NON riordinare: cambierebbe il significato dei dati gia' salvati.
// -----------------------------------------------------------------------------
enum MountOrientation : uint8_t {
    MOUNT_0       = 0,   // frontale (riferimento) — nessuna rotazione
    MOUNT_DX_M90  = 1,   // lato destro  (-90 gradi)
    MOUNT_SX_P90  = 2,   // lato sinistro (+90 gradi)
    MOUNT_180     = 3,   // lato opposto (180 gradi)
    MOUNT_COUNT   = 4,   // sentinella (numero di orientamenti validi)
};

// -----------------------------------------------------------------------------
// FLIP-Z HARDWARE della 1.83 (caratterizzazione 22/07, dati alla mano)
// -----------------------------------------------------------------------------
//  STORIA (importante — cosi' non si ripete la confusione):
//    Fino alla caratterizzazione del 22/07 esisteva un 5o orientamento
//    MOUNT_0_FLIPZ, aggiunto "al volo" perche' sul banco la 1.83 dava alzo
//    invertito. Sembrava un montaggio a se'. La caratterizzazione fisica del
//    chip (pose di gravita' note) ha invece dimostrato che:
//      - in posizione 0 gli assi hanno i RUOLI GIUSTI: X=verticale (grav),
//        Y=laterale (cant), Z=asse-freccia (alzo). NESSUNO scambio di assi.
//      - MA l'asse Z fisico del QMI8658 sulla 1.83 e' orientato all'OPPOSTO di
//        quanto le formule assumono: gesto "punta in su" -> az NEGATIVO
//        (misurato: az=-4.9), mentre la convenzione vuole alzo POSITIVO.
//    Quindi il "flip" NON e' un montaggio: e' una proprieta' FISICA della
//    scheda (il chip ha Z invertito vs la 1.69). Come tale va applicato SEMPRE,
//    a monte, PRIMA di qualunque rotazione di montaggio — non come 5a posizione.
//
//  CONSEGUENZA: le 4 rotazioni tornano a essere quelle PURE, validate sulla
//  1.69 (det=+1). Il flip-Z (nega az e gz) e' un pre-passo dentro mount_apply,
//  attivo se ARCHBB_HW_FLIP_Z=1. VERIFICATO EQUIVALENTE al vecchio
//  MOUNT_0_FLIPZ per la posizione in uso: stessi angoli, nessuna regressione
//  sui dati di campo gia' raccolti.
//
//  Se un domani si montasse una revisione di scheda col chip "diritto", basta
//  ARCHBB_HW_FLIP_Z=0: le 4 matrici restano valide senza altro.
#ifndef ARCHBB_HW_FLIP_Z
#define ARCHBB_HW_FLIP_Z 1     // 1.83 attuale: chip con Z invertito -> flip ON
#endif

// -----------------------------------------------------------------------------
// NOMI — descrivono il LATO FISICO su cui la scheda e' montata.
// -----------------------------------------------------------------------------
//  REGOLA: se la scheda e' a destra, ovunque si deve leggere "destro". Sul
//  display, nell'app, nel seriale, nei file. Il flip-Z e' una proprieta'
//  interna di questa revisione di scheda: l'utente non deve conoscerla per
//  capire cosa sta configurando.
//
//  PERCHE' SERVONO DUE TABELLE
//    I valori 0..3 identificano una TRASFORMAZIONE, non un lato. Il legame fra
//    trasformazione e lato fisico dipende dalla manualita' del frame del chip:
//    il flip-Z e' una RIFLESSIONE (det = -1) e le riflessioni scambiano destra
//    con sinistra. Concretamente, la stessa mappa dei grezzi si scrive in due
//    modi equivalenti:
//        "+90 sinistro con flip PRIMA"  ==  "-90 destro con flip DOPO"
//        (entrambe: cay = -z, caz = -y)
//    Quindi il nome della rotazione, da solo, non dice da che parte sta la
//    scheda. Serve sapere anche se c'e' la riflessione.
//
//  ANCORAGGIO EMPIRICO (DIAG ASSI del 26/07, scheda montata sul riser)
//    Il DIAG ha dato come vincitore il valore 2 con flip attivo, e la scheda
//    era fisicamente a DESTRA. Verdetto con margine 36 su una soglia di
//    allarme di 5, e confermato in modo indipendente dai nove tiri del 25/07:
//    con quel valore l'alzo esce -16,98 (punta in basso, sagoma 3D a terra) e
//    il cant +0,39 con sd 1,22 (rollio involontario di un grado e mezzo).
//    Da qui: con flip attivo, 2 = destro. Gli altri tre seguono per simmetria
//    e NON sono stati verificati sul riser — se un giorno servisse montare la
//    scheda a sinistra, si rifa' il DIAG invece di fidarsi della simmetria.
inline const char* mount_name(uint8_t m) {
#if ARCHBB_HW_FLIP_Z
    // 1.83: chip con Z invertito. Tabella ancorata al DIAG del 26/07.
    switch (m) {
        case MOUNT_0:      return "frontale";
        case MOUNT_DX_M90: return "sinistro";
        case MOUNT_SX_P90: return "destro";      // <- VERIFICATO sul riser
        case MOUNT_180:    return "opposto";
        default:           return "?";
    }
#else
    // Chip "diritto" (1.69 e future revisioni): il nome della rotazione E' il lato.
    switch (m) {
        case MOUNT_0:      return "frontale";
        case MOUNT_DX_M90: return "destro";
        case MOUNT_SX_P90: return "sinistro";
        case MOUNT_180:    return "opposto";
        default:           return "?";
    }
#endif
}

// Nome della ROTAZIONE pura, senza tener conto della riflessione. Serve solo
// alla diagnostica di basso livello: chi legge questo sta guardando le matrici,
// non il lato del riser.
inline const char* mount_name_rotazione(uint8_t m) {
    switch (m) {
        case MOUNT_0:      return "R 0";
        case MOUNT_DX_M90: return "R -90";
        case MOUNT_SX_P90: return "R +90";
        case MOUNT_180:    return "R 180";
        default:           return "R ?";
    }
}

// -----------------------------------------------------------------------------
// Variabile globale con l'orientamento corrente.
//   Aggiornata da main.cpp all'avvio (da Config NVS) e ad ogni scrittura Config
//   via BLE. Letta dall'imu_task nel suo hot-loop.
//   'volatile' perche' scritta da un task (BLE/main, Core 0) e letta da un altro
//   (imu_task, Core 1). Un uint8_t e' letto/scritto atomicamente su Xtensa, quindi
//   non serve mutex: la lettura vede sempre un valore intero coerente (vecchio o
//   nuovo), mai una via di mezzo. Il cambio di montaggio non e' time-critical.
// -----------------------------------------------------------------------------
extern volatile uint8_t g_mount_orientation;

// -----------------------------------------------------------------------------
// mount_apply() — trasforma un campione dal frame SENSORE al frame CANONICO.
//
//   Applica in-place la matrice R corrispondente a 'orient' ai sei assi del
//   campione (ax,ay,az,gx,gy,gz). Implementata come switch con permutazioni
//   dirette: nessuna moltiplicazione di matrice, massima efficienza.
//
//   Parametri passati per riferimento e modificati in-place.
//
//   ATTENZIONE (packed): passare VARIABILI LOCALI (float allineati a 4 byte),
//   NON campi di una struct __attribute__((packed)) come ImuSample. Su Xtensa
//   (ESP32) legare un campo packed non allineato a 'float&' e' un errore di
//   compilazione ("cannot bind packed field to float&"). In imu.cpp la
//   trasformazione avviene infatti sulle locali, prima di riempire ImuSample.
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// mount_apply_ex() — come mount_apply, ma con il flip-Z passato come PARAMETRO.
// -----------------------------------------------------------------------------
//  PERCHE' ESISTE (regola §4.1)
//    Il DIAG assi deve provare tutte le combinazioni montaggio x flip per dire
//    quale descrive la scheda montata sul riser. Riscrivere li' le quattro
//    matrici sarebbe la duplicazione piu' pericolosa possibile: due copie della
//    stessa geometria, e il giorno che una cambia il DIAG certifica un
//    montaggio che il firmware non applica.
//    Quindi: UNA sola implementazione, qui. mount_apply() la chiama col valore
//    di compilazione, il DIAG la chiama variando il flip.
inline void mount_apply_ex(uint8_t orient, bool flipZ,
                           float& ax, float& ay, float& az,
                           float& gx, float& gy, float& gz)
{
    // --- PRE-PASSO: FLIP-Z HARDWARE della 1.83 -------------------------------
    //  Il chip QMI8658 sulla 1.83 ha l'asse Z fisicamente invertito rispetto
    //  alla convenzione delle formule (caratterizzato il 22/07: "punta su" ->
    //  az negativo). Lo raddrizziamo QUI, sui grezzi, PRIMA della rotazione di
    //  montaggio: cosi' le 4 rotazioni restano quelle pure della 1.69 e il flip
    //  vive in UN SOLO posto (proprieta' della scheda, non del montaggio).
    //  Neghiamo az e gz (il gyro e' solidale: coerenza del segno di rollio/beccheggio).
    if (flipZ) { az = -az; gz = -gz; }

    // ax e gx (asse gravita'/rollio) sono INVARIANTI in ogni ROTAZIONE: la
    // rotazione avviene attorno ad ax. (Il flip-Z sopra e' un pre-passo separato,
    // gia' applicato.) Li marchiamo come volutamente inutilizzati nello switch
    // per evitare warning -Wunused-parameter senza perdere la simmetria.
    (void)ax; (void)gx;

    switch (orient) {

        case MOUNT_0:
            // Identita': nessuna trasformazione. Caso piu' frequente -> primo.
            break;

        case MOUNT_DX_M90: {
            // cax=ax; cay=-az; caz=ay   |   cgx=gx; cgy=-gz; cgz=gy
            float t_ay = -az,  t_az = ay;
            float t_gy = -gz,  t_gz = gy;
            ay = t_ay;  az = t_az;
            gy = t_gy;  gz = t_gz;
            break;
        }

        case MOUNT_SX_P90: {
            // cax=ax; cay=az; caz=-ay   |   cgx=gx; cgy=gz; cgz=-gy
            float t_ay = az,   t_az = -ay;
            float t_gy = gz,   t_gz = -gy;
            ay = t_ay;  az = t_az;
            gy = t_gy;  gz = t_gz;
            break;
        }

        case MOUNT_180:
            // cax=ax; cay=-ay; caz=-az   |   cgx=gx; cgy=-gy; cgz=-gz
            ay = -ay;  az = -az;
            gy = -gy;  gz = -gz;
            break;

        default:
            // Valore sconosciuto (NVS corrotta?): tratta come 0 gradi (sicuro).
            break;
    }
}

// -----------------------------------------------------------------------------
// mount_apply() — la versione usata dal firmware: flip fissato a compilazione.
//   Un solo rinvio: la geometria vive tutta in mount_apply_ex().
// -----------------------------------------------------------------------------
inline void mount_apply(uint8_t orient,
                        float& ax, float& ay, float& az,
                        float& gx, float& gy, float& gz)
{
    mount_apply_ex(orient, (bool)ARCHBB_HW_FLIP_Z, ax, ay, az, gx, gy, gz);
}

// -----------------------------------------------------------------------------
// mount_is_valid() — true se il valore e' un orientamento riconosciuto.
// -----------------------------------------------------------------------------
inline bool mount_is_valid(uint8_t m) {
    return m < MOUNT_COUNT;
}
