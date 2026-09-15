#!/usr/bin/env python3
# ============================================================================
#  ArchBB 1.83 — analisi_firmware.py · FASE 5
#  Ri-analisi dei burst grezzi con gli ALGORITMI AUTOREVOLI dell'app 1.27
#  Cesare Pagura · Padova/Noale IT · 20 luglio 2026
# ----------------------------------------------------------------------------
#  PERCHE' QUESTO FILE (e non un ricalcolo "a occhio")
#    Il primo reader (leggi_sessione.py) faceva una media semplice del PRE per
#    stimare cant/alzo — comodo per un colpo d'occhio, ma NON e' cio' che fa il
#    firmware/app. Questo modulo porta in Python la LOGICA ESATTA dell'app 1.27
#    (github CP52/archbb, ArchBB_App_v1_27), cosi' la ri-analisi a valle da' gli
#    STESSI numeri del dispositivo, non un'approssimazione. E' lo strumento
#    giusto per ritarare le soglie: se cambi TAU o le soglie hold/release, qui
#    le rimetti e rilanci sui burst salvati, senza ri-tirare.
#
#  ALGORITMI PORTATI (fedeli all'app 1.27, con i riferimenti alle sue note)
#    - burstSampleRate : fs REALE dalla mediana dei Δts (NON l'ODR nominale).
#        L'app documenta che getDataReady() segue l'ODR dell'ACCELEROMETRO
#        (~249 Hz), non del giroscopio (224 Hz): la base tempi nominale e'
#        inaffidabile. Ogni campione porta il suo timestamp -> mediana robusta.
#    - findCalmWindow  : finestra pre-trigger PIU' FERMA (|a|~9.81, sd minima).
#        Stessa logica di shot_angles_compute nel firmware. Score = |mean-9.81|+sd.
#    - calcStabilityScore : RMS angolare bias-corretto nella finestra calma,
#        mappato con decadimento esponenziale 100*exp(-rms/TAU), TAU=5 dps.
#    - calcCant        : mediana di atan2(ay,ax) su [trig-75, trig-25].
#    - calcHoldRelease : hold = integrale trapezoidale di (gy-bias) per 900ms a
#        dt REALE; release = max |d(ay)/dt| nei 40ms con derivata centrata.
#
#  NOTA SUI SEGNI (dal firmware 1.83, frame canonico dopo mount_apply FLIPZ)
#    cant = atan2(ay, ax)  (+ = destra);  alzo = atan2(az, ax)  (+ = punta su).
#    I burst salvati sono GIA' nel frame canonico, quindi qui si applica atan2
#    direttamente, senza rimappare gli assi.
#
#  USO
#    python3 analisi_firmware.py /percorso/SESS_0001
#    python3 analisi_firmware.py /percorso/SESS_0001 --confronta   # CSV vs ricalcolo
# ============================================================================
import argparse
import math
import struct
import csv as csvmod
from pathlib import Path

IMU_FMT = "<HI6f"                    # seq(H) ts_us(I) ax ay az gx gy gz (6f)
IMU_SIZE = struct.calcsize(IMU_FMT)
BURST_HDR_FMT = "<4sBBHHH4s"
BURST_HDR_SIZE = struct.calcsize(BURST_HDR_FMT)
assert IMU_SIZE == 30 and BURST_HDR_SIZE == 16

# --- Soglie e costanti (identiche all'app 1.27; qui per ritararle a valle) ---
GUARD_S   = 0.050     # gap dal trigger
WIN_S     = 0.200     # ampiezza finestra calma
GRAVITY   = 9.81
TAU_DPS   = 5.0       # stability: RMS a cui lo score vale 37
HOLD_MS   = 900
REL_MS    = 40
DT_MAX_US = 50000
HOLD_SOGLIE   = (4.0, 8.0)     # gradi: <=4 tenuto, <=8 lieve, oltre abbassato/alzato
RELEASE_SOGLIE = (330, 510)    # m/s^3: <=330 pulito, <=510 medio, oltre strappo


# ---------------------------------------------------------------------------
#  Lettura burst
# ---------------------------------------------------------------------------
def load_burst(path: Path):
    d = path.read_bytes()
    magic, ver, odr, n, trig, shot, _ = struct.unpack(BURST_HDR_FMT, d[:BURST_HDR_SIZE])
    if magic != b"ABB1":
        raise ValueError(f"{path.name}: magic {magic!r} != b'ABB1'")
    body = d[BURST_HDR_SIZE:]
    got = len(body) // IMU_SIZE
    S = []
    for i in range(got):
        seq, ts, ax, ay, az, gx, gy, gz = struct.unpack(IMU_FMT, body[i*IMU_SIZE:(i+1)*IMU_SIZE])
        S.append(dict(seq=seq, ts=ts, ax=ax, ay=ay, az=az, gx=gx, gy=gy, gz=gz))
    return dict(ver=ver, odr=odr, n=got, trig=trig, shot=shot), S


# ---------------------------------------------------------------------------
#  burstSampleRate — fs reale dalla mediana dei Δts (app 1.27)
# ---------------------------------------------------------------------------
def burst_sample_rate(S, fallback=224.0):
    if not S or len(S) < 10 or S[0]["ts"] is None:
        return fallback
    dts = []
    for i in range(1, len(S)):
        d = S[i]["ts"] - S[i-1]["ts"]
        if 0 < d < 100000:
            dts.append(d)
    if len(dts) < 5:
        return fallback
    dts.sort()
    med = dts[len(dts)//2]
    fs = 1e6 / med
    return fs if 20 < fs < 2000 else fallback


# ---------------------------------------------------------------------------
#  findCalmWindow — finestra pre-trigger piu' ferma (app 1.27 / firmware)
# ---------------------------------------------------------------------------
def find_calm_window(S, trig, fs):
    guard = round(GUARD_S * fs)
    win = round(WIN_S * fs)
    we_max = trig - guard
    if we_max < win:
        return None
    best = None
    best_score = float("inf")
    for we in range(win, we_max + 1):
        ws = we - win
        s = 0.0
        s2 = 0.0
        for i in range(ws, we):
            m = math.sqrt(S[i]["ax"]**2 + S[i]["ay"]**2 + S[i]["az"]**2)
            s += m
            s2 += m*m
        n = we - ws
        mean = s / n
        sd = math.sqrt(max(0.0, s2/n - mean*mean))
        score = abs(mean - GRAVITY) + sd
        if score < best_score:
            best_score = score
            best = (ws, we)
    return best


# ---------------------------------------------------------------------------
#  Metriche
# ---------------------------------------------------------------------------
def calc_angles(S, trig, fs):
    """cant/alzo dalla finestra calma (frame canonico: atan2 diretto)."""
    cw = find_calm_window(S, trig, fs)
    if cw is None:
        return None
    win = S[cw[0]:cw[1]]
    mx = sum(s["ax"] for s in win) / len(win)
    my = sum(s["ay"] for s in win) / len(win)
    mz = sum(s["az"] for s in win) / len(win)
    gmean = sum(math.sqrt(s["ax"]**2+s["ay"]**2+s["az"]**2) for s in win) / len(win)
    var = sum((math.sqrt(s["ax"]**2+s["ay"]**2+s["az"]**2)-gmean)**2 for s in win) / len(win)
    sd = math.sqrt(max(0.0, var))
    cant = math.degrees(math.atan2(my, mx))
    alzo = math.degrees(math.atan2(mz, mx))
    stable = (abs(gmean - GRAVITY) < 1.5) and (sd < 1.0)
    return dict(cant=cant, alzo=alzo, gmean=gmean, sd=sd, stable=stable)


def calc_stability(S, trig, fs):
    """RMS angolare bias-corretto -> score esponenziale (app 1.27, TAU=5)."""
    cw = find_calm_window(S, trig, fs)
    if cw is None:
        return None
    win = S[cw[0]:cw[1]]
    if len(win) < 5:
        return None
    bgx = sum(p["gx"] for p in win)/len(win)
    bgy = sum(p["gy"] for p in win)/len(win)
    bgz = sum(p["gz"] for p in win)/len(win)
    vals = [math.sqrt((p["gx"]-bgx)**2 + (p["gy"]-bgy)**2 + (p["gz"]-bgz)**2) for p in win]
    rms = math.sqrt(sum(v*v for v in vals)/len(vals))
    return round(100 * math.exp(-rms / TAU_DPS)), round(rms, 2)


def calc_hold_release(S, trig, fs):
    """hold (integrale gy 900ms) e release (jerk ay 40ms) — app 1.27."""
    cw = find_calm_window(S, trig, fs)
    if cw is None:
        return None
    calm = S[cw[0]:cw[1]]
    bias_gy = sum(p["gy"] for p in calm) / len(calm)

    # HOLD
    hold_deg = None
    hold_class = 4
    t0 = S[trig]["ts"]
    acc = 0.0
    n_int = 0
    for i in range(trig, len(S)-1):
        if S[i]["ts"] - t0 > HOLD_MS * 1000:
            break
        dt = S[i+1]["ts"] - S[i]["ts"]
        if dt <= 0 or dt > DT_MAX_US:
            continue
        acc += 0.5 * ((S[i]["gy"]-bias_gy) + (S[i+1]["gy"]-bias_gy)) * (dt/1e6)
        n_int += 1
    if n_int >= 20:
        hold_deg = acc
        a = abs(hold_deg)
        hold_class = 0 if a <= HOLD_SOGLIE[0] else (1 if a <= HOLD_SOGLIE[1] else (2 if hold_deg < 0 else 3))

    # RELEASE
    rel_jerk = None
    rel_class = 3
    jmax = 0.0
    n_rel = 0
    for i in range(trig+1, len(S)-1):
        if S[i]["ts"] - t0 > REL_MS * 1000:
            break
        dt2 = S[i+1]["ts"] - S[i-1]["ts"]
        if dt2 <= 0 or dt2 > 2*DT_MAX_US:
            continue
        j = abs((S[i+1]["ay"] - S[i-1]["ay"]) / (dt2/1e6))
        if j > jmax:
            jmax = j
        n_rel += 1
    if n_rel >= 5:
        rel_jerk = jmax
        rel_class = 0 if jmax <= RELEASE_SOGLIE[0] else (1 if jmax <= RELEASE_SOGLIE[1] else 2)

    return dict(hold_deg=hold_deg, hold_class=hold_class,
                release_jerk=rel_jerk, release_class=rel_class)


HOLD_NAMES = {0: "TENUTO", 1: "LIEVE", 2: "ABBASSATO", 3: "ALZATO", 4: "n/d"}
REL_NAMES = {0: "PULITO", 1: "MEDIO", 2: "STRAPPO", 3: "n/d"}


# ---------------------------------------------------------------------------
#  Analisi di una sessione
# ---------------------------------------------------------------------------
def analizza(sess_dir: Path, confronta=False):
    sess_dir = Path(sess_dir)
    bursts = sorted(sess_dir.glob("burst_*.bin"))
    if not bursts:
        print(f"Nessun burst in {sess_dir}")
        return

    # Se --confronta, carico i valori del CSV per il raffronto.
    csv_rows = {}
    if confronta:
        csvf = sess_dir / "shots.csv"
        if csvf.exists():
            for line in csvf.read_text().splitlines():
                if line.startswith("#") or line.startswith("shot,") or not line:
                    continue
                parts = line.split(",")
                try:
                    csv_rows[int(parts[0])] = parts
                except (ValueError, IndexError):
                    pass

    print(f"\n═══ {sess_dir.name} — ri-analisi con algoritmi app 1.27 ═══\n")
    hdr = f"{'tiro':>4} {'fs_Hz':>6} {'cant':>7} {'alzo':>7} {'stab':>5} {'rms':>5} " \
          f"{'hold°':>7} {'hold':>9} {'jerk':>6} {'rel':>7}"
    print(hdr)
    print("─" * len(hdr))

    for b in bursts:
        meta, S = load_burst(b)
        fs = burst_sample_rate(S)
        ang = calc_angles(S, meta["trig"], fs)
        stab = calc_stability(S, meta["trig"], fs)
        hr = calc_hold_release(S, meta["trig"], fs)

        cant = f"{ang['cant']:+.2f}" if ang else "  n/d"
        alzo = f"{ang['alzo']:+.2f}" if ang else "  n/d"
        st = f"{stab[0]}" if stab else "n/d"
        rms = f"{stab[1]}" if stab else " n/d"
        if hr and hr["hold_deg"] is not None:
            hd = f"{hr['hold_deg']:+.2f}"
            hc = HOLD_NAMES[hr["hold_class"]]
        else:
            hd, hc = "  n/d", "n/d"
        if hr and hr["release_jerk"] is not None:
            jk = f"{hr['release_jerk']:.0f}"
            rc = REL_NAMES[hr["release_class"]]
        else:
            jk, rc = " n/d", "n/d"

        print(f"{meta['shot']:>4} {fs:>6.1f} {cant:>7} {alzo:>7} {st:>5} {rms:>5} "
              f"{hd:>7} {hc:>9} {jk:>6} {rc:>7}")

        if confronta and meta["shot"] in csv_rows:
            # CSV: cant_cdeg(7) alzo_cdeg(8) hold_cdeg(10) release_jerk(12)
            p = csv_rows[meta["shot"]]
            def cd(i):
                try: return int(p[i]) / 100.0
                except (ValueError, IndexError): return None
            def iv(i):
                try: return int(p[i])
                except (ValueError, IndexError): return None
            c_cant, c_alzo, c_hold = cd(7), cd(8), cd(10)
            c_jerk = iv(12)
            cc = f"{c_cant:+.2f}" if c_cant is not None else "n/d"
            ca = f"{c_alzo:+.2f}" if c_alzo is not None else "n/d"
            ch = f"{c_hold:+.2f}" if c_hold is not None else "n/d"
            cj = f"{c_jerk}" if c_jerk is not None else "n/d"
            print(f"     {'(CSV)':>6} {cc:>7} {ca:>7} {'':>5} {'':>5} {ch:>7} {'':>9} {cj:>6}")

    print("\nNota: cant/alzo dalla finestra calma adattiva (non media del PRE). "
          "hold/release significativi solo su tiri VERI con follow-through reale.")


def main():
    ap = argparse.ArgumentParser(
        description="Ri-analizza i burst di una sessione con gli algoritmi dell'app 1.27.")
    ap.add_argument("session_dir", help="cartella SESS_NNNN")
    ap.add_argument("--confronta", action="store_true",
                    help="mostra anche i valori del CSV per il raffronto")
    args = ap.parse_args()
    analizza(args.session_dir, args.confronta)


if __name__ == "__main__":
    main()
