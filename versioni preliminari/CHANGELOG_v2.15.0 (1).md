# ArchBB — Firmware v2.15.0 + App v1.27

## Metrica RELEASE + 3 fix dashboard + riorganizzazione per fasi

Tutto tarato sui **42 tiri reali** della sessione 14JUL. Nessuna soglia scelta a
tavolino.

---

## 1. Nuova metrica: `release_jerk` — la pulizia del rilascio

**Cos'è:** massimo `|d(ay)/dt|` nei ~40 ms in cui la freccia è ancora sulla
corda. Misura quanto **bruscamente** la mano di corda spinge lateralmente il
riser (*plucking*). Unità: m/s³.

**Perché 40 ms:** la freccia lascia la corda dopo ~40 ms (9 campioni @224 Hz).
Solo ciò che accade lì può influenzare il volo. Dopo è territorio di `hold_deg`.

### Perché il JERK e non l'ampiezza — questo è il punto

La scelta ovvia sarebbe l'ampiezza (quanto `ay` si scosta dalla baseline).
**Misurato: è un confondente geometrico.**

```
ampiezza <-> alzo:  rho = -0.389  p = 0.011   *** SPORCA
jerk     <-> alzo:  rho = -0.242  p = 0.122       pulita
```

L'ampiezza misura **l'inclinazione del bersaglio**, non il gesto: tirando in
salita o in discesa la gravità si proietta diversamente sugli assi e `ay` cambia
da sola.

**La derivata no, e c'è una ragione fisica esatta:**

```
ay_misurato = ay_moto + g·sin(θ)
d/dt[ay_misurato] = d/dt[ay_moto] + d/dt[g·sin(θ)]
```

Se θ è ~costante nella finestra, il secondo termine sparisce. **E lo è:**
misurato, l'assetto varia di **0.24°** nei 40 ms → g·sin(θ) cambia di 0.041 m/s²
contro un jerk medio di 451 m/s³ × 0.04 s = 18 m/s². **Il termine gravitazionale
è lo 0.2% del segnale.**

Verifica indipendente con correlazione parziale:

```
jerk <-> alzo, controllando l'ampiezza : rho=+0.041  p=0.796  (pulito)
ampiezza <-> alzo, controllando il jerk: rho=-0.291  p=0.061  (sporco)
```

**Il confondente sta nell'AMPIEZZA, non nella RAPIDITÀ.** Il jerk è pulito *per
costruzione*, non per fortuna.

### Validazione

| test | risultato |
|---|---|
| SNR vs rumore | **3.9×** (451 contro p95=115 su finestra a vuoto) |
| ripetibilità | split-half pari/dispari **p=0.218** (coerente) |
| outlier IQR | **nessuno**; distribuzione continua 108–934 |
| indipendenza da `hold` | **rho = -0.044, p=0.783** |
| confondente geometrico | **assente** (p=0.122) |

### `release` e `hold` sono davvero due errori distinti

```
                ABBASSATO  LIEVE  TENUTO
  PULITO             2       6      6
  MEDIO              3       1     10
  STRAPPO            3       3      8

chi² = 5.05  p = 0.282  ->  INDIPENDENTI
```

Il valore pratico è nei fuori-diagonale:
- **8 tiri**: rilascio sporco **ma** tenuta perfetta
- **2 tiri**: rilascio pulito **ma** braccio caduto (fra cui il tiro 34, il peggiore)

Una metrica sola non li avrebbe visti.

### ⚠️ Limite dichiarato: le soglie sono RELATIVE

`330 / 510 m/s³` sono i **terzili di questa sessione** (un arciere, un arco, un
giorno): ripartiscono esattamente 14/14/14. Dicono *"pulito per te oggi"*, non
*"pulito in assoluto"*. Servono più sessioni per sapere se 330 è universale.

La soglia PULITO sta comunque **2.9× sopra il rumore**: la distinzione è reale, è
la **taratura** a essere provvisoria.

**Nota implementativa:** le soglie sono tarate sui valori di *questa*
implementazione (derivata centrata a 2 punti). Con `np.gradient` (3 punti pesati)
la differenza media è 17 m/s³ — **bastava a spostare 5 tiri di classe**. Se si
cambia la formula, vanno ritarate.

---

## 2. FIX — `peak_hz` produceva frequenze NEGATIVE

**Bug reale**, osservato sui 42 tiri: range `[-1.30 .. 21.90]` Hz.

**Causa:** `delta = 0.5*(a-c)/(a-2b+c)`. La guardia `|denom|>1e-9` impedisce la
divisione per zero ma **non** impedisce a `delta` di valere 50 o −200 quando il
denominatore è piccolo-ma-non-zero (spettro piatto: a≈b≈c). Con delta enorme,
`(pk+delta)` finisce fuori dallo spettro.

**Fix in tre punti:**
1. `delta` **vincolato a [-0.5, +0.5]** — è la sua definizione: il vertice della
   parabola per 3 punti equispaziati sta per costruzione entro mezzo bin.
2. `denom` con soglia **relativa** all'ampiezza del picco (non 1e-9 assoluto).
3. risultato **validato contro la banda fisica [8, 100] Hz** → `null` se fuori.
   Meglio "non misurato" che un numero falso.

---

## 3. FIX — `stability` era quasi degenere

**Problema misurato:** CV = **0.08**, tutti i tiri compressi in `[59..90]` su una
scala 0-100, zero agli estremi, **p=0.33** nel distinguere i follow-through rotti.
Non discriminava nulla.

### Fix 3a — finestra ADATTIVA al posto dell'offset fisso

La v1.2b usava `[triggerIdx-75, triggerIdx-25]`, **indici fissi**. Due problemi:
- il commento diceva "300→100 ms @250 Hz", ma l'ODR reale è 224.91 Hz: quegli
  indici sono in realtà **333→111 ms**. La finestra era *dichiarata in ms e
  implementata in campioni*.
- a offset fisso si pesca dove capita, non dove l'arco è fermo.

Ora usa `findCalmWindow()`, la stessa logica del firmware. Misurato: trova
finestre **7% più ferme** (RMS 3.88 vs 4.17 dps).

### Fix 3b — scala ESPONENZIALE, tarata sul RMS reale

**Causa della degenerazione:** il divisore era **20 dps**, ma il RMS angolare
reale in mira è **3.88 dps** (p90 = 5.51). Con div=20 l'argomento sta sempre
attorno a 0.2 → score sempre attorno a 80. **Il divisore era tarato su
un'ipotesi ("5-20 dps"), non su una misura.**

**Perché non basta abbassare il divisore:** con `100*(1-rms/div)` servono due
cose incompatibili — `div >> rms` per score alti, `div ~ rms` per discriminare.
Provato: div=6 → score medio 40, nessun tiro sopra 65. Si ribalta il problema.

**Soluzione:** decadimento esponenziale `100*exp(-rms/τ)`.
- non satura mai (nessuna perdita agli estremi)
- τ ha significato fisico: il RMS a cui lo score vale 37
- **τ = 5 dps** scelto perché **massimizza la dispersione** dello score
  (sd 13.2, misurata) e lo centra attorno a 49

**Risultato: CV da 0.13 a 0.31 → 2.4× più discriminante.**

---

## 4. Dashboard riorganizzata per FASI DEL TIRO

**Il problema:** 11 card per ~6 informazioni reali (PCA: 6 componenti per l'80%
della varianza). Nessuna dominava (PC1 solo 32%): non esiste un "indice di
bravura" unico.

**La struttura nuova segue la cronologia del tiro:**

```
[ ...mira... | SCOCCO |--40ms--|........900ms........]
   FASE 1              FASE 2           FASE 3
   come miri           come rilasci     come tieni
```

| fase | metriche | colore |
|---|---|---|
| **1 · MIRA** | fermezza, cant, ξ consistenza | ciano |
| **2 · RILASCIO** | **pulizia rilascio** (nuova), nettezza scocco, torsione | ambra |
| **3 · TENUTA** | hold, follow-through | verde |
| *secondarie* | vibrazione, energia residua, rinculo, deriva mano | grigio |

**Non è estetica: è la struttura che i dati hanno.** Le tre fasi sono
statisticamente indipendenti:

```
rho(stability, release) = -0.239  p=0.128
rho(stability, hold)    = +0.251  p=0.109
rho(release,   hold)    = -0.044  p=0.783
```

Sono **tre errori diversi**. Un tiro può essere fermo in mira, strappare al
rilascio e tenere benissimo dopo.

**La barretta colorata ora codifica la FASE**, non la singola metrica: prima
erano 7 colori arbitrari, ora 3 con significato. Il raggruppamento si legge anche
senza leggere gli header.

**Le secondarie non sono cancellate** — sono in un blocco `<details>`
collassabile. Restano nel CSV e servono all'analisi offline, ma non competono più
per l'attenzione in campo. Retrocesse perché:
- `follow_energy` ↔ `recoil`: **rho = −0.72**, sono la stessa cosa
- `follow_energy`, `peak_hz`, `recoil` **correlano con l'alzo** (p<0.05): misurano
  la geometria del bersaglio, non il gesto

---

## 5. Protocollo

**`ScorePacket`: 13 → 16 byte**, `SCORE_FORMAT_VERSION` 2 → 3.

```
pkt_type(1) + shot_id(2) + score_flags(1) + distanza_m(1)
+ cant_cdeg(2) + alzo_cdeg(2) + hold_cdeg(2) + hold_class(1)
+ release_jerk(2) + release_class(1) + score_ver(1) = 16
```

16 è **libera** (occupate: 6/8/9/15/24/30). Verificato con `static_assert` e test
host su offset.

`release_jerk` è **uint16, non float**: il massimo misurato è 934 su un fondo
scala di 65535, e il jerk ha sd 197 — la parte decimale è rumore. Un float
sprecherebbe 2 byte per precisione inesistente.

**L'app accetta 10, 13 e 16 byte**: retrocompatibile con qualsiasi firmware
precedente. Le colonne mancanti restano vuote nel CSV.

---

## 6. Validazione

**Test host** (`test_hold.cpp`): la logica reale compilata e girata sui 42 tiri.

```
release: PULITO 14 | MEDIO 14 | STRAPPO 14
hold:    TENUTO 24 | LIEVE 10 | ABBASSATO 8
```

**Test end-to-end**: ScorePacket v3 serializzati in C++ e parsati con la logica
JS dell'app → **4/4**, più retrocompatibilità v1 (10B) e v2 (13B), più rifiuto di
lunghezze non valide.

---

## 6-bis. App v1.27 — compatibilità CSV (due bug trovati rispondendo a una domanda)

### Bug A — il CSV ricostruito non era importabile (e non lo era MAI stato)

`rebuild_csv.py` inseriva `orig_shot_id` in **posizione 2**. L'app importa
leggendo per POSIZIONE:

```
l'app legge   si aspetta      nel ricostruito trovava
c[1]          sample_idx  ->  orig_shot_id     SBAGLIATO
c[2]          ts_us       ->  sample_idx       SBAGLIATO
c[3..8]       ax..gz      ->  ts_us, ax..gy    SBAGLIATO
```

**Gli assi arrivavano tutti sfalsati di una colonna**, senza che nulla segnalasse
l'errore. Il file non era importabile nemmeno nella v1.25: non se n'era accorto
nessuno perché veniva sempre analizzato in Python, mai ricaricato nell'app.

**Fix:** `orig_shot_id` spostata **in coda**. È la stessa regola che il firmware
segue per i campi dei pacchetti BLE: **le colonne nuove vanno sempre in fondo**.
Verificato simulando il parser: mappatura corretta, |a|=9.85, trigger_idx=448.

### Bug B — i tiri importati restavano senza hold e release

`hold` e `release` nascono nel firmware e viaggiano nello ScorePacket. Un CSV
importato non ce l'ha, quindi quei tiri avrebbero avuto le card FASE 3 e "pulizia
rilascio" **vuote pur avendo tutti i campioni grezzi per calcolarle**.

Incoerente: l'app già ricalcola *tutte* le altre metriche all'import ("i colpi
storici beneficiano dei fix"). Non c'era ragione perché queste due facessero
eccezione.

**Fix:** nuova `calcHoldRelease()`, replica in JS della logica del firmware.
Verificata contro il C++ sui 42 tiri:

```
release : 0/42 classi discordanti  (diff media 0.29 m/s³)
hold    : 1/42 classi discordanti  (diff media 0.13°)
```

L'unico discordante è il tiro 42 (−4.02 app vs −3.83 firmware): **a cavallo
esatto della soglia dei 4°**. È il caso già previsto — la soglia TENUTO/LIEVE è
fragile per costruzione (21 tiri su 42 entro ±p95 da essa). Non è un bug, è il
rumore che decide.

### Consolidamento — una sola finestra adattiva

L'app aveva **due copie** della stessa ricerca della finestra calma
(`fwAnglesFromSamples` e `calcStabilityScore`): stessa logica, scritta due volte,
col rischio che una venisse corretta e l'altra no. Ora una sola
(`findCalmWindow`), tre chiamanti.

---

## 7. Cosa manca ancora

Il collo di bottiglia è **sempre lo stesso**, e ora è quantificato:

`follow_energy`, `peak_hz` e `recoil` correlano con l'alzo (p<0.05) → **non sono
confrontabili fra bersagli a altezze diverse**. Il **telemetro con angolo**
scioglierebbe metà delle correlazioni sospette e renderebbe usabile anche l'alzo.

E resta: **CALIBRA A LIVELLA prima di ogni sessione**, ed **esito su TUTTI i
tiri** (finora 11/44).

---

## File modificati

### Firmware v2.15.0
| file | modifica |
|---|---|
| `config.h` | ScorePacket 16B, SCORE_FORMAT_VERSION=3, FW 2.15.0 |
| `shot_angles.h` | v1.4: ReleaseClass, parametri, soglie, documentazione |
| `shot_angles.cpp` | `compute_release_jerk()`, `classify_release()`, `release_class_name()` |
| `scoring.h` | campi release in PendingScore |
| `ble_server.cpp` | release nel pending e nello ScorePacket |
| `scoring_ui.cpp` | banda display RILASCIO \| HOLD affiancati |

### App v1.27
| modifica |
|---|
| FIX `calcFFT`: delta vincolato, denom relativo, validazione banda |
| FIX `calcStabilityScore`: `findCalmWindow()` + scala exp(τ=5) |
| Dashboard riorganizzata per fasi + CSS |
| Parsing ScorePacket v3 (16B) con retrocompatibilità |
| CSV: +`release_jerk`, +`release_class` (32 colonne) |
