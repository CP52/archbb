> **AGGIORNAMENTO — FASE 13 (28 luglio 2026), firmware `1.83-F13-scarta`**
>
> Questo README descrive la Fase 5. La versione corrente del progetto include
> anche le fasi successive; l'ultima è il **download delle sessioni via BLE**.
> Documento di riferimento: **`README_FASE10_DOWNLOAD.md`**.
> App companion aggiornata: **`app/ArchBB_183_App_v0_2.html`**
> (la `v0_1` resta nella cartella solo per confronto).

---

# ArchBB 1.83 — Fase 5: persistenza microSD + stato batteria

Versione firmware: **`1.83-Fase5-microSD`** · 20 luglio 2026 · Cesare Pagura

Questa fase chiude il motivo originale della migrazione a 1.83: **portare il
burst catturato in PSRAM su microSD**, così le sessioni restano a bordo e
ri-analizzabili. Aggiunge inoltre la lettura della **batteria** (PMU AXP2101) e
una **barra di stato** in attesa.

---

## Cosa fa (in una riga)

Al tap **OK** del riepilogo, il tiro viene salvato su microSD: una riga in un CSV
leggibile **più** il burst grezzo binario ri-analizzabile. Tra un tiro e l'altro,
una barra in cima mostra **batteria %**, **spazio SD libero** e lo **stato della
sessione**. Un **tap-lungo** (1.5 s) in attesa apre/chiude la sessione.

---

## Le decisioni di merito (prese, con motivazione)

| Decisione | Scelta | Perché |
|---|---|---|
| Cosa salvare (§3.1) | **CSV risultati + burst grezzo** | i grezzi servono a rifare i conti quando affinerai le soglie hold/release; 20 KB a tiro sono spiccioli su 32 GB |
| Trasferimento (§3.2) | **SD estraibile FAT32** | il BLE 1.83 non esiste ancora (→ Core 0, capitolo suo); `SD.begin()` funziona subito |
| Struttura file | **1 cartella per sessione**, CSV ad append + 1 .bin per tiro | separa il dato prezioso (CSV, non ricreabile) dal sacrificabile (burst); un crash perde al massimo l'ultimo tiro |
| Secondo tasto (§3.3) | **tap-lungo su schermo** | il tasto PWR è cablato all'AXP2101 (serve polling I2C del PMU): rimandato |
| Tiro senza sessione | **auto-apre una sessione** | l'arciere che tira senza aver premuto "start" non perde il dato |
| Feedback SD | **visivo** (barra), beep rimandato | il codec ES8311/I2S è pesante per un solo bip: prima la SD funzionante |

---

## Pin (verificati sul pinout ufficiale 1.83-S3, nessun conflitto)

**microSD — bus SPI dedicato (host FSPI), separato dal display:**

| Segnale | GPIO |
|---|---|
| MOSI | 1 |
| SCK | 2 |
| MISO | 3 |
| CS | 42 |

**Batteria — PMU AXP2101 sul bus I2C già condiviso** (SDA=15/SCL=14, come IMU e
touch), indirizzo 0x34, via `XPowersLib`. Nessun pin nuovo.

---

## Struttura dei file su card

```
/ARCHBB/
   SESS_0001/
      shots.csv         una riga per tiro (append + flush): DATO PREZIOSO
      burst_0001.bin    burst grezzo tiro 1 (header 16B + n×30B ImuSample)
      burst_0002.bin
      session.txt       metadati: fw, formati, ODR, avvio/fine, n. tiri
   SESS_0002/ ...
```

Il numero di sessione e il numero di tiro sono **derivati dal filesystem** (prime
cartelle/righe libere), non da un contatore in RAM: robusti ai reset.

### Colonne di `shots.csv`

```
shot, ts_ms, colpito, zona, dist_m, elev_deg, arcs,
cant_cdeg, alzo_cdeg, angoli_stabili,
hold_cdeg, hold_class, release_jerk, release_class, burst_file
```

Un **campo vuoto** (due virgole di fila) è la sentinella di "non calcolato" —
coerente con i flag `*_valid` del firmware: un `hold` non valido non scrive `0`
(che sarebbe "tenuta perfetta"), scrive niente. I campi nuovi vanno **sempre in
coda**: i file vecchi restano leggibili perché l'app mappa per nome colonna.

### Formato del burst binario (`.bin`)

Header 16 byte, poi `n × 30 byte` di `ImuSample` così come sono in RAM (frame
canonico: `ax`=gravità, `ay`=laterale, `az`=asse freccia). Zero conversioni.

```
header: magic "ABB1"(4) | ver(1) | odr_code(1) | n(uint16) |
        trig_idx(uint16) | shot(uint16) | riservati(4)
sample: seq(uint16) | ts_us(uint32) | ax ay az gx gy gz (6×float)   = 30 B
```

---

## Ri-analisi a freddo (lo strumento è incluso)

`tools/leggi_sessione.py` legge una cartella `SESS_NNNN` copiata dalla card:

```bash
# riepilogo della sessione
python3 tools/leggi_sessione.py /percorso/ARCHBB/SESS_0001

# dettaglio grezzo del burst di un singolo tiro (assetto PRE, durata reale, campioni)
python3 tools/leggi_sessione.py /percorso/ARCHBB/SESS_0001 --burst 3
```

`tools/analisi_firmware.py` ricalcola le metriche con gli **algoritmi esatti
dell'app 1.27** (finestra calma adattiva, stability esponenziale τ=5, cant/alzo,
hold/release). Verificato sui dati di campo: riproduce il CSV del firmware al
centesimo (release_jerk identico, cant/alzo entro pochi centesimi). È lo
strumento per **ritarare le soglie**: cambi `TAU_DPS`, `HOLD_SOGLIE` o
`RELEASE_SOGLIE` in cima al file, rilanci sui burst salvati, vedi l'effetto
senza ri-tirare.

```bash
# ricalcolo con confronto vs CSV del firmware
python3 tools/analisi_firmware.py /percorso/ARCHBB/SESS_0001 --confronta
```

**Nota misurata sui dati veri:** il campionamento effettivo è ~200 Hz reali (dai
timestamp), non i 224 nominali scritti in session.txt. Gli algoritmi usano i
timestamp reali (`burstSampleRate`), quindi il ricalcolo è corretto comunque;
ma è un fatto da ricordare, come il beating del tick FreeRTOS sulla 1.69.

Entrambi gli script usano solo la standard library. È il motivo per cui salviamo
i grezzi: rifare angoli/hold/jerk con parametri diversi senza ri-tirare.

---

## Come si compila e si usa

```bash
# campo (muto, default)
pio run -e archbb_183_touchcal -t upload

# tavolino (log + CSV DIAG seriale)
pio run -e archbb_183_debug -t upload
```

Al boot, la seriale (env debug) riporta lo stato: `IMU`, `buffer`, `mount`,
`BATT`, `SD` (con MB liberi/totali). In campo tutto muto, come da disciplina.

### Flusso in attesa

- **tap breve** → tiro manuale (fallback a banco, come prima)
- **tap-lungo (1.5 s)** → apre/chiude la sessione SD (badge `REC` nella barra)
- **freccia reale** → trigger IMU, come sempre

---

## Cosa resta da validare sul campo

- Che la card monti davvero sui pin 1/2/3/42 (log SD al boot lo conferma).
- Che il tap-lungo non interferisca col tap breve (soglia 1.5 s: se troppo
  sensibile o troppo lenta, è un solo numero — `LONGPRESS_MS` in `main.cpp`).
- Robustezza a card estratta a caldo (il firmware prosegue senza salvare).

---

## Fase 6 — CONFIG in NVS (fatta in questo passo)

I parametri configurabili vivono ora in **NVS** invece che come costanti
compilate. Una `struct Config` in RAM è la fonte di verità a runtime; l'NVS la
persiste con **versione + checksum** (migrazione sicura ai default se la
versione cambia o il blocco è corrotto).

Parametri nel config: trigger (modo, soglia, conferma, rearm), finestre
pre/post, montaggio (indice enum — sulla 1.83 default 4 = FLIPZ), offset di
calibrazione (cant/alzo/G, a 0 = non calibrati, le procedure verranno dopo),
massa arco.

Editing on-device: **sola lettura** dei valori + **ripristina default** (voce
`CONFIG` nel menu d'avvio). L'editing dei valori verrà dal **BLE/app**. IMU/gyro
range restano cablati (8G / 512 dps): non esposti, per non dare all'utente un
piede di porco su un parametro che non deve toccare.

Il montaggio è l'unico parametro già operativo dal config in questo passo
(`config_apply` imposta `g_mount_orientation`). Trigger e finestre restano ai
default compilati — che coincidono col config — finché non aggiungeremo i loro
setter runtime (prossimo passo, insieme al BLE).

---

## Prossimi capitoli

1. **BLE 1.83** (→ Core 0) — editor del config NVS + trasferimento sessioni.
   Il config è già pronto a essere scritto da un secondo editor.
2. **Companion app** — parla col BLE: edita config (montaggio, data sessione,
   parametri) e scarica le sessioni.
3. **Procedure di calibrazione** (livella, G) — misurano gli offset già previsti
   nel config.
4. **Secondo tasto** hardware (se si vuole start/stop fisico via PWR/AXP2101).

---

*I dati comandano. File completi versionati, mai patch. Commenti italiani didattici.*
