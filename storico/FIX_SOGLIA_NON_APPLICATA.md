# ArchBB 1.83 — La causa vera: la soglia del config non arrivava MAI al trigger

**Versione `1.83-Fase8-soglia`** · diagnosi dai dati del 25/07

---

## La causa (finalmente con la prova nel codice)

`config_apply()` cablava **solo** il montaggio. Non toccava il trigger. E
`trigger_init()` non leggeva `g_config`: usava la costante di compilazione
`TRIG_DEFAULT_THRESHOLD`.

**Quando hai abbassato la soglia a 10, il valore e' finito in NVS ma non e' mai
arrivato al trigger, che ha continuato a usare 20.** Ecco perche' "abbassare la
soglia non serviva a niente": non veniva applicata.

E' il limite §2.3 dell'handoff, documentato da mesi, mai collegato a questo
sintomo.

## I dati che lo dimostrano (log continuo raw_diag, nessun trigger di mezzo)

| evento | picco \|az\| misurato |
|---|---|
| **tiri veri (3)** | **11.6 · 12.8 · 16.4** m/s^2 |
| scuotimenti a mano (9) | 15.2 ... 29.9 m/s^2 |
| rumore di fondo (p99) | 9.6 m/s^2 |

Con la soglia REALE di 20: nessun tiro vero la supera (max 16.4) → mai
rilevati. Gli scuotimenti arrivano a 29.9 → sempre rilevati.
**Esattamente il sintomo osservato.**

### Simulazione sui tuoi dati
| configurazione | tiri veri rilevati |
|---|---|
| vecchia (soglia 20 non applicata) | **0 / 3** |
| nuova (soglia 10, operativa) | **3 / 3** |

## Perche' la 1.83 vede meno della 1.69

Sulla 1.69 i tiri picchiavano a **38 m/s^2**; sulla 1.83 a **16**. A parita' di
gesto la 1.83 misura **meta' accelerazione lineare ma PIU' rotazione**
(gy 67 dps contro 42). Su un corpo rigido la velocita' angolare e' identica
ovunque, mentre l'accelerazione lineare cresce con la distanza dal centro di
rotazione: **il chip della 1.83 sta piu' vicino al perno dell'arco.**

Non e' un difetto di montaggio: e' geometria. Ma spiega perche' la soglia
tarata sulla 1.69 e' inadatta a questa scheda.

## Il fix

1. **`trigger_configure()`** (nuova): applica soglia, confirm_n, rearm e finestre
   al trigger, con clamp difensivi.
2. **`config_apply()`** ora la chiama: i parametri del config diventano
   finalmente operativi (chiude l'handoff §2.3).
3. **`trigger_init()`** applica il config in coda: cosi' l'ordine di
   inizializzazione non puo' piu' tradire (prima `trigger_init` sovrascriveva
   le finestre gia' configurate).
4. **Soglia di default 20 → 10**, derivata dai dati misurati su questa scheda.
5. **`CONFIG_VERSION` 2 → 3**: forza la NVS a ripartire dai nuovi default (un
   config v2 conterrebbe ancora 20).

## Margine: onesto, e' STRETTO

Soglia 10 contro il tiro piu' debole misurato a 11.6 = **16% di margine**, su
soli **3 tiri**. Prende tutti e 3 con 3 falsi positivi in ~5 minuti di
manipolazione intensa (durante il tiro vero, senza scuotimenti, saranno meno).

**Da fare alla prossima sessione:** una serie piu' ampia (15-20 frecce) per
vedere la vera distribuzione dei picchi. Se il tiro piu' debole scendesse sotto
10, servira' abbassare ancora — oppure, meglio, **montare la scheda piu' lontano
dal perno di rotazione** (piu' in alto o piu' in basso sul riser): l'ampiezza
del segnale cresce con la distanza e il margine tornerebbe comodo.

## File toccati
- `src/trigger.h` / `src/trigger.cpp` — `trigger_configure()`; `trigger_init()`
  applica il config in coda.
- `src/config_store.cpp` — `config_apply()` chiama `trigger_configure()`.
- `src/config_store.h` — `CONFIG_VERSION = 3`.
- `src/config.h` — soglia default 10 + documentazione della misura; versione.

## Da provare
1. **Tiri veri: devono essere rilevati.** E' il test che conta.
2. Se comparissero falsi trigger durante trazione/mira, alza la soglia di 1-2
   per volta (ora **funziona davvero**: prima non aveva effetto).
3. Verifica nel CSV che `az_peak` dei tiri stia sui 12-17 m/s^2: e' la firma
   attesa su questa scheda.
