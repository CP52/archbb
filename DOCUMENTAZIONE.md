# ArchBB 1.83 — Documentazione

**Cesare Pagura · Padova/Noale IT**
Firmware `1.83-F22b-pretiro` · CSV v5 · Config v7 · App companion v0.8

Questo documento sostituisce i ventitré file `.md` che vivevano in radice. Gli
originali non sono stati cancellati: stanno in `storico/`, e restano la fonte
per i dettagli cronologici di ogni singola fase. Qui c'è ciò che serve **oggi**
per lavorare sul progetto, riorganizzato per argomento invece che per data.

Il principio che governa tutto è uno: **i dati comandano**. Nessun parametro si
cambia senza una prova sul campo, nessuna convenzione di segno si fissa senza
un test statico ad angolo noto.

---

## Indice

1. [Cos'è ArchBB](#1-cosè-archbb)
2. [L'hardware, e i fatti che non sono deducibili](#2-lhardware-e-i-fatti-che-non-sono-deducibili)
3. [Architettura del firmware](#3-architettura-del-firmware)
4. [Convenzioni di segno e montaggio](#4-convenzioni-di-segno-e-montaggio)
5. [Il trigger](#5-il-trigger)
6. [Il flusso di scoring](#6-il-flusso-di-scoring)
7. [Le metriche, e cosa misurano davvero](#7-le-metriche-e-cosa-misurano-davvero)
8. [Contratti dati: NVS, CSV, burst, BLE](#8-contratti-dati-nvs-csv-burst-ble)
9. [Storia delle fasi](#9-storia-delle-fasi)
10. [Gli errori commessi, e le regole che ne sono nate](#10-gli-errori-commessi-e-le-regole-che-ne-sono-nate)
11. [Strumenti di diagnostica](#11-strumenti-di-diagnostica)
12. [Come si compila e si carica](#12-come-si-compila-e-si-carica)
13. [Cosa resta aperto](#13-cosa-resta-aperto)

---

## 1. Cos'è ArchBB

Una scatola nera per l'arco nudo. Una scheda montata sul riser registra con un
IMU cosa fa l'arco negli istanti attorno al rilascio, l'arciere dichiara a
schermo dove è andata la freccia, e il tutto finisce su microSD in un formato
che si può analizzare dopo.

Tre pezzi:

| Pezzo | Cosa fa | Dove sta |
|---|---|---|
| Firmware | acquisisce, rileva lo scocco, raccoglie lo scoring, salva | `src/` |
| App companion | scarica le sessioni via BLE, le legge, le analizza | `app/` |
| Analizzatore | analisi statistica seria, offline, in Python/Streamlit | repo separato |

La differenza rispetto agli strumenti commerciali che fanno cose simili è
l'**attribuzione del tiro**: qui ogni burst di campioni è legato per
costruzione al punteggio che l'arciere ha inserito subito dopo, sulla stessa
macchina. Non c'è nessuna sincronizzazione a posteriori da indovinare.

---

## 2. L'hardware, e i fatti che non sono deducibili

Scheda: **Waveshare ESP32-S3-Touch-LCD-1.83** (ESP32-S3R8, 8 MB PSRAM octal,
16 MB flash, N16R8).

### 2.1 I fatti confermati sul campo

Questa tabella è la parte più cara del documento: ogni riga è costata almeno un
flash sbagliato. Nessuno di questi valori si deduce guardando il BSP.

| Fatto | Valore | Come si è scoperto |
|---|---|---|
| Tipo di memoria | `memory_type = qio_opi` | con `qspi` la scheda va in **boot loop**. La 1.69 voleva `qspi`: è lo stesso problema, invertito |
| Ordine colore | `TFT_RGB_ORDER = TFT_BGR` | il BSP dichiara RGB. Al primo flash il ciano usciva giallo e il rosso blu: firma esatta dell'ordine invertito |
| Offset verticale | `CGRAM_OFFSET` + `setRotation()` | con `setViewport()` compare una banda bianca di ~20 px |
| Pannello | 240×284, offset y=20 | il controller resta 240×320 |
| Backlight | GPIO40, attivo **alto** | sulla 1.69 era GPIO15 |
| Reset display | GPIO38 | sulla 1.69 era GPIO8 |
| I2C condiviso | SDA=15, SCL=14 | touch, IMU, RTC, PMU e codec sullo stesso bus |
| Touch CST816 | INT=13, RST=39, addr 0x15 | sulla 1.69 erano 14 e 13: **i numeri si incrociano**, non trascinare i vecchi define |
| IMU QMI8658 | polling puro, addr 0x6B | GPIO14 qui è SCL, quindi **non esiste** un pin INT |
| microSD | FSPI: MOSI=1, SCK=2, MISO=3, CS=42 | bus dedicato, nessuna contesa col display (HSPI 6/7) |
| Codec ES8311 | addr 0x18, rail **ALDO1** dell'AXP2101 | finché ALDO1 non è acceso a 3,3 V ogni I2C al codec torna NACK |
| Ampli NS4150B | enable GPIO46 | |
| I2S | DOUT=8, BCLK=9, MCLK=16, LRCK=45 | |
| PMU AXP2101 | addr 0x34, su I2C condiviso | `battery_init()` **non** chiama `Wire.begin()` |

### 2.2 La taratura del touch

Il CST816 di questa scheda ha una calibrazione di fabbrica che **espande l'asse
X di circa 1,34×**, e non è riprogrammabile. La correzione è affine, in
firmware:

```
TOUCH_CAL_AX = 0.74591    TOUCH_CAL_BX = 21.84
TOUCH_CAL_AY = 0.86208    TOUCH_CAL_BY = 21.68
```

**Come si tara, e come non si tara.** Si tara su centri di coordinate note. Non
si tara sulle campate ai bordi: il driver satura le letture di bordo, e questo
fa sembrare la campata 1:1 quando non lo è. Chi ha provato a tarare sui bordi
ha concluso che il touch era già a posto, e non lo era.

Il dettaglio completo è in `storico/README_TOUCH_CAL.md`.

---

## 3. Architettura del firmware

### 3.1 I task

| Task | Core | Priorità | Cosa fa |
|---|---|---|---|
| `imu_task` | 1 | 5 | legge l'IMU in polling, applica il frame di montaggio, alimenta trigger e buffer |
| `trigger_task` | 1 | 4 | **solo** transizioni temporali FIRED→REARM→ARMED |
| `loop()` | 1 | — | tutta la UI |
| `ble_task` | 0 | — | esiste solo in modo BLE |

La valutazione della condizione di scocco **non** sta in `trigger_task`: sta in
`trigger_feed_sample()`, chiamata da `imu_task` per ogni campione. Il perché è
nella sezione 5.

### 3.2 Il buffer circolare

750 campioni in PSRAM. Con 2000 ms di pre e 1000 ms di post a 224 Hz reali:
448 campioni prima + 224 dopo = 672, dentro i 750.

Al momento dello scocco il buffer si **congela** (`circular_buffer_freeze`)
prima ancora che il semaforo svegli il main: quando la UI reagisce, i campioni
post sono già in raccolta.

### 3.3 Il flusso applicativo

```
ATTESA ──trigger──> SCORING ──DONE──> RIEPILOGO ──OK──> ATTESA
   │                                      │
   │                                   CORREGGI
   └── tasto BOOT ──> menu (config, clinometro, BLE, sessioni)
```

Il trigger è armato **solo** in ATTESA. Mentre si compila lo scoring o si
guarda il riepilogo non deve sparare, e non spara.

### 3.4 L'ODR reale

Nominale 250 Hz, reale **~224,9 Hz**. Il tick FreeRTOS produce un battito
alternato 4005/5005 µs, quindi la mediana di `dt` è fuorviante: la media dà
l'ODR vero. Ogni integrazione usa i **timestamp reali**, mai un `dt` costante.

---

## 4. Convenzioni di segno e montaggio

### 4.1 I segni (montaggio 2, confermati sul campo)

```
alzo      = atan2(az, ax)      positivo = punta della freccia in ALTO
cant      = atan2(ay, ax)      positivo = inclinato a DESTRA
rate_alzo = −gy
rate_cant = +gz
rate_yaw  = −gx                positivo = a destra
```

**La regola che li governa:** i segni si validano **solo** su test statici ad
angolo noto, con l'arco fermo su un supporto e `|g| ≈ 9,8` con sd < 0,15. Mai
su gesti di campo ambigui.

Costa raccontare perché. La v2.12.3 aveva `atan2(-az, ax)`, tarato su un CSV di
"tocchi simulati" con l'arco maneggiato a caso. Il test statico controllato ha
smentito quel segno: col gesto dichiarato "punta in alto" la formula col meno
dava negativo. Quattro tentativi per inferenza avevano dato quattro risposte
diverse; la questione l'ha chiusa l'**ispezione fisica del chip**.

### 4.2 Offset di montaggio

Nel test statico il "piatto" dava alzo ~+9° e cant ~−5° costanti. Non è errore
di formula — le *variazioni* avevano verso corretto — è inclinazione della
staffa. Vive in `offset_cant_deg` / `offset_alzo_deg`, e viene sottratto
**dopo** l'`atan2`, non dalle componenti: gli offset sono angoli, e sottrarre
angoli è l'unica operazione che ne conserva il significato.

`shot_angles_compute()` li riceve come **parametri**, non li legge da
`g_config`. La ragione: quella funzione è replicata in Python e in JavaScript
per la rianalisi offline, e una funzione pura si replica, una che legge globali
si indovina. Per lo stesso motivo gli offset finiscono scritti in
`session.txt`.

---

## 5. Il trigger

### 5.1 Il modo predefinito, e perché

`TRIG_ACCEL_DEV`: la grandezza confrontata con la soglia è **`| ‖a‖ − g |`**.

A riposo vale ~0 *qualunque* sia l'inclinazione della scheda. È l'unica
proprietà che conta: col vecchio `|az|` grezzo il trigger scattava quando
l'arco veniva semplicemente abbassato, perché la gravità ruotava dentro l'asse.
Il trigger misurava l'inclinazione e la chiamava scocco.

È anche indipendente dall'**asse** su cui arriva l'energia — sulla 1.83 lo
scocco si manifesta prevalentemente su `ax`, non su `az` come sulla 1.69.

### 5.2 Il gate di postura

Movimento c'è, ma se la scheda è molto inclinata non è uno scocco: è maneggio.
Misurato su 19 eventi: **tiri veri entro 18,5° dalla verticale, falsi da 25,5 a
91,3°**. Il confronto usa la gravità *stimata*, quindi è indipendente dal
montaggio.

### 5.3 Parametri validati sul campo

```
soglia         20 m/s²   (poi abbassata a 7 per le sedute in giardino)
confirm_n       2 campioni consecutivi (~9 ms a 224 Hz)
rearm        2000 ms
```

Validato: **0 falsi trigger su 12**. Non sono stati toccati.

### 5.4 Perché la valutazione sta in `imu_task`

Bug del 24/07, scocchi veri non rilevati. La valutazione stava in
`trigger_task`, che faceva polling ogni ~3 ms mentre l'IMU produce un campione
ogni ~4,5 ms. I due ritmi non sono sincronizzati, quindi il trigger vedeva solo
*alcuni* campioni.

- Uno **scuotimento a mano** resta sopra soglia 40–60 ms (9–13 campioni):
  anche saltandone metà ne restano abbastanza → scattava.
- Uno **scocco vero** dura ~10 ms (2–3 campioni): perdendone uno non si
  raggiungono `confirm_n` consecutivi → non scattava **mai**.

Ecco perché abbassare la soglia non serviva a niente: il problema non era
l'ampiezza, era che i campioni del picco non venivano proprio guardati.

E c'era un secondo bug sotto: `config_apply()` cablava solo il montaggio, e la
soglia scritta via BLE finiva in NVS ma **non arrivava mai** al trigger. Da qui
`trigger_configure()`, che esiste per rendere operativi i parametri.

---

## 6. Il flusso di scoring

### 6.1 L'ordine, e perché è quello

```
FASE 22:   [ pre-tiro ]            [ dopo lo scocco ]
           DISTANZA → ELEVAZIONE   ESITO → IMPATTO
```

Fino alla F21b il contesto si dichiarava dopo il tiro. Dalla F22 si **arma
prima**, e la sequenza di campo diventa quella vera: telemetro, tocco sulla
card in ATTESA, distanza ed elevazione, OK, attesa dello scocco. Dopo lo
scocco resta solo l'osservazione.

Tre conseguenze:

- **Il contesto resta raggiungibile all'indietro.** Da ESITO si torna a
  ELEVAZIONE e poi a DISTANZA. Accorgersi dopo il tiro che era rimasta armata
  la piazzola precedente è un caso reale, e serve un modo di rimediare. Se la
  correzione avviene, vale anche per i tiri successivi: è lo stesso bersaglio.
- **I valori armati vivono solo in RAM.** All'accensione si riparte da 30 m e
  0°, non dagli ultimi usati: dopo uno spegnimento non c'è ragione di credere
  di essere ancora alla stessa piazzola, e un valore ereditato da ieri sarebbe
  più pericoloso di un default palesemente generico.
- **Il bordo della card cambia colore.** Ciano se confermata dopo l'ultimo
  scocco, ambra se ereditata dal tiro precedente. Non impedisce di tirare con
  la distanza sbagliata — niente lo impedisce — ma lo rende visibile mentre si
  incocca, che è l'unica difesa possibile.

La card è un bersaglio **spaziale** e non un doppio tap. Un doppio tap avrebbe
richiesto di attendere ~300 ms per sapere se un tocco è singolo, ritardando
ogni apertura di sessione e riaprendo la classe di bug che il tap-lungo era
già costata.

Il vecchio ordine `ESITO → ZONA → DISTANZA → ELEVAZIONE` era stato invertito
nella F19 per una ragione che la F22 porta alla conclusione:

distanza ed elevazione sono **contesto**, si sanno prima
ancora di scoccare; l'impatto è **osservazione**, e si compie dopo. Metterli
nell'ordine in cui la realtà li produce ha una conseguenza pratica: se lo
scoring viene abbandonato a metà — il 31% dei tiri del 28/07 è finito con
`arcs=0` — quello che resta salvato è comunque utile invece di essere il nulla.

### 6.2 Tre uscite, non due

| Esito | Cosa finisce su microSD |
|---|---|
| `VALUTATO` | riga CSV + burst |
| `SALTATO` | riga CSV + burst, senza punteggio ("è un tiro ma non lo valuto") |
| `SCARTATO` | **nulla** ("non era un tiro") |

Nascono dai dati del 28/07: su 66 catture, 26 non erano tiri ma l'arco
maneggiato o abbassato (|cant| medio 14,8° contro 2,4; jerk 47 contro 214).
Il timeout di 60 s equivale a SCARTA — meglio perdere un tiro vero dimenticato
che sporcare la sessione.

### 6.3 La schermata IMPATTO (Fase 21)

**Il problema.** Nel momento in cui si tocca il cerchio per dire dove ha
colpito la freccia, il polpastrello nasconde esattamente il punto scelto. Ci
sono 2500 ms per correggere, ma correggere alla cieca non è correggere.

Ha un nome ed è documentato dal 1988: *fat finger problem*. Le tre soluzioni
storiche sono:

- **Take-off / Offset Cursor** (Potter, Weldon, Shneiderman 1988): il cursore
  sta sopra il dito e la selezione avviene al sollevamento, non al contatto.
- **Shift** (Vogel & Baudisch, CHI 2007): al tocco compare un riquadro con la
  copia dell'area coperta, spostato in una zona libera.
- **Cross-Keys / Precision-Handle** (Albinsson & Zhai 2003): tasti discreti
  abbinati a un mirino, per la rifinitura fine.

**La scelta: Cross-Keys.** Shift richiede di tenere il dito premuto e muoverlo
sul vetro, che con l'arco in mano e i guanti è scomodo. I tasti discreti danno
un passo *quantizzato e metrico*, che è anche la cosa giusta per un dato che
finisce in un CSV. Il tocco diretto sul cerchio resta per il posizionamento
grossolano — è il take-off, e copre il caso "so già dov'è".

**La geometria.**

```
 y   0..17   breadcrumb (COLPITO · distanza · elevazione)
             R50 a sinistra · passo a destra, entrambi minuscoli
    20..46   lettura in cm, font 4, centrata
    50..238  ┌─ SX ─┬──── SU ────┬─ DX ─┐   fasce 48×188 e 136×36
             │      ├────────────┤      │
             │      │  cerchio   │      │   R = 50 px, centro (120,144)
             │      ├────────────┤      │
             └──────┴─── GIU ────┴──────┘
   244..283  INDIETRO · NON SO · OK
```

**Niente conferma automatica (F21b).** La F19 confermava allo scadere di 2500
ms, con una barra che si riempiva. Il campo ha detto che una barra che avanza
mentre stai ancora decidendo non fa risparmiare un tocco: mette fretta, e la
fretta su una misura è il contrario di quello che serve. Al suo posto c'è un OK
esplicito, spento finché non c'è un marcatore. Il timeout generale dello
scoring (60 s) resta e basta a chiudere una schermata dimenticata.

**Gerarchia nel corpo del carattere**, non solo nel colore: INDIETRO in font 1
(ripiego), NON SO in font 2 (uscita legittima ma rara), OK in font 4 su fondo
verde — l'azione di ogni tiro, che si deve trovare col pollice senza guardare.

**Colori.** Sul colpito l'anello a 20 cm è giallo: è il giallo della visuale,
cioè il riferimento che l'arciere ha davvero negli occhi. Gli altri anelli sono
una griglia e restano grigi. Il marcatore è **bianco** — con l'anello dei 20 cm
giallo, un marcatore ambra ci si perdeva dentro proprio dove serve leggerlo
meglio. Dentro al cerchio non c'è nessuna etichetta numerica: il raggio è
scritto in alto a sinistra, e lì basta.

**Il prezzo, dichiarato.** Per far posto ai tasti il cerchio scende da R=100 a
R=50 px, quindi la risoluzione del *tocco diretto* dimezza: 1,00 cm/px sul
colpito (raggio 50 cm), 3,00 sul mancato (raggio 150 cm), contro 0,50 e 1,50.
Non è una perdita netta: prima quei mezzi centimetri erano teorici, perché il
punto era sotto il dito. Adesso la precisione la danno i tasti, un centimetro
per tocco, con il punto in chiaro. Si scambia una precisione **dichiarata** con
una **ottenibile**.

**Il passo.** Un pixel, arrotondato al centimetro:
`passo = round(raggio_cm / IMP_R_PX)`, minimo 1. Con raggio 50 → 1 cm; con
raggio 150 → 3 cm. Un passo più fine muoverebbe il marcatore meno di un pixel,
e un tasto che si preme senza vedere niente muoversi sembra rotto anche quando
il numero cambia.

**L'accelerazione, e perché non è auto-ripetizione.** Tocchi ripetuti sullo
stesso tasto entro 500 ms fanno crescere il passo: ×1 per i primi tre, ×3 fino
all'ottavo, poi ×8. Non c'è ripetizione a dito premuto perché richiederebbe di
ridisegnare il cerchio 6–8 volte al secondo — circa 14 ms di ridisegno ogni
150, cioè sfarfallio visibile per tutta la pressione. Così ogni ridisegno resta
agganciato a un'azione discreta, che è il comportamento già collaudato.

**Tre dettagli d'implementazione che non si vedono:**

1. La zona **sensibile** è 2 px più grande di quella **disegnata** su ogni
   lato. Il verso è deliberato: un tasto che risponde appena fuori dal bordo
   non se ne accorge nessuno, uno che non risponde dentro il bordo sembra
   rotto.
2. Il valore in **centimetri** è quello primario e i passi si accumulano su
   quello. Accumulando sui pixel, dieci tocchi da 1 cm non farebbero 10 cm.
3. Al bordo la saturazione **ricalcola i centimetri dal pixel saturato**. Un
   marcatore fermo che continuasse a dichiarare valori crescenti sarebbe una
   bugia silenziosa.
4. **Un tocco fuori dal cerchio si ignora**, non si riporta sul bordo (F21b).
   La F19 lo saturava, e con R=100 px aveva senso: un tocco fuori dal cerchio
   era un tocco fuori dal bersaglio. Con R=50 la corona fra cerchio e fasce è
   stretta e sta proprio dove il pollice passa scendendo: un tocco che la
   sfiora produceva un salto al limite estremo — fino a 150 cm in un colpo. È
   il difetto visto in campo sul mancato scendendo dall'alto. Nessun errore di
   aritmetica: la saturazione faceva esattamente quello per cui era stata
   scritta, in un contesto in cui non serve più. Per andare oltre il bordo ci
   sono i tasti, che saturano un passo per volta e sotto gli occhi.

Tre `static_assert` verificano a compilazione che cerchio e marcatore non
invadano le fasce. Se qualcuno alzerà `IMP_R_PX` per guadagnare risoluzione, lo
scoprirà il compilatore e non il campo.

**Coordinate salvate.** L'origine è il **punto di mira**, non il centro della
sagoma: rende il dato indipendente dalla forma dell'animale 3D e lo mette nello
stesso sistema di riferimento in cui la fisica fa le previsioni. I segni
coincidono con quelli degli angoli — `imp_x_cm > 0` = destra, `imp_y_cm > 0` =
alto — così la regressione impatto/metrica si scrive senza cambi di segno da
ricordare, e i cambi di segno da ricordare sono la sorgente di errore più
documentata di questo progetto.

**Avvertenza sull'elevazione.** Con bersagli a ±20° il piano del bersaglio è
inclinato. `imp_y_cm` è misurato **sulla faccia**, non in verticale vera: per
la verticale vera serve moltiplicare per `cos(elev_deg)`. `elev_deg` è
registrato accanto, quindi il dato è recuperabile — ma va saputo adesso, non
scoperto fra sei mesi guardando una regressione che non torna.

### 6.4 Perché non c'è più la griglia 3×3

Non è la significatività statistica: una simulazione dice che tre livelli
costano appena 4 punti di potenza. È che **tre livelli non hanno unità di
misura**. Con tre bin si può arrivare a dire "sì, correla"; non si potrà mai
dire "0,45 cm di impatto per cm di previsione", che è la costante di taratura
senza la quale una metrica biomeccanica non diventa mai un consiglio
utilizzabile da un arciere.

La zona 3×3 non è sparita: si **deriva** dalle coordinate con soglie a un terzo
del raggio, così le sedute vecchie e quelle nuove restano confrontabili. Le
coordinate contengono la zona; la zona non contiene le coordinate. Memorizzare
entrambe significherebbe due campi che possono divergere.

---

## 7. Le metriche, e cosa misurano davvero

### 7.1 Il fatto che riordina tutto: la finestra causale

**La finestra causale è 17 ms dopo il RILASCIO, non dopo il trigger.** Il
trigger scatta circa **29 ms dopo che la freccia è partita**.

Conseguenza diretta e scomoda: `hold_deg` e `release_jerk` misurano il
**follow-through**, non la causa. Restano diagnostici — un arciere che abbassa
il braccio ha un problema reale — ma non sono la causa dello scarto di quella
freccia.

**Il guadagno di trasferimento** dallo yaw del riser alla direzione della
freccia è **k ≈ 0,45**, non 1,0: la freccia acquista velocità mentre il riser
ruota, quindi non subisce tutta la rotazione.

### 7.2 Le metriche in tabella

| Metrica | Finestra | Cosa dice | Soglie |
|---|---|---|---|
| `cant` / `alzo` | finestra calma adattiva pre-scocco | assetto di mira | — |
| `release_jerk` | 0–40 ms post-trigger | strappo laterale al rilascio | terzili di seduta; 330 / 510 m/s³ |
| `hold_deg` | 0–900 ms post-trigger | deriva del braccio | 4° / 8° (2× e 4× il p95 del rumore) |
| `tempo_mira_ms` | macchina RIPOSO→MOTO→ANCORA | quiete prima del rilascio | — |
| `picco_cms2` | −60…+150 ms | ampiezza dell'impulso di rilascio | **nessuna** |

### 7.3 La finestra calma adattiva

Il problema della finestra a offset fisso: nei ~200 ms prima del trigger
l'arciere sta già partendo, e la finestra pescava **dentro** il movimento →
errore 13–40° sui dati reali. La soluzione fa scorrere una finestra da 200 ms
su tutto l'intervallo pre-trigger e sceglie quella col vettore più vicino alla
sola gravità. Errore medio da ~13° a **~3,7°**.

Dalla stessa finestra si ricava anche il **bias del giroscopio** (principio
ZUPT): lì l'arco è fermo, quindi la media di `gy` *è* il bias. Per-tiro, quindi
compensa la deriva termica — molto meglio dei comandi di calibrazione hardware.

### 7.4 Il picco d'urto (Fase 21)

**Perché il picco e non l'integrale.** Sui 18 tiri del 15/08 l'integrale
dell'impulso al quadrato correlava col picco a **r = 0,96**: le due grandezze
dicono la stessa cosa. Il picco ha un'unità di misura vera (m/s²); l'integrale
ha unità (m/s²)²·s, che non significa niente per nessuno. Chiamarlo "energia"
sarebbe dare un nome fisico a un indice — lo stesso errore che la griglia 3×3
aveva fatto con l'ARCS.

**La grandezza è `| ‖a‖ − g |`, identica a quella del trigger**, e non è un
dettaglio: significa che il numero mostrato nel riepilogo e la soglia impostata
via BLE sono sullo stesso metro. Con due grandezze diverse, "picco 31" e
"soglia 7" sarebbero due numeri incomparabili scritti nella stessa unità.

**La finestra è asimmetrica** — [−60, +150] ms — e la ragione è fisica: il
trigger scatta dopo `confirm_n` campioni sopra soglia, cioè quando l'impulso è
già iniziato da ~9 ms; i 60 ms prima servono a non tagliare il fronte di
salita. Oltre i ~200 ms non si misura più l'urto ma il movimento del braccio,
che ha ampiezze confrontabili e natura completamente diversa: una finestra
lunga darebbe un numero più grande e meno significativo, che è il modo più
comune di rovinare una metrica.

**Nessuna classe colorata**, e non è una dimenticanza. Un semaforo
verde/giallo/rosso dichiarerebbe che si sa quale valore è buono: non si sa. È
una grandezza da mettere in serie storica, non da giudicare a colpo d'occhio,
finché il cronografo non avrà detto a cosa corrisponde.

**Cosa ci si aspetta di vedere.** Seduta del 15/08, 18 tiri a 5 m:

```
correlazione col numero del tiro   r = +0,72   (p = 0,001)
primi sei tiri   media 14,5 m/s²
ultimi sei       media 31,5 m/s²
```

Più che raddoppiato. Derivano anche la durata dell'urto (r = +0,750) e la
tenuta (r = +0,562). Tre spiegazioni erano in gioco:

- *Riscaldamento dell'arciere* — ma la crescita prosegue dritta attraverso una
  pausa di quindici minuti fra il tiro 6 e il 7.
- *Accoppiamento meccanico che si allenta* — ma deriva anche `hold`, che si
  calcola integrando `gy`, cioè una grandezza **girometrica**: una staffa
  allentata attenua ciò che vede l'accelerometro, non ciò che vede il
  giroscopio. E renderebbe l'urto più netto, non più lungo.
- *L'allungo che cresce* — spiega picco, durata e tenuta insieme.

Cautela d'obbligo: sono state provate dieci grandezze, e con Bonferroni solo
picco e durata sopravvivono. L'indizio è coerente, non dimostrato.

**Come si chiude la questione, e costa poco:** il cronografo. La velocità della
freccia misura direttamente l'energia immagazzinata. Sei tiri all'inizio e sei
alla fine. In più darebbe la `v` vera per il conto dei 17 ms, che oggi usa un
54 m/s preso dal manuale.

Se confermato, non è un problema da eliminare: è una **metrica**. Un arciere
che allunga di più tirando dopo tirando è un'informazione allenante, e sarebbe
la prima cosa che ArchBB misura senza che nessuno l'abbia progettata apposta.

### 7.5 Potenza statistica: il capitolo che salva dal concludere il falso

Con n = 35 e f ≈ 0,09 la potenza è **0,38**. Un risultato nullo da una seduta
è **inconcludente**, non negativo.

La seduta di validazione in giardino (18 tiri, 5–6 m, −8°) ha confermato che il
firmware funziona — 18/18 impatti, 18/18 tempi, 0/12 falsi trigger — e allo
stesso tempo che **a 5 m non si può testare niente**: la dispersione angolare è
~0,65° contro ~0,4° attesi a distanza vera, e servirebbero n ≈ 236.

L'esempio più chiaro è l'impatto verticale. Non deriva (r = 0,017), il che
sembrerebbe contrario all'ipotesi dell'allungo. Ma a 5 m la caduta della
freccia è ~4,2 cm; un aumento del 5% della velocità la riduce di 0,4 cm, contro
una dispersione verticale di 11 cm. **Un effetto trenta volte sotto il
rumore.** A 40 m la caduta è ~270 cm e il 10% fa 27 cm, ben visibile: è il test
che a distanza vera diventa decisivo e a 5 m è cieco per costruzione.

La regola generale: **un errore sistematico e uno casuale sulla stessa
grandezza hanno conseguenze opposte.** Un bilancio d'errore che conta solo i
sistematici è completo su ciò che guarda e cieco su ciò che non guarda.

### 7.6 Tiri in forte discesa

Con σ < −20° la curva α(D) è quasi piatta: l'inversione della distanza non ha
senso. Il display mostra `!!!` invece di un numero.

---

## 8. Contratti dati: NVS, CSV, burst, BLE

### 8.1 La regola delle sentinelle

**Lo zero non può fare da sentinella** per grandezze in cui lo zero è
legittimo. Vale per `elev_deg` (tiro in piano), `imp_x_cm`/`imp_y_cm` (centro
esatto), `tempo_mira_ms` (rilascio immediato), `picco_cms2` (arco fermo).

| Campo | Sentinella |
|---|---|
| `elev_deg` | `-128` (`ELEV_NOT_SET`) |
| `imp_x_cm` / `imp_y_cm` | `INT16_MIN` (`IMP_NOT_SET`) |
| `zona` | `255` (`ZONA_IGNOTA`) |
| `tempo_*_ms` | `0xFFFF` (`TEMPO_NOT_SET`) |
| metriche del burst | flag `*_valid` a parte |
| nel **CSV** | **campo vuoto** |

### 8.2 Parametri contro calibrazione

Distinzione che è costata una seduta intera.

- **Parametri** (soglia, finestre, raggi): possono tornare ai default a un bump
  di `CONFIG_VERSION`. Nessun danno, si riscrivono.
- **Calibrazione** (orientamento del montaggio, offset cant/alzo, coefficienti
  touch): rappresenta una **misura fisica**, e se si perde va rifatta al banco.
  Vive in **chiavi NVS scalari separate** e sopravvive ai bump di versione.

Il bug: portare `CONFIG_VERSION` da 5 a 6 aveva riportato tutto ai default,
azzerando in silenzio l'orientamento del montaggio. Silenzio: il firmware
continuava a funzionare, produceva numeri plausibili, e gli assi erano
scambiati.

### 8.3 Il CSV

`CSV_FORMAT_VER = 5`. La stringa dell'intestazione vive in **un solo posto**
(`CSV_HEADER`): fino alla F18 era scritta a mano in due punti, ed erano
identiche per fortuna, non per costruzione.

```
shot, ts_ms, colpito, zona, dist_m, elev_deg, arcs,
cant_cdeg, alzo_cdeg, angoli_stabili,
hold_cdeg, hold_class, release_jerk, release_class,
burst_file, imp_x_cm, imp_y_cm, imp_raggio_cm,
tempo_mira_ms, tempo_alzata_ms, assetto_ok, picco_cms2
```

Due regole non negoziabili:

1. **I campi nuovi vanno sempre in coda.** Mai in mezzo.
2. **Si mappa per nome di colonna, mai per posizione.** Un file v1 deve restare
   leggibile da un lettore v5.

Le grandezze angolari sono in **centesimi di grado** interi; `picco_cms2` in
**centesimi di m/s²**. Nessun float nel CSV: niente ambiguità di separatore
decimale fra locale italiana e pandas.

**Storia delle versioni:** v2 (F19) coordinate d'impatto · v3 (F20) tempi del
gesto · v4 (F20b) `assetto_ok` · v5 (F21) `picco_cms2`.

### 8.4 Il burst binario

`burst_NNNN.bin`: header 16 byte (magic `ABB1` + versione + conteggi) seguito
dai campioni grezzi, **già nel frame canonico** del montaggio dichiarato.
`session.txt` porta `mount`, `mount_lato`, `off_cant_deg`, `off_alzo_deg`: una
sessione è interpretabile **solo** se dichiara con che montaggio è stata
registrata.

Le sedute anteriori alla F11 non lo dichiarano, e su quelle cant e alzo sono
**scambiati**. L'app non le corregge da sola: mostra un avviso. Correggere in
silenzio un dato di cui non si conosce la provenienza è peggio che mostrarlo.

### 8.5 Il BLE

Modale, non demone: si entra in modo BLE da menu, i task IMU vengono sospesi,
si esce e si riprende. Stack NimBLE su Core 0.

Tre regole pagate care:

- **Mai chiamare `NimBLEDevice::deinit()`.** Produce panic all'uscita.
- **Il ritardo fra notify va ancorato all'intervallo di connessione realmente
  negoziato**, non a una costante.
- **Mai sospendere un task mentre tiene un mutex.**

I **burst non passano dal BLE**: si copiano dalla microSD. Un burst è ~20 KB e
una sessione ne ha decine; a 512 byte di MTU sarebbe un supplizio. Dal BLE
passano `shots.csv` e `session.txt`, che sono kilobyte.

Il pacchetto di config è di **39 byte**. La F19 ne aveva aggiunti due da 16 bit
portandolo da 35 a 39, e per un po' l'app ha continuato a mandarne 35: il
firmware li rifiutava **in silenzio**. Ogni cambio di struct richiede un
rilascio in pari di firmware e app.

---

## 9. Storia delle fasi

| Fase | Cosa ha portato |
|---|---|
| 1–3 | display, touch calibrato, UI di scoring |
| 4a | IMU + trigger reale; architettura FreeRTOS a due task |
| 4b | buffer circolare in PSRAM |
| 4c | angoli `cant`/`alzo` dal burst, finestra calma adattiva |
| 5 | config in NVS, persistenza microSD, nomi sessione da RTC |
| 6–7 | BLE modale, app companion, config via GATT |
| 10 | download delle sessioni via BLE |
| 11 | montaggi con nomi che dicono il lato fisico + taratura |
| 13 | scoring: SCARTA / SALTA, meno tocchi |
| 15 | slider verticali, una grandezza per schermata |
| 18c | clinometro di banco, matrice di regressione 2×2 accelerometro/giroscopio |
| 19 | riordino DISTANZA→ELEVAZIONE→ESITO→IMPATTO; coordinate in cm |
| 20 / 20b | macchina dei tempi RIPOSO→MOTO→ANCORA; fix migrazione calibrazione |
| 21 | tasti di spostamento ai bordi; picco d'urto; radice riordinata |
| **22** | **pre-tiro: distanza ed elevazione armate PRIMA dello scocco, card in ATTESA, numero del tiro in font 7; riga del gesto muta a riposo** |
| **21b** | **OK esplicito al posto del temporizzatore; lettura in font 4; anello giallo; marcatore bianco; niente saturazione fuori dal cerchio; ARCS tolto dal riepilogo** |

Il ramo WS 1.69 è chiuso al firmware v2.16.10 + app v1.29.

---

## 10. Gli errori commessi, e le regole che ne sono nate

Questa è la sezione che vale di più, perché non sono modifiche: sono vincoli.

**§ Un fatto scritto in due posti prima o poi diverge.** Sulla 1.69 il disegno
usava le costanti e l'handler aveva i numeri della versione precedente scritti
a mano. Toccavi "+" e partiva OK. Otto versioni spese a calibrare un touch che
era già a posto, mentre il bug era un letterale in una riga. Da qui: disegno,
hit-test e feedback leggono le **stesse costanti nominate**. Vale anche per le
stringhe (`CSV_HEADER`), per le coppie di conversione (`pxToCm`/`cmToPx`) e per
i dati (coordinate contro zona).

**§ La macchina a stati va provata con la vera sequenza pre-evento**, mai
chiamata da uno stato pulito. Uno stato pulito non è uno stato che esiste.

**§ Un filtro lento produce risposte pulite e sbagliate**, non risposte
rumorose. La stima di gravità con τ=2 s dava 1492 ms invece di 2500. Un
risultato rumoroso si nota; uno pulito e sbagliato no.

**§ Due stime che concordano non sono una conferma** se condividono il
denominatore.

**§ Un dato inventato è peggio di un dato mancante**, perché entra nelle
statistiche senza dichiararsi. Da qui `ZONA_IGNOTA` e l'uscita "NON SO":
quindici frecce infisse nella sagoma e nessun modo di sapere quale sia quella
appena tirata è un caso reale.

**§ Non riempire una casella con una formula plausibile.** Un numero che sembra
una misura senza esserlo fa più danno di una casella vuota. L'app dichiara
quali metriche non ha portato, e perché.

**§ I font 6/7/8 di TFT_eSPI sono solo numerici.** Scriverci "COLPITO" o una
freccia non dà errore: fa sparire i caratteri. I simboli si disegnano
geometricamente.

**§ `str.replace` senza asserire l'unicità dell'ancora fallisce in silenzio.**

**§ La zona sensibile non deve mai essere più piccola di quella disegnata.**

**§ Un conto alla rovescia visibile su una schermata di misura mette fretta.**
Era stato messo per risparmiare un tocco; costava attenzione mentre si guarda
il bersaglio, che vale più di un tocco.

**§ Due oggetti dello stesso colore, uno sopra l'altro, spariscono.** Vale
banalmente per il marcatore ambra sull'anello giallo, e in generale: quando si
introduce un colore nuovo va controllato cosa ci finisce sopra.

**§ Una soluzione giusta smette di esserlo quando cambia la geometria attorno.**
La saturazione dei tocchi fuori dal cerchio era corretta con R=100 ed è
diventata un difetto con R=50. Non era un bug: era una decisione presa in un
contesto che non c'è più. Vale la pena rileggere le decisioni vecchie ogni
volta che si cambia una dimensione.

**§ Cancellare ed estrarre di nuovo l'archivio**, mai sovrascrivere. E
controllare il numero di versione prima di fidarsi di un risultato.

**§ Non cambiare un parametro che non sta dando errori.**

**§ Una scritta che ripete quello che la schermata già dice non è neutra: occupa
pixel.** "In attesa dello SCOCCO" sulla schermata di attesa era pleonastico, e
la sua fascia — centrata su y=244, alta 17, quindi fino a 235 — mordeva la base
delle cifre del numero del tiro in font 7. La riga del gesto resta viva per gli
stati che portano informazione (arco in movimento, cronometro di mira) e a
riposo si limita a ripulire la fascia.

**§ Un dato che si eredita in silenzio va reso visibile.** L'inversione del
pre-tiro sposta l'errore da "sbagliare attivamente" a "dimenticarsene". Non si
può impedire; si può metterlo in vista e colorarlo diversamente.

---

## 11. Strumenti di diagnostica

Sono infrastruttura **permanente**, non impalcature da togliere.

| Strumento | Comando / voce | A cosa serve |
|---|---|---|
| `controlla.sh` | `sh tools/sintassi/controlla.sh` | sintassi in gnu++11 con stub, 2 secondi; segnala anche le funzioni dichiarate in un `.h` e mai definite |
| Clinometro | menu, voce 3 | pitch/cant assoluti con salvataggio offset; matrice di regressione 2×2 per verificare i segni degli assi |
| DIAG assi | env `archbb_183_debug` | riga CSV seriale con assi grezzi + cant/alzo dei quattro montaggi |
| Registratore scocco | env `archbb_183_shotrec` | ogni campione a 500 Hz, senza trigger né soglie, su microSD |
| Peak-hold | env `archbb_183_raw_diag` | picchi \|ax\| \|ay\| \|az\| a schermo: si tira una freccia e si leggono tre numeri |
| Test montaggi | env `archbb_183_mount_test` | caratterizzazione IMU in pose note |
| Ghost del touch | attivo nella UI | mostra dove il firmware crede che sia il dito |

Il registratore di scocco è nato da una diagnosi sbagliata: si continuava a
ipotizzare perché il trigger non scattasse, **senza avere nemmeno un dato di
uno scocco vero sulla 1.83**. Ogni ipotesi era infalsificabile. Prima il dato,
poi l'ipotesi.

---

## 12. Come si compila e si carica

```powershell
# ambiente di produzione (default, muto)
pio run -e archbb_183_touchcal -t upload

# al tavolino, con i log
pio run -e archbb_183_debug -t upload
pio device monitor -b 115200
```

In PowerShell si usa `;` fra i comandi, non `&&`.

Gli ambienti diversi da quello di produzione vanno **selezionati
esplicitamente** nella barra di stato di VS Code prima di premere Upload: il
pulsante ▶ generico usa il default.

**Due punti critici del `platformio.ini`**, entrambi già spiegati nella sezione
2: `memory_type = qio_opi` e la configurazione TFT_eSPI passata via
`build_flags` invece che con un `User_Setup.h` (che verrebbe cancellato al
primo aggiornamento della libreria).

Con `ARCHBB_DEBUG` attivo e il monitor **chiuso**, su S3 con USB CDC nativo i
`print` restano best-effort: `setTxTimeoutMs(0)` li rende non bloccanti. In
campo si va muti.

---

## 13. Cosa resta aperto

**Il cronografo.** È la prossima seduta. Annotare la V0 accanto al numero di
tiro su un foglio a parte. Chiude la questione del picco che cresce e dà la `v`
vera per il conto dei 17 ms.

**Studi di correlazione a distanza vera.** A 5 m i test sono ciechi per
costruzione. Servono 25–40 m.

**Energia vibrazionale fuori piano come metrica di pulizia del plucking.**
Formalmente **sospesa**: limitata dal rumore di fondo col filtro IMU a 12 Hz.
Serve l'acquisizione col microfono. Il memo dettagliato è in
`storico/BRIEFING_Traccia2D_riprogettazione.md`.

**Guadagno di trasferimento k ≈ 0,45** e biomeccanica della finestra causale:
lavoro analitico, non firmware.

**Feedback sonoro.** `beep_mode = BEEP_SFUMATO` preferito al sempre-acceso per
l'ipotesi della guida (Salmoni/Schmidt/Walter 1984; Winstein & Schmidt 1990):
il feedback continuo crea dipendenza. Pip a 2500 Hz, picco della sensibilità
uditiva. Vincolo rigido: `hold_ms ≥ window_post_ms + 200`. Il beep di tenuta è
ancorato a `g_trigger_ms`, scritto dentro `doFire()`, non all'osservazione dal
loop — evita ~7 ms di ritardo sistematico.

**Celle di carico HX711** per la forza di trazione istantanea: firmware e
dashboard pronti a integrarle, manca il disegno meccanico del supporto.

---

## Riferimenti bibliografici

- Potter, R., Weldon, L., Shneiderman, B. (1988). *Improving the accuracy of
  touch screens: an experimental evaluation of three strategies.* CHI '88.
- Albinsson, P.-A., Zhai, S. (2003). *High precision touch screen interaction.*
  CHI '03.
- Vogel, D., Baudisch, P. (2007). *Shift: a technique for operating pen-based
  interfaces using touch.* CHI '07, 657–666.
- Salmoni, A., Schmidt, R., Walter, C. (1984). *Knowledge of results and motor
  learning: a review and critical reappraisal.* Psychological Bulletin.
- Winstein, C., Schmidt, R. (1990). *Reduced frequency of knowledge of results
  enhances motor skill learning.* JEP: LMC.
