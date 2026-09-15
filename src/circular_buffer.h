// ============================================================================
//  ArchBB 1.83 — circular_buffer.h · FASE 4b (buffer circolare PSRAM)
//  Cesare Pagura · Padova/Noale IT · 13 luglio 2026
// ----------------------------------------------------------------------------
//  Portato dalla 1.69 v2.12.6 (modulo maturo, thread-safe). Sulla 1.83 usiamo
//  la PSRAM Octal R8 (gia' configurata: memory_type=qio_opi): e' la ragione
//  stessa della migrazione a questa scheda, perche' da PSRAM sara' piu' comodo
//  poi riversare il burst su microSD.
//
//  IDEA: il buffer scorre in continuo (RUNNING) accumulando gli ultimi campioni.
//  Sul trigger si "congela" (FROZEN): cattura ancora POST campioni dopo lo
//  scocco, poi diventa READY. A quel punto la finestra PRE+POST attorno al
//  trigger e' estraibile con circular_buffer_get_burst().
//
//  THREAD-SAFETY: push (da imu_task, Core1) e freeze/get (da trigger/UI) sono
//  serializzati da un mutex interno. Lo stato e' un piccolo automa a 3 fasi.
// ============================================================================
#pragma once
#include <Arduino.h>
#include "config.h"

// Stato dell'automa del buffer.
//   RUNNING : scorre in continuo, sovrascrive i campioni vecchi (finestra mobile)
//   FROZEN  : trigger scattato; sta raccogliendo i POST campioni dopo lo scocco
//   READY   : POST completati; il burst PRE+POST e' pronto da leggere
enum BufferState : uint8_t { BUFFER_RUNNING = 0, BUFFER_FROZEN = 1, BUFFER_READY = 2 };

// Alloca il buffer (PSRAM se disponibile, altrimenti fallback heap interno) e
// crea il mutex. Ritorna false se l'allocazione fallisce. Da chiamare in setup().
bool circular_buffer_init();

// Inserisce un campione. In RUNNING avanza la finestra mobile; in FROZEN
// accumula i POST fino a raggiungere post_samples, poi passa a READY. Chiamata
// dall'imu_task a ogni campione.
void circular_buffer_push(const ImuSample& sample);

// Congela il buffer attorno al campione corrente: marca l'indice di trigger e
// avvia la raccolta dei POST campioni. Chiamata dal trigger_task sul FIRED.
void circular_buffer_freeze(uint16_t pre_samples, uint16_t post_samples, uint16_t shot_id);

// Copia il burst PRE+POST (in ordine cronologico) in dst, fino a max_samples.
// Scrive in *trigger_idx_out l'indice del campione di scocco entro dst (= pre).
// Ritorna il numero di campioni copiati. Da usare quando lo stato e' READY.
// FASE 24b — pre_validi_out: quanti dei campioni PRE-trigger appartengono
//  davvero a questo tiro, cioe' sono stati scritti dopo l'ultimo reset. Se il
//  trigger scatta poco dopo il riarmo, il resto della finestra e' residuo
//  dell'anello precedente: presente nel file, ma non utilizzabile per calcolare
//  angoli. Parametro opzionale per non rompere i chiamanti storici — ma chi
//  calcola qualcosa sul pre-trigger DEVE passarlo.
uint16_t circular_buffer_get_burst(ImuSample* dst, uint16_t max_samples,
                                   uint16_t* trigger_idx_out,
                                   uint16_t* pre_validi_out = nullptr);

// Riporta il buffer a RUNNING (dopo aver consumato il burst). Chiamata dalla UI
// quando lo scoring del tiro e' concluso, cosi' il prossimo tiro riparte pulito.
void circular_buffer_reset();

// Stato corrente (per UI/diagnostica).
BufferState circular_buffer_state();

// Quanti POST campioni sono stati raccolti finora (per una progress bar futura).
uint16_t circular_buffer_post_count();
