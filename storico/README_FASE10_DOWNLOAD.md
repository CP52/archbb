# ArchBB 1.83 — FASE 10 · Download delle sessioni via BLE

**Firmware `1.83-F10-dlcsv` · app `ArchBB_183_App_v0_2.html` · 25 luglio 2026**

Consegna completa. Partenza: `ArchBB_183_Fase7` (`1.83-Fase9-soglia4`).
Nulla di validato sul campo è stato toccato: trigger, gate di postura, scoring,
salvataggio microSD sono identici bit per bit.

---

## 0. Cosa fa questa fase, in tre righe

L'app scarica dal device le **metriche** di una sessione (`shots.csv`, poche
centinaia di byte, meno di un quinto di secondo) e le mostra in un quadro di
seduta. Le **forme d'onda** restano sulla card e si aprono da computer estraendo
la microSD. Non c'è download dei burst via BLE, ed è una scelta, non un rinvio.

---

## 1. La decisione: perché i burst NON passano dal BLE

| | `shots.csv` | `burst_NNNN.bin` |
|---|---|---|
| dimensione | ~80 B a tiro (719 B per 9 tiri, misurato) | 20 176 B **a tiro** |
| notify necessarie | 5 in tutto | ~85 a tiro |
| tempo reale | 100–175 ms | 2–3 s a tiro, 20–30 s a sessione |
| si può recuperare altrove? | no, senza device | **sì: si estrae la card** |

L'ultima riga chiude la questione. Il CSV via BLE serve perché in campo la card
non la vuoi smontare; i burst no, perché a casa hai la card in mano e un lettore
SD trasferisce 180 KB in un battito di ciglia. Spendere venti secondi di radio,
una barra di avanzamento, un annullamento e un protocollo con ritrasmissione per
fare peggio di un lettore da tre euro sarebbe ingegneria per il gusto di farla.

L'infrastruttura però è pronta: il canale `DATA` trasporta byte, non CSV. Il
giorno in cui servisse davvero un burst via radio, si aggiunge un `kind` e si
riusa tutto il resto. La costante `2` è deliberatamente **assente** dall'enum
`BleFileKind`: niente segnaposto che sembrino funzionalità.

---

## 2. Il protocollo

### Comando (caratteristica `CMD`, `…0030`)

```
[0x04][index_lo][index_hi][kind]
```

`index` = lo stesso indice 0-based dei `BleSessionRecord` dell'elenco.
`kind`: `0` = `shots.csv`, `1` = `session.txt`.

### Risposta (caratteristica `DATA`, `…0060`, NOTIFY)

| frame | byte | contenuto |
|---|---|---|
| `0x01` HEADER | 8 | `[tipo][kind][index u16][size u32]` |
| `0x02` CHUNK | ≤ MTU−3 | `[tipo][seq u16][dati…]` |
| `0x03` END | 7 | `[tipo][chunks u16][crc32 u32]` |
| `0xFF` ERROR | 2 | `[tipo][codice]` |

Codici di errore: `1` no card · `2` sessione inesistente · `3` file assente ·
`4` kind non gestito · `5` errore di lettura. Cinque codici distinti e non un
"errore generico", perché un'app che riceve *«errore»* non sa se deve dire
all'arciere di infilare la card, di aggiornare l'elenco o di riprovare.

### Tre scelte di progetto, con il loro perché

**La dimensione del chunk non è una costante.** Viene dall'MTU davvero
negoziato (`getPeerMTU` sull'handle di connessione letto in `onConnect`): un
iPhone e un Android negoziano numeri diversi. Un valore fisso o spreca banda o
si fa troncare le notify. Con MTU 247 il payload utile è 241 B.

**Niente ritrasmissione per blocco.** Il CSV reale è 719 byte: 3 chunk. Un
protocollo con NAK per blocco costerebbe più righe di codice di quanti byte
trasferisce. Il CRC32 nel frame END dice se il file è integro; se non torna,
l'app rimanda il comando e ripaga 175 ms. Quando (e **se**) servirà il burst da
20 KB, quella scelta va rimessa in discussione con i dati di quel caso d'uso.

**Il CRC sta nell'END, non nell'HEADER.** Metterlo in testa obbligherebbe a
leggere il file due volte dalla card. Nell'END si accumula mentre i byte
scorrono: una lettura sola.

### La disciplina anti-crash, ora in un punto solo

`pacedNotify()` è usata **sia** dall'elenco sessioni **sia** dal download:

1. attesa del completamento della notify precedente (semaforo su `onStatus`) —
   questo, non il ritardo, è il vero freno: il ritardo è una stima, il semaforo
   è un fatto;
2. pavimento del ritardo ancorato al **connection interval reale** + 5 ms;
3. backoff 2,5× su errore, che **resta** per il resto del trasferimento.

Sulla 1.69 il `rst:0xc` nacque da due percorsi di invio con discipline diverse.
Qui il percorso è uno: la §4.1 applicata al comportamento, non solo alle
costanti.

---

## 3. Modifiche al codice, file per file

### `src/config.h`
Versione → `1.83-F10-dlcsv`, con un vincolo scritto sopra la `#define`:
**massimo 15 caratteri**. `BleStatusPacket.fw_version` è `char[16]`, quindi
`1.83-Fase9-soglia4` (18) arrivava all'app troncato a `1.83-Fase9-sogl`. Non un
bug, ma un'informazione persa proprio quando serve: capire quale build ha
prodotto un file.

### `src/sd_storage.h/.cpp`
- `SD_MAX_ENUM_SESSIONS = 64` — era un letterale dentro `ble_service.cpp`. Ora
  è una costante condivisa fra l'elenco e il risolutore indice→nome. Un indice
  che significa due cose diverse in due punti è il bug più costoso di questo
  progetto.
- `sd_session_name_by_index()` — usa la **stessa** `sd_enumerate_sessions`
  dell'elenco, quindi stesso ordinamento e stesso significato di indice per
  costruzione.
- `sd_reader_open / read / close` — lettura sequenziale.

**Cambio di idea documentato.** In fase di disegno avevo proposto un'API
*stateless* (`size` + `read(offset)`), più difensiva in astratto. Letto il
codice, il trasferimento è una singola funzione sincrona dentro il `ble_task`:
l'handle non attraversa mai un confine di callback. Lo stateless avrebbe
costretto a rienumerare `/ARCHBB/` — con `countCsvRows` che rilegge ogni CSV —
a **ogni blocco**. Il reader con stato è la scelta giusta qui, e il perché sta
nell'header accanto alla dichiarazione.

### `src/ble_service.h/.cpp`
Caratteristica `DATA`, comando `0x04`, `do_send_file()`, `crc32_update()`
bit-a-bit (niente tabella: 1 KB per risparmiare microsecondi su 719 byte),
`chunk_payload_size()`, `pacedNotify()` estratta, `PacedNotifyCallbacks`
condiviso fra `SESSION` e `DATA`.

Un semaforo solo per due canali è corretto, non una semplificazione: entrambi i
flussi girano nel `ble_task`, che è uno e processa un comando alla volta. Due
semafori darebbero l'illusione di un parallelismo che non esiste.

### `app/ArchBB_183_App_v0_2.html`
Nuova app, file singolo, **nessuna dipendenza esterna**. La 1.29 caricava
Chart.js da CDN: in campo la rete può mancare, e uno `<script src>` che fallisce
non degrada l'app, la rompe. I grafici sono canvas 2D scritti a mano.

---

## 4. Quello che i dati hanno detto (e che ha cambiato il codice)

Tre correzioni nate dai nove burst del 25/07, non da un'idea a priori.

### 4.1 La mediana sbaglia l'ODR dell'8,9 %

Istogramma dei 671 intervalli di `burst_0001.bin`:

```
Δt = 4,0 ms → 272 volte
Δt = 5,0 ms → 391 volte
```

Il tick da 1 ms di FreeRTOS batte contro il periodo ideale e produce due valori
alternati. La **mediana** cade su 5,0 ms e dichiara **200,0 Hz**; media e span
danno **217,79 Hz**. La 1.29 usa la mediana (`burstSampleRate`): portarla di
peso sulla 1.83 avrebbe spostato ogni frequenza del 9 %.

L'app usa lo **span** (`(n−1)/(ts_ultimo − ts_primo)`), previa verifica che la
`seq` sia continua — se mancasse un campione lo span mentirebbe. Sui nove burst:
671 salti su 671 pari a 1, nessun campione perso, fs fra 217,70 e 217,79 Hz.

### 4.2 Il filtro dell'IMU taglia a ~12 Hz: la banda «flettenti» non esiste

Energia spettrale per banda, finestra 300 ms post-scocco, nove tiri:

```
  3– 8 Hz   61.7  61.9  74.1  41.5 238.4  29.0  72.5  29.3  30.8
  8–15 Hz   84.1  69.0  76.5  90.7 262.6  22.5 155.7  21.4  29.8
 15–30 Hz    4.5   1.5   1.3   1.8   3.8   1.0   0.7   1.0   0.9
 30–60 Hz    0.0   0.0   0.0   0.0   0.0   0.0   0.0   0.0   0.0
```

Zero esatto sopra i 30 Hz. La causa è in `imu.cpp` riga 112:

```cpp
s_imu->configAccelerometer(acc_range, acc_odr, SensorQMI8658::LPF_MODE_2);
```

`LPF_MODE_2` = **5,39 % dell'ODR** (registro CTRL5, bit `aLPF_MODE = 10`).
A 250 Hz nominali sono 13,5 Hz; ai 217,7 Hz effettivi, ~11,7 Hz.

E l'ODR effettivo è 217,7 e non 250 per una nota del datasheet: **con
accelerometro e giroscopio entrambi attivi tutte le frequenze si sincronizzano
sulla frequenza naturale del giroscopio** (224,2 Hz nominali).

Conseguenza operativa: la banda 15–80 Hz che la 1.69 chiamava «vibrazione dei
flettenti» **su questa configurazione non è misurabile**. Il tab FFT lo dichiara
e ombreggia la zona oltre il taglio. Quello che resta — 5–15 Hz, oscillazione
del sistema arciere-arco dopo il rilascio — è comunque informativo: l'ampiezza
varia di un fattore 4 fra un tiro e l'altro.

Nel `platformio.ini`, sull'env `shotrec`, era già scritto *«doppia banda
passante del filtro interno del QMI8658»*. Il sospetto era giusto; adesso c'è
il numero. **Vedi §7 per come aprire la banda, se un giorno servisse.**

### 4.3 La finestra FFT giusta è 300 ms, e si vede dalla σ

Frequenza di picco misurata sui nove tiri, per larghezza di finestra:

| finestra | picchi [Hz] | media | σ |
|---|---|---|---|
| 300 ms | 10,2 · 9,4 · 8,0 · 8,7 · 8,2 · 11,5 · 8,8 · 7,8 · 7,3 | 8,87 | **1,31** |
| 500 ms | 4,0 · 7,4 · 7,7 · 9,6 · 8,2 · 4,6 · 10,0 · 9,2 · 6,2 | 7,43 | 2,13 |
| 1000 ms | si sfilaccia fino al limite dei 2 Hz | — | — |

La finestra corta è quella in cui la misura **si ripete**. Non è una preferenza
estetica: è dove σ è minima. Default 300 ms.

Due guardie nate dalla stessa prova:

- **soglia bassa `fMin = 2/T`** — sotto due cicli di finestra si legge una
  deriva, non un'oscillazione. Senza, il picco cadeva a 0,1 Hz su 7 tiri su 9:
  la coda del follow-through letta come se fosse una frequenza.
- **picco al bordo** — con finestra 150 ms tutti e nove i tiri davano 13,3 Hz,
  cioè esattamente `fMin`. Un numero identico su nove tiri diversi non è una
  misura ripetibile, è un artefatto. L'app ora colora il valore in rosso e lo
  dice. La finestra da 150 ms è stata tolta del tutto.

---

## 5. L'app v0.2

Sei schede: **DEVICE · QUADRO · TIRI · ONDA · FFT · TRACCIA**. Le ultime tre
si sbloccano solo con i burst; senza, mostrano cosa fare invece di un grafico
vuoto.

### L'aggancio fra le due sorgenti
Scarichi il CSV via BLE in campo; a casa apri la cartella `SESS_…` e i burst si
attaccano alle righe già presenti tramite la colonna **`burst_file`** — la
chiave la scrive il firmware stesso. Nessuna riga duplicata, nessun doppio
caricamento. Badge di provenienza per sessione (`BLE` / `SD`).

**Come viene riconosciuta la sessione (correzione del 25/07, sera).** La prima
versione derivava il nome da `ended_datetime` quando i file arrivavano senza il
nome della cartella. Sbagliato: la cartella porta l'ora di **inizio**
(`SESS_20260725_1648`), `session.txt` l'ora di **fine** (`16:55:23`). Sette
minuti di scarto, e invece di fondersi con la sessione già scaricata via BLE se
ne creava una **doppia** — con l'aggancio dei burst fallito in silenzio.

Il nome è ora solo un'etichetta: l'identità sta nel **contenuto**. Chiave, in
ordine di robustezza:

1. `ended_ms` — millisecondi da boot alla chiusura, praticamente irripetibile;
2. `ended_datetime`;
3. `ts_ms` del primo tiro — funziona anche senza `session.txt`.

Verificato sul caso reale: caricamento BLE (9 tiri, 0 onde) seguito da
caricamento dei file senza nome cartella → **1 sola sessione**, 9 tiri, 9 onde
agganciate, sorgente `ble+sd`, metriche intatte. Se nessuna corrispondenza
esiste, il nome derivato viene marcato con `~` finale e il messaggio lo dichiara:
un'etichetta stimata non deve sembrare un dato.

### Il quadrante del cant
L'elemento portante della scheda QUADRO. Non mostra il cant assoluto — nel
barebow è una **scelta**, non un errore — ma la sua **dispersione** attorno alla
media, con scala zoomata su 3σ (a tutto giro, nove tacche a 0,7° di distanza
sarebbero un pastrocchio unico).

Sulla sessione del 25/07: media **16,98°**, **σ = 0,72°**.

Con la caduta di mira inserita, converte σ in centimetri sul bersaglio:
`Δlat ≈ caduta × sin(Δcant)`. La spiegazione nell'app è esplicita sul punto che
conta davvero: **se sei point-on la caduta è zero e il cant non ti costa nulla**.
Più cresce lo scarto fra punto mirato e punto d'impatto — cioè più vai lontano —
più il cant pesa. È la ragione geometrica per cui in letteratura la perdita da
cant cresce circa col quadrato della distanza.

Riferimenti: Lau, Chung, Park, Chauhan, *A device for measuring the variable
lateral bow angle and its impact on score loss*, Proc. IMechE Part P, 2019
(dove si osserva che 0,1° di variazione da tiro a tiro può incidere in modo
significativo sul punteggio, e si costruisce una suite di sensori a 0,05° /
200 Hz per misurarla); Park, *The impact of lateral bow angle variation on an
archer's score*, 2023. **σ = 0,72° è sette volte la soglia citata**, misurato
con hardware della stessa classe.

### Distanza di Mahalanobis
Nella tabella TIRI, colonna `D`: scarto del tiro dal centroide di seduta su
(cant, alzo, hold, jerk), standardizzato. Versione **diagonale**, non a
covarianza piena: con 9 tiri e 4 variabili la matrice piena sarebbe singolare o
quasi, e stimare 10 covarianze da 9 punti produce numeri che sembrano precisi e
non lo sono. Sui dati del 25/07 isola i tiri **4, 7 e 9** (2,68–2,78).

---

## 6. Collaudo — nell'ordine

**Già verificato a banco, prima del flash:**
- CRC32 in tre implementazioni indipendenti (C++ compilato, `zlib`, JS
  dell'app) → `C0534FC8` su `shots.csv` reale. Identiche.
- Simulazione end-to-end firmware→app: 719 B → 5 notify (HEADER + 3 CHUNK da
  241 B + END), riassemblaggio corretto, CRC verificato, CSV riparsato in 9
  tiri. Tempo stimato **100 ms** a interval 15 ms, **175 ms** a 30 ms.
- Parser CSV: 9 tiri, sentinella preservata (`elev_deg` resta `null`, non `0`).
- Parser burst: 9 su 9, `n=672`, `trig=448`, fs 217,7 Hz.

**Da fare sul dispositivo, in quest'ordine:**

1. `pio run -e archbb_183_touchcal -t upload` — **selezionare l'env
   esplicitamente**.
2. Entrare in modo BLE (PWRKEY), connettersi con l'app, verificare che la
   riga firmware dica `1.83-F10-dlcsv` **per intero** (era la prova del
   troncamento a 15 caratteri).
3. «Aggiorna elenco» → deve funzionare come prima. Se qui si rompe qualcosa, è
   la refattorizzazione di `pacedNotify`, non il download.
4. Scaricare una sessione. Attesa: barra piena in meno di mezzo secondo, toast
   con byte e numero di blocchi. Il numero di blocchi è la prova che l'MTU è
   stato negoziato: 3 blocchi ⇒ MTU 247; molti di più ⇒ MTU 23, e allora c'è da
   guardare la negoziazione.
5. Ripetere il download **tre volte di fila** senza disconnettere. È il test che
   conta per la back-pressure: se il pool mbuf si sporca, si sporca alla
   ripetizione.
6. Estrarre la card, aprire la stessa cartella nell'app e verificare che i burst
   si aggancino alle righe già scaricate **senza creare righe nuove**.

---

## 7. Aperto, e deliberatamente non chiuso qui

**Verso dello yaw nella traccia.** `MOUNT_0_FLIPZ` è una riflessione (det = −1):
ribalta la regola della mano destra, quindi il segno `−1` validato a banco sulla
1.69 potrebbe essere invertito sulla 1.83. Nell'app c'è un pulsante per
commutarlo, ma è una toppa dichiarata. Si risolve in un modo solo: **test
statico a banco con rotazione nota**, mai con un gesto ambiguo.

**Aprire la banda spettrale, se servisse.** Per arrivare ai 15–80 Hz servirebbe
`LPF_MODE_3` (13,37 % dell'ODR) o l'`aLPF` disabilitato, con ODR più alto. Ma:
(a) cambierebbe la risposta del sensore su cui il trigger a soglia 4 è stato
tarato sul campo; (b) non si tocca un parametro che non è rotto. Se lo si vuole
provare, va fatto in un **env dedicato** (`archbb_183_spectro`, sul modello di
`shotrec`), confrontando gli spettri dello stesso gesto nelle due
configurazioni. Prima la prova, poi eventualmente il campo.

**Cancellazione delle sessioni da app.** Non implementata. Un comando che
cancella dati dalla card merita la sua fase, con conferma sul device e non solo
nell'app.
