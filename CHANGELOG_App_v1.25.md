# CHANGELOG — ArchBB App v1.25 (integrita' dati: stand-by, riconnessione, shot_id)

**Data:** 14 luglio 2026
**Base:** v1.24
**File:** `ArchBB_App_v1_25.html`. Firmware invariato (v2.12.6).

Nasce dalla prima sessione reale sul campo (CSV 14/07), che ha rivelato tre
problemi di integrita' dati intrecciati fra loro.

## Diagnosi dai dati (CSV 14/07)
- Molti tiri con 1344 campioni = 2 burst DIVERSI fusi sotto lo stesso shot_id,
  con gap temporali da -419s a +384s tra i due blocchi.
- 3 header nel file (riscritti dopo riconnessione).
- shot_id 26 mancante (tiro perso in una riconnessione).
Causa comune: lo shot_id veniva dal FIRMWARE, che riparte da valori bassi a ogni
reset/riconnessione → collisioni → fusione dei tiri.

## Fix #1 — Wake Lock durante REC (stand-by)
Col telefono in stand-by la app si sospendeva; i burst BLE si accodavano e
arrivavano tutti al risveglio (bing a raffica + scritture disordinate).
Ora: Wake Lock API tiene lo schermo acceso MENTRE REC e' attivo (rilasciato allo
STOP → batteria risparmiata quando non registri). Ri-acquisito su
visibilitychange se il SO lo revoca. Rilasciato anche su disconnessione.

## Fix #2 — shot_id di SESSIONE monotono (riconnessione/reset)
Nuovo contatore `g_sessionShotSeq` (app): parte da 1, incrementa a ogni burst,
NON riparte mai (se non su "nuova sessione" esplicita). Diventa la prima colonna
del CSV. Lo shot_id del firmware e' conservato come colonna extra `fw_shot_id`.
Cosi' reset e riconnessioni del firmware non possono piu' fondere tiri diversi:
ogni tiro ha un id univoco. Verificato: con fw_shot_id che collide (1,2,3,1,2,1),
i shot_id di sessione restano 1..6, monotoni e distinti.

## Fix #2b — header CSV non piu' duplicato
L'header era deciso da existing.size>0, fragile quando getFile() falliva in
background. Ora l'header e' legato all'HANDLE del file: scritto una sola volta
per handle, mai riscritto a meta' file.

## Nuova colonna CSV
`fw_shot_id` aggiunta in coda (l'id firmware, per riferimento). L'ordine delle
colonne esistenti non cambia: i parser vecchi restano compatibili.

## Verifica
- node --check: sintassi OK.
- Test logico contatore sessione: monotono e univoco anche con reset/riconnessione.

## Da fare ancora (concordato)
- Criticita' #3: ridisegno schermata DISTANZA (firmware) — slider grande dx/sx,
  +/- laterali, due tasti grandi verticali INDIETRO/OK come nel riepilogo.
- Criticita' #4 (reset scheda isolato): sotto osservazione, nessuna perdita dati.
