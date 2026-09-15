# ArchBB 1.83 — Revisione pre-flash (dopo il fix trigger)

**Versione: `1.83-Fase8-trigfix-r2`**

Revisione sistematica cercando difetti della STESSA FAMIGLIA di quello appena
trovato (timing, campioni persi, race, assunzioni non piu' valide).

---

## TROVATO E CORRETTO

### 1. Trigger manuale rotto — REGRESSIONE MIA di oggi ⚠️
Spostando la valutazione in `trigger_feed_sample`, il percorso manuale era
rimasto scollegato: impostava `TRIG_FIRED` e usciva, ma nessuno faceva piu'
`shot_id++`, `freeze` e semaforo. Il tiro manuale non avrebbe fatto **nulla**.

*Latente* (oggi `trigger_manual()` non e' chiamato da nessuna parte), ma sarebbe
esploso appena collegato a un pulsante.

**Fix:** unificato il fire in `doFire()`, usata da entrambi i percorsi. Ora e'
impossibile che divergano.

### 2. `TRIG_FIRED` come codice morto silenzioso
Dopo il refactoring nessuno imposta piu' quella fase; se un percorso futuro lo
facesse, il tiro sarebbe svanito passando a REARM senza congelare il buffer.

**Fix:** il case ora chiama `doFire()` — rete di sicurezza, il tiro si completa.

---

## VERIFICATO E SANO (nessuna azione)

| controllo | esito |
|---|---|
| Altri consumatori di `g_latest_sample` con lo stesso difetto di polling | Nessuno |
| Deadlock `push` -> `freeze` (stesso task, mutex non ricorsivo) | Sequenziali, non annidati: **sicuro** |
| Doppio-fire durante REARM | Impossibile (`feed_sample` esce se fase != ARMED/CONFIRMING) |
| Ordine in `enterAttesa`: reset buffer PRIMA di armare | Corretto (reset -> svuota semaforo -> arma) |
| `startScoringForShot` disarma subito | Si, prima riga |
| Divisione per zero in `windowStats` (`n = b-a`) | Protetta: due uscite anticipate, `n = win` costante > 0 |
| Buffer path `char[48]` (lezione Fase 5) | Contengono solo la dir (26 char): ampio margine. I path completi usano `char[64]` |
| Dimensionamento `s_burst` vs `CIRCULAR_BUFFER_SIZE` | Coerente (750 campioni, burst 672) |
| Perdita campioni POST durante FROZEN | Nessuna contesa sul mutex in quella fase: main non tocca il buffer finche' non e' READY |

### Macchina a stati del trigger — simulata, 5/5 test superati
scocco automatico · trigger manuale · niente doppio-fire · rearm e riarmo ·
disarmo durante CONFIRMING.

---

## RISCHI RESIDUI NOTI (non corretti, per scelta)

### A. `circular_buffer_freeze` fallisce in SILENZIO su timeout mutex (5 ms)
Se fallisse, il tiro verrebbe segnalato ma senza burst congelato -> a DONE il
main trova stato != READY e salta le metriche (il tiro resta comunque
punteggiabile: degrado controllato).
**Perche' non l'ho toccato:** la contesa e' minima per costruzione (freeze e'
chiamata da imu_task subito dopo che `push` ha rilasciato; `get_burst` gira solo
in stato READY, quando il trigger e' gia' disarmato). Modificare la firma
introdurrebbe rischio senza beneficio misurato. **"I dati comandano": se sul
campo dovesse mancare un burst a fronte di un tiro rilevato, e' il primo
sospetto.**

### B. Scoring molto veloce (< 1 s) -> metriche assenti
Il burst diventa READY solo dopo i 1000 ms di finestra POST. Se l'arciere
completasse ESITO+ZONA+DISTANZA in meno di un secondo, a DONE il buffer non
sarebbe pronto e le metriche resterebbero a zero (tiro comunque salvato).
Realisticamente 3 tap richiedono piu' di 1 s. Nessuna azione, ma e' bene saperlo.

### C. `DBG` dentro il mutex del buffer — solo build DEBUG
`circular_buffer_push` e `freeze` fanno `DBG(...)` mentre tengono il mutex. Nel
build **da campo** (`archbb_183_touchcal`) `DBG` e' no-op: nessun impatto. Nel
build **debug**, una `Serial.printf` bloccante nell'hot-loop dell'IMU puo' far
saltare campioni. **Raccomandazione: per i test sul campo usare sempre l'env
touchcal**, non il debug.

---

## NOTA IMPORTANTE SULLA CONFIGURAZIONE
`CONFIG_VERSION` e' stato portato a 2 (per la ristrutturazione montaggi), quindi
al primo avvio la NVS viene **resettata ai default**:
- soglia trigger -> **20 m/s^2** (il tuo 10 manuale viene scartato: e' quello
  che serve, con 10 rischieresti falsi positivi ora che il trigger vede tutto)
- `confirm_n` -> 2, rearm -> 2000 ms, finestre -> 2000/1000 ms
- montaggio -> `MOUNT_0` (il flip-Z lo applica `mount_apply`)

## Da provare sul campo, in ordine
1. Tiri veri: devono essere rilevati in modo affidabile (era il problema).
2. Nessun falso trigger durante armo/mira. Se ne comparissero, alzare
   `confirm_n` a 3 — **non** rialzare la soglia (la discriminante giusta e' la
   DURATA dell'impulso, che ora il trigger misura davvero).
3. Verificare che a ogni tiro rilevato corrisponda un `burst_NNNN.bin` sulla SD
   (se ne mancasse uno, vedi rischio A).
4. Angoli nel CSV coerenti con quelli gia' visti (la ristrutturazione montaggi
   e' verificata equivalente, non dovrebbero cambiare).
