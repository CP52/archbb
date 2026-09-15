#!/bin/sh
# ============================================================================
#  ArchBB — controllo di sintassi senza toolchain ESP32
# ----------------------------------------------------------------------------
#  A COSA SERVE
#    Compilare per ESP32 richiede la toolchain completa e un paio di minuti.
#    Questo banco usa g++ con stub minimi di Arduino.h / TFT_eSPI.h / FreeRTOS
#    e fa un -fsyntax-only in gnu++11 — lo STESSO standard del core Arduino-ESP32.
#    Non sostituisce la build vera: non linka, non vede gli header veri delle
#    librerie. Ma intercetta in due secondi gli errori di linguaggio, che sono
#    la maggioranza di quelli che si fanno scrivendo.
#
#  PERCHE' gnu++11 E NON PIU' RECENTE
#    E' lo standard con cui il core compila. Provare in C++17 darebbe per buone
#    cose che sul dispositivo non passano — ad esempio l'inizializzazione con
#    graffe di una struct che ha inizializzatori di default sui membri: legale
#    da C++14, errore in C++11. E' esattamente l'errore che ha motivato questo
#    banco.
#
#  USO   sh tools/sintassi/controlla.sh          (dalla radice del progetto)
# ============================================================================
STUB="$(dirname "$0")"
SRC="$(dirname "$0")/../../src"
STD="-std=gnu++11 -Wall -Wextra -fsyntax-only"
esito=0

prova() {
  file="$1"; shift
  printf "  %-28s " "$(basename "$file")"
  if g++ $STD -I"$STUB" -I"$SRC" "$@" "$file" 2>/tmp/archbb_sint.log; then
    if [ -s /tmp/archbb_sint.log ]; then echo "ok (con warning)"; cat /tmp/archbb_sint.log | head -6
    else echo "ok"; fi
  else
    echo "ERRORE"; head -12 /tmp/archbb_sint.log; esito=1
  fi
}

echo "Controllo di sintassi (gnu++11)"
prova "$SRC/clino.cpp"
prova "$SRC/mount.cpp"
prova "$SRC/shot_angles.cpp"
prova "$SRC/sd_storage.cpp"
prova "$SRC/ble_service.cpp"
prova "$SRC/config_store.cpp"
prova "$SRC/scoring.cpp"
prova "$SRC/circular_buffer.cpp"
prova "$SRC/main.cpp"
prova "$SRC/display.cpp"
# F19: config_ui.cpp entra nel banco. E' stato toccato (due righe nuove) e la
# sua geometria dei pulsanti aveva gia' un difetto silenzioso: la lista dei
# valori passava sotto i tasti. Un file "che non si tocca da mesi" smette di
# esserlo nel momento in cui lo si tocca — ed e' quello il momento in cui il
# cono d'ombra del banco costa.
prova "$SRC/config_ui.cpp"
prova "$SRC/tempo.cpp"
# FASE 24a: la politica energetica entra nel banco. Non dipende da librerie
# hardware (il backlight lo possiede display.cpp, il PMU battery.cpp),
# quindi qui e' controllabile per intero.
prova "$SRC/power_mgr.cpp"

# NON coperti: touch.cpp, imu.cpp, battery.cpp, rtc_clock.cpp,
# touch_cal.cpp. Dipendono da librerie hardware (SensorLib/QMI8658, driver del
# PMU) e da define che arrivano dai build_flags di PlatformIO. Stubbarli
# costerebbe piu' della copertura che darebbero: sono file stabili, che non si
# toccano da mesi. Se un giorno li si modifica, si compila per davvero.
prova "$SRC/mount.cpp" -D ARCHBB_HW_FLIP_Z=0   # anche col flip spento
# FASE 24a: display.cpp contiene DUE rami LEDC dietro una guardia di versione
# del core. Senza questa seconda passata il banco controllerebbe solo il ramo
# 2.x, cioe' quello che NON verra' compilato sul dispositivo (core 3.0.5).
# Un ramo mai controllato e' un ramo che rompe la build il giorno del campo.
prova "$SRC/display.cpp" -D ESP_ARDUINO_VERSION_MAJOR=3
# ============================================================================
#  Controllo dei FONT NUMERICI di TFT_eSPI
# ----------------------------------------------------------------------------
#  I font 6, 7 e 8 contengono SOLO cifre, punto, due punti e il meno (il 6
#  aggiunge poche lettere per l'AM/PM). Scriverci testo o un "+" non produce
#  alcun errore: i caratteri mancanti spariscono e basta. E' capitato due volte
#  — il "+" degli slider e "COLPITO" nel riepilogo — quindi tanto vale cercarlo.
echo
echo "Font numerici (6/7/8) con caratteri non disponibili"
trovati=0
for f in "$SRC"/*.cpp; do
  grep -n 'drawString(".*", *[0-9A-Za-z_ +*/()-]*, *[0-9A-Za-z_ +*/()-]*, *[678])' "$f" 2>/dev/null |
  while IFS= read -r riga; do
    testo=$(printf '%s' "$riga" | sed 's/.*drawString("\([^"]*\)".*/\1/')
    case "$testo" in
      *[!0-9.:\ -]*) echo "  SOSPETTO $(basename "$f"):$riga" ;;
    esac
  done
done
echo "  (le variabili non sono controllabili qui: valgono gli occhi)"

# ============================================================================
#  Dichiarato ma non definito  (il controllo che mancava)
# ----------------------------------------------------------------------------
#  PERCHE' ESISTE
#    Il banco qui sopra fa -fsyntax-only: NON linka. Il 31/07, ripulendo
#    display.cpp da una funzione ritirata, il taglio ha portato via anche
#    displayBleMode(), che stava subito dopo. Sintassi: tutto ok. Link sul
#    dispositivo: "undefined reference". Un intero giro di compilazione buttato
#    per un errore che si vede con una grep.
#
#  COSA FA
#    Per ogni funzione dichiarata in un .h, cerca una definizione in qualche
#    .cpp. Non e' un linker e non pretende di esserlo: non vede le firme, non
#    vede i template, non vede i namespace. Vede il caso che ci ha morso, che e'
#    poi il caso frequente — una definizione sparita durante una pulizia.
#
#  PROVATO IN NEGATIVO: rimuovendo a mano la definizione di displayBleMode il
#  controllo la segnala. Uno strumento che non si accende e' peggio di non averlo.
echo
echo "Dichiarato in un .h ma non definito in alcun .cpp"
: > /tmp/archbb_manca.log
for h in "$SRC"/*.h; do
  grep -hE '^[A-Za-z_][A-Za-z0-9_ \*&:]*[ \*&]([A-Za-z_][A-Za-z0-9_]*) *\(' "$h" \
  | grep -v 'inline' | grep -v 'typedef' | grep -v 'define' | grep -v '__attribute__' \
  | sed -E 's/.*[ \*&]([A-Za-z_][A-Za-z0-9_]*) *\(.*/\1/' | sort -u \
  | while read -r fn; do
      [ -z "$fn" ] && continue
      if ! grep -qE "^[A-Za-z_][A-Za-z0-9_ \*&:]*[ \*&]$fn *\(" "$SRC"/*.cpp; then
        echo "  MANCA  $fn()   dichiarata in $(basename "$h")" | tee -a /tmp/archbb_manca.log
      fi
    done
done
if [ -s /tmp/archbb_manca.log ]; then esito=1; else echo "  nessuna"; fi

exit $esito
