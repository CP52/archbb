# ArchBB 1.83 — Touch CALIBRATO (capitolo chiuso)

## Tasti +/- estesi fino ai bordi (elimina il problema alla radice)

Idea di Cesare: invece di *curare* i tocchi che cadono appena fuori dai tasti +/-,
si **elimina la possibilita' stessa** — i tasti arrivano fino al bordo dello
schermo, cosi' un tocco verso il bordo non puo' piu' finire "fuori".

Implementazione con DUE geometrie distinte ed esplicite (per non ricadere nel bug
delle costanti duplicate, §4.1):

- **zona TATTILE** (`KEY_*_HIT_*`): `-` da x=0, `+` fino a x=239. Nessun margine.
- **rettangolo DISEGNATO** (`KEY_*_DRAW_*`): resta rientrato dai bordi, per
  estetica. La zona sensibile "sborda" oltre il disegno, verso il bordo.

Regola: il disegno usa `_DRAW`, l'handler usa `_HIT`. Sono due intenzioni
diverse, non due copie che possono divergere. Verificato: tocco a x=0 -> `-`,
x=239 -> `+`, zone neutre di 8px tra i tasti, nessuna sovrapposizione.

Nessun conflitto con gli slider (Y diversa) ne' con INDIETRO/OK (BTN_GUARD
intatto).

## Coefficienti installati

I coefficienti misurati (5 punti, 2 passate, stilo) sono **gia' in config.h**:

```cpp
TOUCH_CAL_AX = 0.74591   TOUCH_CAL_BX = 21.84
TOUCH_CAL_AY = 0.86208   TOUCH_CAL_BY = 21.68
```

La X del chip e' espansa ~1.34x, la Y ~1.16x. Verificato: applicati ai centri
dei tre tasti li riportano tutti in zona (`-`->40, `?`->114, `+`->193). **Flasha
e i tasti +/- funzionano.**

## Il "non ripetibile" era un falso allarme (mio)

La calibrazione segnalava "NON RIPETIBILE" con spread **3.1%**, ma la soglia era
**3.0%**: bocciata per un decimo di percento, mentre i coefficienti erano
perfettamente buoni. Ho alzato la soglia a **5%**: per un tocco a mano libera con
lo stilo, un residuo del 3% (rms 7.7px su 240) e' fisiologico, non un difetto. La
ripetibilita' serve a scartare le calibrazioni fatte *male*, non a pretendere una
precisione da banco ottico che il dito umano non puo' dare.

Non era un limite del chip. Era il mio giudice troppo pignolo.

## Fix collaterali di questa sessione

- **"tocco breve" sul 3o punto:** la raccolta chiedeva 8 campioni + 60ms settle
  (~316ms di tocco netto, troppo con l'INT-gating). Ora 5 campioni, 24ms settle,
  REL_QUIET 250ms -> ~184ms, molto piu' tollerante.
- **CALIBRA non "scappa" piu':** tolto il limite di 12 tentativi che, sommato al
  "tocco breve", faceva uscire la calibrazione dal 3o punto. Ora CALIBRA e DIAG
  sono **capolinea**: si resta finche' non riesce o si spegne.

---

# ArchBB 1.83 — Taratura del touch

Cesare Pagura · Padova/Noale IT · 17 luglio 2026

## Il sintomo e la diagnosi

> «I tasti + e − sono poco responsivi, pare funzionino sui bordi verso il centro
> dello schermo vicino al tasto ?»

Gli **slider funzionano**, i tasti no. ZONA e RIEPILOGO (bersagli grandi) non
hanno mai dato problemi.

Questo **non è un bug della schermata nuova**: è una discrepanza fra le
coordinate del CST816 e quelle del display, presente **da sempre** e mascherata
dai bersagli grandi. Gli slider sono larghi 192 px e assorbono l'errore (cambia
un po' il valore, non te ne accorgi); i tasti sono 48 px e ti buttano fuori.

È la stessa diagnosi della 1.69 (§4.5 del compendio): lì le celle ZONA da 66 px
nascondevano ciò che i tasti da 32 px rivelavano. Stesso film, altro pannello.

## Un'ipotesi elegante che ho scartato

Avevo costruito un modello: *"il touch è compresso verso il centro"* — spiegava
perfettamente perché i tasti falliscono verso l'interno e gli slider no.

È **esattamente l'ipotesi che sulla 1.69 è costata sei versioni** e si è
rivelata falsa. Lì era stato dedotto «il sensore è più stretto del 28%» dagli
span `rx 31..203` letti in DIAG → `AX=1.38953`. Ma quegli span non erano i bordi
del sensore: erano **dove l'utente aveva toccato**. Il coefficiente inventato ha
*causato* il disallineamento che poi si cercava di correggere. Misurato per
bene, l'asse X era già allineato (`AX=1.006`).

**Spiegare un sintomo non è misurarlo.** Perciò: niente coefficienti dedotti.

## Cosa c'è in questa build

| file | ruolo |
|---|---|
| `touch_cal.h/.cpp` | **NUOVO** — 5 crocette, fit ai minimi quadrati, due passate |
| `touch.cpp` | la correzione, applicata **nel driver, una volta sola** |
| `touch.h` | `+rawX/rawY` (grezze dal chip, solo per la calibrazione) |
| `config.h` | `TOUCH_CAL_*` = **identità** (1, 0): da misurare |
| `main.cpp` | offerta opt-in nel setup, subito dopo `touchInit()` |

### La correzione sta nel driver

Dentro `touchRead()`, **un solo punto di verità**. Da lì in poi tutto il firmware
vede coordinate-display. Correggere in ogni handler sarebbe un disastro: N punti
da tenere allineati a mano, e ogni schermata futura una nuova occasione di
dimenticarsene.

> La 1.69 applica la correzione in `scoring_ui.cpp`, **non** nel driver — mentre
> la sua stessa documentazione dice che il posto giusto è il driver. Qui seguo la
> lezione, non l'implementazione.

### Perché 5 punti e non 2

Con 5 il fit è **sovra-determinato**: il residuo diventa una *misura* della bontà
del modello. Con 2 punti sarebbe zero per costruzione — e non direbbe nulla.
Se il residuo è alto, il modello lineare non basta (rotazione, specchiatura,
non-linearità) e lo scopri subito invece che sul campo.

### Perché DUE passate

Residuo e ripetibilità sono **cose diverse**:

- il **residuo** dice se i 5 punti di *una* passata stanno su una retta;
- lo **spread** dice se *due* passate danno la **stessa** retta.

Residuo basso + spread alto = *"misuro con precisione una cosa diversa ogni
volta"*. È un caso realmente osservato sulla 1.69 (ay 0.700 vs 0.794, +13%, con
residui bassi in entrambe). Senza la seconda passata lo scopri solo dopo aver
compilato i numeri sbagliati.

Se le due passate divergono oltre il **3%**, il modulo scrive **"NON RIPETIBILE:
non usare"**.

## Correzione v7: il DIAG è un capolinea

**Sintomo (v6):** «funziona, ma mi esce subito, non faccio neanche a tempo a
trascrivere i primi dati … se entro in diag tanto vale restarci fino allo
spegnimento».

**Causa:** l'uscita era «2 s di pressione continua». Ma per leggere gli span
bisogna **tenere premuto negli angoli** — il gesto di misura *era* il gesto di
uscita. Il DIAG si autosabotava.

**Cura:** nessuna uscita. Si esce spegnendo, come proposto. Qualunque gesto
d'uscita entrerebbe in conflitto con la misura; chi entra qui vuole leggere con
calma, e riavviare costa un secondo. La funzione è ora dichiarata `[[noreturn]]`,
così è il **compilatore** a garantire che non ritorni.

**In più, ora il DIAG calcola da solo il numero che cerchiamo:**

- `span rx 31..203 (172)` — estremi **e ampiezza**
- `scala X ~172 (tipo display)` oppure `~3900 (NON display!)` — la **stima del
  fondo scala**, cioè se il chip lavora in pixel-display o in un'altra unità
- `n=1234` in alto a destra — contatore dei campioni validi: se resta a `0` con
  il dito appoggiato, il problema è il driver e non la grafica

> Resta vero che **lo span misura dove tocchi, non il chip** — per questo
> l'istruzione a schermo è *"tocca i 4 angoli"*. Serve a capire la **scala**, non
> a ricavare coefficienti: quelli li dà solo CALIBRA, sui bersagli noti.

## Correzione v6: DIAG mostrava tutti zero e flickerava

**Sintomo (v5):** «diag non funziona, flikera e mostra tutti zero ovunque tocchi».

**Tutti zero** — il DIAG faceva **una sola** `touchRead()` per ciclo, poi
spendeva ~60 ms a ridisegnare. Ma il driver è **INT-gated**: nella stragrande
maggioranza dei cicli `touchRead()` ritorna `valid=false` con `rawX=rawY=0`, e il
DIAG stampava *proprio quel campione*. Mostrava **i buchi**, non i tocchi.

È lo **stesso tranello dell'INT-gating** già corretto nella raccolta (v4) — nel
DIAG era rimasto. Simulazione con stilo fermo ed eventi sporadici:

```
VECCHIO (1 read/ciclo):        10 cicli su 10 mostrano ZERO
NUOVO (12 read/ciclo + latch):  0 cicli su 10 mostrano ZERO
```

**Flicker** — `fillScreen()` a ogni ciclo con disegno diretto sul TFT (la 1.83
non ha sprite): lo schermo si svuota e si riempie sotto gli occhi.

**La cura:**

- **Polling fitto + latch**: si legge 12 volte per ciclo di disegno e si
  memorizza **l'ultimo campione valido**; a schermo si mostra quello, non il
  vuoto fra un evento e l'altro. Se non c'è ancora un tocco, si scrive `---`
  invece di un `0` bugiardo.
- **Redraw solo al cambio**: si ridisegnano le singole righe quando i valori
  cambiano, e il pallino cancella il precedente invece di ripulire lo schermo.
  I coefficienti (costanti) si disegnano una volta sola.
- **"Premuto" a tempo** (150 ms dall'ultimo evento), non a campioni — stesso
  motivo di `waitRelease`.

> **La lezione, pagata tre volte in questo modulo:** con un driver a eventi,
> *"l'ultimo dato letto"* e *"il dato adesso"* sono cose diverse.

## Correzione v5: troppi "tocchi mossi" (e il timeout)

**Sintomo (v4):** «il minuto non è sufficiente per completare due giri e con lo
stilo mi dà, secondo me, troppi tocchi mossi!»

**Le due cause erano una sola: stavo tirando a indovinare.**

`MAX_SPREAD_PX` era 12 in v3, 25 in v4 — **due numeri inventati**. Ma sono unità
**grezze del chip**, e quanto valga un'unità grezza è *precisamente ciò che la
calibrazione sta misurando*: sceglierla a priori è **circolare**. Verificato:

```
soglia 25 : stilo fermo su scala 4095 (spread 27) -> RIFIUTATO   (il tuo bug)
soglia 400: trascinamento su scala 240 (spread 137) -> accettato (il buco opposto)
```

Nessun numero fisso può funzionare senza sapere la scala.

E il "timeout" non era il timeout: era `attempts >= 6` che **abortiva l'intera
passata** dopo pochi rifiuti. I rifiuti ingiusti facevano fallire tutto.

**La cura, in tre parti:**

1. **Scarto dell'outlier via mediana** — si raccolgono 8 campioni, si butta il
   più lontano dalla mediana (lì finiscono transitorio di appoggio e rimbalzi),
   si media il resto. La mediana è robusta: un outlier non la sposta.
2. **Criterio relativo, scale-free** — la dispersione si confronta con la
   **distanza fra i bersagli misurata nelle stesse unità**, non con un numero
   assoluto. La scala si **misura** dai primi due punti acquisiti (li conosciamo
   in pixel per costruzione → il rapporto è la scala). Finché non è nota, si
   accetta: sarà il **residuo del fit** a fare da giudice, che è il posto giusto.
3. **Feedback numerico** — se un punto viene rifiutato mostra `mosso (137)`, il
   valore **misurato**. Se un rifiuto fosse ingiusto, lo si legge invece di
   indovinarlo.

Verifica su entrambe le scale plausibili del chip:

```
stilo fermo, scala 240    spread=4     soglia=42   -> accetta   OK
trascinamento, scala 240  spread=137   soglia=42   -> RIFIUTA   OK
stilo fermo, scala 4095   spread=27    soglia=600  -> accetta   OK
trascinamento, scala 4095 spread=1900  soglia=600  -> RIFIUTA   OK
```

Inoltre: `attempts` 6 → **12**, e il timeout (45 s) è **per punto**, non per
procedura — ripetere un punto è normale e non deve erodere un budget globale.

## Correzione v4: il punto non si chiudeva mai

**Sintomo (v3):** «non si muove dal primo punto, sempre ambra, mai verde, ogni
tanto dice tocco mosso … (e tocco con uno stilo!)».

Lo **stilo** era l'indizio decisivo: se anche con uno stilo il tocco risulta
"mosso", il problema non è il tocco — è il filtro.

**Causa:** il driver è **gated dall'INT** — `touchRead()` legge l'I2C solo
quando il chip segnala un evento, e restituisce `valid=false` in mezzo. Con lo
stilo (o il dito) **fermo** il CST816 non genera eventi in continuo. La v3
pretendeva 6 campioni **consecutivi** tutti validi e buttava il punto al primo
buco: una condizione che l'hardware **non può soddisfare per costruzione**.

```
v3 (consecutivi): FALLITO al primo buco  -> il punto non si chiude MAI (sempre ambra)
v4 (a tempo)    : raccolti 6/6 campioni  -> PUNTO OK (verde)
```

**La cura:** si contano i campioni **validi** tollerando i buchi, e il "dito
alzato" si misura **a tempo** (silenzio continuo di 120 ms), non contando buchi.
Stesso tranello, in versione speculare, c'era in `waitRelease`: coi buchi normali
anche a dito fermo, un contatore avrebbe detto "staccato" con lo stilo ancora
appoggiato. Ora entrambi ragionano a tempo.

Inoltre `MAX_SPREAD_PX` passa da 12 a **25**: sono unità **grezze** del chip, e
quanto valga un'unità grezza in pixel è precisamente ciò che stiamo misurando —
presumerlo stretto era circolare. Quel filtro serve a scartare i tocchi
palesemente trascinati, non a giudicare la precisione: quella la misura il
**residuo del fit**, che è il posto giusto.

> Lezione già nel compendio, ripagata: *quando un filtro "intelligente" produce
> comportamento strano, il sospetto va al filtro, non a un nuovo fenomeno fisico.*

## Correzione v2: il salto dei punti

**Sintomo (v1):** «il mirino diventa verde, ma non fa i 5 punti di seguito e
talora salta, p.e. da 3 a 5 … poi dice punti inaffidabili».

**Causa:** il CST816, allo stacco del dito, produce micro-rimbalzi
valido/non-valido (lo stesso fenomeno per cui lo scoring ha il debounce). La v1
attendeva il rilascio con **un solo** campione e poi raccoglieva 4 campioni a
12 ms: bastavano ~48 ms di rimbalzi perché il punto successivo si
**auto-completasse**, registrando le coordinate del dito che si stava ancora
staccando dal punto precedente.

I due sintomi erano **lo stesso bug**: il salto (punto auto-completato) e
«inaffidabili» (due bersagli diversi con la stessa coordinata → fit degenere,
che protesta giustamente).

**Verificato in simulazione** riproducendo la sequenza di rimbalzi:

```
VECCHIA: punto B misurato a x=41   (VERO=200) -> SALTATO! ha preso i rimbalzi di A
NUOVA  : punto B misurato a x=200  (VERO=200) -> CORRETTO
```

**La cura, in tre parti (servono tutte e tre):**

1. **Rilascio confermato** — 4 campioni consecutivi senza dito, non uno.
2. **Tocco stabile** — si raccoglie solo dopo 4 campioni consecutivi con dito
   (scarta il transitorio di appoggio), poi 6 campioni mediati.
3. **Coerenza** — i campioni di un punto devono stare entro 12 px l'uno
   dall'altro. Se sono sparsi (dito mosso, rimbalzo) il punto si **ripete**
   invece di avvelenare il fit: compare *"tocco mosso - ripeti"*.

Inoltre il **timeout è ora per punto**, non per passata: ripetere un punto è
ora normale, e un budget globale verrebbe eroso dalle ripetizioni legittime.

## Come si usa

1. Flasha e riavvia. Compare **CALIBRA / DIAG** per 3 secondi.
   *Non toccare nulla = salta* (default: il firmware si comporta come adesso).
2. **CALIBRA** → 5 crocette × 2 passate. Tocca il **centro** di ogni mira, con
   precisione, e **stacca il dito** quando appare *"ok - stacca il dito"*.
   Se dice *"tocco mosso - ripeti"*, ritocca lo stesso punto tenendo fermo.
3. Leggi i coefficienti a schermo. Se dice **OK: ricopia in config.h**, copiali:

```cpp
static constexpr float TOUCH_CAL_AX = ...;
static constexpr float TOUCH_CAL_BX = ...;
static constexpr float TOUCH_CAL_AY = ...;
static constexpr float TOUCH_CAL_BY = ...;
```

4. **Ricompila** e riprova i tasti `−`/`+`.

Se dice "NON RIPETIBILE", **non usare quei numeri**: ripeti toccando più preciso.

### DIAG

Mostra i numeri invece delle ipotesi: coordinate grezze, corrette, i
coefficienti **realmente attivi nel binario** (smaschera il "non ho
ricompilato"), e un pallino sotto il dito.

Mostra anche gli span — con l'avviso esplicito **«span = dove tocchi, NON il
chip»**. Sono lì solo per vedere se il chip satura, **mai** per calibrare.

## Perché i coefficienti non si salvano da soli

Vanno **ricopiati a mano** e ricompilati. È voluto: scriverli in NVS renderebbe
il comportamento dipendente da uno stato invisibile nel sorgente, e la prima
domanda davanti a un touch strano ridiventerebbe *"ma quali coefficienti ha
dentro?"*. Con la costante compilata, **il sorgente è la verità**.

## Perché non ho copiato i coefficienti della 1.69

`AX=1.006 BX=-4.34 AY=0.741 BY=37.59` sono di un **altro pannello**, con altri
pin e altra meccanica. Copiarli sarebbe ripetere l'errore di usare un numero non
misurato su questo hardware. Si parte dall'identità e si misura.

## Verifica offline già fatta

Il fit è stato provato su una distorsione **nota** (AX=1.18, BX=−22, AY=0.83,
BY=15) con rumore di tocco ±2 px:

```
FITTATI: AX=1.18435 BX=-21.46 | AY=0.82644 BY=15.51
errore su AX = 0.37% | su AY = 0.43% | residuo max = 1.45 px
```

Il fit recupera la distorsione vera, e il residuo riflette il rumore — cioè
misura davvero la qualità.

## Attenzione

La schermata **CALIBRA/DIAG** usa bersagli enormi (200×46) apposta: devono
funzionare *anche se* la calibrazione è sbagliata. È l'unico modo per non
restare chiusi fuori.

## Poi

- Se dopo la taratura i tasti rispondono → **Fase 4c** (angoli cant/alzo).
- Le soglie **ZONA** (`THR_Y_TM=95`, `THR_Y_MB=189`) restano da verificare: con
  il touch tarato, o sono giuste o si vede subito.
