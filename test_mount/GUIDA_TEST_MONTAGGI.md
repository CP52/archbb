# ArchBB 1.83 — Test Montaggi: guida e protocollo

**Firmware separato · env `archbb_183_mount_test` · 22 luglio 2026**

## Scopo
Rispondere coi dati a due domande sul montaggio dell'IMU (QMI8658) della 1.83,
sollevate osservando che la posizione 0 di questa scheda ha una geometria
diversa dalla 1.69:

- **A) Dove punta ogni asse FISICO del chip?** (caratterizzazione)
- **B) Quale montaggio da' cant/alzo col segno giusto?** e in particolare:
  *serve davvero il flip-Z? per una sola posizione o per tutte?*

## Come si carica

⚠️ **IMPORTANTE — seleziona ESPLICITAMENTE l'env del test.** Il default del
progetto e' il firmware di produzione (`archbb_183_touchcal`): se builder/flashi
col pulsante ▶ generico di VS Code o con `pio run` senza `-e`, carichi il
firmware NORMALE, non il test (ed e' esattamente il sintomo "mi builda la
versione precedente, non vedo i montaggi").

Da terminale (il modo sicuro):
```
pio run -e archbb_183_mount_test -t upload
pio device monitor -b 115200
```

In VS Code: nella barra di stato PlatformIO in basso, clicca sull'ambiente
(di solito mostra "env:archbb_183_touchcal") e scegli **env:archbb_183_mount_test**;
solo allora premi Upload. In alternativa, dal pannello PlatformIO ->
Project Tasks -> **archbb_183_mount_test** -> Upload.

Poi apri il monitor seriale a 115200. Sul display, **tocca** per commutare fra
le due viste (CHIP RAW / MONTAGGI). Il report seriale esce ~1 volta al secondo.

### Come funziona la selezione (per chi vuole saperlo)
Entrambi i main vivono in `src/` ma sono avvolti in guardie opposte: `main.cpp`
e' attivo `#ifndef ARCHBB_MOUNT_TEST`, `main_mount_test.cpp` e' attivo `#ifdef
ARCHBB_MOUNT_TEST`. L'env del test definisce quel flag, quindi compila il test e
svuota il main di produzione. Nessun conflitto, nessun `src_filter` fragile.

## Riferimento di montaggio (fissato dall'arciere)
- **Posizione 0**: schermo verso l'arciere, **retro della scheda verso il
  bersaglio** (l'asse-freccia ESCE dal retro, perpendicolare al PCB), USB/tasti
  a destra.
- **Posizione -90**: schermo verso destra, USB verso il bersaglio.

## Convenzione funzionale attesa dalle formule
Lo scoring calcola: `cant = atan2(ay, ax)`, `alzo = atan2(az, ax)`, dove
implicitamente X=gravita'/rollio, Y=laterale (cant), Z=mira (alzo). Il test
mostra assi **fisici e funzionali affiancati**: se all'alzo risponde un asse
diverso da Z, o con segno inatteso, il flip-Z stava mascherando uno **scambio di
assi**, non un semplice segno.

---

## PROTOCOLLO — Parte A: caratterizzazione (vista CHIP RAW)

A riposo l'accelerometro misura solo la **gravita'** (~9.8 m/s^2, verso il
basso). In ogni posa nota sai da che parte e' il "basso": leggi quale asse
fisico legge +9.8 (o -9.8) e costruisci la mappa del chip. Fai le 6 pose e
annota, per ognuna, l'**asse dominante** (lo mostra il display e il seriale).

Usa la scatola/angolo del tavolo come riferimento ripetibile. Non serve
precisione al grado: serve sapere quale asse e con che verso.

| # | posa della scheda | cosa e' rivolto in BASSO | annota asse dominante |
|---|---|---|---|
| 1 | **posizione 0** (schermo verso te, retro al muro/bersaglio) | il bordo INFERIORE | ........ |
| 2 | ruota di 180 nel suo piano (schermo verso te, capovolta) | il bordo SUPERIORE (ex) | ........ |
| 3 | posa lo **schermo** in orizzontale rivolto in ALTO (piatta sul tavolo) | il RETRO (asse-freccia) | ........ |
| 4 | posa il **retro** in orizzontale (schermo verso il tavolo) | lo SCHERMO (asse-freccia opposto) | ........ |
| 5 | posizione 0 poi inclina la **punta** (retro) verso l'ALTO ~30 | — (osserva quale asse CAMBIA) | alzo -> asse ........ segno ........ |
| 6 | posizione 0 poi inclina a **DESTRA** ~30 (cant) | — (osserva quale asse cambia) | cant -> asse ........ segno ........ |

**Lettura chiave (pose 5 e 6):**
- Posa 5 (punta in su) simula ALZO positivo. **Quale asse cambia?** Se e' `az` e
  diventa piu' positivo -> alzo = atan2(az,ax) **senza** flip. Se `az` diventa
  piu' NEGATIVO -> serve il flip-Z. Se cambia un asse **diverso da z** -> le
  formule hanno gli assi scambiati (il flip era un cerotto).
- Posa 6 (inclina a destra) simula CANT positivo. Stesso ragionamento su `ay`.

---

## PROTOCOLLO — Parte B: verifica matrici (vista MONTAGGI)

Tieni la scheda in **posizione 0** su un piano il piu' possibile neutro
(cant~0, alzo~0). La vista MONTAGGI mostra cant/alzo calcolati per TUTTI i
montaggi sullo stesso grezzo. Poi ripeti i gesti noti (punta su, inclina a
destra) e guarda **quale riga** (quale montaggio) da' i segni concordi:

- **punta in su** deve dare **alzo positivo**
- **inclina a destra** deve dare **cant positivo**

Il montaggio la cui riga rispetta ENTRAMBI i versi in TUTTE le prove e' quello
corretto per la 1.83. Annota quale.

**La domanda del flip-Z si risolve cosi':**
- Se il montaggio corretto risulta `0_FLIPZ` -> il flip serve, ed e' reale.
- Se risulta uno dei 4 "puliti" (0/-90/+90/180) -> il flip-Z era superfluo e la
  quinta posizione si puo' eliminare.
- Se NESSUNO da' entrambi i segni giusti -> le formule stesse vanno riviste per
  questa geometria (assi da rimappare), e ne ridisegniamo le matrici partendo
  dalla mappa fisica della Parte A.

---

## Cosa mandarmi
Per chiudere la questione mi bastano:
1. La tabella della Parte A compilata (6 righe: asse dominante per posa).
2. Dalla Parte B: quale montaggio da' alzo+ con "punta su" e cant+ con "destra".
3. Se comodo, incolla 3-4 blocchi del report seriale (uno per posa 1, 3, 5, 6):
   contengono i grezzi e la tabella dei montaggi, cosi' verifico i numeri.

Da li' decidiamo insieme, sui DATI, se la 1.83 tiene le 4 matrici della 1.69 +
flip, oppure se merita un set di matrici tutto suo. "I dati comandano" — e
finalmente li avremo per QUESTA scheda, non ereditati dalla 1.69.

## Nota
Il test riusa i moduli REALI (`mount.cpp`, `imu`, `touch`, `display`): stai
verificando il codice di produzione, non una copia. L'unica differenza col
firmware vero e' il `main` (test invece di scoring) e l'assenza di BLE/SD, non
necessari qui.
