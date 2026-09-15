# ArchBB 1.83 — Registratore scocco: prendiamo il dato che manca

## Perche' cambio approccio
Ho fatto quattro ipotesi (soglia, campionamento del trigger, geometria, filtro
LPF) e nessuna ha risolto. Il motivo di fondo e' semplice e va detto chiaro:

> **Non abbiamo un solo campione di uno scocco vero.** Tutti i burst analizzati
> finora erano scuotimenti a mano.

Finche' non si vede cosa legge DAVVERO il sensore quando parte la freccia, ogni
teoria e' infalsificabile — e continuare a tirare a indovinare fa perdere
sessioni di campo. Questo firmware non decide e non giudica: **registra tutto**.

## Cosa fa
- Registra **ogni** campione IMU in PSRAM: nessun trigger, nessuna soglia,
  nessun filtro applicato dal firmware.
- **Peak-hold a schermo**: dopo il tiro leggi subito il massimo |ax| |ay| |az|
  raggiunto. Risposta immediata, senza aspettare l'analisi al PC.
- A STOP scarica tutto su microSD: `/ARCHBB/REC/rec_NNNN.bin`.
- Registra in RAM (non su SD durante la cattura): zero latenze di scrittura che
  potrebbero alterare proprio l'istante che ci interessa.
- Durante la REC **non fa mai un ridisegno pieno** dello schermo (bloccherebbe il
  loop per decine di ms, perdendo i campioni dello scocco): aggiorna solo i tre
  numeri, e legge il touch a bassa frequenza.

## ODR 500 Hz (non 250)
Due motivi, entrambi utili alla diagnosi:
1. **Risoluzione temporale doppia**: un impulso di 10 ms passa da ~2 a ~5
   campioni, quindi la forma d'onda diventa leggibile.
2. Il filtro interno del QMI8658 (`LPF_MODE_2`) taglia a una **percentuale
   dell'ODR**: raddoppiando l'ODR raddoppia la banda passante. **Se lo scocco
   fosse ben visibile a 500 Hz e non a 250, avremmo la prova che ODR/filtro
   sono la causa** — e sarebbe anche la spiegazione del perche' la 1.69 ha
   iniziato a "faticare".

## Come si usa
```
pio run -e archbb_183_shotrec -t upload
pio device monitor -b 115200
```
(In VS Code: seleziona **env:archbb_183_shotrec** nella barra di stato PlatformIO
*prima* di premere Upload — il default e' il firmware di produzione.)

1. Monta la scheda sul riser **come sempre** (posizione 0).
2. Tocca lo schermo: parte **REC** (fino a ~40 s, poi salva da solo).
3. **Tira 2-3 frecce vere.** Non serve altro.
4. Tocca di nuovo: **STOP**, salva su SD.
5. **Leggi subito il picco aZ a schermo.**

## La risposta te la da' gia' il display
| picco aZ letto | significato |
|---|---|
| **> 20 m/s2** (verde) | Il sensore VEDE lo scocco. Allora il problema e' nella logica del trigger, e ora sappiamo dove cercare. |
| **< 20 m/s2** (ambra) | Il sensore NON vede l'impulso: e' attenuato/filtrato prima di arrivare al firmware. Nessuna soglia potrebbe mai funzionare. |
| picco alto su **aX o aY** invece che aZ | L'energia dello scocco e' su un altro asse: il montaggio/geometria va rivisto (e la soglia va spostata di asse). |
| ~**78 m/s2** su qualche asse | Saturazione del fondo scala 8g: va alzato a 16g. |

Qualunque sia il risultato, **e' un dato**, non un'ipotesi.

## Poi mandami il file
Mandami `rec_NNNN.bin` (e dimmi quanti tiri hai fatto e all'incirca quando).
Dal file ricavo: forma d'onda completa dello scocco, durata reale dell'impulso,
ampiezza su ogni asse, contenuto in frequenza (per capire se il filtro lo sta
mangiando) ed eventuale saturazione. Da li' la soglia e la logica del trigger si
tarano sui numeri veri, una volta sola.

## Formato del file (per riferimento)
Header 16 B: magic `ABBR` · versione u16 · n_campioni **u32** · odr_hz u16 ·
flags u16 (bit0 = frame di montaggio applicato) · riservato.
Poi n x 30 B, struct `ImuSample` packed identica ai burst
(`seq u16`, `ts_us u32`, `ax ay az gx gy gz` float).

## Nota
Il firmware di produzione nel pacchetto e' invariato (`1.83-Fase8-trigfix-r2`,
con il fix del trigger e la revisione). Il registratore e' un env separato: non
tocca nulla, e quando avremo il dato torniamo al firmware normale con la taratura
giusta.
