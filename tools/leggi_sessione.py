#!/usr/bin/env python3
# ============================================================================
#  ArchBB 1.83 — leggi_sessione.py · FASE 5 (lettura sessioni microSD)
#  Cesare Pagura · Padova/Noale IT · 20 luglio 2026
# ----------------------------------------------------------------------------
#  A COSA SERVE
#    Legge una cartella di sessione (SESS_NNNN) copiata dalla microSD e:
#      - stampa un riepilogo leggibile di shots.csv
#      - decodifica i burst_NNNN.bin nel loro contenuto grezzo (ImuSample)
#    E' lo strumento di RI-ANALISI a freddo: la decisione §3.1 (salvare anche i
#    burst grezzi) esiste apposta per poter rifare i conti — angoli, hold, jerk —
#    con parametri diversi quando affinerai le soglie, senza dover ri-tirare.
#
#  FORMATO DEI FILE (deve restare allineato col firmware — sd_storage.cpp)
#    shots.csv    : righe di testo, header con '#', poi una riga per tiro.
#    burst_NNNN.bin: header 16 byte + n * 30 byte di ImuSample.
#      header : magic "ABB1"(4) | ver(1) | odr_code(1) | n(uint16) |
#               trig_idx(uint16) | shot(uint16) | riservati(4)
#      ImuSample (30B, packed, little-endian, IDENTICO alla 1.69):
#               seq(uint16) | timestamp_us(uint32) | ax,ay,az,gx,gy,gz (6*float)
#      NB: gli assi sono GIA' nel frame canonico (mount_apply applicato a monte
#          nel firmware): ax=verticale/gravita', ay=laterale, az=asse freccia.
#
#  USO
#    python3 leggi_sessione.py /percorso/ARCHBB/SESS_0001
#    python3 leggi_sessione.py /percorso/ARCHBB/SESS_0001 --burst 3   # dettaglio tiro 3
#    python3 leggi_sessione.py /percorso/ARCHBB/SESS_0001 --csv-out out.csv
#
#  DIPENDENZE: solo la standard library (struct, csv, argparse). Niente numpy
#  richiesto per la lettura base; se vuoi plottare, aggiungi tu matplotlib.
# ============================================================================
import argparse
import struct
import sys
from pathlib import Path

# ODR reali del QMI8658 (Hz) per codice OdrSetting — coerenti con config.h.
ODR_HZ = {0: 112.1, 1: 224.2, 2: 448.4}

# Formato ImuSample: '<' little-endian, H=uint16, I=uint32, f=float.
# seq(H) ts_us(I) ax ay az gx gy gz (6f). Totale 2+4+24 = 30 byte.
IMU_FMT = "<HI6f"
IMU_SIZE = struct.calcsize(IMU_FMT)
assert IMU_SIZE == 30, f"ImuSample deve essere 30 byte, non {IMU_SIZE}"

BURST_HDR_FMT = "<4sBBHHH4s"   # magic, ver, odr, n, trig, shot, riservati
BURST_HDR_SIZE = struct.calcsize(BURST_HDR_FMT)
assert BURST_HDR_SIZE == 16, f"header burst deve essere 16 byte, non {BURST_HDR_SIZE}"


def read_burst(path: Path):
    """Legge un burst_NNNN.bin -> (meta, campioni). campioni = lista di dict."""
    data = path.read_bytes()
    if len(data) < BURST_HDR_SIZE:
        raise ValueError(f"{path.name}: file troppo corto per l'header")
    magic, ver, odr, n, trig, shot, _ = struct.unpack(BURST_HDR_FMT, data[:BURST_HDR_SIZE])
    if magic != b"ABB1":
        raise ValueError(f"{path.name}: magic errato {magic!r} (atteso b'ABB1')")

    body = data[BURST_HDR_SIZE:]
    got = len(body) // IMU_SIZE
    if got != n:
        print(f"  ! {path.name}: header dice n={n} ma il file contiene {got} campioni "
              f"(file troncato?) — leggo {got}", file=sys.stderr)
    samples = []
    for i in range(got):
        off = i * IMU_SIZE
        seq, ts, ax, ay, az, gx, gy, gz = struct.unpack(IMU_FMT, body[off:off + IMU_SIZE])
        samples.append(dict(seq=seq, ts_us=ts, ax=ax, ay=ay, az=az, gx=gx, gy=gy, gz=gz))
    meta = dict(ver=ver, odr_code=odr, odr_hz=ODR_HZ.get(odr, float("nan")),
                n=got, trig_idx=trig, shot=shot)
    return meta, samples


def print_burst_detail(path: Path):
    meta, s = read_burst(path)
    print(f"\n=== {path.name} ===")
    print(f"  versione formato : {meta['ver']}")
    print(f"  ODR              : {meta['odr_hz']:.1f} Hz (code {meta['odr_code']})")
    print(f"  campioni         : {meta['n']}")
    print(f"  trigger @ idx    : {meta['trig_idx']}  "
          f"(~{meta['trig_idx']/meta['odr_hz']*1000:.0f} ms di PRE)")
    if not s:
        print("  (nessun campione)")
        return
    # Durata reale del burst dai timestamp (non dt costante: c'e' il battimento
    # del tick FreeRTOS, quindi i timestamp reali sono la verita').
    dur_ms = (s[-1]["ts_us"] - s[0]["ts_us"]) / 1000.0
    print(f"  durata reale     : {dur_ms:.0f} ms")
    # Assetto medio nella finestra PRE (pre-scocco): stima di cant/alzo a riposo.
    import math
    pre = s[:meta["trig_idx"]] or s[:10]
    mx = sum(v["ax"] for v in pre) / len(pre)
    my = sum(v["ay"] for v in pre) / len(pre)
    mz = sum(v["az"] for v in pre) / len(pre)
    g = math.sqrt(mx*mx + my*my + mz*mz)
    cant = math.degrees(math.atan2(my, mx))
    alzo = math.degrees(math.atan2(mz, mx))
    print(f"  PRE medio        : |g|={g:.2f} m/s^2  cant={cant:+.1f}°  alzo={alzo:+.1f}°")
    # Primi/ultimi campioni per un colpo d'occhio.
    print("  primi 3 campioni (ax,ay,az | gx,gy,gz):")
    for v in s[:3]:
        print(f"    seq={v['seq']:5d}  {v['ax']:+6.2f} {v['ay']:+6.2f} {v['az']:+6.2f} | "
              f"{v['gx']:+7.1f} {v['gy']:+7.1f} {v['gz']:+7.1f}")


def print_session(sess_dir: Path, burst_id=None, csv_out=None):
    if not sess_dir.is_dir():
        sys.exit(f"Non e' una cartella: {sess_dir}")

    # --- session.txt --------------------------------------------------------
    meta_file = sess_dir / "session.txt"
    if meta_file.exists():
        print("── session.txt " + "─" * 40)
        print(meta_file.read_text().rstrip())
        print()

    # --- shots.csv ----------------------------------------------------------
    csv_file = sess_dir / "shots.csv"
    if not csv_file.exists():
        sys.exit(f"Manca shots.csv in {sess_dir}")
    lines = csv_file.read_text().splitlines()
    header = [l for l in lines if l.startswith("#")]
    rows = [l for l in lines if l and not l.startswith("#")]
    for h in header:
        print(h)
    print(f"── shots.csv: {len(rows)-1 if rows else 0} tiri " + "─" * 30)
    for l in rows:
        print("  " + l)

    if csv_out:
        Path(csv_out).write_text("\n".join(rows) + "\n")
        print(f"\n[CSV pulito scritto in {csv_out}]")

    # --- burst --------------------------------------------------------------
    bursts = sorted(sess_dir.glob("burst_*.bin"))
    print(f"\n── burst grezzi: {len(bursts)} file " + "─" * 30)
    if burst_id is not None:
        target = sess_dir / f"burst_{burst_id:04d}.bin"
        if target.exists():
            print_burst_detail(target)
        else:
            print(f"  (burst_{burst_id:04d}.bin non trovato)")
    else:
        for b in bursts:
            try:
                meta, _ = read_burst(b)
                print(f"  {b.name}: n={meta['n']}, trig={meta['trig_idx']}, "
                      f"ODR={meta['odr_hz']:.0f}Hz")
            except ValueError as e:
                print(f"  {b.name}: ERRORE — {e}")
        print("\n  (usa --burst N per il dettaglio di un singolo tiro)")


def main():
    ap = argparse.ArgumentParser(description="Legge una sessione ArchBB dalla microSD.")
    ap.add_argument("session_dir", help="cartella SESS_NNNN copiata dalla card")
    ap.add_argument("--burst", type=int, default=None,
                    help="mostra il dettaglio grezzo del burst del tiro N")
    ap.add_argument("--csv-out", default=None,
                    help="riscrive shots.csv pulito (senza commenti) in questo file")
    args = ap.parse_args()
    print_session(Path(args.session_dir), args.burst, args.csv_out)


if __name__ == "__main__":
    main()
