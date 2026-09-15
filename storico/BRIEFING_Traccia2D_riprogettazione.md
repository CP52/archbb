# ArchBB — Riprogettare la visualizzazione del tiro «in soggettiva»

**30 luglio 2026 · briefing per ripartire da capo sulla traccia 2D**

Questo documento serve a riprendere il lavoro in una chat nuova senza
ricostruire nulla. La prima parte dice cosa è solido, la seconda cosa è rotto e
perché, la terza cosa costruire.

---

## 0. Versioni al momento del congelamento

| | versione | stato |
|---|---|---|
| firmware | `1.83-F16b-font` | buono, in uso sul campo |
| app companion | `v0.4` | buona, tranne la traccia |
| analizzatore | `2026.07.30-16` | buono, tranne la traccia |

---

## 1. Quello che è SOLIDO, e non va rimesso in discussione

### Gli assi del sensore — accertati smontando la scheda

Ispezione fisica del QMI8658C sulla WS 1.83, montaggio attuale (scheda ruotata
di 90° a destra):

```
X = giù (gravità)      Y = asse freccia, verso la punta      Z = destra
verifica mano destra:  X × Y = giù × avanti = destra = Z    ✓
```

Datasheet: sistema destrorso, velocità angolare positiva in senso antiorario
attorno all'asse.

Ne segue, per costruzione e non per inferenza:

| rotazione | asse | asse grezzo | canonico (mount=2 + flip) |
|---|---|---|---|
| **alzo** | laterale | `rgz` | `cgy`, **segno invertito** |
| **cant** | freccia | `rgy` | `cgz`, segno invertito |
| **yaw** | verticale | `rgx` | `cgx` |

> Prima di questa ispezione la questione è stata affrontata quattro volte con
> inferenze statistiche, un DIAG dinamico e ragionamenti geometrici: tutti
> inconcludenti o sbagliati. L'unica cosa che ha funzionato è stato guardare il
> chip. **Non riaprire la questione senza una nuova osservazione fisica.**

### Il segno di hold — corretto e verificato

`cgy = −(rate di alzo)`, quindi l'integrale di `gy` era l'alzo rovesciato.
Misurato su 55 tiri veri: `hold` usciva **positivo** quando il braccio calava
(media +1,78°, 34 positivi su 55), cioè il dispositivo dichiarava un braccio
d'arco che sale. L'arciere conferma che cala.

Corretto in `integrate_gy()` (firmware F16), in `archbb_metrics.ricalcola` e in
`calcHold` dell'app. Le sedute anteriori si riconoscono dall'assenza di
`hold_sign=corretto` in `session.txt` e vengono invertite in lettura.

### L'alzo dall'accelerometro — validato sul campo

Seduta del 28/07, 35 tiri con punteggio, distanze 14–48 m ed elevazioni
−10°/+20°:

```
alzo misurato vs elevazione dichiarata:  r = +0,962   p < 0,0001
retta:  alzo = 1,138 × elev − 0,85
```

La distanza non aggiunge nulla (R² da 0,9263 a 0,9268). **L'alzo statico è
affidabile.** Il problema è tutto nell'integrazione del giroscopio.

### Il rilascio comincia prima del trigger

Misurato su 71 tiri: il primo istante in cui il giroscopio supera cinque volte
il suo valore di mira cade in mediana a **−55 ms** dal trigger (quartili −78 e
−41). La soglia su `||a|−g|` scatta a rotazione già iniziata.

---

## 2. Quello che è ROTTO, e perché

### Il sintomo che ha fatto scattare tutto

> «tiro 17, angolo di mira −11,16° allo scocco 4,08°, come è possibile? con
> queste variazioni non dovrei mai centrare il bersaglio»

Osservazione corretta e decisiva. **4° a 20 m sono 1,4 metri fuori bersaglio.**
Un numero che contraddice il fatto che le frecce arrivino a segno è un numero
sbagliato, e nessuna eleganza di calcolo lo salva.

### Causa 1 — la finestra calma cade troppo lontano

`finestra_calma()` cerca i 200 ms **più fermi in tutta la finestra pre-scocco**
(2 secondi). Il risultato finisce spesso a un secondo e mezzo dallo scocco, e
l'integrazione da lì al trigger accumula deriva.

```
 tiro   finestra calma            distanza dal trigger   mira→scocco
  t 1   da -1304 a -1102 ms            1102 ms              4,22°
  t 3   da -1938 a -1736 ms            1736 ms              8,41°
  t 7   da  -308 a  -106 ms             106 ms              0,30°   ←
  t 9   da -1998 a -1796 ms            1796 ms              9,08°
  t10   da -1759 a -1557 ms            1557 ms             10,35°

correlazione (distanza temporale, spostamento):  r = +0,294  p = 0,036  n = 51
pendenza:  +7 gradi al secondo di distanza
```

**Il tiro 7 è la prova**: unica finestra calma vicina (106 ms), unico
spostamento fisicamente credibile (0,30°). Gli altri misurano soprattutto
deriva, a circa **7 °/s**.

### Causa 2 — resta qualcosa oltre la deriva

Rifacendo il conto con un riferimento **fisso** da −280 a −80 ms, lo
spostamento scende ma non collassa:

```
con riferimento fisso:   media 5,20°   mediana 5,20°   max 13,09°
```

Quindi c'è ancora ~5° di rotazione fra −80 ms e lo scocco. Può essere:

- **rilascio vero** — la rotazione dell'arco durante lo scocco, che però
  avviene mentre la freccia è ancora sulla corda e dovrebbe deviarla;
- **deriva residua** su 80 ms — a 7 °/s farebbe 0,6°, quindi non basta;
- **saturazione o non-linearità** del giroscopio durante l'impulso (picchi
  misurati fino a 291 dps su fondo scala 512: non satura, ma va verificato il
  comportamento in banda);
- **accoppiamento g-sensitivity** del giroscopio: il datasheet dà ±0,1 dps/g, e
  con picchi di 40 m/s² (4 g) sono 0,4 dps — trascurabile.

**Questa è la domanda aperta numero uno.** Va risolta prima di disegnare
qualunque cosa, altrimenti si disegna bene un numero sbagliato.

### Causa 3 — la fase «mira» conteneva il rilascio (già corretta)

Il confine ereditato dalla 1.29 faceva finire la mira allo scocco. Corretto a
−60 ms. Sul tiro 4 della seduta 1005:

```
da -289 a -60 ms:   laterale 0,22°   alzo 0,44°     ← la mira
da  -60 a   0 ms:   laterale 5,73°   alzo 0,65°     ← il rilascio
```

### Nota sull'orientamento attuale

La traccia è oggi ruotata di 90° orari (`TRACCIA_SCAMBIA_ASSI = True` in
`archbb_dsp.py`, `TRACCIA_RUOTA_90 = true` nell'app). È una **scelta di
presentazione richiesta dall'arciere**, non una correzione di misura: gli assi
verificati dicono che l'alzo sta su `cgy`. Nella riprogettazione la questione va
riaperta da zero, perché con le coordinate assolute il problema si pone in modo
diverso.

---

## 3. Cosa costruire — specifica dell'arciere

### 3.1 Lo scoring, in evidenza

Subito sotto la barra di navigazione, ben visibili:

- **colpito / mancato**
- **dove** (zona)
- **distanza**
- **elevazione**

Oggi sono sparsi e piccoli. Sono il contesto senza cui il resto non si legge.

### 3.2 Il 2D in coordinate ASSOLUTE

Non più scostamenti da un riferimento, ma la posizione vera:

- **0° = arco orizzontale**
- l'asse verticale è l'**alzo assoluto**
- il puntino giallo dello scocco cade **dove cade**

### 3.3 Il riferimento: finestra calma IMMEDIATAMENTE precedente

Non «la più ferma in due secondi», ma quella **subito prima** del rilascio.
È la richiesta esplicita, ed è confermata dai numeri della §2: la distanza
temporale del riferimento è la sorgente principale dell'errore.

Proposta operativa: finestra fissa che termina a −80 ms (prima dell'inizio del
rilascio, mediana −55 ms) e dura 150–200 ms. Da validare.

### 3.4 Il bersaglio come cerchietto verde

Quando distanza ed elevazione sono note:

- **posizione**: all'elevazione dichiarata del bersaglio
- **diametro**: l'angolo sotteso da un bersaglio standard da **30 cm**

```
distanza    diametro angolare
     6 m         2,86°
    10 m         1,72°
    15 m         1,15°
    18 m         0,95°
    20 m         0,86°
    25 m         0,69°
    30 m         0,57°
    40 m         0,43°
    50 m         0,34°
```

**Questo è il controllo di realtà più forte che possiamo costruire.** Se la
traccia dice che l'arco si sposta di 5° e il bersaglio ne copre 0,9°, il
disegno stesso dichiara che i numeri non tornano — e finché non tornano, si sa
di non poterli usare.

### 3.5 Vista «in soggettiva»

L'idea guida: mostrare quello che vede l'arciere, non un diagramma astratto.
Da definire insieme.

---

## 4. Ordine di lavoro suggerito

1. **Risolvere la §2 causa 2.** Capire da dove vengono i 5° residui fra −80 ms
   e lo scocco. Senza questo, ogni grafico è un bel disegno di un numero
   sbagliato. Strada: guardare i segnali grezzi attorno al rilascio su qualche
   tiro, e confrontare la rotazione integrata con quella che l'accelerometro
   vede *dopo* che tutto si è calmato (a +500 ms l'assetto è di nuovo statico e
   misurabile — è un riferimento indipendente e finora mai usato).
2. **Scoring in evidenza.** Indipendente da tutto il resto, si può fare subito.
3. **Cerchietto del bersaglio.** Anche questo indipendente, e dà il metro di
   giudizio per il punto 1.
4. **Coordinate assolute e riferimento vicino.**
5. **Solo alla fine**, riaprire l'orientamento degli assi nel nuovo contesto.

---

## 5. Materiale disponibile

**Sedute**

| cartella | tiri | note |
|---|---|---|
| `SESS_20260714_1700` | 44 | 1.69 convertita, distanze 14–40 m |
| `SESS_20260725_1648` | 9 | pre-F11, colonne da mappare |
| `SESS_20260727_1230b` | 20 | F11, mount e taratura corretti, 6 m |
| `SESS_20260728_0937` | 15 | quasi tutta maneggio, 4 tiri veri |
| `SESS_20260728_1005` | 51 | **la migliore**: distanze 14–48 m, elevazioni −10/+20 |

Sulla 1005 il `session.txt` dichiara 39 tiri e ne contiene 51 (chiusa e poi
ripresa prima della correzione F13b).

**Strumenti di controllo già pronti**

- `tools/sintassi/controlla.sh` — 10 sorgenti C++ in `gnu++11`, più il
  controllo sui font numerici di TFT_eSPI
- `tools/prova_app.js` — carica l'app in DOM headless con una seduta vera e
  verifica che le schermate si popolino

**Verifica incrociata fra strumenti**: analizzatore Python e app HTML calcolano
le stesse metriche e devono dare gli stessi numeri. Ultimo controllo, tiro 4
della seduta 1005:

```
                       app HTML     analizzatore
fase mira, orizzontale    0,44          0,44
fase mira, verticale      0,22          0,22
hold                     −7,16         −7,16
```

---

## 6. Regole di metodo, confermate sul campo

**«I dati comandano.»** Quando i dati non bastano a decidere, dirlo invece di
scegliere e tacere.

**Un numero che contraddice la realtà è sbagliato**, per quanto elegante sia il
calcolo che lo produce. I 4° del tiro 17 hanno smascherato un difetto che tre
verifiche statistiche non avevano visto.

**Le ispezioni fisiche battono le inferenze.** Sugli assi, quattro tentativi
statistici hanno prodotto quattro risposte diverse; smontare la scheda ha
prodotto la risposta.

**Le sostituzioni automatiche vanno verificate sull'unicità dell'ancora**, non
sulla presenza — e quando si sostituisce un intervallo di righe, guardare cosa
c'è *dentro*, non solo agli estremi.

**Uno strumento di controllo che non si accende è peggio di non averlo**: dà
sicurezza falsa. Va sempre provato reintroducendo il difetto che deve trovare.
