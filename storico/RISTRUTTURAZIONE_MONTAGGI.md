# ArchBB 1.83 — Ristrutturazione montaggi: flip-Z come proprieta' hardware

**Versione: `1.83-Fase7-mount4pure`** · caratterizzazione 22/07

## La domanda (di Cesare)
"Perche' DUE zeri (0 e 0_FLIPZ)? Se la 1.83 ha il chip con Z invertito, non basta
specchiare Z per tutte le posizioni, invece di avere una quinta posizione a se'?"

## La risposta, coi DATI (caratterizzazione fisica del chip)
Firmware di test in pose di gravita' note (schermo su, posizione 0, punta su):

| posa | grezzo misurato | conclusione |
|---|---|---|
| schermo in alto | az=+9.80 | Z e' perpendicolare al PCB |
| posizione 0 (arco dritto) | ax=+9.89 | X e' la verticale (gravita') |
| punta su ~30 | az=-4.9 | Z e' INVERTITO vs convenzione |

**Conclusioni:**
1. In posizione 0 gli assi hanno i RUOLI GIUSTI: X=verticale (grav), Y=laterale
   (cant), Z=asse-freccia (alzo). **Nessuno scambio di assi** (era il timore).
2. L'unico problema: l'asse Z del chip 1.83 e' orientato all'OPPOSTO della
   convenzione delle formule (punta su -> az negativo, dovrebbe essere positivo).
3. Quindi il "flip" NON e' un montaggio: e' una **proprieta' fisica della
   scheda**. Va applicato SEMPRE, a monte, non come 5a posizione.

Cesare aveva ragione: basta specchiare Z, e va fatto per tutte le posizioni.

## Cosa e' cambiato
- **Eliminato MOUNT_0_FLIPZ** (la quinta posizione). Restano le 4 rotazioni
  PURE, quelle validate sulla 1.69 (det=+1), invariate.
- **Flip-Z hardware** (`ARCHBB_HW_FLIP_Z=1`, mount.h): pre-passo dentro
  `mount_apply` che nega az e gz PRIMA della rotazione. Un solo punto, non
  quattro matrici modificate. Se un domani una revisione di scheda avesse il
  chip "diritto", basta `ARCHBB_HW_FLIP_Z=0`.
- **Default montaggio** torna a `MOUNT_0` (0): il flip lo mette mount_apply.
- **CONFIG_VERSION 1 -> 2**: il bump scarta i config v1 col vecchio default 4
  (ora invalido, MOUNT_COUNT=4) -> riparte pulito da MOUNT_0.

## Verifica di NON REGRESSIONE (fondamentale)
Il nuovo schema e' stato verificato EQUIVALENTE al vecchio MOUNT_0_FLIPZ sui
grezzi reali misurati:

| posa | vecchio FLIPZ (cant,alzo) | nuovo (cant,alzo) | esito |
|---|---|---|---|
| schermo su   | +85.8, -89.8 | +85.8, -89.8 | identico |
| posizione 0  | +2.1, +0.2   | +2.1, +0.2   | identico |
| punta su ~30 | +10.0, +30.0 | +10.0, +30.0 | identico |

Gesto "punta su" -> alzo **+30.0** (positivo, corretto). In posizione 0 ->
cant/alzo ~0 (il residuo e' il micro-tilt dell'appoggio, non errore di formula).

**Conseguenza pratica: i dati che raccoglierai domani saranno coerenti con
quelli gia' registrati con MOUNT_0_FLIPZ.** Nessuna rottura.

## File toccati
- `src/mount.h` — enum a 4 orientamenti; `ARCHBB_HW_FLIP_Z`; flip-Z pre-passo in
  `mount_apply`; rimosso case e nome MOUNT_0_FLIPZ.
- `src/config.h` — `ARCHBB_MOUNT_DEFAULT = 0`; documentazione caratterizzazione.
- `src/config_store.h` — `CONFIG_VERSION = 2`; commento montaggio aggiornato.
- `src/config_store.cpp` — commento default.
- `src/display.cpp` — array SHORT a 4 elementi (rimosso "0 flipZ").
- `src/main.cpp` — DIAG angoli a 4 montaggi (rimossa colonna FLIPZ).
- `src/config.h` — versione -> `1.83-Fase7-mount4pure`.

## Da verificare sul campo (domani, coi tiri)
1. In ATTESA, DIAG angoli (se lo usi): posizione 0 a riposo -> cant/alzo ~0.
2. Punta su -> alzo positivo; cant a destra -> cant positivo. Come prima.
3. I valori di cant/alzo nel CSV devono essere coerenti con quelli che gia'
   vedevi (il cambio e' trasparente). Se lo sono, la ristrutturazione e' pulita.

## Nota di metodo
Questa e' la dimostrazione perfetta del "i dati comandano": un dubbio strutturale
("perche' due zeri?") ha portato a CARATTERIZZARE il chip invece di fidarsi di
un'assunzione ereditata dalla 1.69. Il risultato e' piu' pulito (4 posizioni +
una proprieta' hardware, invece di 5 posizioni con una speciale) e ora
SAPPIAMO perche', con tre letture di gravita' invece di una congettura.
