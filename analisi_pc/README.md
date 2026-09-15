# ArchBB — Analizzatore · 2026.08.25-01

Rifatto attorno alle tre cose che nella versione del 30/07 non esistevano:
l'**impatto in centimetri** (F19), i **tempi del gesto** (F20/F20b), e la
**finestra causale** di 17 ms.

## Avvio

```
pip install -r requirements.txt
./avvia.sh          (Windows: avvia.bat)
```

## Sorgenti

Nel campo «Percorso» si può puntare:

| cosa | esempio |
|---|---|
| radice della card | `D:\ARCHBB` — trova cartelle `SESS_*` **e** archivi `.zip` |
| una singola seduta | `D:\ARCHBB\SESS_20260816_1017` |
| un singolo zip | `D:\ARCHBB\SESS_20260816_1017.zip` |

Gli zip si aprono come le cartelle, sia contenendo la cartella `SESS_.../`
sia contenendo direttamente `shots.csv` e i burst. Se una cartella e un
archivio hanno lo stesso nome, **vince la cartella**: è l'unica su cui si possa
scrivere una correzione, e mostrare due volte la stessa seduta è peggio che non
mostrarne una.

Più sedute si possono selezionare insieme: la potenza statistica arriva da n, e
n arriva accumulando sedute. La colonna `seduta` resta sempre, così si può
controllare che un effetto non sia solo la differenza fra due giornate.

## Sedute con gli assi scambiati

Se una seduta è stata registrata con il montaggio sbagliato — è successo il 15
agosto su tre sedute — l'analizzatore **se ne accorge da solo** e propone la
correzione nel pannello di sinistra. Il criterio è quello della scheda Assetto:
puntando un bersaglio a elevazione nota, se è il *cant* a seguirla (col segno
invertito) invece dell'alzo, gli assi sono ruotati.

La correzione **si propone, non si applica da sola**: ruotare gli assi è
un'affermazione su come era montata la scheda quel giorno, e quella cosa la sa
l'arciere.

Cosa fa: ruota di +90° i sei assi di ogni burst, scambia cant e alzo nel CSV, e
**ricalcola tenuta e rilascio dai dati grezzi** — quei due non sono ricavabili
dai valori già scritti, perché sotto la rotazione gli assi che li generano
diventano altri. La rotazione è invertibile senza perdita: non si perde nulla.
I file sul disco non vengono toccati.

## Le schede

**Panoramica** — quanti tiri hanno impatto, tempo, mira statica. Le tre fasi e
cosa può stare in ciascuna.

**Impatti** — la grandezza **dipendente**: l'unica con cui si possano giudicare
tutte le altre. Scatter in centimetri con origine nel punto di mira, σ laterale
e verticale, e la **dispersione angolare**, che è il numero da guardare quando
si confrontano sedute a distanze diverse.

**Tempi** — andamento del tempo di mira, e raggio medio dal centro del gruppo
per terzile di tempo. Il raggio si misura dal *centro del gruppo* e non dal
punto di mira, perché la distanza dal punto di mira include l'errore di
taratura del mirino, che col tempo di mira non c'entra.

**Finestra causale** — il test vero. Mostra la **potenza prima del risultato**:
se è sotto 0,80 lo dice a chiare lettere, perché un nullo con potenza bassa non
è un'informazione. La regressione **misura** il guadagno di trasferimento `k`,
non lo verifica.

**Assetto** — la verifica del montaggio in dieci secondi: puntando un bersaglio
a elevazione nota, l'alzo deve valere l'elevazione e il cant restare piccolo. Se
sono scambiati lo dice esplicitamente. È il controllo che il 15/08 sarebbe
costato tre sedute.

**Tiro per tiro** — le tre velocità angolari attorno al trigger, con evidenziate
la finestra di mira ferma, l'istante del rilascio, i 17 ms causali, la partenza
della freccia e il trigger. Sotto, la **traccia 2D**: il percorso della punta
dell'arco dal rilascio in poi, con il tratto spesso sui 17 ms che contano e
quello sottile sul follow-through. I due assi hanno la stessa scala, così la
forma è fedele. Oltre 250 ms non si disegna, perché lì l'errore del giroscopio
sarebbe indistinguibile dal movimento vero.

**Dati** — tabella completa ed esportazione.

## Cosa NON è stato portato dalla versione del 30/07

- **Sovrapposizione d'onda e DTW di consistenza.** Confrontavano forme d'onda su
  finestre lunghe; senza una grandezza dipendente da correlare non dicevano se
  una forma fosse migliore di un'altra.

*(La **traccia 2D** invece è tornata, nella scheda Tiro per tiro: era stata
tolta con la motivazione «costruita su integrazioni da 900 e 2000 ms, dove la
deriva domina» — vera per quella traccia, falsa in generale. Sui 17 ms causali
l'errore totale è sotto 0,04° contro un segnale di 0,58°. Ora si ferma a 250 ms,
dove la deriva vale ancora un quinto del segnale.)*
- **hold e release_jerk come metriche in evidenza.** Ci sono, ma nella tabella
  dei predittori e marcati come **follow-through**: la freccia parte ~29 ms
  prima del trigger, quindi non possono essere cause dell'impatto. Restano
  diagnostica del gesto.
- **Il DTW di consistenza.** Confrontava forme d'onda su finestre lunghe; senza
  una grandezza dipendente da correlare non diceva se una forma fosse migliore
  di un'altra.

## Colori

L'app **eredita** il tema di Streamlit invece di cablarlo. I toni secondari
(etichette e sottotitoli delle card) non sono grigi fissi: la frazione di
miscela viene *cercata* fino al tono più tenue che rispetta ancora le soglie
WCAG — 4,5:1 per il testo piccolo, 3:1 per quello di supporto — e il confronto
è fatto contro lo sfondo della **card**, non contro quello della pagina, perché
è lì che le etichette vivono.

Il contrasto non è lineare nella miscela: la stessa frazione del 30% dà 7,2:1
su fondo scuro e 4,4:1 su fondo chiaro. Fissarla significava avere una delle
due situazioni illeggibile — che è esattamente com'era la prima versione.

Il tema scuro resta il default (si guardano grafici per ore), ma sta in
`.streamlit/config.toml`: cambiando lì cambia tutto, card comprese.

## Il rilevamento del rilascio

Si cerca **all'indietro**: l'ultimo ritorno sotto soglia prima dello scocco, cioè
l'inizio della salita che porta al rilascio — non la prima agitazione della
finestra.

La versione precedente cercava in avanti, e sulla seduta del 25/08 ha agganciato
due tiri a −298 e −299 ms, cioè al bordo stesso della finestra: erano
assestamenti di mira. Il tiro con il rumore di mira più basso (1,26 dps) aveva
soglia 6,3 dps, e qualunque piccolo aggiustamento la superava — la soglia
adattiva protegge dai tiri rumorosi e rende ipersensibili quelli tranquilli.

Effetto: dispersione dell'istante da 73,7 a **16,4 ms** sulla seduta colpita,
invariata dove il metodo funzionava già (16/08: 14,0 → 15,3), e un tiro
recuperato che prima restava senza rilascio.

## Il riferimento dei tempi

Il trigger scatta quando `| ‖a‖ − g |` supera una soglia. Con soglia bassa si
aggancia al **primo sussulto** dello scocco: un punto poco definito, che cade in
posti diversi della curva da tiro a tiro.

L'opzione «metà dell'urto» riporta lo zero a metà della salita, che è
adimensionale e sta sempre nello stesso punto anche quando lo scocco è debole.
Sui 18 tiri del 16/08 la dispersione dell'istante di rilascio scende da 14,0 a
13,1 ms. **Un guadagno modesto**, e vale la pena dirlo: la dispersione dei 46 ms
non è dominata dall'ancoraggio, ma da variabilità vera del gesto più incertezza
sul rilevamento del rilascio.

Una soglia *assoluta* più alta (per esempio 20, il valore validato a luglio) non
è un'alternativa praticabile su queste sedute: il picco mediano dell'urto vale
22 m/s² il 16 agosto e 10–12 il 15, quindi a soglia 20 metà dei tiri non
esisterebbe proprio.

## Il k della barra laterale

Il guadagno di trasferimento riser → freccia. **Non è 1**: la freccia acquista
velocità mentre l'arco ruota, quindi conta la media dell'angolo pesata sugli
incrementi di quantità di moto (≈ Δθ/2), e la rotazione è attorno al pivot della
mano, non alla cocca. Il default 0,45 è una **stima**, con riscontro esterno
indipendente (0,1° di torque → ~50 mm a 70 m contro 122 mm geometrici: 0,41).

Va sostituito dalla pendenza che uscirà dalla campagna di taratura. Finché è una
stima, i centimetri che ne derivano sono indicativi.
