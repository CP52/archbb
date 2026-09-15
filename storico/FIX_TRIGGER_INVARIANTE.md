# ArchBB 1.83 — Il trigger misurava l'INCLINAZIONE, non lo scocco

**Versione `1.83-Fase9-accdev`** · diagnosi dalla sessione SESS_20260725_1133

---

## La causa, finalmente

Il trigger usava `|az| > soglia`. **`|az|` contiene la gravita'.**

Quando la scheda si inclina — arco abbassato fra un tiro e l'altro — la gravita'
si sposta sull'asse az. A RIPOSO `|az|` vale gia' **7-8.5 m/s^2**: basta un
fremito da 2 per superare la soglia. Il trigger scattava sull'INCLINAZIONE.

### La prova, dai 7 eventi della tua sessione

| | az a riposo | movimento reale |
|---|---|---|
| **2 tiri veri** | −2.5 (scheda dritta) | **16.0 · 17.4** |
| **5 falsi allarmi** | **−7.5 … −8.5** (gravita' su az) | **1.2 … 3.0** |

I 5 falsi **non avevano quasi movimento**: nell'intero burst da 3 secondi non
c'e' un solo campione con movimento sopra 6. Erano pura inclinazione.

## Perche' la 1.69 funzionava e la 1.83 no

Due effetti sommati, entrambi ora misurati:

1. **Asse diverso.** Sulla 1.69 lo scocco si manifestava su az (44 tiri su 44).
   Sulla 1.83 l'energia arriva prevalentemente su **ax**: il chip e' piu' vicino
   al perno di rotazione dell'arco (a parita' di gesto misura meta'
   accelerazione lineare ma piu' rotazione: gy 67 dps contro 42).
   Una soglia su |az| e' quindi cieca proprio dove il segnale c'e'.
2. **Gravita' sull'asse sbagliato.** Il montaggio della 1.83 fa si' che a riposo
   la gravita' finisca su az quando l'arco e' abbassato — cosa che sulla 1.69
   non accadeva. Da qui i falsi.

Non era ne' la matrice di montaggio ne' il campionamento: era il **criterio**.

## Il fix: `| |a| − g |`

Nuovo modo `TRIG_ACCEL_DEV` (default): si confronta con la soglia il **modulo
del vettore accelerazione meno la gravita'**.

- **Invariante all'orientamento**: a riposo vale ~0 comunque sia inclinata la
  scheda. Niente piu' falsi da inclinazione.
- **Invariante all'asse**: non importa se lo scocco arriva su ax, ay o az.
  Risolve anche il problema dell'asse diverso fra 1.69 e 1.83.

### Separazione misurata (2 sessioni indipendenti)
```
tiri veri : 8.5 · 8.6 · 14.3 · 16.0 · 17.4
falsi     : 1.3 · 2.2 · 2.8 · 2.9 · 3.0
```
**Soglia 6**: margine 2x sopra i falsi, 1.4x sotto il tiro piu' debole.

### Verifica su TUTTI i dati reali disponibili
| dataset | esito |
|---|---|
| sessione 25/07 (2 veri + 5 falsi) | **7/7 corretti** — veri rilevati, falsi respinti |
| log RAW 24/07 (3 tiri veri) | **3/3 rilevati** |
| 315 s di log continuo | **0 eventi spuri** |

## File toccati
- `src/config.h` — `TRIG_ACCEL_DEV` (modo 2, nuovo default); soglia default 6;
  documentazione della misura.
- `src/trigger.cpp` — valutazione del nuovo criterio; include `shot_angles.h`
  per `SHOT_ANGLE_GRAVITY` (costante unica, mai duplicata).
- `src/config_store.h` — `CONFIG_VERSION = 4` (forza i nuovi default);
  commento sul campo `trig_mode`.
- `src/config_ui.cpp` — etichetta del modo a display ("|a|-g").

## Da provare
1. **I 3 tiri che prima non venivano rilevati**: verifica che ora scattino.
2. **I falsi allarmi devono sparire**: abbassa l'arco fra un tiro e l'altro,
   inclinala, appoggiala — non deve succedere nulla.
3. Se qualche tiro debole sfuggisse ancora, abbassa la soglia a 5 (ora i
   parametri del config sono davvero operativi). Se comparissero falsi, alzala
   a 7. Con margine 2x/1.4x dovrebbe esserci spazio.

## Nota di metodo
Questa sessione e' stata risolutiva perche' conteneva **entrambe le classi**:
tiri veri E falsi allarmi, stesso montaggio, stesso minuto. Confrontare i due
gruppi ha reso evidente in un colpo cio' che giorni di ipotesi non avevano
trovato. Quando un rilevatore sbaglia, servono gli errori di ENTRAMBI i segni.
