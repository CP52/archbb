# ArchBB 1.83 — FASE 20 · tempo di mira e di alzata

**1 agosto 2026** · `ArchBB_183_Fase20` · fw `1.83-F20-tempo`
CSV **v3** · config NVS invariata (v6)

---

## 0. La misura non c'era, ed è stato verificato prima di scrivere codice

Il burst copre 2000 ms prima dello scocco. Misurando sui 47 tiri validi della
seduta `SESS_20260728_1005` il tempo di quiete che precede il rilascio:

```
mediana                        1924 ms
quartili                       1908 / 1934
estensione reale del buffer    2058 ms
```

**Saturata contro il bordo.** Quando il buffer si apre, l'arco è già in trazione
da un pezzo. Solo tre tiri su 47 mostrano un punto di assestamento dentro la
finestra (418, 813, 1090 ms), e sono verosimilmente riassestamenti a metà mira,
non l'inizio della trazione.

Allungare la finestra avrebbe funzionato — 30 byte a campione, da 2 a 6 secondi
il burst passa da 20 a 46 KB e la PSRAM non se ne accorge — ma avrebbe spostato
il problema: chi trattiene otto secondi sarebbe di nuovo tagliato. Qui **non
serve la traccia della trazione, servono tre istanti**, e tre istanti stanno in
tre variabili.

## 1. La macchina

`src/tempo.cpp` — FSM a tre stati alimentata da ogni campione IMU, dallo stesso
punto che alimenta il trigger (`trigger_feed_sample`). Due percorsi di
alimentazione separati sarebbero due modi diversi di perdere campioni.

```
RIPOSO  ──|ω| > 25 dps──►  MOTO  ──quiete 150 ms──►  ANCORA  ──scocco──►
```

```
t_alzata    inizio dell'ultimo movimento in postura di tiro
t_ancora    inizio dell'ultima quiete continua        ← non quando è confermata
t_rilascio  lo scocco, già rilevato dal trigger
```

`t_ancora` è l'istante in cui la quiete **è cominciata**, non quello in cui
l'abbiamo creduta. Confondere i due accorcerebbe ogni misura di 150 ms in modo
sistematico e invisibile.

Base dei tempi: `ImuSample::timestamp_us`, la stessa del burst. Due basi dei
tempi in un sistema che misura tempi sono un errore gratuito.

L'aggancio è in `doFire()`, che era già l'unico punto di scocco condiviso da
trigger automatico e manuale: i due percorsi non possono divergere nemmeno sui
tempi.

### Cosa si misura davvero

**Il tempo di quiete prima del rilascio.** Coincide col tempo di mira solo se la
trazione è un movimento continuo. Un riassestamento a metà mira supera la soglia
di moto e **fa ripartire il conteggio** — è voluto: la grandezza che la
letteratura chiama *aiming duration* è l'ultima fase stabile prima del rilascio,
non il tempo totale in trazione.

**Il tempo di alzata è una metrica di qualità inferiore** e va usato sapendolo.
Il suo cronometro parte quando l'arco entra in postura di tiro, non quando
comincia a muoversi da terra: il gate di postura ha bisogno di qualche decimo di
secondo. Sul banco, su 900 ms reali ne dichiara 620. Il tempo di mira invece è
esatto (2493 su 2500) perché è ancorato all'inizio di una quiete, non a un fronte
di movimento.

### Precisione

Il trigger scatta sull'urto dello scocco, ~29 ms dopo che la freccia è partita.
Su una misura di 2–4 secondi è meno dell'1,5%: irrilevante. Sulla finestra
causale di 17 ms era tutto. **La stessa imprecisione cambia peso a seconda di
cosa si misura**, e va detto ogni volta — la tentazione di riusare un numero già
validato fuori dal suo contesto è esattamente l'errore dei 5 gradi.

## 2. Le soglie non sono misurate

```
TEMPO_MOTO_DPS     25.0    sopra = l'arco si muove
TEMPO_QUIETE_DPS    8.0    sotto = fermo
TEMPO_QUIETE_MS2    0.8    sotto = fermo  (||a|−g|)
TEMPO_QUIETE_MS     150    quiete continua per entrare in ANCORA
TEMPO_POSTURA_COS  0.85    stessa soglia del gate del trigger
```

Sono scelte con margine largo a partire dal rumore noto (2,9 dps nella finestra
di mira, briefing 31/07): l'oscillazione di mira sta sotto i 3 dps, la trazione
ruota il riser di decine di gradi in circa un secondo, quindi 30–60 dps. Le due
soglie stanno larghe ai due lati di quell'intervallo vuoto. **Vanno validate sul
campo**, e per questo la fase porta con sé la riga di stato dal vivo.

## 3. La riga di stato dal vivo

Occupa la stessa riga che portava il testo fisso «In attesa dello SCOCCO»:
stesso posto, più informazione, nessun cambio di impaginazione.

```
RIPOSO   «In attesa dello SCOCCO»   ambra
MOTO     «arco in movimento»        ciano
ANCORA   «in mira  2.4 s»           verde, conta
```

Ridisegno parziale con cache sul testo: chiamabile a ogni giro del loop senza
sfarfallio. Quota e altezza della fascia sono costanti condivise fra disegno
completo e ridisegno parziale (§4.1) — altrimenti il parziale cancellerebbe una
fascia diversa da quella che scrive, e si vedrebbe solo come una riga di pixel
sporchi che nessuno riesce a spiegare.

Senza un modo di vedere la macchina mentre gira, le soglie resterebbero
un'opinione fino alla lettura del CSV a fine seduta.

## 4. Dati

```cpp
uint16_t tempo_mira_ms;      // TEMPO_NOT_SET (0xFFFF) = non misurato
uint16_t tempo_alzata_ms;
```

In coda a `ScoreResult`, due colonne in coda al CSV (`v3`). Lo **zero** non
poteva fare da sentinella: è un tempo di mira legittimo (rilascio immediato).
Stessa disciplina di `ELEV_NOT_SET` e `IMP_NOT_SET`.

I tempi si copiano nel risultato **prima** di tutti i `return` anticipati di
`computeShotAngles`: non dipendono dal burst, e un tiro col buffer non pronto ha
comunque tempi validi. Perderli per un motivo che non li riguarda sarebbe un
accoppiamento inventato.

## 5. Verifiche

**Banco di sintassi** — `sh tools/sintassi/controlla.sh`: tutto ok, con
`tempo.cpp` aggiunto alla copertura.

**Banco funzionale nuovo** — `sh tools/tempo/prova.sh`. La FSM è logica pura, non
dipende da hardware: è il primo pezzo di questo firmware che si può *provare* e
non solo compilare.

```
1) mira=2493 ms  alzata=620 ms    (2500 e 900 attesi)
2) mira=1194 ms  alzata=248 ms    riassestamento: il cronometro riparte
3) mira=n/d      alzata=n/d       mai in postura
4) mira=4992 ms  alzata=n/d       arco tenuto su fermo, nessuna trazione vista
```

Lo scenario 4 è il più istruttivo: l'alzata vuota è il segnale che nessuna
trazione è stata vista, e va letto così invece che come un buco.

**Un errore trovato dal banco, che il compilatore non avrebbe visto.** La prima
stesura usava τ = 2 s per la stima della verticale, copiata concettualmente dal
trigger. Con τ lungo la stima impiega secondi a seguire il passaggio da «arco
abbassato» a «arco in postura», il gate di postura restava falso per il primo
secondo di mira, e in quel secondo la quiete candidata veniva azzerata a ogni
campione: **mira 1492 ms invece di 2500, alzata persa del tutto**. Corretto a
τ = 0,3 s con aggiornamento sospeso quando `||a|−g| > 2` (durante lo scocco
l'accelerazione arriva a decine di m/s², e lasciarla entrare farebbe ruotare la
verticale proprio nell'istante in cui serve ferma).

Regola di metodo: **un filtro troppo lento non produce un numero rumoroso, ne
produce uno pulito e sbagliato.** È il gemello della lezione del 30/07 — «filtro
strano → sospetta il filtro, non un nuovo fenomeno fisico» — visto dall'altro
lato: qui il filtro non produceva stranezze, produceva plausibilità.

## 6. Cosa aspettarsi dai dati

La letteratura è netta su un punto e contraddittoria su un altro.

Nello studio multiparametrico su otto arcieri d'élite (Frontiers in Sports &
Active Living, 2025) la **durata di mira risulta una strategia individuale**, non
un determinante di gruppo. E mentre diverse ricerche associano durate più brevi a
prestazioni migliori, Callaway, Wiedlack e Heller riportano il contrario.

Due conseguenze operative:

1. **Nessun valore assoluto ha senso come soglia.** Quello che conta è lo scarto
   dalla propria mediana — lo stesso principio che ha raddrizzato l'analisi dello
   yaw il 31/07, dove la componente sistematica era assorbita dal mirino.
2. **Un dispositivo personale è lo strumento giusto per questa domanda.** Se
   l'effetto è individuale, uno studio su otto arcieri d'élite non può trovarlo e
   cento tiri tuoi sì.

Calibrazione delle aspettative: negli stessi lavori le variabili che meglio
predicono la prestazione sono il tempo di reazione al clicker, la forza di
trazione e la velocità massima di oscillazione posturale. Il clicker non esiste
nel barebow, l'oscillazione posturale la vede una pedana di forza. Il tempo di
mira è ciò che possiamo misurare, non ciò che in letteratura ha l'effetto più
forte.

## 7. Cosa manca a valle

- **Analizzatore**: leggere `tempo_mira_ms` e correlarlo con `imp_x_cm`/`imp_y_cm`
  — centrando entrambi sulla propria mediana.
- **Validazione delle soglie**: una ventina di tiri con trattenute deliberate
  (contando «uno-due-tre») e confronto col CSV, guardando la riga dal vivo.
- **App**: mostra già le colonne nell'esportazione perché conserva i campi
  grezzi, ma non le visualizza. Da fare quando ci saranno dati veri.
