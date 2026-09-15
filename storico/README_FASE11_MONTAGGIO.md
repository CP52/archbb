# ArchBB 1.83 — FASE 11 · Montaggio: nomi veri e taratura

**Firmware `1.83-F11-mount` · app `ArchBB_183_App_v0_3.html` · 26 luglio 2026**

---

## 1. I nomi dicono il lato fisico

**Regola: se la scheda è a destra, ovunque si legge «destro».** Display, app,
seriale, file di sessione. Il flip-Z è una proprietà interna di questa revisione
di scheda e non compare in nessuna interfaccia.

### Perché servivano due tabelle

I valori 0–3 identificano una **trasformazione**, non un lato. Il legame fra le
due dipende dalla manualità del frame del chip: il flip-Z è una **riflessione**
(det = −1), e le riflessioni scambiano destra con sinistra. La stessa mappa dei
grezzi si scrive in due modi equivalenti:

```
"+90 sinistro con flip PRIMA"  ==  "-90 destro con flip DOPO"
        (entrambe: cay = -z, caz = -y)
```

Il nome della rotazione, da solo, non dice da che parte sta la scheda.

### Ancoraggio empirico

`mount_name()` con flip attivo: **2 = destro**, ancorato al DIAG ASSI del 26/07
(margine 36 su soglia di allarme 5) e confermato in modo indipendente dai nove
tiri del 25/07 — con quel valore l'alzo esce −16,98° (punta in basso, sagoma 3D
a terra) e il cant +0,39° con σ 1,22° (rollio involontario).

Gli altri tre nomi seguono per simmetria e **non sono verificati sul riser**: se
un giorno servisse montare a sinistra, si rifà il DIAG invece di fidarsi.

`mount_name_rotazione()` resta disponibile per la diagnostica di basso livello.

---

## 2. Taratura del montaggio

Comandi BLE **`0x05`** (tara) e **`0x06`** (azzera).

L'arciere mette l'arco a piombo e preme un tasto nell'app. Il device legge
l'assetto e lo salva come offset in NVS: da lì cant e alzo sono riferiti
all'arco a piombo, non al chip, e l'inclinazione della staffa sparisce.

**Perché serve.** Sulla 1.69 c'era, sulla 1.83 no: i 17° del 25/07 erano alzo
vero *più* inclinazione della staffa, e senza taratura i due non si separano. Le
variazioni restano affidabili comunque (l'offset è costante e sparisce nelle
differenze), ma il valore assoluto no — e serve, per esempio, per il confronto
col simulatore balistico.

**Con i task IMU sospesi.** In modo BLE `g_latest_sample` non si aggiorna, quindi
si usa `imu_read_raw()`, che parla direttamente col sensore prendendo il mutex
I2C. È nata per il menu d'avvio ed è esattamente questo caso d'uso.

**Media su 48 letture, poi l'atan2.** Non il contrario: mediare due atan2 non è
lineare. Stessa scelta di `shot_angles_compute` nella finestra calma. Se meno di
24 letture riescono, o se |a| si discosta di oltre 1,5 m/s² dalla gravità (arco
in movimento), **la taratura non viene scritta**: meglio nessuna taratura che una
sbagliata — la prima si rifà, la seconda falsa ogni tiro successivo.

---

## 3. Gli offset ora vengono applicati

Erano dichiarati in `Config` dalla Fase 6 e **ignorati da tutti**. Adesso
`shot_angles_compute()` li riceve come parametri e li sottrae **dopo** l'atan2 —
sono angoli, e sottrarre angoli è l'unica operazione che ne conserva il
significato.

Parametri e non lettura di `g_config` perché la funzione è replicata in Python e
in JavaScript per la ri-analisi: se leggesse una globale, le repliche dovrebbero
indovinarne il valore. Resta pura, e gli offset viaggiano in `session.txt`.

---

## 4. La sessione descrive se stessa

`session.txt` ora porta:

```
mount=2
mount_lato=destro
off_cant_deg=-0.42
off_alzo_deg=9.18
```

Senza queste righe un file letto fra sei mesi non dice su quale asse stava il
cant né quanto offset è stato sottratto, e lo strumento di analisi dovrebbe
indovinarlo.

**Le sedute anteriori non le hanno**, e su quelle cant e alzo sono scambiati.
App e analizzatore lo **segnalano** e mostrano i numeri come stanno nel file: non
correggono da soli. Aggiustare in silenzio un dato di provenienza ignota è peggio
che mostrarlo con un avviso.

---

## 5. App v0.3

- **Montaggi per lato fisico**, tabella unica in `MONTAGGI`, allineata a
  `mount_name()`. La voce `4 · frontale-flipZ` **non esiste più**: in questo
  firmware `MOUNT_COUNT` è 4, quindi i valori validi sono 0–3.
- **Card «Taratura del montaggio»**: offset correnti, stato (tarato / mai
  tarato), tasto di taratura con conferma e tasto di azzeramento.
- **Provenienza in chiaro** nella scheda QUADRO: montaggio, taratura, firmware.
- **Avviso sulle sessioni ambigue**, con la spiegazione del perché.
- **Etichette delle tre fasi** allineate ai 42 tiri del 14/07: le correlazioni
  misurate (−0,24 / +0,25 / −0,04, tutte p > 0,1), la PCA con PC1 al 32 %, la
  provenienza della tenuta dall'integrazione del giroscopio con la pendenza dopo
  150 ms come discriminante, e la nota che i 40 ms sono la finestra del jerk
  mentre la freccia lascia la corda attorno ai 15.

---

## 6. Il banco di sintassi ora copre quasi tutto

`tools/sintassi/controlla.sh` è passato da 2 a **10 file**, con stub di
`Arduino.h`, `TFT_eSPI.h`, FreeRTOS, `FS`/`SD`/`SPI`, `Preferences`, `NimBLE`,
`esp_pm`/`esp_mac`/`esp_system`:

```
main_diag_assi · mount · shot_angles · sd_storage · ble_service
config_store · scoring · circular_buffer · main · display        -> tutti ok
```

Girano in pochi secondi con `-Wall -Wextra -std=gnu++11`, senza toolchain ESP32.

**Verificato che serva davvero:** togliendo i due `#include` che mancavano in
`sd_storage.cpp`, il banco riproduce l'errore identico a quello arrivato dal
compilatore vero (`'g_mount_orientation' was not declared in this scope`).

Uno stub deve rispecchiare il **tipo vero**, non uno comodo: `getValue()` di
NimBLE ritorna `NimBLEAttValue`, il cui `.data()` è un `const uint8_t*`. Uno
`std::string` avrebbe dato errori che sul dispositivo non esistono — o peggio,
non ne avrebbe dati dove ci sono.

**Non coperti:** `touch`, `imu`, `battery`, `rtc_clock`, `config_ui`,
`touch_cal`. Dipendono da librerie hardware (SensorLib/QMI8658, driver del PMU)
e da define che arrivano dai `build_flags`. Stubbarli costerebbe più della
copertura che darebbero: sono file stabili. Se un giorno si toccano, si compila
per davvero.

**Non sostituisce la build:** non linka, non vede gli header veri delle
librerie, non controlla le dimensioni in flash. Intercetta gli errori di
linguaggio, che sono la maggioranza di quelli che si fanno scrivendo.

---

## 6bis. Prova di rendering dell'app

`node tools/prova_app.js app/ArchBB_183_App_v0_3.html /percorso/SESS_...`

Carica l'app in un DOM headless (jsdom), le inietta una sessione **vera** letta
da disco, disegna tutte le schede e controlla che le caselle contengano
qualcosa. Serve perché `node --check` verifica la sintassi, non il
comportamento: un `getElementById('mFermR')` che ritorna `null` passa il
controllo di sintassi e poi, in esecuzione, uccide l'intero `renderDash`
lasciando la dashboard vuota senza dire perché. È successo.

Verifica: le cinque schede senza eccezioni, le otto pillole presenti e
valorizzate, la navigazione fra i tiri (i valori devono **cambiare**), la
coerenza fra chip e intestazioni della tabella.

**Controprova**: rimuovendo il `<div class="rif" id="mFermR">` il test fallisce
con `elemento assente -> mFermR`, cioè intercetta il bug esatto che era
arrivato in mano all'utente.

Come effetto collaterale ha trovato un difetto vero: l'app **crollava
all'avvio** se `indexedDB` non era disponibile — succede in alcune modalità
private. Ora c'è una guardia: si perde la cache, non l'applicazione.

---

## 7. Da fare, in ordine

1. Flash `archbb_183_touchcal`, config **montaggio = Destro**.
2. Arco a piombo, **taratura** dall'app. Controlla che gli offset compaiano.
3. Una seduta di prova: `session.txt` deve avere `mount_lato=destro`, e nel
   QUADRO il cant deve stare su qualche grado con l'alzo sulla distanza reale.
4. Solo dopo, tirare sul serio: da lì i dati sono confrontabili fra sedute.
