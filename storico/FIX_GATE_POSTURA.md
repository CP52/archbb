# ArchBB 1.83 — Gate di postura: l'arco dev'essere in posizione di tiro

**Versione `1.83-Fase9-postura`** · dalle due sessioni del 25/07

---

## Cosa hanno detto i dati

La sessione con **cambio di montaggio a meta'** (frontale → −90) e' stata
preziosa: dimostra che `mount_apply` funziona correttamente (con entrambi i
montaggi, a riposo la gravita' finisce su ax ≈ +9.6) e isola il vero problema
residuo.

Il criterio `| |a|−g |` aveva gia' eliminato i falsi da **inclinazione statica**
(arco appoggiato). Restavano i falsi da **maneggio**: spostare o urtare l'arco
produce movimento vero, e il trigger non poteva distinguerlo.

### La misura che risolve (19 eventi, 2 sessioni, 2 montaggi)

| | inclinazione dalla verticale |
|---|---|
| **11 tiri veri** | **13.0° – 18.5°** (arco impugnato, in mira) |
| 8 falsi | 25.5° · 46.3° · 53.9° · 54.0° · 57.5° · 65.3° · 80.2° · 91.3° |

**L'arco in posizione di tiro sta entro ~20° dalla verticale.** Quando lo si
maneggia e' molto piu' inclinato. Separazione quasi perfetta.

## Il fix: gate di postura

Il trigger stima la **direzione della gravita'** con una media esponenziale
lenta (τ ≈ 1 s) del vettore accelerazione — abbastanza lenta da non essere
trascinata dallo scocco (~50 ms), abbastanza veloce da seguire l'arciere che
alza e abbassa l'arco.

Quando la condizione di movimento e' soddisfatta, il trigger **conferma solo se
la scheda e' entro 31.8° dalla verticale** (`TRIG_POSTURE_GATE = 0.85`).

Funziona con **qualsiasi montaggio**: `mount_apply` normalizza sempre
ax = verticale, e il dato lo conferma (frontale e −90 danno entrambi ax ≈ +9.6).

## Verifica su tutti i dati

| | esito |
|---|---|
| sessione 12:07 (9 veri, 3 falsi) | **11/12** — tutti i veri, 2 falsi su 3 respinti |
| sessione 11:33 (2 veri, 5 falsi) | **7/7** — perfetto |
| **totale** | **18/19 corretti** |

L'unico residuo e' un urto avvenuto con l'arco quasi in postura (29.6°). Alzando
il gate a `0.88` (28.4°) verrebbe respinto anche quello — 19/19 — ma il margine
sui tiri con alzo maggiore (distanze lunghe) si assottiglierebbe. Il valore e'
in `config.h`, una riga: se i falsi da maneggio danno fastidio, alzalo.

## Riepilogo dei tre difetti risolti

1. **La soglia del config non arrivava al trigger** (`config_apply` cablava solo
   il montaggio) → cambiarla non aveva effetto.
2. **Il criterio `|az|` conteneva la gravita'** → il trigger misurava
   l'inclinazione, non lo scocco. E su questa scheda l'energia arriva
   prevalentemente su ax, non su az.
3. **Nessun filtro di postura** → il maneggio dell'arco veniva scambiato per un
   tiro.

## File toccati
- `src/config.h` — `TRIG_POSTURE_GATE`, `TRIG_GRAVITY_TAU_S` + documentazione
  della misura; versione.
- `src/trigger.cpp` — stima della gravita' (EMA) aggiornata a ogni campione;
  gate applicato prima della conferma.

## Da provare
1. I tiri veri devono continuare a essere rilevati (11/11 nei dati).
2. **Appoggia, sposta, urta l'arco**: non deve piu' scattare.
3. Se restasse qualche falso da urto in postura, alza `TRIG_POSTURE_GATE` a 0.88.
