// ============================================================================
//  ArchBB 1.83 — clino.h · FASE 18 (clinometro di banco)
//  Cesare Pagura · Padova/Noale IT · 31 luglio 2026
// ----------------------------------------------------------------------------
//  A COSA SERVE, E PERCHE' SOSTITUISCE DUE DIAGNOSTICHE
//
//  Il progetto ha affrontato quattro volte la domanda "quali sono gli assi e
//  che segno hanno", con inferenze statistiche, un DIAG dinamico e ragionamenti
//  geometrici: quattro risposte, tre sbagliate. L'ha risolta l'ispezione fisica
//  del chip. La lezione del briefing del 30/07 e' esplicita: non riaprire la
//  questione senza una nuova OSSERVAZIONE.
//
//  Questo modulo e' quell'osservazione, resa ripetibile. Non e' un'altra
//  inferenza: e' uno STRUMENTO DI MISURA ASSOLUTO. L'operatore conosce la
//  verita' (l'arco e' a piombo, la freccia e' orizzontale, la punta e' alzata
//  di 30 gradi) e legge cosa dichiara il dispositivo. La verifica diventa
//  binaria, non statistica.
//
//  Sostituisce:
//    - la voce di menu "DIAG angoli" (mostrava cant/alzo dei 4 montaggi in
//      parallelo: serviva a SCEGLIERE il montaggio, scelta ormai fatta e
//      congelata in config);
//    - l'ambiente di build archbb_183_assi / main_diag_assi.cpp (il DIAG
//      dinamico della Fase 12, mai passato sul campo).
//
//  DUE PAGINE, DUE DOMANDE DIVERSE
//
//   1) ASSETTO — quanto vale l'angolo, adesso.
//      Alzo e cant dall'accelerometro, nel frame canonico e col montaggio
//      configurato. E' la verifica STATICA dei segni: sei pose note al banco e
//      sei letture attese (protocollo nel README della fase).
//      Da qui si azzerano anche gli offset di montaggio (finora possibile solo
//      via BLE), con l'arco tenuto in assetto di riferimento.
//
//   2) COERENZA — il giroscopio racconta la stessa storia dell'accelerometro?
//      Questa e' la parte che vale davvero. Confronta la VARIAZIONE d'angolo
//      misurata dall'accelerometro con l'INTEGRALE del giroscopio sulla stessa
//      finestra. E' una verifica CHIUSA: non richiede alcuna verita' esterna,
//      perche' i due sensori si controllano a vicenda. Produce una matrice 2x2
//      di pendenze che dice, in un colpo d'occhio:
//          diagonale ~ +1, fuori diagonale ~ 0   -> assi e segni corretti
//          diagonale ~ -1                         -> segno invertito
//          fuori diagonale dominante              -> assi scambiati
//
//  PERCHE' LA COERENZA E' IL PONTE VERSO L'ANALISI EX-POST
//    Il briefing chiede da dove vengano i ~5 gradi residui fra -80 ms e lo
//    scocco, e indica come strada l'assetto statico a +500 ms — un riferimento
//    indipendente mai usato. Quel confronto ha senso solo se ci si fida della
//    funzione che ricava l'assetto dall'accelerometro. Qui quella funzione
//    viene validata a mano, al banco, contro il giroscopio. E' il calibro, non
//    un accessorio.
//
//  VINCOLI OPERATIVI
//    - Si esegue dal MENU D'AVVIO, prima che i task FreeRTOS partano: nessuno
//      muove g_latest_sample, si legge direttamente con imu_read_raw() (frame
//      SENSORE, mount NON applicato: lo applica questo modulo).
//    - RITORNA al chiamante (a differenza del vecchio runAngleDiag, che era un
//      capolinea): si misura, si esce, si va a tirare senza spegnere.
// ============================================================================
#pragma once

#include <TFT_eSPI.h>

// ----------------------------------------------------------------------------
//  clino_run — esegue il clinometro fino a che l'operatore preme ESCI.
// ----------------------------------------------------------------------------
//  Richiede: display gia' inizializzato, touch gia' inizializzato, IMU gia'
//  inizializzata (imu_init in setup) e task IMU NON ancora avviati.
//  Effetto collaterale possibile: se l'operatore conferma AZZERA, scrive
//  g_config.offset_alzo_deg / offset_cant_deg e salva in NVS (config_save).
void clino_run(TFT_eSPI& tft);
