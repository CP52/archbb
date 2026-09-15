#!/usr/bin/env python3
# ============================================================================
#  ArchBB 1.83 — leggi_raw.py
#  Legge i file /ARCHBB/RAW_NNNN.BIN prodotti dal firmware di diagnostica
#  (env archbb_183_raw_diag) e ne analizza il contenuto.
#
#  USO:   python leggi_raw.py RAW_0001.BIN
#         python leggi_raw.py RAW_0001.BIN --csv uscita.csv
#
#  FORMATO FILE
#    header 16B: "ABBR" + ver(u16) + odr_hz(u16) + n_samples(u32) + riserva(u32)
#    poi N record da 30B: seq(u16) ts_us(u32) ax ay az gx gy gz (float32 LE)
#
#  A COSA SERVE
#    Il trigger della 1.83 non scatta con la freccia vera, quindi non abbiamo
#    mai registrato uno scocco reale su questa scheda. Questo log registra TUTTO
#    senza trigger: qui si cerca l'evento e se ne misura la firma, da
#    confrontare con i 44 tiri della 1.69 (picco |az| mediano 38.4 m/s^2,
#    13 campioni sopra 20, asse dominante az in 44 casi su 44).
# ============================================================================
import struct, sys, math

HDR = 16
REC = 30

def load(path):
    d = open(path, 'rb').read()
    if d[:4] != b'ABBR':
        raise SystemExit(f"{path}: magic non valido ({d[:4]!r}), atteso b'ABBR'")
    ver, odr, n = struct.unpack('<HHI', d[4:12])
    disp = (len(d) - HDR) // REC
    if n == 0 or n > disp:
        n = disp                      # header non aggiornato: usa cio' che c'e'
    S = []
    for i in range(n):
        o = HDR + i * REC
        seq, ts, ax, ay, az, gx, gy, gz = struct.unpack('<HIffffff', d[o:o+REC])
        S.append((seq, ts, ax, ay, az, gx, gy, gz))
    return ver, odr, S

def analizza(path, csv_out=None):
    ver, odr_nom, S = load(path)
    n = len(S)
    if n < 2:
        raise SystemExit("file troppo corto")
    dur = (S[-1][1] - S[0][1]) / 1e6
    odr_real = n / dur if dur > 0 else 0

    print(f"File      : {path}")
    print(f"Versione  : {ver}   ODR nominale: {odr_nom} Hz")
    print(f"Campioni  : {n}   durata: {dur:.2f} s   ODR reale: {odr_real:.1f} Hz")

    # continuita' dei seq (se salta, il log ha perso campioni)
    salti = sum(1 for i in range(n-1)
                if ((S[i+1][0] - S[i][0]) & 0xFFFF) != 1)
    print(f"Salti seq : {salti}   {'(log integro)' if salti == 0 else '<-- ATTENZIONE: campioni persi'}")

    mods = [math.sqrt(s[2]**2 + s[3]**2 + s[4]**2) for s in S]
    print()
    print(f"|a| medio : {sum(mods)/n:6.2f} m/s^2   (a riposo deve essere ~9.8)")
    print(f"|a| max   : {max(mods):6.2f} m/s^2  al campione {mods.index(max(mods))}")
    print()
    print("PICCHI PER ASSE (moduli):")
    for k, nome in ((2, 'ax'), (3, 'ay'), (4, 'az')):
        v = [abs(s[k]) for s in S]
        i = v.index(max(v))
        print(f"  |{nome}| max = {max(v):6.2f} m/s^2  (campione {i}, t={S[i][1]/1e6:.2f}s)")

    # Eventi: finestre in cui |a| supera nettamente la gravita'
    TH = 20.0
    print()
    print(f"EVENTI con |az| > {TH} m/s^2:")
    ev, cur = [], None
    for i, s in enumerate(S):
        if abs(s[4]) > TH:
            if cur is None: cur = [i, i]
            else: cur[1] = i
        else:
            if cur: ev.append(tuple(cur)); cur = None
    if cur: ev.append(tuple(cur))
    if not ev:
        print("  NESSUNO. -> con la soglia a 20 il trigger non sarebbe MAI scattato.")
        # allora cerchiamo dove sta davvero l'energia
        print()
        print("  Ricerca dell'evento sull'asse a piu' alta energia:")
        for k, nome in ((2,'ax'), (3,'ay'), (4,'az')):
            v = [abs(s[k]) for s in S]
            sopra = sum(1 for x in v if x > 15)
            print(f"    |{nome}|: max={max(v):5.1f}  campioni sopra 15 = {sopra}")
    else:
        for a, b in ev:
            seg = [abs(s[4]) for s in S[a:b+1]]
            print(f"  campioni {a}-{b} ({b-a+1} camp, {(b-a+1)/odr_real*1000:.0f} ms)"
                  f"  picco {max(seg):.1f} m/s^2")

    print()
    print("RIFERIMENTO — scocco vero misurato sulla 1.69 (44 tiri):")
    print("  asse dominante az in 44/44 | picco |az| mediano 38.4 | 13 campioni sopra 20")

    if csv_out:
        with open(csv_out, 'w', newline='') as f:
            f.write("idx,seq,ts_us,ax,ay,az,gx,gy,gz\n")
            for i, s in enumerate(S):
                f.write(f"{i},{s[0]},{s[1]},{s[2]:.4f},{s[3]:.4f},{s[4]:.4f},"
                        f"{s[5]:.4f},{s[6]:.4f},{s[7]:.4f}\n")
        print(f"\nCSV scritto: {csv_out}")

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__ or "uso: python leggi_raw.py RAW_0001.BIN [--csv out.csv]")
        raise SystemExit(1)
    csv_out = None
    if '--csv' in sys.argv:
        csv_out = sys.argv[sys.argv.index('--csv') + 1]
    analizza(sys.argv[1], csv_out)
