# ArchBB — Analisi sessione 14 luglio 2026 (44 tiri) — Briefing handoff

## 1. Ricostruzione del dato

**Problema:** `BBarch_14JUL.csv` (pre-app v1.25) era corrotto: 3 header, 26 shot_id
di cui 18 con burst FUSI (1344 campioni = 2 tiri diversi sotto lo stesso id),
shot 26 mancante. Causa: lo shot_id veniva dal firmware, che riparte da valori
bassi a ogni reset/riconnessione → collisioni.

**Ricostruzione** (script `rebuild_csv.py`):
split dei burst fusi (1344 → 2×672), riordino per `ts_us`, rinumerazione 1..44,
id originale conservato in `orig_shot_id`.

**Risultato verificato:** 44 tiri, tutti 672 campioni, 0 sovrapposizioni
temporali, 29.568 righe conservate (nessuna persa/inventata), sessione continua
di 45.7 minuti.

**Deliverable:**
- `BBarch_14JUL_ricostruito.csv` (28 col, + orig_shot_id)
- `BBarch_14JUL_ricostruito_angoli.csv` (35 col, + alzo_calc/cant_calc,
  alzo_corr/cant_corr, g_mean/g_sd/win_stable)

## 2. Struttura del burst (confermata)
672 campioni @224.9 Hz = **2000 ms pre-scocco + 1000 ms post**, trigger_idx=448.

## 3. Offset di montaggio — scoperta chiave

La sessione è stata registrata **senza calibrazione a livella**: i dati portano
l'offset addosso.

**Prova:** cant medio dei 44 tiri = **−4.8°**, contro **−5.1°** misurato nel test
statico a banco (13/07f). Coincidono. Un arciere non tiene 5° di cant
sistematico: è montaggio, non gesto.

Sottraendo l'offset del test statico (alzo +9.3°, cant −5.1°) gli alzo diventano
fisicamente sensati:
- gruppo A (tiri 1-10):  **−6.0°** (bersagli sotto)
- gruppo B (tiri 11-38): **+6.3°** (bersagli sopra)
- gruppo C (tiri 39-44): **−6.7°** (bersagli sotto)

Coerente col campo 3D: sagome a distanze (~30 m medie, alcune 40-50) **e altezze
diverse**. Senza correzione l'alzo medio era +11.1°, incompatibile con qualsiasi
modello balistico.

→ **AZIONE: fare CALIBRA A LIVELLA prima di ogni sessione.**

## 4. Tiri anomali
Tiri **8 e 27**: alzo −96°/−100°, cant −71°/−87°, stability 27/29. Non sono tiri:
è la scheda maneggiata/riposta. **Esclusi da tutte le analisi (n=42).**

⚠️ **Limite del flag `win_stable`:** il tiro 27 risulta `stable=1` pur avendo
alzo −100°. Il flag dice "l'arco era fermo", NON "l'arco era in mira". Serve un
criterio aggiuntivo (es. scartare |alzo| o |cant| > 45°).

## 5. Ripetibilità della mira

**Metodo:** si usa il **cant** come indice, perché non dipende dal bersaglio
(l'alzo sì: cambia legittimamente con distanza/altezza sagoma, la sua sd non è
un difetto).

**Risultati (42 tiri):**
- 27 tiri (64%) ripetibili (entro ±2.3° = 1sd)
- 9 (21%) accettabili (entro ±4.5°)
- 6 (14%) fuori — di cui **5 su 6 nei primi 7 tiri**

**Warm-up misurato:** sd cant 3.81° (primi tiri) → ~1.7-2.0° (dalla metà).
Prima metà 3.40°, seconda 2.25° = **−34% di dispersione**. Trend confermato
(r = −0.31 tra progressivo e deviazione). **Assestamento ≈ 12 tiri.**
Escludendo il warm-up: **71% di tiri ripetibili**.

→ **AZIONE: in gara, fare i 12 tiri di riscaldamento prima. In analisi,
considerare di scartare i primi 10-12 tiri dalle statistiche.**

Unico outlier genuino fuori warm-up: **tiro 24** (cant +5.2° vs media −0.35°).

## 6. Metriche post-scocco — risultato negativo (onesto)

Outlier IQR: nessun tiro accumula >1 anomalia. Correlazioni tra metriche: tutte
deboli (|r|<0.5); le maggiori sono recoil↔alzo (−0.43), recoil↔drift_vert
(−0.42), follow_through↔drift_vert (−0.40).

**Perché non si trovano i "tiri sporchi":**
1. esito presente solo su 11/44 tiri, e sbilanciato (9 colpiti, 2 mancati)
2. confondente: sagome a distanze/altezze diverse → le metriche variano per
   geometria, non solo per gesto
3. `drift_lat`/`drift_vert` sono marcate "IPOTESI da validare" (v1.22): vanno
   validate loro prima di usarle come diagnosi

Il confronto colpiti/mancati dà un risultato controintuitivo (colpiti con drift_lat
MAGGIORE: 143 vs 35) → è quasi certamente il confondente distanza, non segnale.
**Con 2 mancati non si conclude nulla.**

## 7. Follow-through — IL RISULTATO PIÙ PROMETTENTE

**Metodo:** la finestra calma pre-scocco dà (a) l'angolo assoluto di partenza da
gravità e (b) il bias del giroscopio. Da lì si integra `gy` nel post-scocco —
necessario perché dopo il rilascio l'accelerometro è sporcato dal rinculo e
atan2(az,ax) non è più utilizzabile.

**Classificazione (42 tiri), deriva a +900ms rispetto alla mira:**
- **21 tiri (50%) TENUTI** (|deriva| ≤ 3°)
- **15 (36%) lieve deriva** (3-6°)
- **6 (14%) ABBASSATI PRESTO** (oltre −6°): tiri **10, 12, 19, 33, 34, 44**
- caso peggiore: **tiro 34 = −34°** (braccio che collassa); tiro 33 −14.9°,
  tiro 44 −14.4°, tiro 19 −12.5°

**La firma temporale (scoperta importante):**
- **lo strappo iniziale NON discrimina**: +2/+3° nei primi 30-40ms in TUTTI i
  tiri (è il riser che reagisce al rilascio, fisiologico)
- **discrimina cosa succede dopo i ~150ms**: chi tiene rientra verso zero e si
  stabilizza; chi molla scende con **pendenza costante** e non si ferma
- → la metrica sta nella pendenza 150-900ms, non nel picco

⚠️ **La metrica `follow_through` attuale del firmware NON cattura questo:** il
tiro 34 (il peggiore in assoluto) ha follow_through=16, dentro la norma.

## 8. PROSSIMO PASSO: metrica `hold_deg` (da implementare)

**Idea (di Cesare):** avere l'indicazione PRIMA dello scoring dà la diagnosi:
- **mancato + tenuto** → errore di stima distanza/drop
- **mancato + abbassato** → errore di esecuzione
- **colpito + abbassato** → fortuna/compensazione

**Definizione proposta:** `hold_deg` = variazione dell'angolo di elevazione a
+900ms rispetto all'assetto di mira (negativo = abbassato).

**Soglie empiriche dai 42 tiri:** |hold| ≤3° tenuto | 3-6° lieve | >6° abbassato.

**Implementazione (semplice):** la finestra calma dà già angolo iniziale + bias
gyro; poi si integra `gy` per 900ms. ~10 righe, nessuna memoria extra (i campioni
sono già nel circular buffer). Output: un solo float, mostrabile a display prima
dello scoring.

### ⚠️ VERIFICA PENDENTE (da fare per prima cosa)
**Non è stata completata** (container saturo): quantificare la **deriva del
giroscopio integrato su 1 secondo**. Test: integrare `gy` per 1s nella finestra
PRE-scocco (arco fermo) e misurare quanto sbanda a vuoto.
- se l'errore tipico è ≪1° → le soglie proposte reggono, metrica valida
- se fosse ~2-3° → la soglia dei 3° è inutilizzabile e va rialzata

## 9. Idea aperta: telemetro con angolo
Cesare ha un telemetro elettronico che può dare **anche l'angolo**. Sarebbe il
ground-truth per (distanza, dislivello) per ogni tiro → permetterebbe di separare
la componente geometrica da quella del gesto, e renderebbe anche l'**alzo** un
indice utilizzabile (non solo il cant).

## 10. Stato software
- **Firmware v2.13.0** (1.69): scoring UX completo, schermata distanza ridisegnata
  (slider + tasti verticali), angoli validati (finestra adattiva + flag qualità),
  beep singolo.
- **App v1.25**: wake lock durante REC, shot_id di sessione monotono (+ colonna
  `fw_shot_id`), header CSV non duplicato. Installata come PWA locale via
  mini-server su Android.
- **microSD**: non presente sulla 1.69, arriverà con la **1.83** → renderà il
  telefono secondario per l'integrità dei dati.