#!/bin/sh
# ============================================================================
#  ArchBB — banco della macchina dei tempi (Fase 20)
# ----------------------------------------------------------------------------
#  La FSM di tempo.cpp e' logica pura: dipende solo dai campi di ImuSample e
#  da nessun hardware. Quindi si puo' PROVARE, non solo compilare — ed e' il
#  primo pezzo di questo firmware per cui vale.
#
#  Quattro scenari sintetici, con l'esito atteso scritto accanto. Il quarto e'
#  il piu' istruttivo: arco tenuto su fermo senza mai una trazione. La macchina
#  riporta una mira lunga e un'alzata NON MISURATA, ed e' corretto cosi' — il
#  campo alzata vuoto e' il segnale che nessuna trazione e' stata vista.
#
#  USO   sh tools/tempo/prova.sh        (dalla radice del progetto)
# ============================================================================
D="$(dirname "$0")"
g++ -std=gnu++11 -Wall -I"$D/../sintassi" -I"$D/../../src" \
    "$D/prova_tempo.cpp" "$D/../../src/tempo.cpp" -o /tmp/archbb_prova_tempo || exit 1
/tmp/archbb_prova_tempo
