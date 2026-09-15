# ArchBB 1.83 — Taratura della soglia: perche' il tiro a -90 si era perso

**Versione `1.83-Fase9-soglia4`**

---

## Correzione: erano 19/19, non 18/19

Il "falso" numero 9 era uno **scossone intenzionale con l'arco in postura**:
il firmware ha fatto bene a rilevarlo. Con quella correzione il bilancio della
sessione precedente e' **perfetto**.

## Il tiro perso: era AMPIEZZA, non timing

La tua ipotesi del timing (finestra non ancora aperta) e' ragionevole ma i dati
indicano altro. Guarda i margini sulla soglia allora in vigore (6.0):

| tiro | montaggio | \|\|a\|-g\| | margine |
|---|---|---|---|
| 2,3,4,5,6 | frontale | 9.8 – 15.7 | +3.8 … +9.7 |
| 8 | −90 | 8.7 | +2.7 |
| **10** | **−90** | **6.1** | **+0.1** ⚠ |
| 11 | −90 | 10.2 | +4.2 |
| 12 | −90 | 12.6 | +6.6 |

Il tiro 10 e' passato per **un decimo**. Un tiro appena piu' debole finiva sotto
soglia: e' quasi certamente quel che e' successo a quello perso.

**Il montaggio a −90 da' un segnale 1.2x piu' debole del frontale**
(mediana 9.5 contro 11.8): con quella soglia si lavorava sul filo.

## La misura che ha permesso di abbassare la soglia

La domanda giusta non e' "quanto e' forte lo scocco" ma **"quanto rumore c'e'
durante trazione e mira"** — l'unica fase in cui il gate di postura non protegge,
perche' l'arco e' gia' in posizione di tiro.

```
rumore in trazione/mira (9 tiri) : 0.95 – 1.40 m/s^2
scocco piu' debole misurato      : 6.1 m/s^2
```

**Divario 4.4x.** La vecchia soglia 6.0 era inutilmente vicina al segnale
(1.02x) invece che al rumore.

## Nuova soglia: 4.0

- **2.9x sopra** il rumore di trazione/mira (1.40)
- **1.5x sotto** il tiro piu' debole misurato (6.1)
- Recupera tutti i tiri nella banda 4.0 – 6.0 che prima venivano persi

### Verifica su tutti i dati
| sessione | esito |
|---|---|
| 12:07 (10 veri incl. scossone, 2 falsi) | **12/12** |
| 11:33 (2 veri, 5 falsi) | **7/7** |
| **totale** | **19/19** |

## Sul timing (l'altra ipotesi, per completezza)

Il rearm dura 2 s, quindi con tiri ogni ~30 s non e' un problema. **Ma c'e' un
caso reale da conoscere:** dopo un tiro il trigger resta DISARMATO finche' non
completi lo scoring e torni in ATTESA. Se tiri mentre sei ancora nella
schermata di punteggio o nel RIEPILOGO, quel tiro **non viene rilevato**.

Se ti ricapita un tiro perso, guarda cosa mostrava il display in quel momento:
se era ancora la schermata di scoring, e' questa la causa e non l'ampiezza.

## Consiglio pratico
Se puoi scegliere, **il montaggio frontale e' preferibile**: segnale 1.2x piu'
forte, quindi piu' margine. Il −90 funziona ma lavora piu' vicino al limite.

## File toccati
- `src/config.h` — soglia default 6.0 → 4.0 con la documentazione della misura;
  versione.
- `src/config_store.h` — `CONFIG_VERSION = 5` (forza il nuovo default).
