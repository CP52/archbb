# FASE 24b — Integrità del dato, tasto PWR, preavviso di batteria

Cesare Pagura · Padova/Noale IT · 14 settembre 2026
Origine: analisi della sessione del 12/09 (41 tiri, 5 cartelle) e della scarica
completa al banco del 13/09.

---

## 1. Buffer stantio — il difetto che aveva già falsificato un dato

### Il caso reale

`SESS_20260912_1028/burst_0005.bin`: dentro un burst di 3,36 s c'è un buco di
**60,99 secondi** fra il campione 298 e il 299, e `seq` salta da 52963 a 709.

Ricostruzione: 136 s di riepilogo sul tiro 4 (buffer `FROZEN`, push scartati,
`seq` che continua a correre), poi il tap OK, poi il trigger **745 ms dopo** —
un maneggio. Dei 448 campioni di pre-trigger solo **149** erano freschi.

| | tiro 5 | tiro 3 (vero) |
|---|---|---|
| picco \|a\|−g | 7,14 m/s² | 18,36 m/s² |
| elevazione armata | 30° | 11° |
| alzo misurato | **6,88°** | 10,84° |
| esito | **valutato, arcs=265** | valutato |

La finestra di calma è stata cercata su campioni di un minuto prima, e ha
prodotto un angolo plausibile, sbagliato e salvato come buono.

### La causa

```cpp
void circular_buffer_reset() {
  s_state      = BUFFER_RUNNING;
  s_post_count = 0;      // e basta
}
```

Il reset rimetteva lo stato a RUNNING ma non teneva conto di quanti campioni
nuovi fossero arrivati, e `get_burst()` copiava comunque 448 campioni di
pre-trigger, freschi o no.

### Il rimedio

Non svuotare l'anello (costoso e inutile): **sapere quanti campioni valgono**.

- `circular_buffer`: contatore `s_freschi`, azzerato dal reset, saturato a
  `BUF_SIZE`; istantanea `s_freschi_al_trigger` presa **al congelamento** (non
  alla lettura: fra i due arrivano i POST, freschi ma inutili a giudicare il
  pre-trigger).
- `circular_buffer_get_burst()` restituisce `pre_validi` (parametro opzionale).
- `shot_angles_compute()` accetta `pre_validi` e **pavimenta la ricerca** della
  finestra di calma a `trigger_idx - pre_validi`. Se non c'è spazio, ritorna
  angoli **non validi** → campo vuoto nel CSV, mai un numero inventato.

Il default `0xFFFF` vale "non dichiarato, tutto valido" e conserva il
comportamento per le repliche Python e JavaScript.

---

## 2. La frequenza non si dichiara più: si misura

> **CORRETTO IN 24b.1 — leggere anche la sezione finale.** La misura di 200,12 Hz
> su cui questa sezione era costruita era **sbagliata**: la frequenza reale è
> ~217,8 Hz e l'errore della costante dichiarata è del **3%**, non del 12%.
> Il principio resta, i numeri no.

Tutti e cinque i `session.txt` del 12/09 dicevano `odr_hz=224`.

`odrToHz(ODR_250HZ)` è una costante di compilazione, e quella costante dimensiona
le finestre. La correzione non è aggiustare la costante — sarebbe rimediare a un
numero inventato con un altro numero inventato. È smettere di dichiarare ciò che
non si è misurato: il valore va nell'header del burst e in `session.txt` come
`odr_hz_reale=`, **accanto** a `odr_hz`. Se divergono, il file lo dice da solo.

## 3. Header del burst versione 2 — senza rompere niente

I quattro byte `[12..15]` erano riservati. Adesso portano:

```
[12..13] pre_validi        (uint16)
[14..15] odr in centi-Hz   (uint16)   es. 20012 = 200,12 Hz
```

Header sempre 16 byte, campioni sempre a passo 30: un lettore v1 che salta i
riservati legge i file v2 senza accorgersene.

**Perché qui e non in coda al CSV:** sono proprietà del *burst*, non del tiro.
Chi apre un `.bin` da solo, a mano, fra sei mesi, deve poter sapere quanti dei
suoi campioni valgono e a che passo sono stati presi, senza avere il CSV accanto.

## 4. CSV versione 6 — due colonne in coda

`scarto_cdeg` — |alzo misurato − elevazione armata|, in centesimi di grado.
Dalla F22 le due misure sono indipendenti, e il loro scarto è un rivelatore di
catture spurie che non costa niente: sui 41 tiri del 12/09 i valutati avevano
scarto mediano **1,62°**, i saltati **9,57°**.

**Viene solo calcolato e scritto. Non decide nulla, e non deciderà nulla** finché
non ci saranno ~180 tiri: con 8 saltati l'intervallo di Wilson sulla sensibilità
va da 0,53 a 0,98, cioè non dice niente. E l'etichetta non è verità fisica — è il
giudizio dell'arciere quando ha premuto NON SO.

`pre_validi` — campioni pre-trigger appartenenti davvero al tiro.

Entrambi vuoti quando non calcolabili: uno zero su `scarto_cdeg` significherebbe
"coincidono perfettamente", l'esatto contrario di "non lo sappiamo".

---

## 5. Tasto PWR — tre misure su tre livelli

Il problema è ergonomico: il PWRKEY è sul fianco, la mano di scocco porta la
patella. Il 12/09 è costato uno spegnimento accidentale e più ingressi in BLE.

**Il danno vero non è lo spegnimento.** `enterBleMode()` sospende `imu_task` e
`trigger_task`: ogni freccia scoccata in quella finestra non esiste. Nessuna
riga, nessun burst, nessun segnale. È l'unico modo in cui questo strumento può
perdere un dato senza dichiararlo.

### 5.1 Pressione lunga a 10 secondi

`setPowerKeyPressOffTime(XPOWERS_POWEROFF_10S)`. Oltre qualunque sfregamento,
ancora comodo da fare apposta.

### 5.2 La pressione lunga si vede arrivare

Si abilita anche `PKEY_LONG_IRQ`. Non per impedire lo spegnimento — quello resta
hardware — ma per fare, nella finestra prima dello stacco, l'unica cosa che il
PMU non può fare: **lasciare scritto perché**. Sessione chiusa con `ended_ms`
veri, nota in `ENERGIA.CSV`, schermata di spegnimento.

Il 12/09 `SESS_1215` è rimasta senza `ended_ms` e ho dovuto ricostruire la
timeline da due ancoraggi indipendenti. Due righe qui valgono quella mezz'ora.

Nota di implementazione: `getIrqStatus()` popola i registri e `clearIrqStatus()`
li azzera **tutti**. Due funzioni indipendenti che leggono e puliscono a turno si
mangerebbero gli eventi a vicenda, e il sintomo sarebbe una pressione su tre che
sparisce — intermittente, quindi impossibile da inseguire. Si legge una volta, si
memorizzano i due esiti, si pulisce una volta.

### 5.3 La pressione breve chiede, non entra

Nuova schermata `BLE_CONFIRM`, con il verso della sicurezza scelto apposta:

- il **NO sta sopra**, dove cade il pollice per primo, ed è la risposta di default;
- il **timeout di 5 s decade su NO**;
- un tocco **fuori dai pulsanti** vale NO — un tocco a caso, come quello che ha
  fatto comparire la schermata, non deve poter entrare in BLE;
- a sessione aperta l'avvertenza cambia parole: *"i tiri NON saranno registrati"*;
- **il trigger resta armato**. Se una freccia parte mentre la domanda è a
  schermo, viene registrata e la domanda decade. Sospendere la misura per
  aspettare una risposta sarebbe il difetto che questa schermata esiste per
  impedire.

Fra "entrare per sbaglio" e "non entrare al primo colpo", solo il primo perde dati.

---

## 6. Le correzioni minori

**Cooldown di 2 s sul toggle di sessione.** Il 12/09 sono nate due sessioni
vuote: `SESS_1134` chiusa 3 secondi dopo l'apertura, `SESS_1135` dopo 18. Due tap
ravvicinati non possono voler dire "apri e chiudi". Non è un debounce (quello è
sul tocco): è una regola sul *significato*, e sta dove il significato si decide.

**`last_shot_datetime` non mente più.** Prima ci andava sempre l'ora della
scrittura; alla ripresa di una sessione `s_shotCount` arriva già > 0 dal CSV, e il
file dichiarava un ultimo tiro mai avvenuto (`SESS_1215`: 13:10:56 contro le
13:04:27 vere). Adesso l'ora si memorizza quando il tiro avviene. Se è ignota, la
riga **non si scrive**: meglio un campo assente di un campo che mente.

**Il sync dell'ora via BLE lascia traccia.** Il 13/09 uno spostamento di +198 s a
metà scarica ha fatto saltare la colonna `iso` senza dirlo. Ora finisce in
`ENERGIA.CSV` con il prima e il dopo: da lì la deriva dell'RTC si calcola da sola,
sync dopo sync, senza fare esperimenti apposta.

**Colonna `temp_c` nel log energetico.** Non è temperatura ambiente: è il die del
QMI8658 dentro un case chiuso, qualche grado sopra l'aria per autoriscaldamento.
Ma la cella sta nella stessa scatola, quindi come proxy è il migliore che
abbiamo, ed è gratis. La capacità di una LiPo dipende pesantemente dalla
temperatura: senza questa colonna una scarica di novembre e una di luglio
sarebbero due numeri incomparabili senza che il file lo dica.

**Preavviso di batteria in volt.** Dal ginocchio della curva del 13/09: **3,70 V
→ "~1 ora"**, **3,60 V → "~20 minuti, chiudi la sessione"**. Una volta sola per
soglia (mai un ritorno indietro: sotto carico la tensione oscilla e un avviso che
si ripete si impara a ignorare), solo da ATTESA, mai in mezzo allo scoring.

La percentuale dell'AXP2101 resta fuori da ogni decisione: è stimata dalla
tensione e sotto carico rimbalza di parecchi punti fra righe consecutive.

---

## 7. File toccati

| file | cosa |
|---|---|
| `circular_buffer.h/.cpp` | contatore dei freschi, istantanea al congelamento, `pre_validi` |
| `shot_angles.h/.cpp` | pavimento della finestra di calma, angoli non validi se non c'è spazio |
| `sd_storage.h/.cpp` | ODR misurato, header burst v2, CSV v6, `last_shot_datetime`, `temp_c` |
| `battery.h/.cpp` | power-off a 10 s, `PKEY_LONG_IRQ`, poll unificato delle IRQ |
| `display.h/.cpp` | `displayBleConfirm`, `displaySpegnimento`, `displayAvvisoBatteria` |
| `main.cpp` | `BLE_CONFIRM`, pressione lunga, cooldown sessione, preavviso batteria, `pre_validi` |
| `config.h` | soglie `ENERGIA_V_AVVISO` / `ENERGIA_V_CRITICO` |

Banco di sintassi: **15 controlli verdi**, compresi i due rami LEDC.

---

## 8. Le prove da fare

1. **Buffer stantio.** Arma un bersaglio, tira, e al riepilogo premi OK e scuoti
   subito la scheda (entro un secondo). Nel CSV la riga deve avere `pre_validi`
   molto minore di 448 e le colonne `cant_cdeg`/`alzo_cdeg` **vuote**. Prima
   avrebbero contenuto due numeri plausibili e falsi.
2. **ODR.** Un tiro qualsiasi: `session.txt` deve riportare `odr_hz_reale=` vicino
   a 200,1. Se dice altro, la frequenza è cambiata di nuovo ed è meglio saperlo.
3. **Pressione breve.** Compare la domanda, non il modo BLE. Lasciando fare, dopo
   5 secondi si torna in attesa.
4. **Pressione lunga.** Compare "SPEGNIMENTO", e in `ENERGIA.CSV` resta la nota
   con la tensione. Se molli il tasto prima dei 10 s non si spegne — ma la
   sessione è già stata chiusa pulita, e il prossimo tocco ne apre una nuova.
5. **Due tap rapidi fuori dalla card.** Solo il primo conta.

## 9. Cosa resta sul tavolo

Il soffitto dell'autonomia è il **baseline di 91 mA**, non il display. Con 330 mAh
sono 3,6 ore anche a schermo sempre spento. Per le sei ore serve scendere a 55.

- **CPU a 80 MHz** (−20 mA → 4,5 h): sbloccata dal test PSRAM descritto in
  `config.h`.
- **ALDO1 spento** (−2÷10 mA): il rail del codec ES8311, acceso dal boot e mai
  usato dal firmware di produzione. È l'unico rail che spegnerei con fiducia,
  perché la mappatura è documentata in `DOCUMENTAZIONE.md`.
- **FIFO del QMI8658 con interrupt di soglia** (−~25 mA → 7,7 h): è l'unico
  intervento strutturale. Oggi `imu_task` si sveglia mille volte al secondo e la
  CPU non scende mai in idle profondo. Con la FIFO a 10 campioni dormirebbe 50 ms
  alla volta, stessi 200 Hz, nessun campione perso.

Nessuno dei tre prima che la 24b abbia una sessione di campo alle spalle.


---

# 24b.1 — Correzioni dopo il test da tavolo del 14/09

## 1. La misura dell'ODR della 24b era costruita su un mio errore

Il firmware ha scritto `odr_hz_reale=217.39`. Non 200,12. Il firmware aveva
ragione e l'analisi che ha motivato la 24b no.

Gli intervalli fra campioni non sono uniformi: sono **bimodali**, alternati fra
4000 e 5000 µs. Il tick FreeRTOS è a 1 ms e `imu_task` fa `vTaskDelay(1)`, quindi
ogni campione viene quantizzato attorno al periodo vero di ~4590 µs.

| | 12/09 | 14/09 |
|---|---|---|
| intervalli a 5000 µs | 180 | 217 |
| intervalli a 4000 µs | 129 | 155 |
| mediana degli intervalli grezzi | **200,12 Hz** ← il numero sbagliato | 200,24 Hz |
| frequenza media reale | **217,79 Hz** | 217,72 Hz |

Avevo usato la mediana degli intervalli grezzi. **Su una distribuzione bimodale
la mediana non stima la frequenza: sceglie il modo più numeroso.**

Conseguenze della correzione:

- l'errore della costante dichiarata è il **3%** (224 contro 217,8), non il 12%;
- la finestra pre vale **2,057 s** invece di 2,000, la post 1,028 invece di 1,000;
- il `misuraOdrCentiHz` della 24b (mediana di medie locali su 10 intervalli)
  smorzava la bimodalità e arrivava a 217,39 — quasi giusto, ma tarato su una
  premessa sbagliata.

### Il nuovo stimatore

Span del **solo tratto post-trigger**: `(n − trig − 1) / (t_ultimo − t_trigger)`.

1. **È esatto.** Non è una stima, è la definizione. Nessun ordinamento, nessuna
   mediana, nessun parametro da tarare.
2. **È sempre fresco.** I campioni dopo il congelamento vengono scritti in quel
   momento, uno dopo l'altro: non possono contenere residui dell'anello. Un burst
   col buco da 6 secondi (tiro 2 del 14/09) manderebbe a gambe all'aria qualunque
   media calcolata sull'intera finestra.

Verificato su entrambe le sessioni: **217,77 Hz**, compreso il burst bucato.

## 2. Tutti i file della microSD erano datati 1/1/1980

I **nomi** delle cartelle erano corretti: l'RTC funziona. Sbagliato era il
timestamp FAT, e l'ora che si vedeva non era l'ora — era l'**uptime** (00:26,
00:28, 00:31...).

Leggevamo il PCF85063 per costruire le nostre stringhe ISO, ma non impostavamo
mai l'**orologio di sistema POSIX**. FATFS chiama `get_fattime()` → `time(NULL)`
→ un clock mai impostato, che parte da zero; FAT non sa rappresentare date prima
del 1980 e le tronca lì.

`rtc_sync_system_clock()`, chiamata dopo `rtc_init()` (prima di `sd_init()`) e
dopo ogni `rtc_set()` dal sync BLE.

Non è cosmetico: il timestamp del filesystem è un **secondo registro
indipendente** di quando le cose sono successe, e serve esattamente quando il
primo manca — come `SESS_1215` del 12/09, chiusa da uno spegnimento e rimasta
senza `ended_ms`.

Nota sul fuso: `TZ` resta UTC e l'RTC porta l'ora di casa, quindi l'orologio di
sistema porta la stessa ora dell'RTC. FAT memorizza ora locale senza fuso:
qualunque conversione produrrebbe file datati a un'ora diversa da quella nel nome
della cartella — due orologi nella stessa card, la cosa peggiore.

## 3. Soglia minima di freschezza per gli angoli

`SHOT_ANGLE_PRE_MIN_MS = 1000`. Sotto un secondo di campioni freschi, gli angoli
**non si calcolano affatto**.

Il ragionamento non è una percentuale scelta a occhio. La finestra adattiva esiste
perché negli ultimi ~200 ms prima del trigger l'arciere sta già partendo, e una
finestra fissa lì dentro dava errori di 13-40°. La soluzione fu poter scorrere
all'indietro fino a due secondi.

Quando la parte fresca è corta, il problema non è che la ricerca ha meno scelte:
è che **le scelte rimaste stanno tutte nella zona che la ricerca doveva evitare**
— la parte fresca è per costruzione adiacente al trigger. Con 91 campioni (0,42 s,
tiro 2 del 14/09) si cerca la calma dove l'arciere è in movimento, e si trova
sempre qualcosa, perché il minimo di una funzione esiste comunque. Verrebbe fuori
un numero, e sarebbe il numero sbagliato.

Un secondo: metà della finestra di progetto e cinque volte la zona "sta già
partendo". In millisecondi, non in campioni né in frazione di `trigger_idx`, così
resta valida se cambiano ODR o finestre.

**Il prezzo, dichiarato:** un tiro vero scoccato entro un secondo dalla conferma
del riepilogo precedente perde gli angoli. Il burst resta intero — si rinuncia a
interpretarlo, non a registrarlo. In campo, fra un tiro e il successivo si
cammina: scoccare entro un secondo dal tap OK non è un tiro, è la scheda che
viene rimessa a posto.

## 4. L'intestazione di ENERGIA.CSV non mente più

Il file vive per sempre nella radice, apposta. Ma l'intestazione si scriveva solo
alla creazione: quando la 24b ha aggiunto `temp_c`, il file esistente ha
continuato a dichiarare nove colonne mentre le righe nuove ne avevano dieci. Chi
legge mappando per nome trova un disallineamento, e lo trova **in silenzio**.

Al primo accesso dopo l'avvio si confronta la prima riga con quella attesa; se
differisce si annota `# COLONNE CAMBIATE, da qui in poi: ...`. Il file resta
leggibile da chi salta le righe `#`.

## 5. Quello che il test del 14/09 ha confermato

- **`pre_validi=91`** sul tiro 2: il buffer stantio rilevato e dichiarato, con un
  buco di 6,36 s nel burst. Prima sarebbe passato in silenzio. Da 24b.1 quel tiro
  avrà anche gli angoli vuoti invece di due numeri verosimili.
- **`SPEGNIMENTO DA TASTO`** con la tensione, due volte.
- **`assetto_ok=1`** sul primo tiro (alzo −64,5°, oltre il limite di 45°): il
  controllo di plausibilità ha sparato per la prima volta da quando esiste.
- **`temp_c=38.1`** — trentotto gradi dentro il case chiuso, molto sopra l'aria.
  Buona notizia per novembre: una LiPo a 38 °C rende molto meglio che a 5.


---

# 24b.2 — Lo spegnimento da tasto si annullava da solo

## Il sintomo

In ATTESA: pressione lunga sul PWR → compare "SPEGNIMENTO" → dopo pochi secondi
torna la schermata di attesa, come se il tasto fosse stato rilasciato. In modo
BLE, invece, tenendo premuto si spegne regolarmente.

## La causa, e perché il confronto fra i due casi la indica da solo

Nella 24b la schermata veniva **disegnata** e poi il flusso restava in ATTESA.
Il rinfresco periodico dei 3 secondi la cancellava e rimetteva la schermata di
attesa.

A quel punto chiunque molla il tasto — l'interfaccia ha appena detto che lo
spegnimento è stato annullato. E il PMU, che conta **10 secondi di pressione
continua**, non arrivava mai alla soglia.

In modo BLE lo schermo si ridisegna solo al cambio di connessione, quindi la
schermata restava, il dito restava, e il PMU faceva il suo lavoro. **È lo stesso
firmware e lo stesso PMU: cambia solo chi ridisegna.** Il difetto non era nella
soglia dei 10 secondi né nel PMU: era il nostro rinfresco.

C'era anche un secondo pezzo mancante, di interfaccia. La IRQ di pressione lunga
scatta molto **prima** dei 10 secondi, quindi quando la scritta compare mancano
ancora parecchi secondi di dito sul tasto — e lo schermo non lo diceva.

## Il rimedio

**Uno stato vero**, `AppFlow::SPEGNIMENTO`, invece di un disegno: finché è
attivo nessun altro ramo ridisegna niente.

**"TIENI PREMUTO" con una barra di avanzamento.** È una guida al dito, non un
cronometro: la soglia la conta il PMU in hardware e noi non sappiamo a che punto
sia. Serve a dire "manca ancora un po'", che è l'unica informazione che cambia il
comportamento di chi guarda.

**L'annullamento non lascia effetti collaterali.** Se dopo 12 s siamo ancora vivi
il tasto è stato rilasciato, e la sessione — chiusa al momento della IRQ, a
ragione, perché lì non potevamo sapere se ci sarebbe stata un'altra occasione —
viene **ripresa**: stessa cartella, stessa numerazione, `ended_ms` riscritto alla
chiusura vera. Compare "ANNULLATO — la sessione prosegue", così l'arciere sa che
non ha perso niente.

I 12 secondi sono la soglia del PMU più due di margine, per non dichiarare un
annullamento che non c'è stato.
