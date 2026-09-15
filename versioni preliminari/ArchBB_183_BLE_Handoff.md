# 🏹 ArchBB 1.83 — Handoff per il BLE + Companion App

**Briefing per la prossima sessione · Cesare Pagura · Padova/Noale IT · 21 luglio 2026**

> **Come usare questo file:** aprilo come primo messaggio nella nuova chat e di'
> *"ripartiamo da qui"*. Porta l'ultimo zip firmware
> (`ArchBB_183_Fase6c.zip`, versione `1.83-Fase6c-fixpath`) e il repo
> `github.com/CP52/archbb` come riferimento.

---

## 0. Dove siamo (in una riga)

Il firmware 1.83 è completo e validato sul campo fino alla **persistenza SD +
config NVS + RTC**. Il prossimo capitolo è il **BLE su Core 0**: diventa
l'editor del config NVS (incluso l'orologio) e il canale per la **companion
app**. È l'ultimo grande pezzo mancante, il motivo originale di tutta la catena.

---

## 1. Stato del progetto ArchBB 1.83

### ✅ Completato e validato sul campo

| Fase | Cosa | Stato |
|---|---|---|
| 1-3 | Display ST7789P, touch CST816 (calibrato), scoring UI | ✅ hardware |
| 4a-4c | IMU QMI8658 + trigger + burst PSRAM + angoli cant/alzo (mount FLIPZ) | ✅ campo |
| 5 | Persistenza microSD: CSV risultati + burst grezzo ri-analizzabile | ✅ campo |
| 6 | Config NVS (versione+checksum) + RTC PCF85063 + sessioni datate | ✅ campo |

### 🎯 Cosa fa oggi il dispositivo

- **Scoring** completo con metriche (cant/alzo/hold/release) nel riepilogo.
- **Sessioni su SD**: tap in ATTESA apre/chiude la sessione (tap-lungo abolito:
  qualunque tocco = toggle sessione; il tiro arriva SOLO dall'IMU). Se tiri
  senza sessione, se ne auto-apre una. Riprendi/Nuova alla riapertura.
- **Cartelle datate** `SESS_20260721_1430` se RTC valido, `SESS_0001` numerate
  come fallback se RTC non impostato. Nome file → `session.txt` con `datetime=`.
- **Barra di stato** in ATTESA: batteria (AXP2101), spazio SD, badge REC, orologio.
- **Menu d'avvio** (4 voci): CALIBRA touch, DIAG touch, DIAG angoli, CONFIG
  (lettura parametri NVS + ripristina default).
- **Doppio flash verde "SALVATO"** dopo ogni scrittura SD riuscita.

### 🔧 Strumenti Python (ri-analisi a freddo, nel firmware/tools)

- `leggi_sessione.py` — legge CSV + decodifica burst grezzi.
- `analisi_firmware.py` — ricalcola cant/alzo/hold/release con gli algoritmi
  ESATTI dell'app 1.27 (verificato: riproduce il CSV al centesimo).

---

## 2. 🎯 Il BLE 1.83 — cosa deve fare

La companion app (diversa da quella BLE della 1.69, che "beccava" i burst al
volo) deve fare **due cose**:

1. **Editare i parametri di configurazione** (config NVS) — inclusa la
   **sincronizzazione dell'ora** dallo smartphone all'RTC.
2. **Scaricare le sessioni** dalla SD (scelta di quali).

Il BLE è l'editor del config NVS e il canale di trasferimento file.

### 2.1 Il config è GIÀ pronto per il BLE

`config_store.h` espone la struct `Config` (packed, 35 byte, con
version+checksum) e le API:

```
extern Config g_config;      // fonte di verita' in RAM
bool config_load();          // NVS -> g_config (con migrazione/anti-corruzione)
bool config_save();          // g_config -> NVS
bool config_reset();         // default + save
void config_apply();         // rende operativi i valori (oggi: montaggio)
```

Il BLE deve: leggere `g_config` → mandarla all'app; ricevere modifiche → scrivere
in `g_config` → `config_save()` → `config_apply()`. Esattamente il pattern
"secondo editor sullo stesso store" della roadmap.

**Campi in Config:** trigger (mode, threshold, confirm_n, rearm_ms), finestre
(pre/post), mount_orientation, offset calibrazione (cant/alzo/G, a 0), bow_mass_kg.

### 2.2 L'RTC è GIÀ pronto per il set via BLE

`rtc_clock.h` espone `rtc_set(year,month,day,hour,minute,second)` — **predisposto
ma oggi non chiamato da nessuno**. Il BLE lo chiamerà con l'ora dello smartphone.
Nota: **oggi l'RTC è all'orario fittizio del test** (impostato durante la verifica
hardware); appena il BLE passa l'ora vera, le sessioni saranno datate corrette.

### 2.3 config_apply oggi cabla solo il montaggio

Trigger e finestre leggono ancora i default compilati (che coincidono col
config). Quando il BLE editerà trigger/finestre, servirà aggiungere i loro
**setter runtime** (es. `trigger_configure()`) e chiamarli da `config_apply()`.
È il naturale completamento da fare insieme al BLE.

---

## 3. ⚠️ LE DECISIONI DI MERITO (prima di scrivere codice)

### 3.1 Trasferimento sessioni: come?

- **BLE file transfer**: l'app scarica i file via BLE (serve un protocollo di
  trasferimento: elenca sessioni → scarica CSV + burst). Più lavoro, ma non
  estrai la SD.
- **SD estraibile** (già funziona): il BLE fa solo config, i file si leggono
  estraendo la card. Meno lavoro BLE.
- Decisione originale (handoff SD §3.2): "SD estraibile, BLE dopo". Ora si può
  rivalutare: config via BLE è comodo, ma i burst (20 KB/tiro) via BLE sono lenti.
  Ipotesi sensata: **config + elenco/metadati sessioni via BLE, burst su SD**.

### 3.2 Core del BLE

**Core 0** (la roadmap lo dice, e la lezione 1.69 lo conferma — vedi §4). IMU e
scoring stanno su Core 1; il BLE (NimBLE) va su Core 0, lo stesso dell'host BT.

### 3.3 Struttura dei servizi/caratteristiche BLE

Da progettare: un servizio "config" (read/write della struct), un servizio
"time" (set RTC), un servizio "sessioni" (elenco + eventuale download). Definire
GATT, UUID, formato dei pacchetti.

---

## 4. ⚠️ LEZIONE 1.69 DA NON RIPETERE — il crash BLE `rst:0xc`

Documento completo: `ArchBB_Crash_BLE_Diagnosi_Completa.md` (nel repo). Sunto
delle lezioni che sono costate giorni sulla 1.69:

**La causa reale del crash era il CONNECTION INTERVAL BLE.** Le notify possono
uscire sull'aria solo entro le finestre concordate col telefono (il connection
interval, ~30ms). Chiamare `notify()` più spesso accumula pacchetti nei buffer
mbuf di NimBLE finché si esauriscono → errore `BLE_HS_ENOMEM` (code=6) → in
ripetizione, crash hard `rst:0xc`.

**La soluzione (v2.10.10):**
1. **Sincronizzare le notify sul callback `onStatus()`** di NimBLE (invocato a
   notify realmente completata, non solo accodata) con un semaforo: si manda il
   chunk successivo SOLO dopo che il precedente è confermato.
2. **Ancorare il delay al connection interval REALE** negoziato (letto da
   `ble_gap_conn_desc`) + margine di sicurezza, non a un valore fisso indovinato.

**Le false piste (tutte scartate coi dati):** congestione/timing, race
cross-core, task watchdog, interrupt watchdog, power management. Il `Saved PC`
del dump di boot **non è affidabile**: cattura il core in idle o il meccanismo
di reset, non chi ha causato il crash. La svolta è venuta da una **stringa di
errore leggibile** (da `onStatus`), non da un indirizzo decodificato.

**Regola operativa:** se emergono crash BLE, non decodificare il Saved PC alla
cieca — instrumenta un callback applicativo che produca un errore LEGGIBILE, poi
cercalo nel repo NimBLE-Arduino (l'issue #614 descriveva il nostro caso esatto).

---

## 5. Note hardware/architettura da ricordare (1.83)

- **Bus I2C condiviso** SDA=15/SCL=14: IMU (QMI8658), touch (CST816), PMU
  (AXP2101), RTC (PCF85063). Ogni `*_init()` NON chiama `Wire.begin()` (lo fa
  solo touchInit). **Rischio latente noto:** l'imu_task martella l'I2C in
  polling su Core 1 senza mutex sul BUS (il mutex esistente protegge solo il
  campione). Finora nessun blocco da questo, ma se il BLE aggiungesse letture
  I2C in momenti caldi, valutare un mutex di bus. Per ora NON toccato (nessuna
  evidenza di problema — "i dati comandano").
- **SD su bus SPI dedicato** (FSPI): MOSI=1, SCK=2, MISO=3, CS=42. Separato dal
  display (HSPI, 6/7). Nessuna contesa.
- **Task**: IMU (prio 5) + trigger (prio 4) su Core 1; loop()=UI su Core 1.
  **Il BLE andrà su Core 0** (niente ancora lì).
- **PSRAM** attiva (`qio_opi`, octal). Buffer burst in PSRAM.
- **Display**: ST7789P 240×284, offset y=20 via CGRAM_OFFSET, BGR, backlight
  GPIO40 active-HIGH, setRotation(0). Config TFT_eSPI via build_flags (non
  User_Setup.h).
- **Touch**: CST816 calibrato `TOUCH_CAL_AX=0.74591 BX=21.84 AY=0.86208 BY=21.68`.
  Non toccare.
- **RTC**: PCF85063, indirizzo 0x51, via SensorLib. Tiene l'ora da spento
  (batteria di backup PRESENTE — verificato sul campo 21/07). Flag OS bit7 reg
  0x04 = "ora persa". Oggi impostato a orario FITTIZIO (dal test).
- **Batteria**: AXP2101, 0x34, via XPowersLib.

### Env PlatformIO
- `archbb_183_touchcal` — campo, muto (default).
- `archbb_183_debug` — tavolino, +`ARCHBB_DEBUG=1` (log seriali).
- Seriale HWCDC best-effort: `monitor_dtr=0/monitor_rts=0`, premere RESET dopo
  aver aperto il monitor. In campo muto.

---

## 6. Bug recenti risolti (per non reintrodurli)

- **Blocco dopo primo scocco (Fase 6b→6c):** buffer path `char[40]` troppo
  piccoli per i nomi cartella datati (`/ARCHBB/SESS_20260721_1430/burst_...` =
  fino a 42 char). Il path troncato mandava `SD.open` in blocco → loop congelato.
  Risolto portando TUTTI i buffer path a `char[64]`. **Lezione:** allargare un
  buffer significa allargare tutta la sua catena a valle.
- **Tap-lungo che non funzionava (Fase 5):** il polling CST816 con buchi
  intermittenti azzerava il cronometro del long-press. Risolto abolendo il
  tap-lungo: qualunque tocco = toggle sessione (non c'è più nulla da cronometrare).

---

## 7. Metodo (invariato)

File completi versionati (mai patch), commenti italiani didattici, un pezzo alla
volta validato sul campo. **I dati comandano**: nessun parametro cambiato senza
evidenza. Non difendere un modello contro l'assenza di evidenza (es. non
aggiungere un mutex I2C "per sicurezza" senza un blocco osservato).

---

## 8. Ordine consigliato per la sessione BLE

1. **Decisioni di merito §3** (trasferimento, servizi GATT).
2. **BLE base su Core 0**: NimBLE, advertising, un servizio "config" read/write
   della struct `Config` (già pronta). Test: l'app legge e scrive i parametri.
3. **Servizio "time"**: `rtc_set` dallo smartphone. Test: l'ora diventa reale,
   le sessioni si datano corrette.
4. **Setter runtime** per trigger/finestre in `config_apply` (completamento §2.3).
5. **Sessioni via BLE** (se scelto §3.1): elenco + download, con la disciplina
   anti-crash della §4 (onStatus + connection interval reale).
6. **Companion app** (HTML/PWA, Web Bluetooth) che parla col tutto.

---

*Handoff BLE — 21 luglio 2026 — pronto per la nuova sessione.
Prima le decisioni di merito (§3), poi il GATT, poi il codice. E occhio al
connection interval (§4). 🎯*
