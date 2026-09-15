# ArchBB 1.83 — Briefing di congelamento

**27 luglio 2026 · Cesare Pagura**

Fotografia dello stato a fine giornata. Serve a riprendere il filo — anche in
una chat nuova — senza ricostruire nulla a memoria.

---

## 0. Le tre versioni in gioco

| | versione | dove sta |
|---|---|---|
| firmware | **`1.83-F12-gyro`** | `ArchBB_183_Fase12.zip`, progetto PlatformIO completo |
| app companion | **`v0.3`** | `app/ArchBB_183_App_v0_3.html`, file singolo |
| analizzatore | **`2026.07.27-11`** | `ArchBB_Analizzatore.zip`, Streamlit |

La versione dell'analizzatore si legge **nella barra laterale**. Se non
corrisponde, l'estrazione dello zip non è andata a buon fine: cancellare la
cartella e riestrarre per intero.

---

## 1. Il dispositivo

### Configurazione validata

```
mount = 2  (destro)          verificato col DIAG ASSI del 26/07
off_cant_deg = 0.03          taratura di montaggio fatta
off_alzo_deg = 1.74
```

Con questa configurazione la seduta del 27/07 dà `cant +1,10° ± 1,27` e
`alzo −19,10° ± 1,16` su venti tiri: un rollio involontario di un grado e una
mira tenuta verso il basso. È il comportamento atteso.

### Comandi BLE

| codice | funzione |
|---|---|
| `0x01` | ping (notifica STATUS) |
| `0x02` | elenco sessioni |
| `0x03` | reset del config |
| `0x04` | **download file** — payload `[index u16][kind u8]`, kind 0 = shots.csv, 1 = session.txt |
| `0x05` | **taratura montaggio** — "l'arco è a piombo adesso" |
| `0x06` | azzera la taratura |

Il download usa la caratteristica `DATA` (`…0060`) con frame HEADER → CHUNK →
END e CRC32 end-to-end. Un `shots.csv` da 9 tiri sono 719 byte, 5 notify,
100–175 ms. **I burst non passano dal BLE** ed è una scelta: 20 KB a tiro
sarebbero 2–3 s ciascuno, e la card si estrae.

### Ambienti PlatformIO

Sei, da selezionare **esplicitamente** prima dell'upload:
`archbb_183_touchcal` (campo, default) · `_debug` · `_mount_test` · `_raw_diag`
· `_shotrec` · **`_assi`** (DIAG degli assi).

### session.txt si autodescrive

Dal firmware F11 ogni sessione dichiara il proprio montaggio e la propria
taratura:

```
mount=2
mount_lato=destro
off_cant_deg=0.03
off_alzo_deg=1.74
```

Senza queste righe un file letto fra sei mesi non direbbe su quale asse stava
il cant né quanto offset è stato sottratto.

---

## 2. Cosa è stato accertato (e come)

### 2.1 Gli assi erano scambiati — risolto

Il firmware riportava `cant = +16,98°` e `alzo = +0,39°` mentre l'arciere
tirava verso il basso su sagoma a terra senza ruotare il braccio d'arco. I 17°
erano l'**alzo**.

La prova decisiva è venuta dai 44 tiri della 1.69, che hanno **distanze
diverse** — e l'alzo con la distanza deve crescere, il cant no:

| | 14 m | 18 m | 20 m | 30 m | 40 m | ρ con la distanza |
|---|---|---|---|---|---|---|
| `atan2(ay,ax)` | −3,5 | −3,1 | −8,4 | −4,4 | −4,5 | −0,09 (p 0,80) |
| `atan2(az,ax)` | +2,2 | +7,9 | +5,3 | +16,8 | +14,6 | **+0,53** (p 0,09) |

Sulla 1.69 le etichette erano giuste; sulla 1.83 il rapporto era invertito.
Diagnosi: **rotazione di 90°**, non riflessione. Il `MOUNT_0_FLIPZ` curava un
segno, non lo scambio. Corretto con `mount = 2`.

> **Regola confermata**: la caratterizzazione del 22/07 diceva "nessuno scambio
> di assi" ed era corretta *per la posa in cui fu fatta* — al banco. Il
> montaggio sul riser è un'altra cosa. **Si misura con la scheda montata come
> si tira.**

### 2.2 La frequenza reale è 217,7 Hz, non 200

Istogramma dei Δt su un burst (671 intervalli): 4,0 ms × 272 e 5,0 ms × 391 —
il tick da 1 ms di FreeRTOS batte contro il periodo ideale. La **mediana** cade
su 5,0 ms e dichiara 200,0 Hz; **span e media** danno 217,79 Hz.

Un errore dell'8,9 % su ogni frequenza. L'app 1.27/1.29 usava la mediana.

### 2.3 Il filtro dell'IMU chiude la banda dei flettenti

`imu.cpp` configura `LPF_MODE_2` = **5,39 % dell'ODR** (registro CTRL5): ai
217,7 Hz effettivi il taglio cade a ~12 Hz. Energia per banda, finestra 300 ms:

```
banda        1.69 (11 tiri)   1.83 (9 tiri)
  3– 8 Hz         304.7            126.5
  8–15 Hz         452.7            149.7
 15–30 Hz         246.5              2.5
 30–60 Hz           0.0              0.0
```

**Sopra i 30 Hz l'energia è nulla su entrambe le schede**: la banda 15–80 Hz
che la 1.69 chiamava «vibrazione dei flettenti» non è mai esistita.

**Rimane aperto** lo scarto di 100× fra 15 e 30 Hz. Montaggio più rigido sulla
1.69? Attrezzo diverso? Configurazione? La prova che discrimina è un
**colpetto secco sul riser** con le due schede montate, senza l'arciere in
mezzo.

### 2.4 La finestra spettrale giusta è 300 ms

Frequenza di picco sui nove tiri: a 300 ms media 8,87 Hz con **σ 1,31**; a
500 ms σ 2,13; a 1000 ms si sfilaccia. Si sceglie la finestra dove la misura
**si ripete**, non quella con la risoluzione migliore.

### 2.5 La replica del firmware coincide

`archbb_metrics.py` riproduce gli algoritmi di bordo entro la quantizzazione
del CSV. Ci sono volute tre correzioni, tutte istruttive:

1. **Troncamento, non arrotondamento**: `(uint16_t)(0.200f * 224)` in C tronca a
   44 campioni, non 45.
2. **Deviazione standard campionaria** (`n−1`), non quella di `numpy.std()`.
   L'1,2 % di differenza basta a far vincere un'altra finestra calma.
3. **L'ultimo trapezio**: `integrate_gy()` include ogni trapezio il cui estremo
   *sinistro* cade entro i 900 ms.

Risultato: scarti massimi 0,004° sugli angoli e 0,44 m/s³ sul jerk — la sola
quantizzazione.

### 2.6 Il rilascio comincia prima del trigger

Misurato: lo yaw è piatto fino a **−25 ms**, poi ruota di 5,7° in 23 ms mentre
il modulo del giroscopio passa da 20 a 174 dps. La soglia su `||a|−g|` con
`confirm_n` scatta a rotazione già iniziata.

Conseguenza pratica: le finestre di «mira» non devono arrivare fino allo
scocco, o inghiottono il rilascio.

### 2.7 Il filtro complementare non funziona su questa scheda

Pitch nella fase di mira, riferito allo scocco:

```
α = 0,98         media +8,09°   escursione 2,69
α = 0,995        media +2,30°   escursione 0,65
giroscopio puro  media +0,26°   escursione 0,19
```

In mira l'arciere è fermo e il giroscopio lo conferma (±1° su 800 ms), ma
l'accelerometro nello stesso intervallo oscilla di **14°**: è accelerazione
lineare, non assetto. Il complementare la lascia passare e viene trascinato dal
rilascio *prima* del trigger.

La traccia usa ora **integrazione pura** con bias dalla finestra calma.

---

## 3. Le metriche

Struttura distillata dai 42 tiri del 14/07: PCA con **PC1 al 32 %** — non
esiste un indice unico di bravura — e tre gruppi indipendenti (ρ fra loro
−0,24 / +0,25 / −0,04, tutte p > 0,1).

| fase | finestra | metriche |
|---|---|---|
| **1 · MIRA** | pre-scocco | fermezza · scostamento in mira · ξ consistenza |
| **2 · RILASCIO** | 0–40 ms | pulizia rilascio · nettezza scocco · torsione |
| **3 · TENUTA** | 40–900 ms | hold · follow-through |

**`angolo di mira` non è una metrica di fase**: dipende da distanza e bersaglio,
non dall'esecuzione. In una seduta a distanze diverse la sua media non
significa nulla. Al suo posto lo **scostamento in mira** — quanto la mano d'arco
si muove *mentre* mira, sulla finestra calma. Sui 20 tiri del 27/07: 0,04–0,15°.

### Definizioni

- **fermezza** = `100·exp(−rms/5)`, rms del rate giroscopico nella finestra
  calma, bias-corretto
- **scostamento in mira** = rms di `√(Δyaw²+Δpitch²)` dalla media, sulla
  finestra calma
- **ξ consistenza** = DTW con banda di Sakoe-Chiba 10 % su `[trig−25, trig+79]`,
  confrontata coi **soli tiri precedenti**
- **pulizia rilascio** = `max |d(ay)/dt|` nei primi 40 ms (dal CSV)
- **nettezza scocco** = picco `|az|` diviso il tempo di salita, finestra
  `[trig−5, trig+5)`
- **torsione** = `∫−(gz−bias)dt` su `[trig−100ms, trig+15ms]`, solo giroscopio
- **hold** = `∫(gy−bias)dt` per 900 ms, a dt reale
- **follow-through** = `100·exp(−3·rms)` con `ax` normalizzato al picco,
  finestra `[trig+3, trig+52)`

Valori a confronto con la 1.69, per verificare che le costanti siano quelle:

| | 1.83 (27/07) | 1.69 (schermata) |
|---|---|---|
| nettezza scocco | 961–2700 m/s³ | 1054 |
| follow-through | 9–15 | 16 |
| ξ consistenza | 0,76–0,80 | 0,86–0,89 |

---

## 4. Gli strumenti

### App companion (`v0.3`, HTML singolo)

Sei schede: DEVICE · QUADRO · TIRI · ONDA · FFT · TRACCIA. **Nessuna dipendenza
esterna** — niente CDN, funziona senza rete.

Fa: connessione BLE, editor config, sync RTC, elenco sessioni, **download del
CSV**, taratura del montaggio, quadro per tiro con navigazione `‹ ›`, e lettura
della cartella `SESS_…` da microSD per sbloccare onda/FFT/traccia.

**Aggancio automatico**: scarichi il CSV via BLE, poi apri la cartella dalla
card e i burst si attaccano alle righe già presenti tramite `burst_file`.

### Analizzatore (`2026.07.27-11`, Streamlit)

Sette schede: Panoramica · Tiro per tiro · Progressione · Sovrapposizione ·
Spettro e smorzamento · Verifica del dispositivo · Statistica.

Legge tutta la card e confronta le sedute. Nelle due schede con navigazione,
l'intestazione resta ferma e solo il corpo scorre.

Scrive in un solo caso: la **correzione di distanza ed elevazione**, con copia
di sicurezza in `shots.csv.orig`, modifica chirurgica sul testo e `arcs`
ricalcolato.

### Conversione dalla 1.69

`converti_1_69.py` porta le esportazioni piatte nel formato card. Tre insidie
gestite: intestazione ripetuta, `shot_id` non univoco, `sample_idx` che si
avvolge a 65536 (senza il modulo si passa da 44 tiri veri a 30 sbagliati).

### Controlli automatici

- `tools/sintassi/controlla.sh` — 10 sorgenti C++ in `gnu++11` con `-Wall
  -Wextra`, senza toolchain ESP32, in pochi secondi
- `tools/prova_app.js` — carica l'app HTML in un DOM headless con una sessione
  vera e verifica che le schermate si popolino

Entrambi nati da errori reali arrivati in mano all'utente, ed entrambi
verificati reintroducendo il bug che dovevano intercettare.

---

## 5. Aperto

### 5.1 Gli assi del giroscopio — da rivedere con calma

**Il dubbio.** Sui dati veri il movimento ampio sta su `gx` e non su `gy`
(durante il rilascio −4,28° contro −2,53°; sul pre-scocco 5,61° contro 2,65°),
mentre l'arciere osserva che il movimento ampio dovrebbe essere l'alzo.

**Perché non è risolto.** Il DIAG ASSI a pose statiche valida
l'**accelerometro**: una posa ferma ha velocità angolare zero e non dice nulla
sul giroscopio. Cant e alzo vengono dall'accelerometro e sono giusti; tutto ciò
che si **integra** — traccia, hold, torsione — dipende dal giroscopio.

La prova dinamica (tre rotazioni lente, una per asse) è nel firmware F12 ma
**non è stata superata**. Il monitor seriale stampa i tre integrali per
movimento: sarebbe il primo posto da guardare.

**Stato attuale.** La traccia 2D applica uno scambio d'assi dichiarato in
`archbb_dsp.TRACCIA_SCAMBIA_ASSI = True`, basato sull'osservazione dell'arciere
e **non** su una misura. Riguarda solo la traccia: `hold` e `torsione` restano
sugli assi che usa il firmware. Se un giorno si accerta che il giroscopio è
permutato, vanno riviste anche quelle.

### 5.2 Prova sul campo con distanze ed elevazioni

Le sedute finora sono a distanza unica (o con la distanza sbagliata nel
selettore: il 27/07 diciotto tiri su venti risultano a 18 m mentre erano a 6).
Serve una seduta con **distanze ed elevazioni diverse**: sarà la prima in cui
l'angolo di mira varia davvero, e si vedrà se cambia con la distanza come deve.

### 5.3 Minori

- **ZIP nell'app HTML**: spostare una cartella `SESS_…` dalla card a una
  directory Android è scomodo. `DecompressionStream` è nativo nei browser
  recenti.
- **Lo scarto 15–30 Hz** fra 1.69 e 1.83 (vedi §2.3): colpetto sul riser.
- **`ARCHBB_FW_VERSION` è limitata a 15 caratteri** — `BleStatusPacket.fw_version`
  è `char[16]`.

---

## 6. Metodo — le regole che hanno retto

**«I dati comandano.»** Nessun parametro cambiato senza evidenza. Quando i dati
non bastano a decidere, si dice che non bastano invece di scegliere e tacere.

**I segni si validano sui test statici, mai sui gesti.** Confermato
sperimentalmente: le prove sull'impulso di rilascio e sulla rotazione nel
follow-through sono risultate **inconcludenti**, esattamente come previsto.

**§4.1 — mai costanti né comportamenti duplicati.** `mount_apply_ex()` contiene
la geometria e `mount_apply()` è un rinvio; `pacedNotify()` è usata sia
dall'elenco sia dal download; `serie_traccia()` è l'unico punto dove si applica
l'orientamento.

**Un solo formato dentro, adattatori al bordo.** Il convertitore della 1.69 sta
fuori dall'analizzatore, che conosce un formato solo.

**Le sostituzioni automatiche vanno verificate sull'unicità dell'ancora, non
sulla presenza.** Costata quattro errori: `str.replace` senza corrispondenza non
protesta, e un'ancora contenuta nel testo che si inserisce non è rilevabile
contando le occorrenze prima.

**Uno stub deve rispecchiare il tipo vero.** `getValue()` di NimBLE ritorna
`NimBLEAttValue`, il cui `.data()` è `const uint8_t*`: con uno `std::string` il
banco darebbe errori inesistenti sul dispositivo.

**I selettori CSS si leggono, non si ricordano.** Streamlit non fa scorrere
`body` (è `[data-testid="stMain"]`) e il pannello delle schede non ha un
`data-baseweb` (è `role="tabpanel"`).
