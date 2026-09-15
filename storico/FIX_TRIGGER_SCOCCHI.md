# ArchBB 1.83 — Fix: gli scocchi VERI non venivano rilevati

**Versione: `1.83-Fase8-trigfix`** · dai dati della sessione 24/07

## Il sintomo
Scuotimento a mano -> trigger OK. Freccia vera -> **nessun rilevamento**.
Abbassare la soglia fino a 10 m/s^2 non cambiava nulla. Anche la 1.69, nelle
ultime sessioni, "faceva fatica".

## La causa: il trigger SALTAVA campioni
Non era la soglia. Era la frequenza con cui il trigger *guardava* i dati.

- `imu_task` produce un campione ogni **~4.5 ms** (224 Hz) — e li vede tutti.
- `trigger_task` faceva **polling** di `g_latest_sample` ogni **~3 ms**
  (`vTaskDelay(3)`), leggendo solo *l'ultimo campione disponibile*.
- I due ritmi non sono sincronizzati: il trigger vedeva solo ALCUNI campioni.

Conseguenza, misurata sui dati reali della sessione:

| evento | durata sopra soglia | campioni | vecchio trigger |
|---|---|---|---|
| scuotimento a mano (burst 1-4) | 40-60 ms | 9-13 | rileva **100%** |
| scocco vero | ~10 ms | 2-3 | rileva **28-55%** |

Con 9-13 campioni sopra soglia, anche saltandone la meta' il trigger raccoglie
i `confirm_n=2` consecutivi che gli servono -> scatta. Con 2-3 campioni, basta
perderne uno e la sequenza si spezza -> **non scatta mai**. Ecco perche'
abbassare la soglia era inutile: i campioni del picco non venivano *guardati*,
non erano *troppo piccoli*.

## Il fix: valutare OGNI campione, dove passano tutti
La valutazione si sposta in `trigger_feed_sample()`, chiamata da `imu_task` per
**ogni** campione, subito dopo il push nel buffer circolare.

- **Zero campioni persi per costruzione** (non per dimensionamento di una coda).
- **Nessuna latenza**: il fire avviene nell'istante del campione, quindi il
  `circular_buffer_freeze` cattura la finestra giusta.
- `trigger_task` resta, ma solo per le transizioni **temporali**
  (IDLE/ARMED, REARM allo scadere di `rearm_ms`), che dipendono da `millis()` e
  non dai dati. Periodo rilassato a 10 ms: meno risvegli, piu' CPU all'IMU.

Rimossa `s_last_seq` (il controllo di "freschezza" serviva solo col polling: ora
ogni campione arriva una volta sola, per costruzione).

## Verifica (simulazione sui dati reali + impulso sintetico)

| segnale | vecchio | nuovo |
|---|---|---|
| burst 1-4 reali (scuotimenti) | 100% | 100% |
| scocco 2 campioni (~9 ms) | 28.5% | **100%** |
| scocco 3 campioni (~14 ms) | 55.2% | **100%** |
| scocco 4 campioni (~18 ms) | 76.2% | **100%** |

Il vecchio comportamento riproduce esattamente il sintomo osservato sul campo.

## Nota sui dati della sessione 24/07
I 4 burst sono integri: `seq` consecutivi, dt 4-5 ms, ODR reale 218 Hz, 672
campioni con `trigger_idx=448`. **La catena IMU -> buffer -> SD funziona
perfettamente**: il difetto era solo nel percorso di *decisione* del trigger.
Le metriche calcolate su quei burst sono valide (di scuotimenti, pero').

## File toccati
- `src/trigger.h` — dichiarazione e documentazione di `trigger_feed_sample()`.
- `src/trigger.cpp` — implementazione della valutazione per-campione;
  `s_fired_ms` statica; `trigger_task` ridotto alle transizioni temporali;
  rimossa `s_last_seq`.
- `src/imu.cpp` — include `trigger.h`; chiamata a `trigger_feed_sample(sample)`
  per ogni campione, dopo il push nel buffer.
- `src/config.h` — versione -> `1.83-Fase8-trigfix`.

## Da provare sul campo
1. **Rimetti la soglia al valore normale** (20 m/s^2): non serve piu' abbassarla,
   e con 10 rischi falsi positivi ora che il trigger vede tutto.
2. Tiri veri: devono essere rilevati in modo affidabile.
3. Controlla che NON ci siano falsi trigger durante l'armo/mira (il trigger ora
   e' piu' sensibile perche' non perde campioni: se comparissero falsi positivi,
   la strada e' alzare `confirm_n` a 3, non alzare la soglia).
4. Nel CSV, `az_peak` dovrebbe ora riflettere il picco vero dello scocco.
