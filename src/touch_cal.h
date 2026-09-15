// ============================================================================
//  ArchBB 1.83 — touch_cal.h · strumento di calibrazione del touch
//  Cesare Pagura · Padova/Noale IT · 17 luglio 2026
//  Portato dalla 1.69 v2.16.x, adattato al disegno diretto su TFT (niente sprite)
// ----------------------------------------------------------------------------
//  PERCHE' ESISTE QUESTO MODULO
//    Sintomo sul campo (scoring DIST+ELEV): i tasti "-"/"+" (48px) rispondono
//    male, e solo sul bordo verso il centro; gli slider (192px) funzionano
//    benissimo; ZONA e RIEPILOGO (bersagli grandi) non hanno mai dato problemi.
//
//    Diagnosi: NON e' un bug della schermata nuova. E' una discrepanza fra le
//    coordinate del CST816 e quelle del display, presente DA SEMPRE e mascherata
//    dai bersagli grandi. I tasti piccoli l'hanno solo resa visibile.
//
//    E' la stessa storia della 1.69 (§4.5 del compendio): li' le celle ZONA da
//    66px nascondevano cio' che i tasti da 32px rivelavano. Stesso film, altro
//    pannello.
//
//  PERCHE' NON SI RICAVA LA CALIBRAZIONE "A TAVOLINO"
//    Va documentato perche' non si ripeta il tentativo. Sulla 1.83 il primo
//    modello proposto e' stato: "il touch e' compresso verso il centro, ecco
//    perche' i tasti da 48px falliscono e gli slider da 192px no". Spiega il
//    sintomo alla perfezione. Ed e' ESATTAMENTE l'ipotesi che sulla 1.69 e'
//    costata SEI versioni e si e' rivelata FALSA: li' era stato dedotto "il
//    sensore e' piu' stretto del 28%" dagli span rx 31..203 letti in DIAG ->
//    AX=1.38953. Ma quegli span non erano i bordi del sensore: erano DOVE
//    L'UTENTE AVEVA TOCCATO. Il coefficiente inventato ha CAUSATO il
//    disallineamento che poi si cercava di correggere. Misurato per bene,
//    l'asse X era gia' allineato (AX=1.006).
//
//    Spiegare un sintomo non e' misurarlo. Un modello elegante che rende conto
//    dei fatti osservati resta un'ipotesi finche' non tocca dei bersagli noti.
//
//    REGOLA: gli span misurano l'UTENTE, non il chip. I bersagli a coordinata
//    nota misurano il chip. Si calibra solo cosi'.
//
//  COSA FA
//    Mostra 5 crocette in posizioni NOTE (4 angoli + centro), raccoglie il touch
//    grezzo per ciascuna, calcola la trasformazione affine ai minimi quadrati
//      x_display = ax * x_touch + bx
//      y_display = ay * y_touch + by
//    e la stampa a display insieme all'errore residuo. I coefficienti si
//    ricopiano a mano in config.h (TOUCH_CAL_*).
//
//    Perche' 5 punti e non 2: con 5 il fit e' SOVRA-determinato, quindi il
//    residuo e' una misura della BONTA' del modello, non un numero inventato.
//    Con 2 punti il residuo sarebbe zero per costruzione — e non direbbe nulla.
//    Se il residuo e' alto, il modello lineare non basta (rotazione,
//    specchiatura, non-linearita') e lo si scopre SUBITO invece che sul campo.
//
//    Perche' DUE passate: il residuo e la ripetibilita' sono cose DIVERSE.
//    Il residuo dice se i 5 punti di una passata stanno su una retta. Lo spread
//    dice se due passate indipendenti danno la STESSA retta. Un residuo basso
//    con spread alto significa "misuro con precisione una cosa diversa ogni
//    volta" — ed e' un caso realmente osservato sulla 1.69 (ay 0.700 vs 0.794,
//    +13%, con residui bassi in entrambe). Senza la seconda passata lo si
//    scopre solo sul campo, dopo aver compilato i numeri sbagliati.
//
//  SI PUO' SEMPRE SALTARE
//    Non e' obbligatoria e non blocca mai il dispositivo: tasto SALTA sempre
//    visibile e timeout automatico. Se si salta, il firmware usa i coefficienti
//    compilati in config.h e funziona esattamente come prima. Il default (non
//    toccare nulla) DEVE essere "vai avanti come sempre": la calibrazione e'
//    un'operazione da fare una volta, non a ogni accensione.
// ============================================================================
#pragma once
#include <TFT_eSPI.h>
#include <stdint.h>

// Esito di una sessione di calibrazione.
struct TouchCalResult {
  bool  valid;        // true se i punti sono stati raccolti e il fit e' riuscito
  float ax, bx;       // x_display = ax * x_touch + bx
  float ay, by;       // y_display = ay * y_touch + by
  float err_max;      // errore massimo residuo [px]: coerenza INTERNA di una passata
  float err_rms;      // errore RMS residuo [px]

  // RIPETIBILITA' — un'altra cosa dal residuo (vedi sopra).
  bool  repeatable;   // true se le due passate concordano entro il 3%
  float spread_ax;    // scarto % fra le due passate su ax
  float spread_ay;    // scarto % fra le due passate su ay
};

// Esegue l'intera procedura (BLOCCANTE: siamo in boot, nessun task da servire).
// Mostra i risultati e attende un tocco per uscire.
//   timeout_ms: tempo massimo PER SINGOLO PUNTO (non per l'intera procedura).
//               Se non si tocca nulla entro questo tempo esce con valid=false
//               (come SALTA). Serve a non lasciare MAI il dispositivo bloccato
//               in calibrazione: se il touch fosse rotto, sarebbe l'unico caso
//               in cui non si puo' nemmeno premere SALTA.
//               NB: e' PER PUNTO proprio perche' ripetere un punto e' normale;
//               un budget unico per tutta la procedura verrebbe eroso dalle
//               ripetizioni legittime e la farebbe morire a meta'.
TouchCalResult touch_cal_run(TFT_eSPI& tft, uint32_t timeout_ms = 300000);

// Schermata diagnostica: mostra i NUMERI (grezzi dal chip, corretti, span,
// scala stimata, e i coefficienti realmente attivi nel binario — smaschera il
// caso in cui non si sia ricompilato). Nessun effetto collaterale: sola lettura.
//
// NON RITORNA MAI: il DIAG e' un capolinea, si esce spegnendo. E' voluto —
// per leggere gli span bisogna TENERE PREMUTO negli angoli, cioe' proprio il
// gesto che una qualsiasi uscita-a-pressione intercetterebbe. Meglio nessuna
// uscita che un'uscita che sabota la misura.
[[noreturn]] void touch_cal_diag(TFT_eSPI& tft);

// Schermata d'ingresso a tre esiti:
//   0 = salta (default: timeout o tocco fuori dai bersagli)
//   1 = CALIBRA      -> touch_cal_run()
//   2 = DIAG touch   -> touch_cal_diag()
//   3 = DIAG angoli  -> runAngleDiag() nel main (Fase 4c, test statico segni)
int touch_cal_offer(TFT_eSPI& tft, uint32_t wait_ms = 3000);
