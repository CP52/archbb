# ArchBB 1.83 — Fase 18 · Clinometro di banco

**31 luglio 2026** · sostituisce `DIAG angoli` (voce 3 del menu) e ritira l'ambiente
`archbb_183_assi`.

---

## Perché

Il briefing del 30/07 chiude la questione degli assi con una regola precisa:
*non riaprirla senza una nuova osservazione fisica.* Il motivo è che quattro
tentativi per inferenza avevano prodotto quattro risposte diverse, e l'unica cosa
che ha funzionato è stato smontare la scheda e guardare il chip.

Il clinometro non è un quinto tentativo di inferenza. È **uno strumento di misura
assoluto**: l'operatore conosce la verità (l'arco è a piombo, la punta è alzata di
30°) e legge cosa dichiara il dispositivo. La verifica diventa binaria.

E serve a qualcosa di più immediato. Il punto 1 dell'ordine di lavoro del briefing
chiede da dove vengano i ~5° residui fra −80 ms e lo scocco, e suggerisce come
strada l'assetto statico a +500 ms — *un riferimento indipendente mai usato*. Quel
confronto ha senso solo se ci si fida della funzione che ricava l'assetto
dall'accelerometro. **Il clinometro è quella funzione, validata a mano.** È il
calibro con cui misureremo l'analisi ex-post, non un accessorio diagnostico.

---

## Cosa è stato tolto, e perché

| ritirato | motivo |
|---|---|
| voce di menu `DIAG angoli` | mostrava cant/alzo dei 4 montaggi in parallelo: serviva a **scegliere** il montaggio. Scelta fatta e congelata in config. |
| `main_diag_assi.cpp` + env `archbb_183_assi` | il DIAG dinamico della Fase 12, mai passato sul campo. Rispondeva alla stessa domanda chiusa, col metodo che ha già sbagliato tre volte su quattro. |
| `displayAngleDiag()` in `display.cpp/h` | codice morto una volta rimosso `runAngleDiag()`. |
| `runAngleDiag()` in `main.cpp` | sostituito da `clino_run()`, che **ritorna** invece di essere un capolinea. |

Il file `main_diag_assi.cpp` resta nella storia di git: se un giorno servisse, si
recupera da lì.

---

## Cosa fa

Voce **CLINOMETRO** nel menu d'avvio (verde, terza card). Due pagine, due domande
diverse.

### Pagina 1 — ASSETTO

Alzo e cant dall'accelerometro, nel frame canonico e col montaggio configurato,
con gli offset di calibrazione applicati. Sotto i numeri, due segmenti che si
incrociano: il **lungo ambra** è l'asse freccia ruotato dell'alzo, il **corto ciano**
è l'asse verticale dell'arco ruotato del cant. Il segno si legge senza dover
ricordare una convenzione.

Lo **yaw** compare solo come velocità, mai come angolo. La gravità non osserva la
rotazione attorno alla verticale: integrare `gx` qui produrrebbe una deriva
monotona travestita da misura. Senza magnetometro, la velocità è l'unica cosa
onesta che si possa mostrare.

Il piede mostra gli assi grezzi, `|a|`, la deviazione standard e il semaforo
FERMO. Non servono all'arciere: servono a chi verifica che la mappatura di
montaggio sia quella dichiarata. Se un giorno i numeri grandi mentissero, la
bugia si vede lì.

**AZZERA** scrive `offset_alzo_deg` e `offset_cant_deg` in NVS. Finora era
possibile solo via BLE (comandi 0x05/0x06). Funziona in due tempi — il primo tocco
arma, il secondo conferma — e rifiuta di salvare se il semaforo non dice FERMO.

### Pagina 2 — COERENZA

Qui sta il valore vero. Confronta la **variazione d'angolo** misurata
dall'accelerometro con l'**integrale del giroscopio** sulla stessa finestra di
250 ms. È una verifica **chiusa**: non richiede alcuna verità esterna, perché i
due sensori si controllano a vicenda.

Perché integrare il giroscopio invece di derivare l'accelerometro: derivare
amplifica il rumore, integrare lo media. Su 250 ms la deriva di bias è
irrilevante; il rumore della derivata numerica dell'angolo da gravità non lo
sarebbe affatto.

Il risultato è una matrice 2×2 di pendenze ai minimi quadrati senza intercetta
(entrambe le grandezze sono variazioni: a movimento nullo devono valere zero
tutt'e due, e forzare il passaggio per l'origine è fisica, non comodità):

```
              gyro ALZO   gyro CANT
  muovo ALZO    +1.00       0.00        <- atteso
  muovo CANT     0.00      +1.00
```

| lettura | significato |
|---|---|
| diagonale ≈ +1, fuori ≈ 0 | assi e segni coerenti |
| diagonale ≈ −1 | segno invertito — correggere **qui e in `integrate_gy`**, mai in uno solo dei due |
| fuori diagonale dominante | assi scambiati — rivedere `mount_apply`, non i segni |

Un campione entra nella riga ALZO solo se il movimento di alzo è almeno 3 volte
quello di cant, e viceversa. Serve a tenere pulite le celle fuori diagonale: se si
muovono le due cose insieme, un eventuale scambio d'assi diventa
indistinguibile. Per questo la schermata chiede **un asse per volta**.

L'ipotesi sotto esame è scritta in fondo alla pagina, così fra tre mesi quei
quattro numeri avranno ancora un significato:

```
rate_alzo = -cgy      rate_cant = -cgz
```

Sono le stesse convenzioni che `integrate_gy()` applica in `shot_angles.cpp` dopo
la correzione F16.

---

## Protocollo di banco — sei pose

Serve un goniometro o un'app inclinometro sul telefono appoggiato al riser.
**Montare la scheda sul riser come si tira, prima di misurare.** Se gli offset
sono già stati impostati, azzerarli o tenerne conto: la tabella qui sotto è a
offset nulli.

| # | posizione | alzo atteso | cant atteso |
|---|---|---|---|
| 1 | arco a piombo, freccia orizzontale | 0° | 0° |
| 2 | punta in su 30° | **+30°** | 0° |
| 3 | punta in giù 30° | **−30°** | 0° |
| 4 | cant a destra 30° | 0° | **+30°** |
| 5 | cant a sinistra 30° | 0° | **−30°** |
| 6 | punta verticale in alto | **+90°** | — |

Tolleranza ragionevole: ±2° sull'angolo, purché il semaforo dica FERMO. Errori
più grandi non sono rumore: sono un offset di montaggio (correggibile con AZZERA
nella posa 1) o una mappatura sbagliata (che è un'altra faccenda).

**Sui valori grezzi non do una tabella attesa.** Dipendono da come è orientato il
chip rispetto al riser e sono esattamente ciò che il clinometro deve *misurare*:
pubblicarne una previsione significherebbe rifare per iscritto l'errore che il
briefing chiede di non ripetere. Trascriverli, non predirli.

### Poi il test dinamico

Pagina COERENZA, e muovere l'arco **lentamente** (2–5 °/s):

1. punta su e giù, ampio, per una decina di secondi → si riempie la riga ALZO
2. cant a destra e a sinistra, altrettanto → si riempie la riga CANT
3. leggere il verdetto quando entrambi i contatori superano 40

Se compare *INCOERENTE*, rifare più lentamente prima di dedurre qualunque cosa:
oltre i ~15 °/s le accelerazioni della mano falsano l'angolo ricavato dalla
gravità, che è il riferimento del confronto.

---

## La domanda che questo strumento può chiudere subito

Discussa a parte, ma vale la pena annotarla qui perché il clinometro è lo
strumento giusto e bastano dieci secondi.

Il cant è una rotazione attorno **all'asse freccia** o attorno al **pivot
dell'impugnatura**? Le due ipotesi non sono equivalenti: se il polso ruota attorno
al pivot, che sta 3–4 cm sotto il rest, l'asse della freccia trasla lateralmente e —
con la cocca ferma all'ancoraggio — la freccia acquista una rotazione laterale
aggiuntiva di circa 0,67° ogni 10° di cant. A 40 m sono 47 cm: non un dettaglio.

Il test: pagina ASSETTO, cantare lentamente il polso e guardare la riga dello
**yaw**. Se resta a zero, la rotazione è pura attorno all'asse freccia. Se
accompagna sistematicamente il cant, è composita. Una delle due ipotesi cade in
dieci secondi.

---

## Misura 18c — i due segni corretti sul banco (31/07)

Prima sessione di misura vera. Risultato:

| grandezza | ipotesi di partenza | misurato | esito |
|---|---|---|---|
| `rate_alzo` | `-cgy` | `-cgy` | **confermato** — diagonale ALZO a +1 |
| `rate_cant` | `-cgz` | `+cgz` | corretto — la pagina COERENZA dava −1 |
| `rate_yaw` | `+cgx` | `-cgx` | corretto — ruotando a destra diceva «verso SINISTRA» |

Alzo e cant statici tornano entrambi (alzo negativo verso il basso, cant
positivo a destra): l'accelerometro è a posto, il problema era tutto nei segni
del giroscopio.

**`hold_deg` non è toccato.** Usa `gy`, il cui segno è confermato, ed è lo stesso
segno validato su 55 tiri veri nella correzione F16. Le sedute già raccolte
restano valide.

Verifica fatta con `grep` prima di scrivere una riga: nel firmware **nessuna
metrica consuma `gz` o `gx`**. Vengono salvati nel burst e mai usati. Quindi la
correzione resta confinata a `clino.cpp`.

### Perché non c'è una regola unica, e perché è importante

Se ci fosse stata una spiegazione sistematica, tutte e tre le convenzioni
sarebbero state sbagliate allo stesso modo. Una candidata c'era, ed è seria:
`mount_apply` applica al giroscopio **la stessa matrice** dell'accelerometro, ma
la velocità angolare è uno **pseudovettore**, e sotto una riflessione cambia
segno *in più* rispetto a un vettore vero. Il flip-Z hardware è esattamente una
riflessione — `mount.h` lo dice: `det = -1`. Un'inversione globale del giroscopio
sarebbe stata la conseguenza attesa.

Non è così: solo l'alzo è invertito rispetto alla deduzione geometrica. Il che
significa che non abbiamo ancora una descrizione completa della trasformazione, e
che questi tre segni sono **misure, non conseguenze**.

Conseguenza pratica da non dimenticare: **valgono per il montaggio corrente.**
Non essendo derivati da una trasformazione, non si trasformano insieme a essa. Se
un giorno si cambia `mount_orientation`, vanno rimisurati con questa stessa
pagina — non ricalcolati a tavolino. È scritto anche nella testata di
`clino.cpp`, dove serve leggerlo.

### Verdetto reso specifico per asse

Il messaggio generico *«SEGNO INVERTITO — correggi clino.cpp e integrate_gy»*
era un consiglio pericoloso: in questo caso avrebbe mandato a toccare `hold_deg`,
che era giusto. Ora la schermata distingue:

- **SEGNO INVERTITO: ALZO** → «clino.cpp E integrate_gy insieme»
- **SEGNO INVERTITO: CANT** → «solo clino.cpp: nessuno usa gz»
- **ALZO e CANT** → «attenzione: l'alzo tocca hold_deg»

## Correzione 18b — l'errore di link

La prima consegna non linkava:

```
undefined reference to `displayBleMode(bool, char const*, StatusInfo const&)'
```

Causa: ripulendo `display.cpp` dalla funzione ritirata `displayAngleDiag()`, il
taglio è arrivato fino a fine file — ma dopo `displayAngleDiag` c'era anche
`displayBleMode()`, che è sparita con lei. Sintassi perfetta, link impossibile.

Il banco di controllo non poteva vederlo: fa `-fsyntax-only`, **non linka**. Ed è
esattamente il caso previsto dalla regola di metodo del progetto — *le
sostituzioni automatiche vanno verificate sull'unicità dell'ancora, e quando si
sostituisce un intervallo di righe bisogna guardare cosa c'è dentro, non solo
agli estremi.* Avevo guardato solo gli estremi.

Due rimedi:

1. `display.cpp` è stato rigenerato dall'originale rimuovendo **solo** le righe
   781–892 (il blocco `displayAngleDiag` e il suo commento). Diff verificata:
   112 righe rimosse, 0 aggiunte, `displayBleMode` intatta.
2. `controlla.sh` ha una sezione nuova, **«dichiarato in un .h ma non definito in
   alcun .cpp»**. Non è un linker e non pretende di esserlo — non vede firme,
   template o namespace. Vede il caso che ci ha morso, che è poi il più frequente:
   una definizione sparita durante una pulizia. Provata in negativo: rimuovendo a
   mano `displayBleMode` la segnala.

## Stato della verifica

- `sh tools/sintassi/controlla.sh` → tutti i sorgenti **ok**, controllo font
  numerici pulito, nessuna definizione mancante.
- Il banco è stato **provato in negativo**, come vuole la regola di metodo:
  reintroducendo un errore di sintassi in `clino.cpp` lo segnala, e mettendo un
  `"+"` dentro una `drawString(..., 6)` lo marca `SOSPETTO`. Uno strumento di
  controllo che non si accende è peggio di non averlo.
- **Non compilato per ESP32**: qui non c'è la toolchain. La build vera va fatta
  sulla macchina di sviluppo prima del flash.

## Uso

```
pio run -e archbb_183_touchcal -t upload
```

All'accensione, toccare la card verde **CLINOMETRO** entro i 3 secondi del menu.
Si esce con **ESCI** e il boot prosegue normalmente: si misura al banco e si va a
tirare senza spegnere.
