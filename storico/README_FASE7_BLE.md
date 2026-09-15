# ArchBB 1.83 — Fase 7: BLE modale (colloquio con smartphone)

**Cesare Pagura · Padova/Noale IT · 21 luglio 2026**
Versione firmware: `1.83-Fase7-BLE` · Companion app: `v0.1`

---

## 1. In una riga

Il tasto **BOOT/GPIO0** (ingranaggio) fa entrare la 1.83 in **modo BLE**: il
dispositivo sospende la catena IMU, apre un servizio GATT su Core 0 e colloquia
con lo smartphone (legge/scrive il **config**, sincronizza l'**RTC**, elenca le
**sessioni** su microSD). Si esce con lo stesso tasto: i parametri diventano
operativi e i task IMU riprendono.

---

## 2. Perché "modale" e non demone (la differenza con la 1.69)

Il BLE della 1.69 era un demone **sempre attivo**, che conviveva con lo scoring
e faceva LIVE streaming dei burst. Questo lo costringeva a una macchina
anti-crash complessa (back-pressure adattivo su decine di notify per burst,
PM-lock permanente, race cross-core, contesa I2C).

La 1.83 non ha bisogno del LIVE (non si è dimostrato utile) e ha la microSD che
salva già tutto a bordo. Quindi il BLE diventa un **modo modale** — un
"capolinea" come CALIBRA e DIAG:

```
ATTESA ──[BOOT]──► MODO BLE ──[BOOT]──► ATTESA
                     │
                     ├─ sospende imu_task + trigger_task (Core 1 libero)
                     ├─ NimBLE su Core 0 (ha la scena per sé)
                     └─ config / RTC / elenco sessioni
```

**Le tre garanzie di sicurezza, per costruzione:**

1. **IMU sospesa durante il BLE** → il rischio latente del bus I2C condiviso
   (handoff §5) è annullato: nessuno martella l'I2C in polling mentre il BLE è
   attivo. Non serve un mutex di bus "per sicurezza" — **abbiamo eliminato la
   contesa alla radice**, non l'abbiamo protetta. ("I dati comandano": nessun
   blocco osservato, nessun lucchetto aggiunto — invece, nessuna contesa.)

2. **Niente stream di burst = niente back-pressure complicata.** Le notify di
   questa fase sono singole e rade. La disciplina onStatus + connection-interval
   della 1.69 c'è ancora (per l'elenco sessioni), ma in forma **molto più
   leggera** dei 42-notify-per-burst.

3. **BLE su Core 0 + PM-lock** anti-light-sleep (lezione 1.69 v2.10.4):
   previene la classe di crash `esp_pm_impl_waiti`. Il lock si acquisisce
   all'ingresso nel modo BLE e si rilascia all'uscita (non permanente: il device
   fuori dal modo BLE torna a poter dormire).

---

## 3. Come si usa (sul campo)

1. Dopo il boot (menu 3s → ATTESA), premi **BOOT/ingranaggio**.
2. Il display mostra **MODO BLE · in ascolto…** col nome device
   (`ArchBB-183-XXXX`, dove XXXX sono 2 byte del MAC — così due dispositivi
   vicini si distinguono).
3. Apri la **companion app** (`app/ArchBB_183_Config_App_v0_1.html`) su
   Chrome/Edge (Android o desktop), premi **Connetti**, scegli il device.
4. Da app: leggi/scrivi i **parametri**, **sincronizza l'ora**, **aggiorna
   l'elenco** delle sessioni.
5. Premi di nuovo **BOOT** sul device: esce dal modo BLE, applica il config,
   riprende lo scoring. Se hai sincronizzato l'ora, un flash verde lo conferma.

---

## 4. Cosa fa e cosa (ancora) no

| Funzione | Stato |
|---|---|
| Entra/esci col tasto BOOT | ✅ |
| Legge il config dal device | ✅ |
| Scrive il config sul device (con checksum + validazione) | ✅ |
| Ripristina i default di fabbrica | ✅ |
| Sincronizza l'RTC dall'ora del telefono | ✅ |
| Elenca le sessioni su microSD (nome + n. tiri) | ✅ |
| **Download** del contenuto sessione (CSV + burst) via BLE | ⏳ prossima |
| Visualizzazione dati/grafici come app 1.29 | ⏳ dopo il download |

### ⚠️ Nota importante sui parametri scritti via BLE

Oggi `config_apply()` cabla **solo il montaggio** (`g_mount_orientation`): questo
diventa operativo subito all'uscita dal modo BLE. **Soglia trigger e finestre**
scritte via BLE vengono **salvate in NVS** ma diventano operative solo dopo aver
aggiunto i loro *setter runtime* (`trigger_configure()` + lettura finestre in
`circular_buffer`), come previsto dall'handoff §2.3. È il naturale
completamento della prossima sessione: intenzionale tenerlo separato (un pezzo
alla volta). L'app avvisa l'utente di questo con una nota nel form.

---

## 5. Il protocollo GATT

Servizio `bb1c0000-0001-0000-0000-000000000001` (base "bb1c" = "BB 1.83 config",
diversa dalla 1.69 "bb4a" per non confondere l'app se entrambe fossero in giro).

| Caratteristica | UUID suffix | Prop | Payload |
|---|---|---|---|
| CONFIG  | `…0010` | READ/WRITE | struct `Config` (35 B, packed, con FNV-1a checksum) |
| TIME    | `…0020` | WRITE | 7 B: `[yearLo,yearHi,mon,day,hh,mm,ss]` → RTC |
| CMD     | `…0030` | WRITE | `[cmd][payload]`: 0x01 ping · 0x02 list · 0x03 reset |
| STATUS  | `…0040` | READ/NOTIFY | 28 B: stato + batt + SD + RTC + n.sessioni + fw |
| SESSION | `…0050` | NOTIFY | 30 B per record: `[index][shots][name(26)]` |

**Elenco sessioni**: alla `CMD 0x02`, il device notifica prima un *record-header*
(`index=0xFFFF`, `shots=totale`) e poi un record per sessione, dalla più recente.
Ogni notify aspetta il completamento della precedente (semaforo su `onStatus`):
niente accumulo nei buffer mbuf, la lezione della 1.69.

**Checksum config**: FNV-1a a 32 bit sui primi 31 byte, **identico** fra firmware
(`config_store.cpp`) e app (`cfgChecksum` in JS con `Math.imul`). Verificato:
stesso input → stesso output (nessun rifiuto per falsa corruzione).

---

## 6. File toccati/aggiunti in questa fase

**Nuovi:**
- `src/ble_service.h` / `src/ble_service.cpp` — il modo BLE modale.
- `src/display.cpp` (+`displayBleMode`) — schermata "colloquio".
- `app/ArchBB_183_Config_App_v0_1.html` — companion app (Web Bluetooth).

**Modificati:**
- `src/main.cpp` — tasto BOOT, stato `BLE_MODE`, `enterBleMode`/`exitBleMode`,
  handle dei task IMU salvati per la sospensione.
- `src/config_store.h/.cpp` — `config_checksum_of()` pubblica (per la validazione
  BLE; delega al checksum interno, **nessuna formula duplicata**).
- `src/sd_storage.h/.cpp` — `sd_session_total()` + `sd_enumerate_sessions()`
  (elenco sessioni, ordinato dalla più recente).
- `platformio.ini` — dipendenza `h2zero/NimBLE-Arduino @ ^1.4.1` (stessa 1.69).
- `src/config.h` — versione → `1.83-Fase7-BLE`.

---

## 7. Compilazione e flash

```
pio run -e archbb_183_touchcal -t upload      # campo (muto)
pio run -e archbb_183_debug     -t upload      # tavolino (log seriali)
```

Prima build: PlatformIO scarica NimBLE-Arduino 1.4.1. Il resto invariato.

**Nota firma `onStatus`** (l'unico punto storicamente fragile): usiamo
`onStatus(NimBLECharacteristic*, Status, int)`, **identica** a quella validata
sul campo nella 1.69 v2.16.10 (NimBLE 1.4.1). Se il tuo ambiente avesse una
versione con firma diversa, `ble_service.cpp` (callback `SessionCallbacks`)
documenta le alternative — cambia solo la riga della firma, la logica resta.

---

## 8. Prossimi passi (ordine suggerito)

1. **Campo**: verifica entra/esci col tasto, lettura/scrittura config, sync ora,
   elenco sessioni. ("I dati comandano" anche qui.)
2. **Setter runtime** trigger/finestre in `config_apply` (handoff §2.3): rende
   operativi *tutti* i parametri editati via BLE, non solo il montaggio.
3. **Download sessioni** via BLE: stesso servizio SESSION, con la disciplina
   anti-crash (onStatus + connection interval) del §5 — il burst è ~20 KB/tiro,
   qui la back-pressure serve davvero.
4. **App**: visualizzazione dati/grafici (riuso del motore dell'app 1.29) una
   volta che il download porta CSV+burst sul telefono.
