# ArchBB 1.83 — FASE 19 · coordinate d'impatto in centimetri

**1 agosto 2026** · `ArchBB_183_Fase19` · fw `1.83-F19-impatto`
Config NVS **v6** · CSV **v2**

---

## 0. Perché

La verifica della tesi del briefing 31/07 §4 (`yaw_causale` come predittore della
dispersione laterale) è stata eseguita sui 35 tiri valutati della seduta
`SESS_20260728_1005` ed è uscita **nulla**: ρ = +0,018 dopo aver rimosso la
componente sistematica assorbita dalla taratura del mirino.

Il motivo, misurato e non supposto. Simulazione di potenza (α = 0,05, effetto
`f` = quota di varianza laterale spiegata dallo yaw):

| n | f = .05 | f = .09 | f = .15 | 3 bin, f = .09 |
|---|---|---|---|---|
| 35 | 0,22 | **0,38** | 0,59 | **0,34** |
| 100 | 0,56 | 0,82 | 0,97 | 0,76 |
| 150 | 0,74 | 0,94 | 1,00 | 0,91 |

**La griglia 3×3 costava 4 punti di potenza.** A uccidere il test è stato n = 35.
Con `f ≈ 0,09` (yaw ~6 cm effettivi su ~20 cm di dispersione totale) servono
**~100 tiri**, indipendentemente dallo strumento.

Quello che la griglia distruggeva davvero non è la rilevazione, è la **misura**:
tre livelli ordinali non hanno unità. Si può arrivare a «sì, correla»; non si
arriverà mai a «k = 0,45 cm di impatto per cm di previsione», che è la costante
di taratura senza la quale una metrica biomeccanica non diventa un consiglio.

Il firmware aveva già le coordinate continue del tocco e le buttava via:
`handleZonaTap(x, y)` riceveva un punto su 240×284 e restituiva un numero da 0 a 8.

---

## 1. Cosa cambia

### Ordine del flusso

```
prima    ESITO -> ZONA -> DISTANZA -> ELEVAZIONE
adesso   DISTANZA -> ELEVAZIONE -> ESITO -> IMPATTO
```

Distanza ed elevazione sono **contesto** noto alla piazzola; l'impatto è
**osservazione**, compiuta dopo. Conseguenza pratica: se lo scoring si
interrompe a metà — i 16 tiri su 51 con `arcs=0` del 28/07 — quello che resta
salvato è comunque utile. `distanza_m` ed `elev_deg` si fissano nel risultato
al momento del rispettivo OK, non alla fine del percorso.

Il conto dei tocchi non cambia: **4**. `SCARTA` si sposta sulla prima schermata
(tasto sinistro dello slider distanza), quindi neutralizzare un falso trigger
costa un tocco come prima.

### Schermata impatto

Cerchio centrato in (120, 138), raggio 100 px, con anelli graduati ogni
`raggio/5` cm, croce di mira e marcatore.

| esito | raggio | risoluzione | dito ±4 px |
|---|---|---|---|
| colpito | 50 cm | 0,50 cm/px | ±2,0 cm |
| mancato | 150 cm | 1,50 cm/px | ±6,0 cm |

**L'origine è il punto di mira, non il centro della sagoma.** È ciò che rende il
dato indipendente dalla forma dell'animale 3D e lo mette nello stesso sistema di
riferimento in cui la fisica fa le previsioni.

Sul mancato viene disegnato in rosso il confine a `imp_raggio_colpito_cm` come
promemoria — **non** come zona proibita: toccare dentro registra comunque.

**Conferma automatica.** Il tocco piazza il marcatore e avvia una barra di
**2,5 s**; un altro tocco lo sposta e la riavvia; allo scadere si conferma da
sola. Nessun tasto OK, nessuna zona morta, correggibile fino all'ultimo istante.
(2000 ms provati al banco erano corti: il conto deve reggere il tempo di
*accorgersi* che il puntino e' storto, e accorgersene vuol dire leggere il
numero in centimetri, non guardare il marcatore.)

**Rientro da `CORREGGI`.** Il `CORREGGI` del riepilogo rientra **direttamente**
nella schermata impatto, col marcatore precedente gia' piazzato. Col nuovo
ordine l'impatto e' l'ultima schermata ed e' anche l'unica cosa che si vorra'
davvero correggere: rifare quattro schermate per spostare un puntino di dieci
centimetri era un effetto collaterale del riordino, non una scelta. Distanza,
elevazione ed esito restano raggiungibili risalendo con l'`INDIETRO`.

Due dettagli che rendono questo rientro corretto invece che solo comodo:

- il **countdown non parte** al rientro. Se partisse, la schermata si
  richiuderebbe da sola prima che il dito arrivi sul vetro, confermando proprio
  il valore che si era venuti a cambiare. Servono percio' due booleani distinti,
  `s_impSet` (marcatore presente) e `s_impConferma` (conto in corso);
- se si risale fino a ESITO e si **cambia colpito/mancato**, il raggio cambia e
  `impReset()` scatta: un marcatore ereditato dall'altra scala sarebbe un dato
  falso, non un residuo grafico.

Le coordinate salvate sono in centimetri, che sono assoluti; i pixel no. Al
rientro il punto si ricolloca sulla scala corrente e, se cade fuori dal cerchio
(raggio cambiato in config), viene riportato sul bordo **con i centimetri
ricalcolati dal pixel saturato**: un marcatore sul bordo che dichiarasse ancora
il valore originale sarebbe una bugia silenziosa.

**`NON SO`** (tasto destro): l'uscita esplicita per le quindici frecce infisse
nella sagoma. Salva il tiro con distanza, elevazione, esito e tutte le metriche
IMU, e lascia l'impatto alla sentinella. Obbligare a indicare un punto in quel
caso non produce un dato impreciso, produce un dato **inventato**.

---

## 2. Modello dati

### `ScoreResult` — tre campi in coda

```cpp
int16_t  imp_x_cm;       // + = destra rispetto al punto di mira
int16_t  imp_y_cm;       // + = alto   rispetto al punto di mira
uint16_t imp_raggio_cm;  // raggio usato: senza, i cm non hanno scala
```

I segni coincidono con le convenzioni misurate al banco il 31/07 (`yaw` positivo
= destra, `alzo` positivo = punta alta): la regressione impatto/metrica si scrive
senza cambi di segno da ricordare.

`IMP_NOT_SET = INT16_MIN` è la sentinella. Lo **zero** non poteva farla: è il
valore più desiderabile e legittimo (centro esatto). Stessa logica di
`ELEV_NOT_SET`.

### `zona` non si memorizza più: si deriva

```cpp
uint8_t impattoToZona(int16_t x_cm, int16_t y_cm, uint16_t raggio_cm);
// soglie a ±raggio/3, cioè la stessa semantica della griglia 1.69
```

Fra `zona` e coordinate, le coordinate contengono la zona e non viceversa:
memorizzare entrambe significherebbe due campi che possono divergere. La regola
§4.1 applicata ai dati invece che alle costanti.

Conseguenza: `encodeArcs` è invariato e le sedute vecchie restano confrontabili
sulla colonna `zona`.

### `ZONA_IGNOTA` nella codifica ARCS

`ARCS = ±(dist·10 + zona+1)`. Con zona ignota la cifra delle unità va a **0**,
cioè `±(dist·10)`. Nessun bit nuovo: quella cifra aveva un valore libero e adesso
significa qualcosa. Resta distinguibile da «non valutato» (0 secco, dove anche
la distanza è 0) e nessun file vecchio può contenere unità 0 con distanza ≠ 0.

### Config v6

```cpp
uint16_t imp_raggio_colpito_cm;   // default 50
uint16_t imp_raggio_mancato_cm;   // default 150
```

In coda prima del checksum, `CONFIG_VERSION` 5 → 6 (i config v5 esistenti
ripartono dai default, come da disciplina). Visibili nella schermata CONFIG,
editabili via BLE come tutto il resto.

Sono parametri e non costanti compilate perché il valore giusto è una stima a
occhio che dipende dai gruppi di sagome e cambierà. Stando in config finiscono in
`session.txt`, e **una seduta vecchia resta interpretabile anche dopo che il
parametro è cambiato**.

### CSV v2 e `session.txt`

```
...,burst_file,imp_x_cm,imp_y_cm,imp_raggio_cm
```

Campi vuoti quando l'impatto non è rilevato, come per elev e per le metriche.

```
imp_raggio_colpito_cm=50
imp_raggio_mancato_cm=150
```

L'header CSV era scritto **a mano in due punti** (creazione sessione e ripristino
di cartella senza CSV): erano identici per fortuna, non per costruzione.
Adesso è la costante `CSV_HEADER`, in un posto solo.

---

## 3. Avvertenza sull'elevazione

Con bersagli a ±20° il piano del bersaglio è inclinato: `imp_y_cm` è misurato
**sulla faccia**, non in verticale vera. Per la verticale vera serve
`× cos(elev_deg)`. `elev_deg` è registrato accanto, quindi il dato è
recuperabile — ma va saputo adesso, non scoperto fra sei mesi guardando una
regressione che non torna. La nota è scritta accanto alla definizione dei campi
in `scoring.h`.

---

## 4. Verifiche fatte

**Banco di sintassi** — `sh tools/sintassi/controlla.sh`: tutto ok.
Aggiunto `config_ui.cpp` alla copertura del banco: è stato toccato, e un file
«che non si tocca da mesi» smette di esserlo nel momento in cui lo si tocca.

**Difetto trovato in `config_ui.cpp` grazie a quell'aggiunta**: la lista dei
valori (11 righe, `y=60`, `dy=15`) arrivava a y=210 e passava **sotto** i
pulsanti a y=196. Si sovrapponevano già prima di questa fase. Corretto:
`y=56`, `dy=13`, pulsanti a y=230.

**Geometria, verificata numericamente**

```
cerchio y  38..238   barra nav a 244    -> non si toccano
cerchio x  20..220   schermo 240        -> dentro
impattoToZona: tabella di casi coerente con la griglia 3x3
```

**Provato al banco** (flusso e schermata impatto): fluido. **Non ancora sul campo.**
La conferma automatica è stata provata al banco e portata a 2,5 s; resta da
sentirla in mano con guanto. `IMP_CONFERMA_MS` è in cima a `scoring.cpp`.

---

## 5. Cosa manca a valle

1. **Analizzatore** — leggere `imp_x_cm`/`imp_y_cm`/`imp_raggio_cm` e rifare la
   regressione. Attenzione: la pendenza attesa **non è 1**, è il guadagno di
   trasferimento `k ≈ 0,4–0,5` (la freccia non eredita tutto l'angolo del riser:
   acquista velocità mentre l'arco ruota, quindi conta la media dell'angolo
   pesata sugli incrementi di quantità di moto, ≈ Δθ/2).
2. **App companion v0.5** — inserimento batch degli impatti da telefono, per la
   campagna di taratura. Stessi tre campi, un solo formato.
3. **Campagna `k`** — 6 frecce marcate, punto di mira unico, distanze alternate,
   lettura al bersaglio col metro. ~85 tiri, due sedute. Una volta sola.

---

## 6. Regola di metodo aggiunta

**Un banco che non copre un file non vede i difetti di quel file — e la
copertura va estesa quando il file si tocca, non quando si rompe.** Il difetto
di sovrapposizione in `config_ui.cpp` era lì da versioni; è emerso nel minuto in
cui il file è entrato nel banco. Il cono d'ombra di uno strumento di controllo
si sposta insieme al lavoro.
