// ============================================================================
//  Banco funzionale della macchina dei tempi — F20b
// ----------------------------------------------------------------------------
//  DIFFERENZA CON LA VERSIONE F20, ed e' la ragione per cui questo file e'
//  stato riscritto: la vecchia chiamava tempo_on_fire() da uno stato pulito,
//  subito dopo la mira. La realta' non fa cosi'. Fra l'ultima quiete e il
//  trigger ci sono 30-80 ms di RILASCIO, e in quei millisecondi la macchina
//  vede movimento vero e abbandona l'ancora — correttamente.
//
//  Il banco vecchio provava la macchina; non provava il suo aggancio al mondo.
//  Ogni scenario qui sotto include adesso la fase di rilascio.
// ============================================================================
#include "tempo.h"
#include <cstdio>
#include <cmath>

static uint32_t T = 0;
static void campione(float omega_dps, float dev, int ms, bool postura = true) {
  const float dt_us = 1e6f / 217.7f;
  int n = (int)(ms / 1000.0f * 217.7f);
  for (int i = 0; i < n; i++) {
    ImuSample s{};
    T += (uint32_t)dt_us; s.timestamp_us = T;
    float g = 9.81f + dev;
    if (postura) { s.ax = g; s.ay = 0; s.az = 0; } else { s.ax = 0; s.ay = g; s.az = 0; }
    s.gx = omega_dps; s.gy = 0; s.gz = 0;
    tempo_feed_sample(s);
  }
}
// Il rilascio come lo si vede nei dati del 15/08: ~50 ms di rate elevato PRIMA
// che il trigger scatti. E' il pezzo che mancava.
static void rilascio_poi_trigger(int ms_rilascio = 50) {
  campione(150, 3.0f, ms_rilascio);
  tempo_on_fire(T);
}
static void mostra(const char* etichetta, const char* atteso) {
  TempoTiro r = tempo_ultimo();
  char m[16], a[16];
  if (r.mira_ms   == TEMPO_NOT_SET) snprintf(m, sizeof(m), "n/d(%u)", r.motivo);
  else                              snprintf(m, sizeof(m), "%u", r.mira_ms);
  if (r.alzata_ms == TEMPO_NOT_SET) snprintf(a, sizeof(a), "n/d");
  else                              snprintf(a, sizeof(a), "%u", r.alzata_ms);
  printf("  %-42s mira=%-9s alzata=%-6s   %s\n", etichetta, m, a, atteso);
}

int main() {
  printf("\nBanco tempi F20b — ogni scenario include il rilascio prima del trigger\n\n");

  tempo_init();
  campione(2, 0, 2000, false); campione(45, 0.4f, 900); campione(2, 0.05f, 2500);
  rilascio_poi_trigger();
  mostra("1) alzata 900 + mira 2500 + rilascio", "atteso ~2500 / ~620");

  tempo_init();
  campione(2, 0, 1000, false); campione(45, 0.4f, 800); campione(2, 0.05f, 1800);
  campione(40, 0.3f, 250); campione(2, 0.05f, 1200);
  rilascio_poi_trigger();
  mostra("2) riassestamento a meta' mira", "atteso ~1200 / ~250");

  tempo_init();
  campione(2, 0, 3000, false);
  tempo_on_fire(T);
  mostra("3) mai in postura", "atteso n/d(1)");

  tempo_init();
  campione(2, 0.05f, 5000);
  rilascio_poi_trigger();
  mostra("4) arco su fermo, nessuna trazione", "atteso mira lunga / n/d");

  // Il caso che il banco F20 non poteva vedere e che sul campo ha fallito
  // dieci volte su dodici: un rilascio piu' lungo della media.
  tempo_init();
  campione(2, 0, 1000, false); campione(45, 0.4f, 700); campione(2, 0.05f, 3000);
  rilascio_poi_trigger(80);
  mostra("5) rilascio lungo, 80 ms prima del trigger", "atteso ~3000, NON n/d");

  // Ancora troppo vecchia: fra la fine della mira e il trigger passa mezzo
  // secondo di movimento. Non e' il rilascio di quella mira.
  tempo_init();
  campione(2, 0, 1000, false); campione(45, 0.4f, 700); campione(2, 0.05f, 2000);
  campione(120, 2.0f, 500);
  tempo_on_fire(T);
  mostra("6) ancora finita 500 ms prima del trigger", "atteso n/d(2), non 2000");

  printf("\n");
  return 0;
}
