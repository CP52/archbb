# CHANGELOG — ArchBB Firmware v2.13.0 (ridisegno schermata DISTANZA)

**Data:** 14 luglio 2026
**Base:** v2.12.6
**File:** `src/scoring_ui.cpp`, `src/config.h` (versione).

## Motivazione (criticita' campo #3)
La valutazione dello score sul campo era poco ergonomica, in particolare la
schermata DISTANZA: aveva due tasti "orizzontali" (CONFERMA a barra + tacche
18/30/50) scomodi da centrare col guanto. Dall'uso e' emerso che i tasti a
TUTTA ALTEZZA sul bordo sono i piu' efficaci (feedback tattile del bordo).

## Nuovo layout DISTANZA
- SLIDER orizzontale grande, range fisso 5-60 m: un tocco diretto sulla barra
  imposta la distanza (tocco = posizione). Tacca circolare + porzione attiva.
- Tasti +/- grandi ai lati per l'aggiustamento fine (56x40 px).
- "?" IGNOTA (=0) al centro della fascia tasti.
- In basso DUE TASTI GRANDI VERTICALI a tutta altezza (110px): INDIETRO (sx,
  torna a ZONA) e OK (dx, verde, va al RIEPILOGO). Discriminati per LATO (X),
  come nel riepilogo: massima area tattile, niente bande Y strette.

## Rimosso
- Tacche rapide 18/30/50 (giudicate scomode/inutili sul campo).
- Vecchio CONFERMA a barra orizzontale e vecchio INDIETRO piccolo.

## Coordinate tattili (per riferimento/taratura)
- Slider:  y [76..108], x [24..216] -> xToDist(tx)
- "-":     x [16..72]   y [118..158]
- "?":     x [92..148]  y [118..158]  (IGNOTA)
- "+":     x [168..224] y [118..158]
- INDIETRO/OK: y [170..280], lato sx/dx (confine x=120)

## Invariato
Le altre schermate (esito, zona, riepilogo), le soglie touch, il flusso
scoring e lo ScorePacket restano identici.

## Verifica
- g++ -fsyntax-only: OK.
- Nessun residuo del vecchio layout (drawBackButton resta solo in ZONA).

## Da verificare sul campo
- Centratura dello slider col guanto (soglie x [24..216]).
- Confine INDIETRO/OK a x=120 (come nel riepilogo, gia' collaudato).
