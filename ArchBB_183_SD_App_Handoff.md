# 🏹 ArchBB 1.83 — Handoff per microSD + Companion App

**Briefing per la prossima sessione · Cesare Pagura · Padova/Noale IT · 19 luglio 2026**

> **Come usare questo file:** aprilo come primo messaggio nella nuova chat e di'
> *"ripartiamo da qui"*. Porta con te l'ultimo zip firmware
> (`ArchBB_183_Fase4c.zip`, versione `1.83-Fase4c-angoli`) e il repo
> `github.com/CP52/archbb` come riferimento.

---

## 0. Dove siamo (in una riga)

La **Fase 4c è chiusa**: la catena IMU è completa fino agli angoli, il montaggio
è misurato e validato, il riepilogo mostra cant/alzo/hold/release. Il prossimo
capitolo è la **persistenza su microSD** e la **companion app** che legge le
sessioni — il motivo originale della migrazione a 1.83.

---

## 1. Stato del progetto ArchBB 1.83

### ✅ Completato e validato sul campo

| Fase | Cosa | Stato |
|---|---|---|
| 1 | Display ST7789V2 (240×284, offset y=20, BGR) | ✅ hardware |
| 2 | Touch CST816 (I2C condiviso, polling INT-gated, calibrato) | ✅ hardware |
| 3 | Scoring UI (ESITO→ZONA→DIST+ELEV→RIEPILOGO) | ✅ validato |
| 4a | IMU QMI8658 + trigger reale | ✅ campo |
| 4b | Circular buffer 750 campioni PSRAM + freeze su FIRED | ✅ fatto |
| 4c | **Angoli cant/alzo dal burst + montaggio + riepilogo esteso** | ✅ campo |

### 🎯 La Fase 4c, in dettaglio (appena chiusa)

- **Montaggio MISURATO** col test statico (DIAG angoli, 19/07): il QMI8658 della
  1.83 è saldato **specchiato** sull'asse freccia rispetto alla 1.69. Non è
  nessuna delle 4 rotazioni standard: serve una **riflessione** che nega solo
  `az`/`gz`. Aggiunto `MOUNT_0_FLIPZ` (=4) a `mount.h`, impostato come default
  (`ARCHBB_MOUNT_DEFAULT = 4` in config.h).
- **Validato sul campo:** finto scocco verso l'alto → alzo **positivo**, verso
  il basso → alzo **negativo**. Convenzione canonica confermata. ✅
- **`shot_angles.h/.cpp`** e **`mount.h/.cpp`** portati dalla 1.69, invariati.
  `mount_apply()` cablato in `imu_task` a monte di tutto → frame canonico a
  valle → shot_angles gira invariato.
- **Riepilogo esteso:** card metriche con cant/alzo (grigio+`~` se instabile),
  tenuta (hold), rilascio (release), colorate per classe. n/d se non calcolabili.
- **DIAG angoli** (menu d'avvio, 3ª voce verde): mostra i 5 montaggi affiancati
  + CSV seriale (env debug) per il test statico.

### ⚠️ Ancora da validare (non blocca la SD)

- **cant**: visto positivo-a-destra dai dati grezzi del DIAG, non ancora
  confermato con un gesto dedicato sul riepilogo. Il FLIPZ non lo tocca (nega
  solo z), quindi è quasi certamente ok. Verifica di 30s quando capita.
- **hold / release**: nascono dalla dinamica POST-rilascio (follow-through,
  jerk corda). Il "finto scocco" non li genera realisticamente → si validano
  solo con **tiri veri** sul campo.
- **offset di montaggio**: a riposo il DIAG dà cant≈+4.6, alzo≈−2.9 invece di
  0/0 (supporto non perfettamente verticale / micro-tilt saldatura). NON è un
  errore di segno (le variazioni hanno verso giusto). Correzione offset =
  miglioria futura separata.

---

## 2. 🎯 Da fare: microSD + Companion App

### La microSD

La card 32 GB FAT32 è formattata e pronta. Obiettivo: al DONE dello scoring
(dopo `computeShotAngles`, dove ora c'è il commento `[FUTURO] scrittura SD`),
salvare la sessione su file. È il motivo originale della 1.83 (buffer PSRAM → SD).

**Punto d'innesto già predisposto:** in `main.cpp`, `case AppFlow::RIEPILOGO`,
sul tap OK (`hit == 2`), prima di `enterAttesa()`. C'è già il commento
`// [FUTURO] qui andra' la scrittura su microSD del burst + risultato.`

### La companion app (prevista per questa versione)

Diversa dall'app BLE della 1.69. Deve fare **solo due cose**:
1. **Editare i parametri di configurazione** se/quando necessario.
2. **Scaricare la sessione di tiro** dalla SD.

Operatività start/stop sessione: **per tapping** oppure col **secondo tasto**
hardware (v. §4).

Alcuni **parametri di qualità del tiro** calcolati nel firmware, mostrati nel
riepilogo dopo lo scoring (già fatto in 4c: cant/alzo/hold/release).

---

## 3. ⚠️ LE 3 DECISIONI DI MERITO (da prendere prima di scrivere codice)

Queste tre scelte determinano formato file, dimensioni, complessità dell'app.
Sono le prime domande della prossima sessione.

### 3.1 Cosa fa l'app con la sessione scaricata?

- **Solo visualizzare** i risultati (ARCS + metriche) → salvo su SD solo i
  **risultati compatti**: pochi byte a tiro, un CSV leggibile.
- **Ri-analizzare i burst** (rifare angoli/hold/release con parametri diversi)
  → salvo anche i **burst grezzi interi**: ~22 KB a tiro (750×30 B).

Sono due file MOLTO diversi. La 32 GB regge entrambi (anche 22 KB × migliaia di
tiri sono spiccioli), ma la scelta cambia il formato e cosa deve saper fare l'app.

### 3.2 Come avviene il trasferimento SD → app?

- **Estrai la microSD** e la leggi da PC/telefono → filesystem puro, l'app legge
  file FAT32. Semplice, nessun protocollo.
- **Il firmware serve i file via BLE** alla companion app → serve un protocollo
  di trasferimento file su BLE (→ BLE su Core 0, come 1.69). Più lavoro, ma non
  devi estrarre la card.

### 3.3 Il secondo tasto hardware

Su quale **GPIO** è cablato il secondo tasto (start/stop sessione)? Se non è
ancora deciso fisicamente, va scelto. La gestione sessione dipende da lì.
(NB: attenzione ai GPIO già usati — display, touch, IMU condividono I2C
SDA=15/SCL=14, backlight GPIO40, touch INT=13/RST=39.)

---

## 4. Note hardware/architettura da ricordare (1.83)

- **Frame canonico** applicato a monte in `imu_task` via `mount_apply()`
  (MOUNT_0_FLIPZ): ax=gravità, ay=laterale, az=asse freccia. Da lì tutto il
  firmware vede il canonico.
- **ODR reale 224.9 Hz** (non 249): battimento tick FreeRTOS. Timestamp reali,
  non dt costante.
- **Burst:** `WINDOW_PRE_MS=2000` / `WINDOW_POST_MS=1000`, `CIRCULAR_BUFFER_SIZE=750`
  (~22 KB PSRAM). A 224 Hz un burst è ~672 campioni (margine ok).
- **PSRAM** attiva (`qio_opi`, octal). Il buffer è lì.
- **Tutto su Core 1** (niente BLE ancora). Quando arriva il BLE → Core 0.
- **Touch calibrato:** `TOUCH_CAL_AX=0.74591 BX=21.84 AY=0.86208 BY=21.68`. Non toccare.
- **USB CDC / seriale:** funziona con env debug (`archbb_183_debug`), ma è
  best-effort su HWCDC. Il CSV DIAG esce solo nel DIAG angoli, e serve premere
  RESET dopo aver aperto il monitor (monitor_dtr/rts=0 → il monitor non resetta).
  In campo si usa l'env normale, muto.

### Due env PlatformIO

- `archbb_183_touchcal` — campo, muto (default).
- `archbb_183_debug` — tavolino, +`ARCHBB_DEBUG=1` (log + CSV DIAG seriale).

---

## 5. Sulla domanda "montarlo a destra?" (risposta per il futuro)

Non basta cambiare in `MOUNT_DX_M90`. Poiché la 1.83 è già **specchiata**
(FLIPZ), montarla a destra significa **comporre** la rotazione DX_M90 con lo
specchio → una 6ª matrice che non esiste ancora. Il modo giusto è sempre il
DIAG angoli: monti, fai i 3 gesti noti, leggi quale montaggio torna; se nessuno,
se ne aggiunge uno (come per il FLIPZ, 5 minuti).

Se il cambio di montaggio diventasse ricorrente → renderlo **selezionabile a
runtime** (menu on-device / app) e salvarlo in NVS. Che è la **Fase 5
(NVS+CONFIG)**, già in roadmap dopo la catena IMU.

---

## 6. Roadmap dopo SD + app

1. **microSD** — persistenza sessione (questo capitolo).
2. **Companion app SD-based** — edit config + download sessione.
3. **Secondo tasto** start/stop sessione.
4. **BLE 1.83** (→ Core 0) — se si sceglie il trasferimento via BLE (§3.2).
5. **Fase 5 — NVS + CONFIG**: NVS fonte di verità unica, menù on-device + app
   come editor sullo stesso store. Include il montaggio selezionabile (§5).

---

## 7. Metodo (invariato)

File completi versionati (mai patch), commenti italiani didattici, un pezzo alla
volta validato sul campo. **I dati comandano**: nessun parametro cambiato senza
evidenza. I segni degli angoli si validano SOLO su test statico ad angolo noto —
già fatto per la 4c (FLIPZ).

---

*Handoff microSD + App — 19 luglio 2026 — pronto per la nuova sessione.
Prima le 3 decisioni di merito (§3), poi il formato file, poi il codice. 🎯*
