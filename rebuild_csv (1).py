#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
rebuild_csv.py — ArchBB
Ricostruzione di BBarch_14JUL.csv (generato da App < v1.25).

PROBLEMA:
  1. header ripetuto ad ogni riconnessione BLE (3 occorrenze)
  2. shot_id proveniente dal FIRMWARE -> riparte da valori bassi ad ogni reset
     => collisione: due tiri diversi finiscono sotto lo stesso shot_id
     => burst da 1344 campioni (= 2 x 672) invece di 672
  3. shot mancante nella numerazione originale

STRATEGIA:
  - lettura riga per riga scartando le righe-header ripetute
  - raggruppamento per shot_id ORIGINALE
  - ogni gruppo con N campioni multiplo di 672 viene splittato in N/672 burst
    (lo split e' sicuro perche' i campioni sono gia' in ordine di arrivo e
     ts_us e' monotono dentro ogni burst ma fa un SALTO tra un burst e l'altro)
  - riordino globale dei burst per ts_us del primo campione
  - rinumerazione monotona 1..N, id originale conservato in orig_shot_id

VERIFICHE (fail-fast, "i dati comandano"):
  - nessuna riga persa o inventata
  - ogni burst ha esattamente 672 campioni
  - ts_us monotono dentro ogni burst
  - nessuna sovrapposizione temporale tra burst consecutivi
"""

import sys
import pandas as pd
import numpy as np

SRC = "/home/claude/archbb_dl/archbb-main/BBarch_14JUL.csv"
DST = "/home/claude/out/BBarch_14JUL_ricostruito.csv"

BURST_LEN = 672          # 448 pre + 224 post @ 224.9 Hz = 2000ms + 1000ms
TRIGGER_IDX = 448


def leggi_scartando_header(path):
    """Legge il CSV scartando le righe-header duplicate.

    L'header e' riconoscibile perche' il primo campo e' letteralmente
    la stringa 'shot_id' invece di un numero.
    """
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        righe = f.read().splitlines()

    header = righe[0].split(",")
    dati = []
    n_header_scartati = 0

    for r in righe:
        if not r.strip():
            continue
        if r.startswith("shot_id,"):
            n_header_scartati += 1
            continue
        dati.append(r.split(","))

    print(f"[read] righe totali file : {len(righe)}")
    print(f"[read] header scartati   : {n_header_scartati}")
    print(f"[read] righe dati        : {len(dati)}")
    print(f"[read] colonne           : {len(header)}")

    df = pd.DataFrame(dati, columns=header)
    return df


def to_num(df):
    """Converte in numerico tutto cio' che e' convertibile, lasciando
    stringa/NaN il resto (esito, zona ecc. sono spesso vuoti)."""
    for c in df.columns:
        df[c] = pd.to_numeric(df[c], errors="ignore")
    return df


def main():
    df = leggi_scartando_header(SRC)
    n_righe_in = len(df)

    # colonne chiave in numerico forzato
    for c in ("shot_id", "sample_idx", "ts_us"):
        df[c] = pd.to_numeric(df[c], errors="coerce")

    if df[["shot_id", "sample_idx", "ts_us"]].isna().any().any():
        sys.exit("[FATAL] valori non numerici nelle colonne chiave")

    print("\n=== DIAGNOSI PRE-RICOSTRUZIONE ===")
    conteggi = df.groupby("shot_id").size()
    print(f"shot_id distinti         : {len(conteggi)}")
    print(f"distribuzione lunghezze  :")
    for lung, quanti in conteggi.value_counts().sort_index().items():
        mult = lung / BURST_LEN
        flag = "OK" if lung == BURST_LEN else f"FUSI x{mult:.0f}" if mult == int(mult) else "ANOMALO"
        print(f"   {lung:5d} campioni -> {quanti:3d} shot_id   [{flag}]")

    # ---- SPLIT DEI BURST FUSI ----
    burst = []   # lista di (ts_primo_campione, orig_shot_id, DataFrame)
    anomali = []

    for sid, g in df.groupby("shot_id", sort=False):
        g = g.reset_index(drop=True)
        n = len(g)
        if n % BURST_LEN != 0:
            anomali.append((sid, n))
            continue
        k = n // BURST_LEN
        for i in range(k):
            sub = g.iloc[i * BURST_LEN:(i + 1) * BURST_LEN].copy()
            burst.append((sub["ts_us"].iloc[0], int(sid), sub))

    if anomali:
        print(f"\n[WARN] shot_id con lunghezza non multipla di {BURST_LEN}: {anomali}")

    print(f"\nburst estratti dopo split: {len(burst)}")

    # ---- RIORDINO PER ts_us ----
    burst.sort(key=lambda t: t[0])

    # ---- RINUMERAZIONE ----
    out = []
    for nuovo_id, (ts0, orig, sub) in enumerate(burst, start=1):
        sub = sub.copy()
        sub["orig_shot_id"] = orig
        sub["shot_id"] = nuovo_id
        sub["sample_idx"] = np.arange(BURST_LEN)   # 0..671 pulito
        out.append(sub)

    ric = pd.concat(out, ignore_index=True)

    # ---- VERIFICHE ----
    print("\n=== VERIFICHE POST-RICOSTRUZIONE ===")
    ok = True

    n_out = len(ric)
    print(f"righe in / out           : {n_righe_in} / {n_out}  "
          f"[{'OK' if n_out == n_righe_in else 'PERSE/INVENTATE!'}]")
    ok &= (n_out == n_righe_in)

    lung = ric.groupby("shot_id").size()
    tutte672 = (lung == BURST_LEN).all()
    print(f"tutti i burst = {BURST_LEN}      : {'OK' if tutte672 else 'NO'}")
    ok &= tutte672

    # monotonia ts dentro ogni burst
    mono = ric.groupby("shot_id")["ts_us"].apply(lambda s: s.is_monotonic_increasing).all()
    print(f"ts_us monotono nei burst : {'OK' if mono else 'NO'}")
    ok &= mono

    # sovrapposizioni tra burst consecutivi
    est = ric.groupby("shot_id")["ts_us"].agg(["min", "max"]).sort_index()
    sovr = (est["min"].values[1:] < est["max"].values[:-1]).sum()
    print(f"sovrapposizioni temporali: {sovr}  [{'OK' if sovr == 0 else 'NO'}]")
    ok &= (sovr == 0)

    durata_s = (ric["ts_us"].max() - ric["ts_us"].min()) / 1e6
    print(f"durata sessione          : {durata_s/60:.1f} min")

    # ODR misurato
    dt = ric.groupby("shot_id")["ts_us"].diff().dropna()
    dt = dt[(dt > 0) & (dt < 20000)]
    print(f"ODR misurato             : {1e6/dt.median():.2f} Hz "
          f"(dt mediano {dt.median():.0f} us)")

    print(f"\ntiri ricostruiti         : {ric['shot_id'].nunique()}")
    print(f"ESITO GLOBALE            : {'*** OK ***' if ok else '*** FALLITO ***'}")

    if not ok:
        sys.exit(1)

    # ---- ORDINE DELLE COLONNE ----
    # orig_shot_id va IN FONDO, non in posizione 2.
    #
    # LEZIONE APPRESA (v2): la prima versione la metteva subito dopo shot_id,
    # "per leggibilita'". Ma l'app importa il CSV leggendo per POSIZIONE
    # (c[0]=shot_id, c[1]=sample_idx, c[2]=ts_us, c[3..8]=ax..gz): una colonna
    # infilata in mezzo sfalsa TUTTO di uno, e gli assi arrivano sbagliati senza
    # che nulla segnali l'errore. Il file ricostruito non era importabile e non
    # se n'era accorto nessuno, perche' veniva sempre analizzato in Python.
    #
    # REGOLA: le colonne nuove vanno SEMPRE in coda. E' la stessa regola che il
    # firmware segue per i campi dei pacchetti BLE.
    cols = list(ric.columns)
    cols.remove("orig_shot_id")
    cols.append("orig_shot_id")
    ric = ric[cols]

    import os
    os.makedirs(os.path.dirname(DST), exist_ok=True)
    ric.to_csv(DST, index=False)
    print(f"\n[write] {DST}  ({len(ric)} righe, {len(ric.columns)} colonne)")


if __name__ == "__main__":
    main()
