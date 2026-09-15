// ============================================================================
//  ArchBB 1.83 — config_ui.h · FASE 6 (schermata CONFIG on-device)
//  Cesare Pagura · Padova/Noale IT · 20 luglio 2026
// ----------------------------------------------------------------------------
//  Schermata di sola LETTURA dei parametri config (g_config), con un solo
//  pulsante d'azione: "ripristina default". L'editing vero verra' dal BLE/app
//  (scelta di Cesare: "minimale ora, editing col BLE dopo"). Qui si VERIFICA
//  che i valori caricati dall'NVS siano quelli attesi, e si puo' tornare ai
//  default di fabbrica se qualcosa si e' sballato.
//
//  E' modalita' "capolinea" come DIAG/CALIBRA: si entra dal menu d'avvio, si
//  legge, si esce (o si ripristina). Non fa parte del flusso di tiro.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

// Mostra i valori di g_config e gestisce il pulsante "ripristina default".
// Bloccante: resta finche' l'utente tocca "ESCI" (o ripristina, che poi esce).
// Da chiamare dal menu d'avvio quando touch_cal_offer ritorna 4.
void config_ui_show(TFT_eSPI& tft);
