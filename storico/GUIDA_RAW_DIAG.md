# ArchBB 1.83 — Diagnostica scocco reale (peak-hold + log continuo)

**env `archbb_183_raw_diag`** · versione `1.83-Fase8-rawdiag`

---

## Prima di tutto: la mia diagnosi precedente era SBAGLIATA

Avevo detto che lo scocco fosse un impulso da 2-3 campioni e che il trigger lo
"saltasse". **I tuoi 44 tiri veri della 1.69 lo smentiscono:**

| misura | valore reale |
|---|---|
| asse dominante al picco | **az in 44 tiri su 44** |
| picco mediano su az | **38.4 m/s^2** |
| campioni consecutivi sopra 20 | mediana **13** (minimo 5) |
| tiri con picco sotto la soglia di 20 | **zero** |

Uno scocco vero e' un evento **largo (~60 ms) e forte**, piu' facile da
rilevare di uno scuotimento. Il vecchio trigger l'avrebbe preso senza problemi.
Avevo costruito la teoria su un'assunzione inventata invece che sui dati.

Il fix "valuta ogni campione" resta nel firmware perche' e' comunque piu'
corretto (e innocuo), ma **non era la causa** del tuo problema.

---

## Il vero problema: non abbiamo NESSUN dato di uno scocco sulla 1.83

Il trigger non scatta -> nessun burst salvato -> nessun dato. Stiamo ragionando
al buio. Questo firmware serve a togliere il buio: registra **senza alcun
trigger**, quindi cattura lo scocco comunque.

**Le due sole spiegazioni possibili, e sono distinguibili con UNA misura:**

1. **L'energia c'e' ma finisce su un altro asse** -> vedrai un picco grosso su
   `aY` o `aX` e `aZ` piccolo. Significa che la geometria/montaggio della 1.83
   non e' quella che crediamo, e il trigger (che guarda solo az) e' cieco.
2. **L'energia non arriva al sensore** -> tutti e tre i picchi piccoli.
   Significa che qualcosa smorza (aggancio non rigido, case che flette,
   posizione sul riser che non raccoglie il recoil).

---

## Come si usa

```
pio run -e archbb_183_raw_diag -t upload
```
⚠️ Seleziona l'ambiente ESPLICITAMENTE (barra di stato PlatformIO in VS Code)
prima di premere Upload: il default e' il firmware di produzione.

**Comandi touch:**
- **meta' SUPERIORE dello schermo** -> azzera i picchi
- **meta' INFERIORE** -> avvia / ferma la registrazione su SD

**Procedura consigliata (5 minuti sul campo):**

1. Monta la scheda sul riser **come sempre**, accendi.
2. Tocca SOTTO per avviare la registrazione (compare `REC` in rosso).
3. Tocca SOPRA per azzerare i picchi.
4. **Tira UNA freccia.**
5. Guarda subito il display: leggi `aX`, `aY`, `aZ`. Il valore piu' alto e'
   evidenziato in verde. Annotali.
6. Ripeti 3-4 volte (azzera i picchi prima di ogni tiro).
7. Tocca SOTTO per fermare la registrazione (compare "salvato N campioni").
8. Estrai la microSD e mandami i file `/ARCHBB/RAW_*.BIN`.

**Bonus utile:** fai anche un tiro **a vuoto (a secco)** o un colpetto secco sul
riser, per confronto. E rifai lo scuotimento a mano che invece funziona: cosi'
ho i due estremi nello stesso log.

---

## Cosa guardo io nei file

Nel pacchetto trovi `tools/leggi_raw.py`:
```
python leggi_raw.py RAW_0001.BIN
python leggi_raw.py RAW_0001.BIN --csv uscita.csv
```
Stampa: ODR reale, integrita' del log (salti di `seq`), picchi per asse, e
individua gli eventi sopra soglia con durata e ampiezza — da confrontare
direttamente con la firma dei 44 tiri della 1.69.

---

## Nota tecnica sul firmware
- Non c'e' **nessun trigger**: solo `imu_task` che alimenta un hook
  (`raw_diag_feed`) chiamato per OGNI campione.
- Il log usa un **doppio buffer** (256 campioni = 7.7 KB ciascuno): l'hook non
  tocca mai la SD, scrive solo in RAM; il loop UI scrive i blocchi pieni. Cosi'
  la scrittura non rallenta il campionamento. Se la SD non stesse dietro, il
  display mostrerebbe "persi N chunk" (non dovrebbe succedere: servono 6.7 KB/s).
- I file sono `/ARCHBB/RAW_NNNN.BIN`, stesso layout campioni dei burst.
