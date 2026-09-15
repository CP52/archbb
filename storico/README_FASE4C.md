# ArchBB 1.83 — Fase 4c (angoli cant/alzo dal burst)

**Versione firmware:** `1.83-Fase4c-angoli` · Cesare Pagura · 19 luglio 2026

Chiude l'ultimo anello della catena IMU: al termine dello scoring il burst
congelato nel circular buffer viene letto e trasformato negli angoli
biomeccanici, mostrati nel riepilogo.

---

## 1. Cosa è cambiato rispetto alla base (Scoring/TouchCal)

| File | Modifica |
|---|---|
| `shot_angles.h/.cpp` | **NUOVI** — portati invariati dalla 1.69 v2.15. Codice già validato: non toccato. |
| `mount.h/.cpp` | **NUOVI** — portati dalla 1.69. Rimappa gli assi grezzi nel frame canonico. |
| `config.h` | + `ARCHBB_MOUNT_DEFAULT` (default `0` = MOUNT_0 frontale, prova 19/07; confermare 0 vs 180). Versione FW aggiornata. |
| `imu.cpp` | `mount_apply()` cablato nel punto `[FUTURO]` dell'`imu_task`, a monte di tutto. + `imu_read_raw()` per il DIAG angoli. |
| `imu.h` | + prototipo `imu_read_raw()`. |
| `scoring.h` | `ScoreResult` esteso **in coda** con i campi metrici (angles/hold/release + flag di validità). |
| `display.h/.cpp` | Riepilogo esteso (nuova firma che riceve `ScoreResult`); + `displayAngleDiag()`. |
| `touch_cal.cpp` | Menu d'avvio a **tre voci**: aggiunta *DIAG angoli* (ritorna 3). |
| `main.cpp` | `computeShotAngles()` al DONE; burst **static**; IMU init prima del menu; DIAG angoli + **CSV seriale** (env debug). |

Nessuna nuova libreria: `mount` e `shot_angles` usano solo `<math.h>`.

---

## 2. ✅ Montaggio MISURATO: MOUNT_0_FLIPZ (test del 19/07)

Il test statico col DIAG angoli ha dato il verdetto, e **non** è quello che la
prova del trigger lasciava supporre. Dati misurati (arco fermo, |g|≈9.98):

| gesto | asse che si muove | valore grezzo | segno voluto |
|---|---|---|---|
| riposo verticale | — | x+9.9 y+0.8 z+0.5 | ~0 |
| punta freccia **su** | z | **−1.6** (negativo) | positivo → **INVERTITO** |
| cant a **destra** | y | **+5.8** (positivo) | positivo → ok |

Il **cant era già giusto**, ma l'**alzo invertito**: alzando la punta, `az`
andava negativo. Serviva una trasformazione che lasci `ay` e neghi solo `az` —
cioè **non una rotazione** (le 4 di `mount.h` sono a det=+1), ma una
**riflessione**: il chip della 1.83 è saldato *specchiato* sull'asse freccia
rispetto alla 1.69.

**Soluzione:** ho aggiunto a `mount.h` un quinto montaggio `MOUNT_0_FLIPZ`
(`cax=ax, cay=ay, caz=−az`), impostato come default
(`ARCHBB_MOUNT_DEFAULT = 4`). Verificato sui tre gesti: riposo≈0, punta su →
alzo **+9.3**, cant destra → cant **+34.9**. Tutti i segni corretti.

> **La lezione:** il test empirico "il trigger scatta frontale" aveva ristretto
> a {0, 180} guardando solo `|az|`. Ma il montaggio vero non era né 0 né 180 —
> era uno *specchio* che nessuna rotazione produce. Solo il DIAG, guardando i
> **segni** e non il modulo, l'ha rivelato. È esattamente il motivo per cui i
> segni si validano SOLO su test statico ad angolo noto (§4 handoff).

### Offset residuo (non è un errore)

A riposo il DIAG mostra cant≈+4.6, alzo≈−2.9 invece di 0/0: è l'**offset di
montaggio** (supporto non perfettamente verticale, o micro-tilt di saldatura).
La 1.69 aveva lo stesso fenomeno (cant residuo ~−5° costante, in
`shot_angles.cpp`). **Non è un errore di segno** — le *variazioni* hanno verso
corretto. La correzione dell'offset è un miglioramento futuro separato.

### Se vuoi ri-verificare col supporto ad angolo noto

Menu d'avvio → `DIAG angoli`. La riga **`0 flipZ`** (evidenziata in ciano, è il
default) deve dare: riposo ≈0, punta su alzo **positivo**, cant destra cant
**positivo**. Col supporto ad angolo noto puoi validare anche il *valore*, non
solo il segno.

### Leggere il CSV dalla seriale (test al tavolino)

Il DIAG emette una riga CSV/s sulla seriale (env debug). Colonne:
```
t_ms,ax,ay,az,gmean,gsd,n,cant0,alzo0,cantDX,alzoDX,cantSX,alzoSX,cant180,alzo180,cantFZ,alzoFZ
```
`cantFZ/alzoFZ` sono le colonne del montaggio scelto (FLIPZ). Upload con:
```
pio run -e archbb_183_debug -t upload
pio device monitor
```
(su PowerShell i comandi vanno su due righe: `&&` non è valido lì).
Per il campo, env `archbb_183_touchcal` (muto).

---

## 3. Il riepilogo esteso

Dopo lo scoring il riepilogo mostra, oltre a esito/zona/distanza/elev/ARCS, una
**card metriche**:

- **cant / alzo** — assetto pre-scocco. Se la finestra non è stabile
  (`stable=false`) i valori sono in **grigio con `~`** e compare l'avviso
  *"arco non fermo: assetto incerto"*: l'angolo c'è ma è poco attendibile.
- **tenuta** (hold) — follow-through a +900ms, con classe colorata
  (verde TENUTO / ambra LIEVE / rosso ABBASSATO).
- **rilascio** (release) — jerk laterale nei 40ms, con classe
  (verde PULITO / ambra MEDIO / rosso STRAPPO).

Metrica non calcolabile (burst troncato, tiro manuale senza IMU) → **n/d**,
mai uno zero muto. Stessa filosofia della sentinella elevazione.

> **Nota sulle soglie release (330/510):** sono **relative** alla sessione
> 14JUL della 1.69 (un arciere, un arco, un giorno). Dicono "pulito per te oggi",
> non in assoluto. Fino a più sessioni, usa `release` per confrontare tiri fra
> loro, non per un giudizio assoluto. (Documentato in `shot_angles.h`.)

---

## 4. Scelte tecniche degne di nota

- **Burst static, non sullo stack.** `750 × 30 B ≈ 22 KB`: come locale del loop
  farebbe overflow. Vive nel `.bss`, allocato una volta.
- **Calcolo al DONE, prima del reset.** `computeShotAngles()` legge il buffer
  quando è `READY`; `enterAttesa()` lo resetta *dopo*. Ordine: leggi il burst,
  poi torna in attesa.
- **IMU inizializzata prima del menu.** Serve al DIAG angoli, che legge il
  sensore direttamente (`imu_read_raw`) mentre i task non sono ancora avviati:
  nessuno muove `g_latest_sample`, nessun conflitto sul bus.
- **`mount_apply` a monte, non a valle.** Applicato in `imu_task` prima di
  trigger/buffer/angoli: da lì tutto il firmware vede il frame canonico e
  `shot_angles` gira invariato. È l'architettura della 1.69.

---

## 5. Dopo la 4c (roadmap)

Nell'ordine, un pezzo alla volta validato:

1. **TEST STATICO** dei segni (sopra) → chiudere `ARCHBB_MOUNT_DEFAULT`.
2. **microSD** — salvare burst + risultato su file (il motivo della 1.83).
3. **Companion app SD-based** — solo download sessione + edit config.
4. **Secondo tasto** start/stop sessione.
5. **BLE 1.83** (→ Core 0) e **Fase 5 NVS+CONFIG**.

**Non** procedere a SD/app finché il test statico non ha confermato i segni.
