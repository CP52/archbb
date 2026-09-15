# ArchBB 1.83 — FASE 20b · correzioni dal primo campo

**15 agosto 2026** · `ArchBB_183_Fase20b` · fw `1.83-F20b-latch`
CSV **v4** · config NVS **v7**

Nasce dalle due sedute di giardino `SESS_20260815_1116` e `_1123`, 12 tiri a
5–6 m con bersaglio a −8°. Primo dato di campo per F19 e F20.

---

## 0. Cosa ha detto il campo

**Funziona** — Fase 19: `12/12` impatti registrati, `0/12` con `arcs=0`
(erano 16/51, il 31%, nella seduta di luglio). Il riordino del flusso ha
eliminato il problema che lo aveva motivato.

**Non funziona** — Fase 20: `2/12` tempi di mira.

**Silenziosamente rotto** — `mount=0 frontale` e offset azzerati, senza che
nessuno avesse toccato niente.

---

## 1. La taratura distrutta da un bump di versione

### Diagnosi

`session.txt` del 15/08 contro quella del 28/07:

```
                     luglio      15 agosto
mount                2 destro    0 frontale
off_cant_deg         0,03        0,00
off_alzo_deg         1,74        0,00
```

Il montaggio fisico non era cambiato. Era stato il bump `CONFIG_VERSION` 5 → 6
della **Fase 19** — introdotto per i due raggi del cerchio d'impatto — a
riportare l'intera struct ai default, e il default di `mount_orientation` è 0.

### La prova sui dati

Riapplicando a posteriori la trasformazione mancante (`cay = az`, `caz = −ay`):

```
                come registrato        con la trasformazione
                cant     alzo          cant     alzo
1116/0003       +7,38   −0,37         −0,37    −7,38
1116/0005       +7,06   −0,87         −0,87    −7,06
1116/0007       +7,83   +1,66         +1,66    −7,83
1123/0001       +7,06   +0,54         +0,54    −7,06
1123/0004       +7,97   −3,62         −3,62    −7,97
```

Bersaglio a **−8°**. Con la trasformazione rimessa l'alzo dà −7,0/−8,0 su dieci
tiri su dodici. Il «cant di 8° costante» del CSV **era l'elevazione del
bersaglio sull'asse sbagliato**.

Di rimbalzo è la conferma indipendente più pulita finora delle convenzioni di
segno e della tabella di `mount.h`: nessuno ha detto al sistema che il bersaglio
era a −8°, e la trasformazione lo fa uscire da sola.

### Il difetto vero, e la correzione

Il difetto non era il reset in sé, era che trattava allo stesso modo due
categorie:

| | esempi | se sbagliato |
|---|---|---|
| **parametri** | soglia trigger, finestre, raggi | ricostruibili in dieci secondi, e **si nota subito** |
| **taratura** | mount, offset angoli | misurati al banco, e **non si nota**: il sistema continua a produrre numeri plausibili sull'asse sbagliato |

Il README della Fase 19 diceva *«i config v5 ripartono dai default, come da
disciplina»*. La disciplina era giusta per i parametri e sbagliata per la
taratura.

**Correzione**: i tre campi di taratura vengono scritti anche come **chiavi NVS
scalari fuori dal BLOB**. Chiavi scalari non dipendono dal layout della struct,
quindi sopravvivono a qualunque bump futuro — non solo a questo. Alla migrazione
si riparte dai default e poi si rimette la taratura salvata.

Non è un backup da ricordarsi di fare: `config_save()` le scrive a ogni
salvataggio, così non esiste uno stato in cui il blob è aggiornato e loro no.
`isKey()` distingue «mai salvata» da «salvata a zero» — lo stesso problema dello
zero-come-sentinella, in un altro vestito.

**Da fare a mano una volta**: rimettere mount=2 e i due offset dal menu CONFIG.
Da lì in poi sopravvivono da soli.

---

## 2. La macchina dei tempi leggeva nell'istante sbagliato

### Diagnosi

Modulo del giroscopio attorno al trigger, tutti e dodici i burst:

```
burst    primo istante con |ω| > 25 dps      tempo registrato
1116/1   −78 ms                               no
1116/3   −32 ms                               no
1116/4   −78 ms                               no
1116/7   mai                                  SÌ  4156 ms
1123/1   +0 ms (esattamente al trigger)       SÌ  7555 ms
1123/4   −37 ms                               no
```

Corrispondenza perfetta. Il movimento del rilascio supera la soglia MOTO **da 30
a 80 ms prima** che il trigger scatti; la macchina passa correttamente a MOTO e
abbandona l'ancora — giustamente, l'arco *si sta muovendo* — e `doFire()` legge
uno stato che il rilascio ha già invalidato.

**Il numero era noto.** Il briefing del 31/07 misura il rilascio a mediana −46 ms
dal trigger; è lo stesso fatto che fonda la finestra causale dei 17 ms. La
macchina è stata scritta come se non lo fosse.

### Correzione: non una toppa, la definizione giusta

```
sbagliato   mira = t_trigger − t_ancora     letto al trigger, ancora già persa
giusto      mira = durata di ANCORA, congelata quando ANCORA finisce
```

L'uscita da ANCORA **è** il rilascio, rilevato dalla stessa soglia che usa
l'analisi causale. Si guadagna in più l'eliminazione del ritardo di 46 ms del
trigger, che era stato dichiarato «irrilevante, sotto l'1,5%» ma restava un
errore sistematico gratuito.

`chiudiAncora()` è chiamata da **un solo punto logico** per tutte e tre le uscite
(movimento, fuori postura, timeout): ognuna che se la sbrigasse da sola sarebbe
un'occasione di dimenticarne una. Il timeout è l'eccezione voluta — una mira di
più di un minuto non si congela, altrimenti verrebbe offerta al tiro successivo.

`tempo_on_fire()` accetta la durata solo se l'ancora è finita da meno di
`TEMPO_LATCH_MAX_MS = 300` (sei volte il margine sui 46 ms).

### Perché il banco F20 non poteva vederlo

Chiamava `tempo_on_fire()` da uno stato pulito, subito dopo la mira. **Provava la
macchina, non il suo aggancio al mondo.** Ogni scenario del banco F20b include
adesso i 50 ms di rilascio che nella realtà stanno fra l'ancora e il trigger, e
due scenari sono nuovi:

```
5) rilascio lungo, 80 ms prima del trigger    mira=2999      ← riproduce il fallimento sul campo
6) ancora finita 500 ms prima del trigger     mira=n/d(2)    ← il latch non deve essere troppo generoso
```

### Diagnostica dell'assenza

`TempoTiro::motivo` distingue «mai raggiunta un'ancora» (1) da «ancora troppo
vecchia» (2). Senza, un campo vuoto nel CSV non dice quale delle due, e si
correggono in modi opposti.

---

## 3. I due falsi trigger, e la marcatura dell'assetto

Nelle sedute:

```
1116/0002   alzo −19,9°  cant +45,7°  hold −77°  movimento continuo da −496 ms
1123/0003   alzo −35,4°  jerk 17 (contro 600–1400 degli altri)
```

Assetti impossibili per un tiro: è l'arco maneggiato con il trigger armato.

Nuovo campo `assetto_ok`: `0` plausibile, `1` fuori dai limiti
(`|cant|` o `|alzo| > 45°`), `2` angoli non calcolati.

**Si marca, non si scarta.** La decisione di escludere un tiro spetta a chi
analizza, non al firmware, che non sa cosa si stia cercando. E il limite a 45° è
volutamente larghissimo (nel 3D l'elevazione massima sta sotto i 25°): serve a
prendere l'impossibile, non a fare da filtro fine.

**Conto aperto**: 12 frecce tirate, 12 record, 2 falsi ⇒ **due tiri veri non
catturati**. Da capire se la soglia del trigger sia alta per 5 metri o se siano
andati persi durante lo scoring del precedente.

---

## 4. Una violazione di una regola già scritta

La nota sull'alzata («su 900 ms reali ne dichiara 620, metrica indicativa e non
misura») **non è mai arrivata nel file consegnato con la Fase 20**. Il
`str.replace` che doveva inserirla aveva un'ancora con spaziatura diversa, non
ha trovato nulla, e non essendoci un assert è fallito in silenzio.

`str.replace` senza asserzione di unicità dell'ancora è **già** fra le regole di
metodo del progetto. Averla violata due settimane dopo averla scritta dice che
una regola che vive solo in un documento non protegge niente. La nota è adesso in
`tempo.h`, con accanto la storia di come si era persa.

---

## 5. Verifiche

```
sh tools/sintassi/controlla.sh    13 file, tutti ok
sh tools/tempo/prova.sh            6 scenari, tutti attesi
```

Il banco di sintassi ha trovato subito che gli stub di `Preferences.h` non
avevano `putUChar`/`putFloat`/`getUChar`/`getFloat`. Aggiunti — uno stub
incompleto è un cono d'ombra, e si chiude quando lo si incontra.

**Non verificato**: nulla di F20b è passato dall'hardware.

---

## 6. Regole di metodo aggiunte

- **Distinguere i parametri dalla taratura.** I primi si possono azzerare, la
  seconda no — e il discrimine non è quanto costa rifarla, è **se un valore
  sbagliato si nota**. Un mount sbagliato produce numeri plausibili sull'asse
  sbagliato: è invisibile per definizione.
- **Una macchina a stati che riporta qualcosa a un evento va provata con la
  sequenza reale che precede l'evento.** Il banco F20 chiamava `on_fire()` da
  uno stato pulito; la realtà ci arriva attraverso 50 ms di rilascio. Provare la
  macchina non è provare il suo aggancio al mondo.
- **Un numero già misurato va riusato dove serve, non solo dove è stato
  trovato.** I −46 ms del rilascio erano in tre documenti e hanno fondato la
  finestra causale; poi è stata scritta una macchina che li ignorava. Non è
  mancanza del dato, è mancanza di collegamento.
- **Una regola che vive solo in un documento non protegge niente.** Il
  `str.replace` senza assert è fallito due settimane dopo essere stato
  proibito. Se serve, va nello strumento — non nel README.
