# ArchBB 1.83 — FASE 13 · Scoring: scarta, salta, e meno tocchi

**Firmware `1.83-F13-scarta` · 28 luglio 2026 · passo 1 di 3**

---

## Perché

Dalle due sedute del 28/07: su **66 catture, 26 non erano tiri** ma l'arco
maneggiato o abbassato. Si separano nettamente:

| | n | \|cant\| | jerk | \|a\|max | \|hold\| |
|---|---|---|---|---|---|
| tiri | 40 | 2,4° | 214 | 24,4 | 3,8° |
| maneggio | 26 | 14,8° | 47 | 16,3 | 13,3° |

E il punteggio manuale non le separava: **dodici disaccordi**, in entrambe le
direzioni — cinque tiri veri senza esito, sette catture di maneggio
punteggiate. Il caso peggiore è `0937 t3`: punteggiato zona 5 a 25 m, con
`hold −68,04°`. Un hold di 68 gradi non è un braccio d'arco: è l'arco che viene
abbassato durante la finestra.

Migliorare il trigger resta utile, ma **il maneggio si aggira prima**: basta
poter dire «questo buttalo».

---

## Cosa cambia

### Tre uscite dallo scoring, non due

```c
enum class EsitoScoring : uint8_t {
  VALUTATO,   // riga CSV + burst, col punteggio
  SALTATO,    // riga CSV + burst, SENZA punteggio
  SCARTATO,   // NULLA sulla microSD
};
```

Sulla schermata ESITO, in basso: **SCARTA** a sinistra (rosso), **SALTA** a
destra. Chi decide se scrivere guarda `esito`, non il vecchio booleano
`valutato` — che da solo non distingue «salta» da «scarta», e sono due cose
opposte.

- **SCARTA** → nessuna riga, nessun burst, lampeggio rosso `SCARTATO`, ritorno
  immediato in attesa
- **SALTA** → salva subito e torna in attesa, **senza riepilogo**: si salta per
  fare in fretta, e chiedere una conferma vanificherebbe il gesto. Le metriche
  biomeccaniche si calcolano e si salvano lo stesso
- **VALUTATO** → percorso normale, riepilogo e scrittura al tap OK

### Il timeout scarta

Prima il timeout a 60 s produceva una riga non valutata sulla card. Ora scarta.

Se lo scoring resta aperto un minuto, quasi sempre il trigger ha preso un
movimento e nessuno sta valutando nulla. Meglio perdere un tiro vero dimenticato
che sporcare la sessione con righe di maneggio.

### Meno tocchi fra un tiro e l'altro

- default distanza **da 18 a 30 m** — centro pratico delle distanze viste sul
  campo (14–48 m il 28/07)
- default elevazione **da «mai misurata» a 0°** — il tiro in piano è il caso
  più frequente e non deve costare un tocco
- **memoria del tiro precedente**: ogni scoring riparte dalla distanza ed
  elevazione dell'ultimo tiro *valutato*. Su un percorso 3D i tiri vicini hanno
  spesso valori uguali o simili: la conferma diventa il caso normale e la
  modifica l'eccezione

> **Nota sulla sentinella `ELEV_NOT_SET`.** Resta nella struct e nel CSV per i
> dati vecchi, ma nel flusso normale non si presenta più: ogni tiro valutato
> esce con un'elevazione, foss'anche zero. È una perdita **voluta** —
> distinguere «in piano» da «non misurata» costava un tocco a ogni tiro.

---

---

# Passo 2 — riepilogo e contatore di sessione

## Il riepilogo, rifatto

- **fascia esito** da 40 a **56 px, font 6**: si legge con l'arco in mano e il
  sole in faccia, che è la condizione reale
- **due semafori**, TENUTA e RILASCIO: quadrato verde / giallo / rosso da 46 px,
  col numero accanto in piccolo. Il colore *è* l'informazione; un quadrato verde
  si legge in un decimo di secondo, «−2,81°» no
- **distanza ed elevazione in font 4**: sono i due numeri che hai appena
  inserito e che vuoi ricontrollare prima di confermare
- zona e ARCS restano, piccoli in alto a destra: sono conferme, non decisioni
- **pulsanti verticali contro i bordi**, 92 × 58 px. Prima erano due barre
  orizzontali sovrapposte al centro: verticali e ai lati si premono col pollice
  senza spostare la presa e senza coprire il contenuto — la mano entra dal
  bordo, non dal mezzo dello schermo

Le soglie dei colori sono quelle del firmware (`HoldClass` / `ReleaseClass`): il
display non ne inventa di proprie, altrimenti il colore e la classe salvata nel
CSV potrebbero dire cose diverse.

Geometria in **cinque costanti** lette sia dal disegno sia dall'hit-test. La
regola §4.1 nasce proprio qui: sulla 1.69 i numeri di posizione duplicati fra i
due hanno prodotto pulsanti che si vedevano in un posto e rispondevano in un
altro.

## Il contatore di sessione

**Il difetto.** `SESS_20260728_1005` dichiarava `shots=39` avendo 51 righe e 51
burst. La sessione era stata chiusa dopo il tiro 39 e poi ripresa, e nessuno ha
riscritto `session.txt` alla fine — l'ultimo tiro ha un timestamp un milione di
millisecondi dopo `ended_ms`. Chi legge il file si fida del contatore e perde
dodici tiri.

**La causa vera** non è la ripresa: è che `session.txt` veniva scritto **solo
alla chiusura**, e questo presuppone che la chiusura avvenga. A batteria scarica
o spegnendo a fine percorso non avviene.

**La cura.** Un unico `writeSessionTxt()`, chiamato da quattro punti:
apertura, ripresa, **ogni tiro salvato**, chiusura. Il file è sempre vero, anche
se il dispositivo si spegne di colpo. Costa ~300 byte accanto ai 20 KB del burst
che si sta già scrivendo.

Prima erano due blocchi quasi identici in `sd_session_start` e
`sd_session_stop`: due copie di un formato divergono, ed era già bastato
aggiungere `mount`/`off_*` per doverli toccare entrambi.

Aggiunto `last_shot_datetime` per le sessioni ancora aperte: dice quando è stato
salvato l'ultimo tiro senza pretendere che la sessione sia finita.

> **Un errore trovato facendo questa modifica.** Nella `sd_session_start`,
> `s_sessionId` e `s_shotCount` venivano assegnati *dopo* il punto in cui ora si
> scrive il file. Scrivendo prima, `session.txt` usciva con
> `ArchBB session 0` e il conteggio della sessione precedente — silenzioso e per
> niente ovvio da rileggere. Lo stato ora si imposta prima della scrittura.

---

# Passo 3 — lo slider

## Il rischio si è rivelato inesistente

Temevo che il CST816 in polling non seguisse il dito. Provato sul dispositivo:
**segue**. Quindi non serve leggere l'INT del touch, e il problema era solo
ergonomico:

> «la zona di sliding è piccola e il dito copre il numero che varia»

## Cosa cambia

**Due schermate separate**, distanza e poi elevazione: una grandezza per volta,
ciascuna con tutto lo schermo. Lo stato `ELEVAZIONE` è nuovo nella macchina.

**Il valore sta in alto**, font 7, dove il dito non arriva mai. Prima era
accanto a uno slider orizzontale e finiva sotto il polpastrello.

**La traccia è verticale e larga 120 px** — metà dello schermo. Prima erano
192 × 32 px orizzontali: si doveva mirare. Il verso è quello naturale, in alto
il valore cresce.

**Il cursore sporge dai lati** della traccia, così resta visibile anche col dito
appoggiato in mezzo.

**Due zone ai bordi** per il `+1` / `−1`: il pollice le trova senza guardare e
non interferiscono con la traccia al centro. `+` a sinistra, `−` a destra come
richiesto — se preferisci l'inverso sono due costanti.

**Il trascinamento agisce mentre il dito è giù**, non al rilascio. Era questo a
far comportare lo slider come due tap.

Il ridisegno durante il trascinamento è **parziale** — solo il numero e il
riempimento della traccia. Ridisegnare l'intero schermo a ogni campione di touch
darebbe uno scorrimento a scatti, cioè la sensazione da eliminare.

## Geometria in un posto solo

Nove costanti `VS_*` lette dal disegno, dal trascinamento e dall'hit-test. E una
sola coppia di conversioni `valToY` / `yToVal`, generiche su min/max: due coppie
separate per distanza ed elevazione sarebbero due occasioni di arrotondare in
modo diverso.

Le due grandezze sono descritte da due `SliderCfg` — titolo, unità, minimo,
massimo, colore, segno — e tutto il resto è codice condiviso.

---

## Da provare sul campo

1. **SCARTA** su una cattura di maneggio: deve lampeggiare rosso e non
   comparire in `shots.csv`.
2. **SALTA** su un tiro vero non valutato: la riga deve esserci con `arcs=0` e
   il burst deve esserci.
3. **Timeout**: lasciare aperto uno scoring un minuto e verificare che non
   scriva nulla.
4. **Memoria**: due tiri di fila alla stessa distanza — il secondo deve
   proporre già il valore giusto.
