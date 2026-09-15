# ArchBB 1.83 — Fix panic all'uscita dal BLE: niente deinit di NimBLE

**Versione: `1.83-Fase7-BLE-fix6`**

## Sintomo
Entra correttamente in BLE (fix5 ok). Ma con la seconda pressione (per USCIRE)
va in **PANIC -> reset -> torna in ATTESA** con refresh regolare. Il reset
"pulisce" lo stato, per questo poi riparte bene.

## Causa
`ble_stop()` chiamava `NimBLEDevice::deinit(true)` a ogni uscita. Su ESP32-S3
(NimBLE 1.4.1) il ciclo **init/deinit ripetuto** e' instabile: la deinit
liberava lo stack BLE mentre callback o il ble_task su Core 0 potevano ancora
toccarne le strutture -> **panic**. In piu' la vecchia sequenza procedeva a
deinit anche se il task non era ancora terminato (race).

Prova a conferma: la 1.69, stabile sul campo, **non chiama MAI deinit**. Il suo
BLE era un demone permanente. Io invece facevo start/stop a ogni entrata/uscita,
e il deinit era la mina.

## Fix — inizializzare NimBLE UNA VOLTA, non deinizializzare mai
Adottiamo il modello della 1.69:
- **`ble_start`**: la PRIMA volta costruisce lo stack NimBLE + il GATT (server,
  caratteristiche, advertising) e alza un flag `s_ble_initialized`. Le entrate
  successive **riusano** lo stack: fanno solo `startAdvertising()` e rilanciano
  il task di servizio.
- **`ble_stop`**: NON chiama deinit. Ferma l'advertising, sblocca eventuali
  attese, e **attende che il ble_task termini davvero** (fino a ~1.5s) prima di
  restituire il controllo; se il task fosse appeso, lo termina esplicitamente.
  Lo stack resta vivo e pronto per la prossima entrata.

Costo: dopo la prima entrata BLE, ~40KB restano occupati dallo stack. E'
accettabile (i task IMU girano comunque) e comprato in cambio della stabilita'.

## Dettagli di robustezza
- **Terminazione task sincronizzata**: ble_stop non prosegue finche' il task non
  e' morto (s_ble_task==nullptr). Niente piu' race "deinit mentre il task vive".
- **PM-lock a livello file** (`s_ble_pm_lock`): creato una volta, riusato. Se il
  task viene terminato a forza, ble_stop rilascia il lock al posto suo ->
  nessuno squilibrio (verificato: acquire/release sempre bilanciati, mai doppio
  release, in entrambi i casi "esce da solo" / "ucciso").
- **Config aggiornato a ogni entrata**: la caratteristica CONFIG viene
  ricaricata col valore corrente di g_config ogni volta che si entra nel BLE
  (non solo alla costruzione).

## File toccati
- `src/ble_service.cpp` — `ble_start` con init one-shot (`s_ble_initialized`);
  `ble_stop` senza deinit, con terminazione task sincronizzata; PM-lock a
  livello file.
- `src/config.h` — versione → `1.83-Fase7-BLE-fix6`.

## Da verificare sul campo
1. Entra in BLE (PWRKEY breve) -> schermata "in ascolto".
2. **Esci (PWRKEY breve) -> NIENTE panic**, torna in ATTESA fluido.
3. **Ripeti entra/esci molte volte**: deve essere ripetibile, mai un reset.
4. App: connetti, config/ora/sessioni; poi esci e rientra -> l'app deve poter
   riconnettersi (l'advertising riparte).

## Nota
Se all'uscita comparisse ancora la schermata rossa "RESET: ..." vuol dire che un
panic residuo sopravvive: in quel caso segnalami cosa dice la riga (PANIC vs
WATCHDOG) e da li' isolo l'ultimo punto. Ma con lo stack non piu' deinizializzato
la causa nota e' rimossa.
