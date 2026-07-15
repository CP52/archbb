#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ricerca_rilascio.py — ArchBB
DOMANDA: esiste una metrica della "PULIZIA DEL RILASCIO" che sia misurabile?

Cos'e' un rilascio sporco, fisicamente:
  la corda lascia le dita. Se le dita si aprono in modo pulito, la corda parte
  dritta e il riser reagisce in modo simmetrico e ripetibile. Se le dita
  "strappano" (rilascio sporco / plucking), la corda riceve una spinta LATERALE
  e il riser reagisce con una componente laterale/torsionale anomala.

Vincolo fisico: la freccia e' sulla corda per ~40ms (9 campioni @224Hz). Solo
cio' che accade in QUELLA finestra puo' influenzare il tiro. Dopo, e' diagnostico
del gesto ma non causa piu' nulla.

REGOLA DEL GIOCO (i dati comandano):
  per OGNI candidata si misura PRIMA il rumore (test a vuoto su finestra
  equivalente pre-scocco, arco fermo -> valore vero noto), POI il segnale.
  Nessuna metrica viene proposta se non supera il proprio floor.

Candidate testate:
  A. asimmetria laterale    : |ay| deviato dalla baseline nei 40ms
  B. jerk laterale          : derivata di ay (lo "strappo" vero e proprio)
  C. energia torsionale     : integrale |gz| nei 40ms (imbardata da plucking)
  D. rapporto lat/long      : quanto del rinculo va di lato invece che dritto
  E. ripetibilita' di forma : distanza della firma ay dal profilo mediano
  F. picco gx (rollio)      : il riser che ruota nel piano dell'arco
"""

import numpy as np
import pandas as pd
from scipy import stats

SRC = "/home/claude/out/BBarch_14JUL_ricostruito.csv"
ESCLUSI = [8, 27]
TRIG = 448
FS = 224.91
N_ARROW = 9        # ~40ms: finestra di influenza della freccia


def ms2n(ms):
    return int(round(ms * FS / 1000.0))


def sez(t):
    print("\n" + "=" * 78)
    print(t)
    print("=" * 78)


def finestra_calma(g):
    """Stessa logica del firmware: passo 1, guard 11, win 44."""
    GUARD, WIN = 11, 44
    a = np.sqrt(g.ax.values**2 + g.ay.values**2 + g.az.values**2)
    best, bs = None, np.inf
    for we in range(WIN, TRIG - GUARD + 1):
        ws = we - WIN
        w = a[ws:we]
        sc = abs(w.mean() - 9.81) + w.std(ddof=1)
        if sc < bs:
            bs, best = sc, (ws, we)
    return best


def main():
    df = pd.read_csv(SRC)
    tiri = [t for t in sorted(df.shot_id.unique()) if t not in ESCLUSI]

    rows = []
    profili_ay = {}
    for sid in tiri:
        g = df[df.shot_id == sid].sort_values("sample_idx").reset_index(drop=True)
        cs, ce = finestra_calma(g)
        calm = g.iloc[cs:ce]

        # baseline dalla finestra calma (arco fermo)
        ay0, ax0, az0 = calm.ay.mean(), calm.ax.mean(), calm.az.mean()
        gz0, gx0, gy0 = calm.gz.mean(), calm.gx.mean(), calm.gy.mean()

        rel = g.iloc[TRIG:TRIG + N_ARROW]
        ts = rel.ts_us.to_numpy(float) / 1e6
        ay = rel.ay.to_numpy(float)
        ax = rel.ax.to_numpy(float)
        az = rel.az.to_numpy(float)
        gz = rel.gz.to_numpy(float)
        gx = rel.gx.to_numpy(float)

        rec = {"shot_id": sid}

        # --- A. asimmetria laterale: quanto ay si scosta dalla baseline ---
        rec["A_ay_dev"] = np.abs(ay - ay0).max()
        rec["A_ay_int"] = np.trapezoid(np.abs(ay - ay0), ts)      # m/s

        # --- B. jerk laterale: derivata di ay ---
        rec["B_jerk"] = np.abs(np.gradient(ay, ts)).max()

        # --- C. energia torsionale: |gz| integrato (imbardata) ---
        rec["C_yaw_e"] = np.trapezoid(np.abs(gz - gz0), ts)       # deg

        # --- D. rapporto laterale/longitudinale del rinculo ---
        lat = np.abs(ay - ay0).max()
        lon = np.abs(az - az0).max()
        rec["D_ratio"] = lat / lon if lon > 1e-6 else np.nan

        # --- F. picco gx (rollio nel piano dell'arco) ---
        rec["F_gx_pk"] = np.abs(gx - gx0).max()

        # profilo ay normalizzato per l'analisi di forma (E)
        prof = (ay - ay0)
        profili_ay[sid] = prof

        # --- riferimento: rumore su finestra equivalente PRE-scocco ---
        # stessa lunghezza, arco fermo -> il valore vero e' ~0
        pre = g.iloc[ce - N_ARROW:ce]
        tsp = pre.ts_us.to_numpy(float) / 1e6
        ayp = pre.ay.to_numpy(float)
        gzp = pre.gz.to_numpy(float)
        gxp = pre.gx.to_numpy(float)
        rec["n_ay_dev"] = np.abs(ayp - ay0).max()
        rec["n_jerk"]   = np.abs(np.gradient(ayp, tsp)).max()
        rec["n_yaw_e"]  = np.trapezoid(np.abs(gzp - gz0), tsp)
        rec["n_gx_pk"]  = np.abs(gxp - gx0).max()

        rows.append(rec)

    r = pd.DataFrame(rows).set_index("shot_id")

    # --- E. ripetibilita' di forma: distanza dal profilo MEDIANO ---
    P = np.vstack([profili_ay[s] for s in tiri])
    med = np.median(P, axis=0)
    for i, s in enumerate(tiri):
        r.loc[s, "E_shape"] = np.sqrt(np.mean((P[i] - med) ** 2))   # RMSE dal mediano

    # =========================================================================
    sez("1. SEGNALE vs RUMORE — ogni candidata contro il proprio floor")
    # =========================================================================
    print("\n(rumore = stessa misura su finestra equivalente PRE-scocco, arco fermo)\n")
    print(f"  {'candidata':22s} {'segnale':>10s} {'rumore p95':>11s} {'SNR':>7s}  esito")
    print("  " + "-" * 62)
    coppie = [("A_ay_dev", "n_ay_dev", "A. dev. laterale ay"),
              ("B_jerk",   "n_jerk",   "B. jerk laterale"),
              ("C_yaw_e",  "n_yaw_e",  "C. energia torsionale"),
              ("F_gx_pk",  "n_gx_pk",  "F. picco gx (rollio)")]
    for sc, nc, lbl in coppie:
        sig = r[sc].mean()
        noi = np.percentile(r[nc], 95)
        snr = sig / noi if noi > 0 else np.inf
        ok = "USABILE" if snr > 2 else ("marginale" if snr > 1 else "SOTTO IL RUMORE")
        print(f"  {lbl:22s} {sig:10.2f} {noi:11.2f} {snr:7.1f}x  {ok}")

    # =========================================================================
    sez("2. LE CANDIDATE DISCRIMINANO QUALCOSA? (vs hold_deg come riferimento)")
    # =========================================================================
    cp = pd.read_csv("/home/claude/out/hold_cpp_output.csv").set_index("shot_id")
    r["hold"] = cp.hold_deg
    r["hold_class"] = cp.hold_class

    print("\nNB: hold e' il FOLLOW-THROUGH, non il rilascio. Se una candidata")
    print("    NON correla con hold, e' un'informazione NUOVA (non ridondante).\n")
    cands = ["A_ay_dev", "A_ay_int", "B_jerk", "C_yaw_e", "D_ratio", "F_gx_pk", "E_shape"]
    print(f"  {'candidata':12s} {'rho vs hold':>12s} {'p':>9s}   interpretazione")
    print("  " + "-" * 66)
    for c in cands:
        rho, p = stats.spearmanr(r[c], r.hold)
        if abs(rho) > 0.6:
            note = "ridondante con hold"
        elif p < 0.05:
            note = "parzialmente legata a hold"
        else:
            note = "INDIPENDENTE da hold  <-- info nuova"
        print(f"  {c:12s} {rho:+12.3f} {p:9.4f}   {note}")

    # =========================================================================
    sez("3. LE CANDIDATE CORRELANO FRA LORO? (ridondanza interna)")
    # =========================================================================
    C = r[cands].corr(method="spearman")
    print()
    print(C.round(2).to_string())

    # =========================================================================
    sez("4. STABILITA': la metrica e' RIPETIBILE entro l'arciere?")
    # =========================================================================
    # Un buon indice del gesto deve avere dispersione contenuta ma non nulla,
    # e non deve derivare col progressivo (o e' warm-up, non rilascio).
    print(f"\n  {'candidata':12s} {'media':>9s} {'sd':>8s} {'CV':>7s} {'rho(shot)':>10s} {'p':>8s}")
    print("  " + "-" * 60)
    for c in cands:
        v = r[c]
        rho, p = stats.spearmanr(r.index, v)
        cv = abs(v.std() / v.mean()) if v.mean() != 0 else np.inf
        drift = " <- deriva!" if p < 0.05 else ""
        print(f"  {c:12s} {v.mean():9.2f} {v.std():8.2f} {cv:7.2f} {rho:+10.3f} {p:8.4f}{drift}")

    # =========================================================================
    sez("5. IL CONFONDENTE GEOMETRICO: le candidate dipendono dal bersaglio?")
    # =========================================================================
    r["alzo_fw"] = cp.alzo_deg
    print("\n(se una candidata correla con alzo_fw, misura la geometria del campo,")
    print(" non il rilascio -> inutilizzabile per confronti fra bersagli diversi)\n")
    for c in cands:
        rho, p = stats.spearmanr(r[c], r.alzo_fw)
        m = "  *** CONFONDENTE" if p < 0.05 else "  pulita"
        print(f"  {c:12s} rho(alzo)={rho:+.3f}  p={p:.4f}{m}")

    r.to_csv("/home/claude/out/ricerca_rilascio.csv")
    print("\n[write] /home/claude/out/ricerca_rilascio.csv")

    print("\n\n--- tabella completa ---")
    print(r[cands + ["hold"]].round(2).to_string())


if __name__ == "__main__":
    main()
