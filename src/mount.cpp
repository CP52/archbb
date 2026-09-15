// =============================================================================
// mount.cpp — ArchBB: definizione stato orientamento di montaggio
// Versione 1.0 (firmware v2.9)
// =============================================================================
//
// Contiene solo la definizione della variabile globale dichiarata extern in
// mount.h. Tutta la logica di trasformazione e' inline nell'header (funzioni
// piccole, chiamate nell'hot-loop dell'imu_task: l'inlining elimina l'overhead
// di chiamata).
//
// Valore iniziale MOUNT_0: se main.cpp non lo aggiorna (es. NVS vergine), il
// comportamento e' identico a v2.8 — nessuna trasformazione, montaggio frontale.
// =============================================================================

#include "mount.h"

volatile uint8_t g_mount_orientation = MOUNT_0;
