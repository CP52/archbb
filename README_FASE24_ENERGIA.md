# FASE 24a — Autonomia e leggibilità del segno

Cesare Pagura · Padova/Noale IT · 12 settembre 2026
Origine: sessione di campo del 12/09, percorso tecnico, due ore abbondanti.

---

## 1. I due problemi, e quale dei due era diagnosticato male

### 1.1 Il segno "−" che non compariva

**Ipotesi di partenza:** problema di font.
**Verdetto: falsa.** Le tabelle di larghezza dei font di TFT_eSPI dicono altro.

| font | carattere 45 (`-`) | larghezza | glifo |
|------|--------------------|-----------|-------|
| 6 (`Font64rle.c`) | presente | 17 | barra piena 10×4 px |
| 7 (`Font7srle.c`) | presente | 32 (come una cifra) | segmento centrale |
| 8 (`Font72rle.c`) | larghezza 29 | — | da verificare a vista |

Il **meno c'è**. Quello che manca in tutti e tre è il **più** — ed è vero da
sempre, tant'è che `controlla.sh` lo dice già nella sua sezione sui font.

La causa reale era in `display.cpp`, dentro `displayAttesaTiroIMU_v5`:

```cpp
// Valore ASSOLUTO: il font 6 non ha il '+'. La direzione sta nell'etichetta.
snprintf(b, sizeof(b), "%d", (int)(elevDeg < 0 ? -elevDeg : elevDeg));
```

Il valore assoluto era **una nostra decisione**, con la direzione delegata a
`"gradi GIU"` in font 1 sotto il numero. In campo l'occhio prende il numero
grande e se ne va: quella didascalia non si legge con l'arco in mano e la luce
di taglio.

Vale la pena fermarsi un secondo su *come* questo errore è sopravvissuto. Non è
sopravvissuto perché nessuno guardava: è sopravvissuto perché era **documentato
in modo convincente**. Il commento dava una ragione tecnica plausibile (vera per
il `+`, falsa per il `−`) e chiunque lo rileggesse annuiva e passava oltre. È la
classe di difetto più costosa del progetto: quella che si difende da sola.

### 1.2 L'autonomia

Il colpevole principale era in chiaro, in `display.cpp`:

```cpp
void displayBacklight(bool on) {
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, on ? HIGH : LOW);
}
```

Retroilluminazione **al 100% dal boot allo spegnimento**, anche mentre la scheda
penzola dal riser nella custodia fra due piazzole.

---

## 2. Bilancio energetico stimato (DA MISURARE)

| voce | stima | note |
|---|---|---|
| retroilluminazione 100% | 40–60 mA | il candidato numero uno |
| ESP32-S3 @240 MHz, radio spenta | 35–45 mA | |
| logica pannello ST7789 | ~5 mA | |
| IMU QMI8658 a+g 250 Hz | ~1,5 mA | non si tocca: è la misura |
| microSD a riposo | ~0,3 mA | a raffiche in scrittura |
| LED di carica del PMU | 2–5 mA | acceso per nessuno, in campo |
| **totale** | **~100–130 mA** | |

Su una cella da 400 mAh: 3–4 ore **teoriche**, che diventano due reali per il
sag di tensione sotto carico. Coerente con quello che è successo.

**Obiettivo di progetto: sei ore.** Non "più di adesso": un campagna a 24
piazzole dura 4–5 ore, e l'autonomia deve avere margine sopra la durata della
gara, non sotto.

---

## 3. Che cosa fa la 24a

### 3.1 Retroilluminazione PWM (`display.cpp`)

- LEDC a **20 kHz, 10 bit** su GPIO40.
- **Curva gamma 2.2** fra percentuale richiesta e duty.

La frequenza non è arbitraria. La IEEE 1789-2015 colloca il "nessun effetto
osservabile" dello sfarfallio sopra circa **1,25 kHz**, per qualunque profondità
di modulazione. A 200 Hz — valore tipico di metà degli esempi in rete — uno
schermo che si muove nel campo visivo mentre si incocca produce un effetto
stroboscopico proprio sul gesto che stiamo misurando. 20 kHz è anche sopra la
banda udibile: niente fischio dall'induttore.

Il gamma è il regalo nascosto. La sensazione di luminosità cresce come una
potenza della luce emessa (legge di Stevens): **chiedendo "60" si consuma un
terzo invece che due terzi, e l'occhio non se ne accorge.** Non è un compromesso
di leggibilità, è luce che stavamo buttando.

**Trappola del core 3.x:** da Arduino-ESP32 3.0 `ledcSetup()`/`ledcAttachPin()`
non esistono più; si usa `ledcAttach(pin, freq, bit)` e il duty si scrive **sul
pin**. Il codice tiene entrambi i rami dietro una guardia di versione, e
`controlla.sh` ora compila `display.cpp` **due volte**, una per ramo: un ramo mai
controllato è un ramo che rompe la build il giorno del campo.

### 3.2 Macchina a stati (`power_mgr.cpp`, nuovo)

```
ATTIVO ──40 s──▶ PENOMBRA ──60 s──▶ SPENTO
   ▲                 │                 │
   └──── tocco / tasto / scocco ───────┘
```

- **PENOMBRA non serve a risparmiare: è il preavviso.** Uno schermo che si
  spegne di colpo sembra rotto. Vedere la luce calare è l'unico modo che
  l'arciere ha di sapere cosa sta per succedere senza impararlo a memoria.
  La rampa dura ~90 ms: si vede, non sfarfalla.
- **SPENTO** = retro a zero + pannello in sleep (ST7789 `0x10`) + CPU bassa
  (quando il flag sarà attivo, § 3.5).

**Tre profili**, perché non tutte le schermate sono uguali:

| profilo | attenua | spegne | dove |
|---|---|---|---|
| `ATTESA` | 40 s | 100 s | è dove si passano le ore |
| `INTERATTIVO` | 90 s | **mai** | pre-tiro, scoring, riepilogo |
| `TRASFERIMENTO` | 40 s | 100 s | modo BLE: comanda il telefono |

`INTERATTIVO` non spegne perché quelle schermate si **guardano mentre si pensa**,
e hanno già un timeout proprio che le chiude: non restano accese per sbaglio.

La corrispondenza flusso → profilo è una **funzione pura** (`profiloPer()` in
`main.cpp`), chiamata a ogni giro, non sei chiamate sparse nei sei punti in cui
`s_flow` cambia. Il settimo punto — quello nuovo fra tre mesi — sarebbe stato
dimenticato.

### 3.3 Il vincolo che non si negozia

> **Lo schermo spento non disarma niente.**

`imu_task` e `trigger_task` non si fermano mai, il buffer circolare continua a
riempirsi, la freccia che parte a pannello nero viene registrata come tutte le
altre. Al `FIRED` si chiama `power_risveglia()` **prima** di
`startScoringForShot()`.

Uno strumento di misura che perde un dato per risparmiare corrente ha fallito il
suo unico compito. Il risveglio è un fatto di **interfaccia**, non di
acquisizione.

### 3.4 Il tocco di risveglio non è un comando

Toccare uno schermo spento lo accende **e basta**. Senza questa regola, ogni
risveglio in ATTESA aprirebbe o chiuderebbe una sessione a caso — o armerebbe il
bersaglio, se il dito cade sulla card. È il difetto peggiore possibile in un
registratore di dati, perché agisce da solo e non si vede.

Dettaglio difensivo: il flag scade da solo dopo **1,5 s**. Se il tocco di
risveglio non produce mai un rilascio pulito (dito fuori da ogni zona attiva,
tocco scartato dal debounce), il flag resterebbe carico e si mangerebbe il primo
tocco buono successivo.

### 3.5 Scalatura della CPU: predisposta, **NON attiva**

`ENERGIA_CPU_SCALING 0` in `config.h`.

240 → 80 MHz vale ~20 mA, ed è il secondo risparmio per ordine di grandezza. Non
lo attivo alla cieca: su S3 con **PSRAM octal** il cambio di frequenza a runtime
tocca la temporizzazione della PSRAM, e lì dentro vive il buffer che `imu_task`
riempie a 224 Hz. **Un burst corrotto falsifica il dato senza dichiararlo**: è il
danno peggiore che questo strumento possa fare.

**Il test che sblocca il flag** (banco, mezz'ora):

1. `env archbb_183_shotrec` con `ENERGIA_CPU_SCALING=1`;
2. si registra a 80 MHz una serie di scuotimenti noti;
3. si confronta il burst con la stessa serie a 240 MHz: se la forma d'onda è la
   stessa e nessun campione è spazzatura, il flag si accende.

**Mai sotto gli 80 MHz.** A 80/160/240 l'APB resta a 80 MHz e SPI e I2C non
cambiano. A 40 l'APB scende con la CPU e cambiano di colpo le frequenze reali di
display, IMU, touch, PMU e RTC.

### 3.6 LED di carica spento (`battery.cpp`)

`setChargingLedMode(XPOWERS_CHG_LED_OFF)`. In campo illumina il terreno. Lo stato
di carica è già nella barra di stato, con la percentuale.

**Nient'altro sul PMU.** In particolare non si toccano né la corrente di carica
né la misura sul pin TS: su una cella al litio quelli sono i due parametri che,
sbagliati, trasformano un risparmio in un incendio.

---

## 4. `/ARCHBB/ENERGIA.CSV` — l'ipotesi da falsificare per prima

Waveshare dichiara che la percentuale dell'AXP2101 è **stimata dalla tensione**,
e che fluttua parecchio sotto carico. Con ~130 mA il sag di una cella può valere
150–250 mV. Il PMU ha una soglia programmabile di spegnimento di sistema
(`getSysPowerDownVoltage` / `setSysPowerDownVoltage`, range 2600–3300 mV).

**Se quella soglia è tarata alta, il PMU stacca con il 20–25% di carica ancora
dentro la cella.** Sarebbero 30 minuti gratis, senza toccare una riga di politica
energetica — e lo spegnimento del 12/09 sarebbe spiegato senza bisogno di nulla
di quanto sopra.

Si legge **prima** di cambiarla. Se risultasse già al minimo, l'ipotesi è
falsificata e si smette di pensarci: è il punto di misurare.

### Formato

```
iso,ms,v_batt,v_sys,pct,carica,bl_pct,cpu_mhz,power
```

più righe di **nota** (prefisso `#`) per gli eventi datati: motivo dell'ultimo
reset, stato all'avvio, censimento dei rail del PMU.

Il file sta nella **radice**, non nella sessione: lo spegnimento del 12/09 ha
interrotto la sessione più volte, e un log che si spezza insieme alla sessione
perde esattamente la giuntura che vogliamo guardare. Cadenza 30 s → 120 righe
l'ora, ~8 KB al giorno.

### Cosa ci si legge

- la **curva di scarica** reale → il consumo medio in mA, se si dichiara la
  capacità della cella;
- il **delta fra `v_batt` e `v_sys`** sotto carico: se il sistema crolla mentre
  la batteria tiene, il collo di bottiglia è il sag;
- **a che tensione è avvenuto lo spegnimento**: se è molto sopra i 3,0 V la
  colpevole è la soglia VOFF, non la cella;
- `esp_reset_reason` datato: spegnimento pulito del PMU o **brownout**? Sono due
  guasti diversi con due rimedi diversi.

### Censimento dei rail

`battery_rail_report()` scrive quali DC/ALDO/BLDO/DLDO sono accesi e a che
tensione. Sulla 1.83 ci sono **due chip audio** (ES8311, ES7210) più un
amplificatore che il firmware di produzione non usa mai — il tang acustico è un
ambiente di banco separato. Se il loro rail è acceso dal boot, stiamo alimentando
tre chip per niente tutto il giorno.

**Sola lettura.** Non si spegne niente alla cieca: prima si guarda il file,
poi — schematico alla mano — si decide. Spegnere un rail a caso su questa scheda
vuol dire, nel migliore dei casi, un touch che non risponde più.

---

## 5. Il segno: tre canali, non uno

`displayElevazione()` in `display.cpp` è **l'unico disegnatore dell'elevazione in
tutto il firmware**. Compone, centrato:

```
[freccia piena] [segno disegnato] [cifre nel font grande]
```

Perché tre canali per un numero di due cifre:

- il **colore** da solo esclude circa un maschio su dodici (deficit rosso-verde,
  ~8% dei maschi di origine europea) e sparisce dietro gli occhiali gialli
  polarizzati che mezzo campo di tiro indossa;
- il **segno** da solo è un rettangolo di pochi pixel: il canale più debole che
  esista, ed è esattamente quello che il 12/09 non si è visto;
- la **freccia** da sola non dice quanto.

Insieme, ognuno copre il buco dell'altro.

### Perché non verde/rosso di default

`ELEV_PALETTE 0` = **ambra (su) / ciano (giù)**. Due ragioni indipendenti:

1. **Visione dei colori.** Ambra e ciano restano separati per protanopi e
   deuteranopi, e hanno luminanze diverse: restano distinguibili persino in
   bianco e nero.
2. **Coerenza interna — e qui è dirimente.** In ArchBB `GOOD` e `BAD` hanno già
   un significato: *centro* e *fuori*. Un angolo di −20° non è un errore, è una
   piazzola in discesa. Colorarlo di rosso insegna all'occhio che rosso vuol dire
   due cose diverse a seconda di dove guarda — ed è esattamente così che un colpo
   d'occhio smette di funzionare.

`#define ELEV_PALETTE 1` torna a verde/rosso se dopo una sessione risultasse
comunque più immediato. L'informazione resta ridondata in ogni caso.

### Un disegnatore solo

Card di ATTESA e slider del pre-tiro mostrano lo **stesso numero a due secondi di
distanza**. Prima erano due disegnatori separati, e `disegnaPiu`/`disegnaMeno`
vivevano private in `scoring.cpp` mentre `display.cpp` ne aveva bisogno: la
premessa esatta del difetto più costoso della 1.69 (§4.1). Ora le primitive
stanno in `display.cpp`, pubbliche, e in `scoring.cpp` restano due alias locali
che ne conservano il nome breve.

---

## 6. File toccati

| file | cosa |
|---|---|
| `src/power_mgr.h` / `.cpp` | **nuovi** — macchina a stati, profili, risveglio |
| `src/display.h` / `.cpp` | PWM backlight, sleep pannello, primitive del segno, `displayElevazione`, card di ATTESA |
| `src/scoring.cpp` | alias alle primitive condivise, slider bipolare via `displayElevazione` |
| `src/battery.h` / `.cpp` | LED di carica, VSYS, soglia VOFF, censimento rail |
| `src/sd_storage.h` / `.cpp` | `sd_energia_log()`, `sd_energia_nota()` |
| `src/main.cpp` | `power_init`, `power_tick`, profilo dal flusso, risveglio su FIRED, tocco di risveglio consumato, refresh saltato a pannello spento, note d'avvio |
| `src/config.h` | sezione parametri energia + `ELEV_PALETTE` |
| `tools/sintassi/controlla.sh` | `power_mgr.cpp` nel banco; `display.cpp` anche col ramo core 3.x |

**Banco di sintassi: tutto verde**, compresi i due rami LEDC.

### Perché i parametri sono costanti di compilazione e non NVS

Mettere subito in NVS dei valori che non sappiamo ancora se sono giusti
significherebbe un bump di `CONFIG_VERSION` per ogni ripensamento. Prima si
misura in campo, poi si promuovono i valori vincenti a parametro persistente.
Stessa disciplina del bersaglio armato.

---

## 7. Cosa serve prima del prossimo passo

1. **La capacità della cella in mAh.** Senza, il bilancio energetico è un
   esercizio di stile: la curva di scarica non si converte in consumo medio.
2. **Una sessione con `/ENERGIA.CSV`**, anche corta, anche al banco. Due ore con
   la retro al 60% e i tempi di default dicono quasi tutto.
3. **`getSysPowerDownVoltage()` al primo avvio** — è nella nota `AVVIO` del log.

## 8. Sul tavolo per la 24b/24c

- **24b** — schermata ENERGIA nel menu d'avvio: regolazione del livello base al
  volo (controluce), lettura dei rail a schermo, eventuale spegnimento dei rail
  audio *dopo* averli visti nel log.
- **24c** — **light sleep** dell'S3 fra le piazzole (~1–2 mA contro i ~25 di
  adesso a schermo spento), a task sospesi, con risveglio sull'INT del touch.
  È una modalità **dichiarata dall'arciere**, non automatica: sospendere
  `imu_task` significa non poter registrare, e questa è una decisione che prende
  una persona, non un timer.
- **24c** — scalatura CPU, dopo il test al banco del § 3.5.

Nessuna delle tre prima che la 24a abbia due ore di campo alle spalle.

---

# 24a.1 — CORREZIONE DOPO IL PRIMO CAMPO (13/09)

## Sintomi

1. Nessuna attenuazione a 40 s.
2. A 100 s schermo nero **ma con un leggero chiarore**, non spento.

## Causa: due proprietari sullo stesso piedino

`TFT_eSPI::init()` finisce così (TFT_eSPI.cpp, ~riga 786):

```cpp
#if defined (TFT_BL) && defined (TFT_BACKLIGHT_ON)
  if (TFT_BL >= 0) {
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
  }
#endif
```

E la nostra `displayInit()` faceva, in quest'ordine:

1. `displayBacklight(true)` → `ledcAttach(40, ...)`, duty 333/1023
2. `tft.init()` → **`pinMode` + `digitalWrite(40, HIGH)`**

Il `digitalWrite` riporta la matrice GPIO al modo semplice e **stacca il canale
LEDC**. Da quel momento il pin è inchiodato a HIGH e ogni `ledcWrite()`
successiva scrive nel vuoto. `power_init()` non se ne accorge perché
`s_blReady` era già `true`: niente riaggancio, solo una scrittura inutile.

Entrambi i sintomi vengono da lì, e la coppia è diagnostica:

- **niente attenuazione** — la macchina a stati funzionava benissimo, entrava in
  PENOMBRA a 40 s e portava il target a 25%; semplicemente il duty non arrivava
  al LED;
- **nero con chiarore** — a 100 s `displayPanelSleep(true)` funzionava (è SPI,
  non è stata rubata a nessuno) e il pannello smetteva di pilotare le colonne,
  ma **la retro restava accesa al 100% dietro un LCD spento**. Il chiarore è la
  retroilluminazione che filtra attraverso il vetro non pilotato.

Se avessi visto solo uno dei due sintomi la diagnosi sarebbe stata ambigua.
Insieme puntano a una cosa sola.

## Rimedio

**Il piedino ha un proprietario solo** (§4.1 applicata a un pin). In
`platformio.ini` spariscono `-D TFT_BL=40` e `-D TFT_BACKLIGHT_ON=HIGH`, e al
loro posto c'è `-D ARCHBB_BL_PIN=40`. Senza `TFT_BL` definito, quel blocco della
libreria non viene nemmeno compilato: TFT_eSPI non sa che il backlight esiste.

**Cintura e bretelle:** in `displayInit()`, subito dopo `tft.init()`, si forza
`s_blReady = false` e si riscrive il livello corrente, cioè si riaggancia il
LEDC. Se il pin non è stato rubato non succede niente; se un domani qualcuno
rimettesse `-D TFT_BL` "per simmetria con gli altri pin del display", il difetto
non tornerebbe.

Il commento in `platformio.ini` spiega per esteso perché quelle due righe non
devono tornare. È esattamente il tipo di commento che sarebbe servito sulla card
di ATTESA, e che invece lì diceva la cosa sbagliata con sicurezza.

## File toccati

| file | cosa |
|---|---|
| `platformio.ini` | `TFT_BL`/`TFT_BACKLIGHT_ON` → `ARCHBB_BL_PIN`, con la spiegazione |
| `src/display.cpp` | uso di `ARCHBB_BL_PIN`; riaggancio del LEDC dopo `tft.init()` |
| `tools/sintassi/TFT_eSPI.h` | stub allineato |

Banco di sintassi verde, entrambi i rami LEDC.

## Cosa deve succedere adesso

- **40 s**: la luce cala in ~90 ms fino a un livello chiaramente più basso
  (25% percepito ≈ 4,7% di duty: in interni è evidente, all'aperto quasi buio).
- **100 s**: buio vero, niente chiarore.
- **tocco**: luce piena immediata, e quel tocco non fa altro.

Se a 100 s restasse ancora un filo di luce, il sospetto successivo è
`ledcWrite(pin, 0)`: in quel caso si stacca il LEDC e si porta il pin a LOW con
`digitalWrite`. Ma prima si guarda, perché con duty 0 il LEDC su S3 dovrebbe
tenere l'uscita bassa e non c'è motivo di complicare senza prova.
