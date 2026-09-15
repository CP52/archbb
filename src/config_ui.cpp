// ============================================================================
//  ArchBB 1.83 — config_ui.cpp · FASE 6 (schermata CONFIG on-device)
//  Cesare Pagura · Padova/Noale IT · 20 luglio 2026
// ----------------------------------------------------------------------------
//  Sola lettura di g_config + "ripristina default". Vedi config_ui.h.
//  Nota fmtFixed: su ESP32 snprintf("%f") e' inaffidabile (memoria di progetto),
//  ma qui siamo in una modalita' bloccante di diagnostica, non nell'hot-path, e
//  i valori sono pochi: usiamo snprintf con cast a int per i decimali dove serve,
//  evitando comunque "%f" grezzo.
// ============================================================================
#include "config_ui.h"
#include "config_store.h"
#include "display.h"     // ArchColor
#include "touch.h"
#include "mount.h"       // mount_name

// Formatta un float con 1 decimale senza usare "%f" (intero.parte).
static void fmt1(char* out, size_t n, float v) {
  bool neg = v < 0; if (neg) v = -v;
  int ip = (int)v;
  int dp = (int)((v - ip) * 10.0f + 0.5f);
  if (dp >= 10) { ip++; dp = 0; }
  snprintf(out, n, "%s%d.%d", neg ? "-" : "", ip, dp);
}

// Geometria pulsanti (condivisa disegno+hit, lezione §4.1).
namespace CfgUI {
  constexpr int BTN_H   = 44;
  // F19: i pulsanti scendono da 196 a 230. Con 13 righe di valori la lista
  // arrivava a y=210 e passava SOTTO i pulsanti: si sovrapponevano gia' con
  // 11 righe, e le due nuove lo rendevano evidente.
  constexpr int RESET_Y = 230;
  constexpr int EXIT_Y  = 230;
  constexpr int RESET_X = 20;
  constexpr int EXIT_X  = 128;
  constexpr int BTN_W   = 92;
}

static void drawConfigScreen(TFT_eSPI& t) {
  using namespace CfgUI;
  t.fillScreen(ArchColor::BG);
  t.drawRect(0, 0, ARCHBB_W, ARCHBB_H, ArchColor::BORDER);

  t.setTextDatum(TC_DATUM);
  t.setTextColor(ArchColor::ACCENT, ArchColor::BG);
  t.drawString("CONFIG", ARCHBB_W/2, 8, 4);
  t.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  char vb[24]; snprintf(vb, sizeof(vb), "NVS v%u", g_config.version);
  t.drawString(vb, ARCHBB_W/2, 40, 2);

  // Lista valori (label a sinistra, valore a destra). Font 1 per densita'.
  int y = 56; const int dy = 13;
  t.setTextDatum(TL_DATUM);

  auto row = [&](const char* label, const char* val) {
    t.setTextColor(ArchColor::TEXT3, ArchColor::BG);
    t.drawString(label, 12, y, 1);
    t.setTextColor(ArchColor::TEXT, ArchColor::BG);
    t.setTextDatum(TR_DATUM);
    t.drawString(val, ARCHBB_W - 12, y, 1);
    t.setTextDatum(TL_DATUM);
    y += dy;
  };

  char v[24];
  // trigger
  snprintf(v, sizeof(v), "%s", g_config.trig_mode == 2 ? "|a|-g"
                                : g_config.trig_mode == 1 ? "jerk" : "az-soglia");
  row("trigger", v);
  fmt1(v, sizeof(v), g_config.trig_threshold_ms2); strncat(v, " m/s2", sizeof(v)-strlen(v)-1);
  row("soglia", v);
  snprintf(v, sizeof(v), "%u", g_config.trig_confirm_n);        row("conferma", v);
  snprintf(v, sizeof(v), "%u ms", g_config.trig_rearm_ms);      row("rearm", v);
  // finestre
  snprintf(v, sizeof(v), "%u ms", g_config.window_pre_ms);      row("pre-scocco", v);
  snprintf(v, sizeof(v), "%u ms", g_config.window_post_ms);     row("post-scocco", v);
  // montaggio
  snprintf(v, sizeof(v), "%u %s", g_config.mount_orientation, mount_name(g_config.mount_orientation));
  row("montaggio", v);
  // offset calibrazione
  fmt1(v, sizeof(v), g_config.offset_cant_deg);                 row("off. cant", v);
  fmt1(v, sizeof(v), g_config.offset_alzo_deg);                 row("off. alzo", v);
  fmt1(v, sizeof(v), g_config.offset_g_ms2);                    row("off. G", v);
  // fisica
  fmt1(v, sizeof(v), g_config.bow_mass_kg); strncat(v, " kg", sizeof(v)-strlen(v)-1);
  row("massa arco", v);
  // scoring (F19)
  snprintf(v, sizeof(v), "%u cm", g_config.imp_raggio_colpito_cm);  row("cerchio colpito", v);
  snprintf(v, sizeof(v), "%u cm", g_config.imp_raggio_mancato_cm);  row("cerchio mancato", v);

  // Pulsanti: RIPRISTINA (rosso, sinistra) + ESCI (ciano, destra).
  t.setTextDatum(MC_DATUM);
  t.fillRoundRect(RESET_X, RESET_Y, BTN_W, BTN_H, 8, ArchColor::BG_CARD);
  t.drawRoundRect(RESET_X, RESET_Y, BTN_W, BTN_H, 8, ArchColor::BAD);
  t.setTextColor(ArchColor::BAD, ArchColor::BG_CARD);
  t.drawString("DEFAULT", RESET_X + BTN_W/2, RESET_Y + BTN_H/2, 2);

  t.fillRoundRect(EXIT_X, EXIT_Y, BTN_W, BTN_H, 8, ArchColor::BG_CARD);
  t.drawRoundRect(EXIT_X, EXIT_Y, BTN_W, BTN_H, 8, ArchColor::ACCENT);
  t.setTextColor(ArchColor::ACCENT, ArchColor::BG_CARD);
  t.drawString("ESCI", EXIT_X + BTN_W/2, EXIT_Y + BTN_H/2, 2);

  t.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  t.setTextDatum(BC_DATUM);
  t.drawString("editing dei valori: via app BLE", ARCHBB_W/2, ARCHBB_H - 6, 1);
}

// Conferma bloccante per il ripristino (doppio tocco, per non perderlo a caso).
static bool confirmReset(TFT_eSPI& t) {
  t.fillScreen(ArchColor::BG);
  t.drawRect(0, 0, ARCHBB_W, ARCHBB_H, ArchColor::BORDER);
  t.setTextDatum(MC_DATUM);
  t.setTextColor(ArchColor::BAD, ArchColor::BG);
  t.drawString("RIPRISTINA?", ARCHBB_W/2, 60, 4);
  t.setTextColor(ArchColor::TEXT2, ArchColor::BG);
  t.drawString("azzera tutti i parametri", ARCHBB_W/2, 96, 2);
  t.drawString("ai valori di fabbrica", ARCHBB_W/2, 116, 2);

  const int bh = 48, sY = 160, nY = 220, bx = 30, bw = ARCHBB_W - 60;
  t.fillRoundRect(bx, sY, bw, bh, 8, ArchColor::BG_CARD);
  t.drawRoundRect(bx, sY, bw, bh, 8, ArchColor::BAD);
  t.setTextColor(ArchColor::BAD, ArchColor::BG_CARD);
  t.drawString("SI, RIPRISTINA", ARCHBB_W/2, sY + bh/2, 2);
  t.fillRoundRect(bx, nY, bw, bh, 8, ArchColor::BG_CARD);
  t.drawRoundRect(bx, nY, bw, bh, 8, ArchColor::TEXT2);
  t.setTextColor(ArchColor::TEXT, ArchColor::BG_CARD);
  t.drawString("annulla", ARCHBB_W/2, nY + bh/2, 2);

  for (;;) {
    TouchData td = touchRead();
    if (td.valid && td.fingers > 0 && td.coordsFresh) {
      if (td.y >= sY && td.y < sY + bh) { delay(250); return true; }
      if (td.y >= nY && td.y < nY + bh) { delay(250); return false; }
    }
    delay(20);
  }
}

void config_ui_show(TFT_eSPI& t) {
  using namespace CfgUI;
  drawConfigScreen(t);
  for (;;) {
    TouchData td = touchRead();
    if (td.valid && td.fingers > 0 && td.coordsFresh) {
      // ESCI
      if (td.x >= EXIT_X && td.x < EXIT_X + BTN_W &&
          td.y >= EXIT_Y && td.y < EXIT_Y + BTN_H) { delay(250); return; }
      // DEFAULT (con conferma)
      if (td.x >= RESET_X && td.x < RESET_X + BTN_W &&
          td.y >= RESET_Y && td.y < RESET_Y + BTN_H) {
        delay(250);
        if (confirmReset(t)) {
          config_reset();   // default + save NVS
        }
        drawConfigScreen(t);   // ridisegna coi valori (ripristinati o invariati)
      }
    }
    delay(20);
  }
}
