#!/usr/bin/env python3
# ============================================================================
#  ArchBB — archbb_dsp.py
#  L'IMPLEMENTAZIONE DI RIFERIMENTO delle convenzioni di analisi.
#  Cesare Pagura · Padova/Noale IT · luglio 2026
# ----------------------------------------------------------------------------
#  PERCHE' ESISTE QUESTO FILE
#    Dopo la Fase 10 gli strumenti che analizzano i burst sono due: l'app HTML
#    (JavaScript, occhiata rapida sul telefono) e questo Streamlit (Python,
#    analisi seria alla scrivania). Due implementazioni della stessa fisica in
#    due linguaggi sono la regola §4.1 travestita: il giorno in cui il JS dice
#    8,8 Hz e Python 9,4 Hz sullo stesso tiro, nessuno dei due e' credibile.
#
#    La cura non e' "scrivere lo stesso codice due volte con attenzione": e'
#    dichiarare UNA SPECIFICA e implementarla due volte sapendo di farlo.
#    Questo file E' la specifica. Il JS dell'app la cita nei commenti; questo
#    modulo la scrive per esteso, con i numeri che l'hanno stabilita.
#
#  LE CINQUE CONVENZIONI (e da dove vengono)
#    1. fs MISURATA dallo SPAN, non dalla mediana dei Δt.      -> §fs
#    2. fs NOMINALE (224 Hz) per replicare il firmware.        -> §fs
#    3. finestra spettrale di default 300 ms.                  -> §finestra
#    4. sotto f_min = 2/T si legge deriva, non frequenza.      -> §f_min
#    5. sopra 5,39 % dell'ODR c'e' il filtro dell'IMU, non l'arco. -> §lpf
#
#  Ogni convenzione ha una costante nominata qui sotto. Nessun numero magico
#  sparso nel codice dell'interfaccia: se una soglia va cambiata, si cambia in
#  questo file e cambia ovunque.
# ============================================================================
from __future__ import annotations

# VERSIONE DEL MODULO — deve coincidere con quella attesa da app.py.
#  Serve perche' i quattro file sono un insieme: estrarre lo zip sopra una
#  cartella esistente e sostituirne solo alcuni lascia un miscuglio che si
#  manifesta come AttributeError dentro una scheda, dieci schermate piu' in la'
#  rispetto alla causa. Con la marca, l'app lo dice subito e dice quale file.
VERSIONE = "2026.08.25-01"

import math
import struct
from dataclasses import dataclass, field

import numpy as np
from scipy import signal as sps

# ────────────────────────────────────────────────────────────────────────────
#  §formato — layout binario, identico a sd_storage.cpp del firmware
# ────────────────────────────────────────────────────────────────────────────
BURST_MAGIC     = b"ABB1"
BURST_HDR_FMT   = "<4sBBHHH4s"          # magic ver odr n trig shot riservati
BURST_HDR_SIZE  = struct.calcsize(BURST_HDR_FMT)
IMU_FMT         = "<HI6f"               # seq ts_us ax ay az gx gy gz
IMU_SIZE        = struct.calcsize(IMU_FMT)
assert BURST_HDR_SIZE == 16 and IMU_SIZE == 30, "layout binario divergente dal firmware"

# ────────────────────────────────────────────────────────────────────────────
#  §fs — le DUE frequenze, che non vanno confuse
# ────────────────────────────────────────────────────────────────────────────
#  fs MISURATA: la verita' fisica, dallo span dei timestamp. Serve a tutto cio'
#    che riguarda il SEGNALE (spettri, smorzamento, integrazioni).
#
#    Perche' lo span e non la mediana dei Δt — misurato su burst_0001.bin del
#    25/07/2026, 671 intervalli:
#         Δt = 4,0 ms -> 272 volte
#         Δt = 5,0 ms -> 391 volte
#    Il tick da 1 ms di FreeRTOS batte contro il periodo ideale e produce due
#    valori alternati. La MEDIANA cade su 5,0 ms e dichiara 200,0 Hz; lo span
#    (e la media, qui identici) danno 217,79 Hz. Un errore dell'8,9 % che
#    sposterebbe ogni picco spettrale. L'app 1.27/1.29 e la prima versione di
#    analisi_firmware.py usavano la mediana: e' l'errore corretto qui.
#
#    Lo span e' valido solo se non manca nessun campione: si verifica prima la
#    continuita' della seq (che sui nove burst del 25/07 e' perfetta, 671
#    salti su 671 pari a 1). Se ci fosse un buco, si ripiega sulla media dei
#    Δt plausibili.
#
#  fs NOMINALE: quella che il FIRMWARE crede di avere (odrToHz -> 224 Hz).
#    Serve SOLO a replicare i calcoli di bordo, perche' il firmware dimensiona
#    le sue finestre con quella. Usarla per lo spettro sarebbe sbagliato;
#    usare la misurata per replicare il firmware darebbe finestre di lunghezza
#    diversa e numeri leggermente diversi dal CSV. Due scopi, due frequenze,
#    dichiarate.
ODR_NOMINALE_HZ = {0: 112, 1: 224, 2: 448}     # specchio di odrToHz() in config.h
FS_FALLBACK_HZ  = 218.0

# ────────────────────────────────────────────────────────────────────────────
#  §finestra — larghezza della finestra spettrale post-scocco
# ────────────────────────────────────────────────────────────────────────────
#  300 ms non e' una preferenza: e' dove la misura si RIPETE. Frequenza di
#  picco sui nove tiri del 25/07, per larghezza di finestra:
#      300 ms  -> 10,2 9,4 8,0 8,7 8,2 11,5 8,8 7,8 7,3   media 8,87  sd 1,31
#      500 ms  ->  4,0 7,4 7,7 9,6 8,2  4,6 10,0 9,2 6,2  media 7,43  sd 2,13
#     1000 ms  -> si sfilaccia fino al limite dei 2 Hz (domina la coda del
#                 follow-through, che non e' un'oscillazione)
#  Si sceglie la finestra con sd minima. Le altre restano selezionabili
#  nell'interfaccia perche' vedere il confronto insegna piu' di leggerlo.
FINESTRA_SPETTRO_MS = 300
FINESTRE_DISPONIBILI_MS = (150, 300, 500, 1000)

# ────────────────────────────────────────────────────────────────────────────
#  §f_min — sotto due cicli di finestra non c'e' frequenza
# ────────────────────────────────────────────────────────────────────────────
#  f_min = CICLI_MINIMI / T. Sotto quella soglia la finestra non contiene due
#  oscillazioni complete e quello che si legge e' una DERIVA. Senza questa
#  regola, sui dati veri il picco cadeva a 0,1 Hz su 7 tiri su 9: la coda del
#  follow-through interpretata come frequenza.
#
#  Corollario diagnostico: se il picco cade ENTRO un bin da f_min, non e' un
#  picco — e' l'algoritmo che restituisce l'estremo. Con finestra 150 ms tutti
#  e nove i tiri davano 13,3 Hz, cioe' esattamente f_min. Un numero identico
#  su nove tiri diversi non e' una misura ripetibile: e' un artefatto.
CICLI_MINIMI = 2.0

# ────────────────────────────────────────────────────────────────────────────
#  §lpf — il muro del filtro interno del QMI8658
# ────────────────────────────────────────────────────────────────────────────
#  imu.cpp configura accelerometro e giroscopio con LPF_MODE_2, che il
#  datasheet QMI8658 (registro CTRL5, aLPF_MODE = 10) definisce come 5,39 %
#  dell'ODR. Ai 217,7 Hz effettivi il taglio a -3 dB cade a ~11,7 Hz.
#
#  Verifica sui dati (energia per banda, finestra 300 ms, nove tiri):
#      3– 8 Hz   61.7  61.9  74.1  41.5 238.4  29.0  72.5  29.3  30.8
#      8–15 Hz   84.1  69.0  76.5  90.7 262.6  22.5 155.7  21.4  29.8
#     15–30 Hz    4.5   1.5   1.3   1.8   3.8   1.0   0.7   1.0   0.9
#     30–60 Hz    0.0   0.0   0.0   0.0   0.0   0.0   0.0   0.0   0.0
#
#  Zero ESATTO sopra i 30 Hz. Conseguenza: la banda 15–80 Hz che la 1.69
#  chiamava "vibrazione dei flettenti" con questa configurazione NON e'
#  misurabile. Quello che resta, 5–15 Hz, e' l'oscillazione del sistema
#  arciere-arco dopo il rilascio — dentro la banda passante, quindi misurabile
#  bene, e la base della metrica di smorzamento qui sotto.
LPF_FRAZIONE_ODR = 0.0539
BANDA_OSCILLAZIONE_HZ = (4.0, 16.0)    # dove vive il modo dominante misurato


# ============================================================================
#  Burst — contenitore e lettura
# ============================================================================
@dataclass
class Burst:
    """Un burst letto da burst_NNNN.bin, in array numpy per asse."""
    ver: int
    odr_code: int
    n: int
    trig: int
    shot: int
    seq: np.ndarray
    ts: np.ndarray          # microsecondi da boot
    ax: np.ndarray; ay: np.ndarray; az: np.ndarray
    gx: np.ndarray; gy: np.ndarray; gz: np.ndarray
    fs: float               # MISURATA (span) — per il segnale
    fs_nominale: float      # dal codice ODR  — per replicare il firmware
    seq_continua: bool
    nome: str = ""
    # Scostamento del riferimento temporale, in ms. Serve al riancoraggio del
    # t=0 (archbb_causale.applica_riancoraggio): l'istante del trigger e' un
    # riferimento mediocre, perche' dipende da una soglia che si aggancia a
    # punti diversi della coda dello scocco.
    # E' un campo ASSOLUTO e non cumulativo: riapplicare il riancoraggio
    # riscrive questo valore invece di sommarlo, quindi non puo' accumulare.
    off_ms: float = 0.0

    @property
    def t_ms(self) -> np.ndarray:
        """Tempo in millisecondi con lo ZERO sullo scocco (o sul riferimento
        riancorato, se off_ms e' stato impostato)."""
        return (self.ts - self.ts[self.trig]) / 1000.0 - self.off_ms

    @property
    def f_taglio_lpf(self) -> float:
        return self.fs * LPF_FRAZIONE_ODR

    def post(self, campo: str, durata_ms: float) -> np.ndarray:
        """Porzione post-scocco di un asse, lunga al piu' durata_ms."""
        m = min(self.n - self.trig, int(round(durata_ms / 1000.0 * self.fs)))
        return getattr(self, campo)[self.trig:self.trig + m]


def leggi_burst(dati: bytes, nome: str = "") -> Burst | None:
    """Legge un burst_NNNN.bin. Ritorna None se il file non e' un burst valido."""
    if len(dati) < BURST_HDR_SIZE:
        return None
    magic, ver, odr, n, trig, shot, _ = struct.unpack(BURST_HDR_FMT, dati[:BURST_HDR_SIZE])
    if magic != BURST_MAGIC:
        return None

    corpo = dati[BURST_HDR_SIZE:]
    disponibili = len(corpo) // IMU_SIZE
    if disponibili == 0:
        return None
    if disponibili < n:
        # File troncato (card estratta durante la scrittura). Si legge quel che
        # c'e' invece di scartare tutto: un burst parziale ha comunque un pre.
        n = disponibili
        trig = min(trig, n - 1)

    # Lettura vettoriale: un dtype strutturato invece di 672 unpack in un ciclo.
    dt = np.dtype([("seq", "<u2"), ("ts", "<u4"),
                   ("ax", "<f4"), ("ay", "<f4"), ("az", "<f4"),
                   ("gx", "<f4"), ("gy", "<f4"), ("gz", "<f4")])
    rec = np.frombuffer(corpo[: n * IMU_SIZE], dtype=dt, count=n)

    seq = rec["seq"].astype(np.int64)
    ts = rec["ts"].astype(np.float64)
    fs, continua = frequenza_misurata(seq, ts)

    return Burst(
        ver=ver, odr_code=odr, n=int(n), trig=int(trig), shot=int(shot),
        seq=seq, ts=ts,
        ax=rec["ax"].astype(np.float64), ay=rec["ay"].astype(np.float64),
        az=rec["az"].astype(np.float64), gx=rec["gx"].astype(np.float64),
        gy=rec["gy"].astype(np.float64), gz=rec["gz"].astype(np.float64),
        fs=fs, fs_nominale=float(ODR_NOMINALE_HZ.get(odr, 224)),
        seq_continua=continua, nome=nome,
    )


def frequenza_misurata(seq: np.ndarray, ts: np.ndarray,
                       fallback: float = FS_FALLBACK_HZ) -> tuple[float, bool]:
    """fs reale dai timestamp. Vedi §fs per il perche' dello span.

    Ritorna (fs, seq_continua). Il secondo valore va mostrato all'utente: se e'
    False, lo span non e' affidabile e si e' ripiegato sulla media.
    """
    n = len(ts)
    if n < 10:
        return fallback, False

    salti = np.diff(seq) & 0xFFFF          # il contatore a 16 bit puo' avvolgere
    continua = bool(np.all(salti == 1))

    if continua:
        span = ts[-1] - ts[0]
        if span > 0:
            fs = (n - 1) * 1e6 / span
            if 20 < fs < 2000:
                return float(fs), True

    d = np.diff(ts)
    d = d[(d > 0) & (d < 100000)]
    if len(d) < 5:
        return fallback, continua
    fs = 1e6 / float(np.mean(d))
    return (float(fs) if 20 < fs < 2000 else fallback), continua


# ============================================================================
#  Raddrizzamento del frame — sedute anteriori al firmware 1.83-F11
# ============================================================================
#  I burst di quelle sedute sono salvati nel frame canonico di `mount = 0`,
#  mentre la scheda era fisicamente a DESTRA (`mount = 2`). Il passaggio fra i
#  due si semplifica, perche' il flip-Z e' involutivo e M0 e' l'identita':
#
#      salvato = (x,  y, -z_grezzo)              M0 + flip
#      voluto  = (x, -z_grezzo, -y)              M2 + flip
#      voluto in funzione del salvato = (x, z_salv, -y_salv)
#
#  cioe' basta applicare M2 ai campioni gia' salvati. Verificato sui nove tiri
#  del 25/07: dopo il raddrizzamento il cant sta fra -2,4 e +1,7 gradi (rollio
#  involontario) e l'alzo a -17 (punta in basso, sagoma 3D a terra).
#
#  PERCHE' RADDRIZZARE INVECE DI SCARTARE
#    Le sedute registrate non si rifanno. L'informazione per rimetterle a posto
#    esiste — il DIAG ASSI del 26/07 l'ha stabilita, con margine 36 su soglia 5
#    — quindi usarla e' il minimo. Il riflash serve al DISPOSITIVO (display
#    corretto mentre si tira, taratura del montaggio, session.txt che si
#    autodescrive), non a rileggere quello che si ha gia' in mano.
def raddrizza(b: Burst) -> Burst:
    """Applica M2 ai campioni: (x, y, z) -> (x, z, -y) su accel e giroscopio."""
    import dataclasses
    return dataclasses.replace(
        b,
        ay=b.az.copy(),  az=-b.ay,
        gy=b.gz.copy(),  gz=-b.gy,
        nome=b.nome + "+raddrizzato",
    )


# ============================================================================
#  Spettro
# ============================================================================
@dataclass
class Spettro:
    freq: np.ndarray
    ampiezza: np.ndarray
    ris_hz: float           # risoluzione VERA = 1/T
    passo_bin_hz: float     # passo dei bin dopo zero-padding
    f_min: float
    n_campioni: int
    durata_ms: float
    picco_hz: float
    picco_amp: float
    picco_al_bordo: bool    # se True il "picco" e' l'estremo, non una misura


def spettro(x: np.ndarray, fs: float, n_fft: int = 2048,
            f_max: float = 60.0) -> Spettro | None:
    """Spettro di ampiezza a finestra di Hann, con zero-padding.

    ATTENZIONE alla distinzione che si confonde sempre:
      · passo dei bin    = fs/n_fft  -> lo migliora lo zero-padding
      · risoluzione VERA = fs/m = 1/T -> la migliora SOLO una finestra piu'
                                         lunga
    Lo zero-padding INTERPOLA: aiuta a stimare dove cade il picco, non a
    separare due modi vicini. Con 300 ms la risoluzione vera e' ~3,3 Hz.
    """
    m = len(x)
    if m < 8:
        return None
    w = np.hanning(m)
    xw = (x - x.mean()) * w
    X = np.fft.rfft(xw, n_fft)
    amp = 2.0 * np.abs(X) / w.sum()
    freq = np.fft.rfftfreq(n_fft, 1.0 / fs)

    ris = fs / m
    f_min = CICLI_MINIMI * ris

    banda = (freq >= f_min) & (freq <= f_max)
    if not np.any(banda):
        return None
    k = int(np.argmax(amp[banda]))
    f_pk = float(freq[banda][k])
    a_pk = float(amp[banda][k])

    return Spettro(freq=freq, ampiezza=amp, ris_hz=ris, passo_bin_hz=fs / n_fft,
                   f_min=f_min, n_campioni=m, durata_ms=m / fs * 1000.0,
                   picco_hz=f_pk, picco_amp=a_pk,
                   # "Al bordo" = il massimo e' caduto praticamente SU f_min,
                   # cioe' l'algoritmo ha restituito l'estremo della banda.
                   # Tolleranza al 5 %, non "una risoluzione": con finestra
                   # 300 ms la risoluzione vera e' 3,3 Hz e usarla come
                   # tolleranza marcava come sospetti 7 picchi su 9 che erano
                   # perfettamente buoni. Il 5 % coglie il caso reale (finestra
                   # 150 ms: f_min 13,2 e picco 13,3 su tutti e nove i tiri)
                   # senza falsi allarmi su quello sano.
                   picco_al_bordo=bool(f_pk <= f_min * 1.05))


def spettrogramma(x: np.ndarray, fs: float, finestra_ms: float = 300.0,
                  sovrapposizione: float = 0.90):
    """STFT del segnale post-scocco: mostra COME l'energia decade nel tempo.

    Compromesso tempo/frequenza dichiarato: con finestra 300 ms la risoluzione
    in frequenza e' ~3,3 Hz e quella temporale 300 ms. Non e' un difetto della
    STFT, e' il principio di indeterminazione: si sceglie cosa vedere.
    La forte sovrapposizione (90 %) non aggiunge informazione, rende solo
    leggibile il decadimento invece di mostrarlo a scalini.
    """
    nper = max(16, int(round(finestra_ms / 1000.0 * fs)))
    nper = min(nper, len(x))
    nover = int(nper * sovrapposizione)
    f, t, Sxx = sps.spectrogram(x - x.mean(), fs=fs, window="hann",
                                nperseg=nper, noverlap=nover,
                                nfft=max(512, nper * 4), mode="magnitude")
    return f, t, Sxx


# ============================================================================
#  Smorzamento — la metrica nuova che questo hardware PUO' misurare
# ============================================================================
@dataclass
class Smorzamento:
    f0_hz: float            # frequenza del modo dominante
    lambda_s: float         # tasso di decadimento [1/s]
    tau_ms: float           # costante di tempo = 1/lambda
    t90_ms: float           # tempo per scendere al 10 % dell'ampiezza iniziale
    zeta: float             # rapporto di smorzamento (adimensionale)
    r2: float               # bonta' dell'adattamento sul log dell'inviluppo
    t_ms: np.ndarray        # asse tempi dell'inviluppo (per il grafico)
    inviluppo: np.ndarray
    adattamento: np.ndarray
    valido: bool
    motivo: str = ""


def smorzamento(b: Burst, campo: str = "az", durata_ms: float = 900.0,
                banda: tuple[float, float] = BANDA_OSCILLAZIONE_HZ) -> Smorzamento:
    """Quanto ci mette il sistema arciere-arco a fermarsi dopo il rilascio.

    PERCHE' QUESTA METRICA E PERCHE' PROPRIO ORA
      Il filtro interno dell'IMU (§lpf) chiude la banda dei flettenti, quindi
      la "vibrazione dell'attrezzo" non e' misurabile. Ma il modo dominante
      misurato — 8–11 Hz — sta comodamente DENTRO la banda passante, quindi il
      suo decadimento si misura bene. E' una grandezza diversa e forse piu'
      interessante: non dice quanto vibra l'arco, dice quanto in fretta
      l'insieme arciere+arco torna fermo. Un follow-through che si assesta in
      200 ms e uno che oscilla per mezzo secondo sono due gesti diversi.

    METODO
      1. passa-banda attorno al modo dominante (elimina la deriva lenta del
         follow-through, che non e' oscillazione, e il rumore alto);
      2. inviluppo analitico con la trasformata di Hilbert;
      3. retta ai minimi quadrati su log(inviluppo) -> il coefficiente
         angolare e' -lambda;
      4. zeta = lambda / sqrt(lambda^2 + omega^2).

    L'R^2 dell'adattamento viene restituito e MOSTRATO: se l'inviluppo non e'
    esponenziale, zeta non significa niente e va detto, non nascosto.
    """
    x = b.post(campo, durata_ms)
    fs = b.fs
    vuoto = np.array([])
    if len(x) < 32:
        return Smorzamento(0, 0, 0, 0, 0, 0, vuoto, vuoto, vuoto, False,
                           "finestra post-scocco troppo corta")

    # 1) passa-banda. Il taglio alto non supera il filtro dell'IMU: chiedere
    #    banda dove il sensore non risponde produrrebbe solo rumore amplificato.
    lo, hi = banda
    hi = min(hi, b.f_taglio_lpf * 1.6, fs / 2 * 0.95)
    if hi <= lo:
        return Smorzamento(0, 0, 0, 0, 0, 0, vuoto, vuoto, vuoto, False,
                           "banda utile nulla: filtro IMU troppo stretto")
    sos = sps.butter(4, [lo, hi], btype="bandpass", fs=fs, output="sos")
    xf = sps.sosfiltfilt(sos, x - x.mean())

    # 2) inviluppo analitico
    env = np.abs(sps.hilbert(xf))
    # lisciatura leggera: l'inviluppo di Hilbert e' rumoroso a campione singolo
    k = max(3, int(round(0.03 * fs)) | 1)
    env = sps.savgol_filter(env, k, 2) if len(env) > k else env
    env = np.maximum(env, 1e-9)

    t = np.arange(len(env)) / fs

    # 3) l'adattamento parte dal MASSIMO dell'inviluppo (prima c'e' la salita
    #    dell'impulso, non il decadimento) e si ferma quando l'ampiezza scende
    #    sotto il 5 % del picco (sotto quella soglia si sta adattando rumore).
    i0 = int(np.argmax(env))
    soglia = env[i0] * 0.05
    sotto = np.where(env[i0:] < soglia)[0]
    i1 = i0 + int(sotto[0]) if len(sotto) else len(env)
    if i1 - i0 < 16:
        return Smorzamento(0, 0, 0, 0, 0, 0, t * 1000, env, vuoto, False,
                           "decadimento troppo breve per un adattamento")

    tt = t[i0:i1]
    yy = np.log(env[i0:i1])
    A = np.vstack([tt, np.ones_like(tt)]).T
    coef, *_ = np.linalg.lstsq(A, yy, rcond=None)
    pend, inter = float(coef[0]), float(coef[1])
    lam = -pend

    yhat = A @ coef
    ss_res = float(np.sum((yy - yhat) ** 2))
    ss_tot = float(np.sum((yy - yy.mean()) ** 2))
    r2 = 1.0 - ss_res / ss_tot if ss_tot > 0 else 0.0

    fit = np.full_like(env, np.nan)
    fit[i0:i1] = np.exp(yhat)

    if lam <= 0:
        return Smorzamento(0, lam, 0, 0, 0, r2, t * 1000, env, fit, False,
                           "l'inviluppo non decade (lambda <= 0)")

    # f0: la frequenza del modo che sta decadendo. Va stimata sulla parte
    # INIZIALE, dove l'ampiezza e' alta e il rapporto segnale/rumore migliore —
    # e sulla stessa finestra di convenzione (§finestra), non sui 900 ms interi.
    # Stimandola su tutta la finestra lunga i valori uscivano fra 4,3 e 10,6 Hz
    # sugli stessi nove tiri: non una variabilita' del gesto, una variabilita'
    # del metodo. E siccome zeta dipende da f0, l'errore si propagava dritto
    # nel numero finale.
    n_f0 = min(len(xf), int(round(FINESTRA_SPETTRO_MS / 1000.0 * fs)))
    sp = spettro(xf[i0:i0 + n_f0] if i0 + n_f0 <= len(xf) else xf[:n_f0],
                 fs, f_max=hi)
    f0 = sp.picco_hz if sp else 0.0
    omega = 2 * math.pi * f0
    zeta = lam / math.sqrt(lam * lam + omega * omega) if omega > 0 else float("nan")

    return Smorzamento(
        f0_hz=f0, lambda_s=lam, tau_ms=1000.0 / lam,
        t90_ms=1000.0 * math.log(10.0) / lam, zeta=zeta, r2=r2,
        t_ms=t * 1000, inviluppo=env, adattamento=fit, valido=True,
    )


# ============================================================================
#  Finestra calma — la porzione pre-scocco piu' ferma
# ============================================================================
#  Vive qui e non in archbb_metrics perche' e' una primitiva di segnale, e
#  perche' la traccia ne ha bisogno: importarla da metrics creerebbe un ciclo.
#  Punteggio da minimizzare: |media(|a|) - g| + sd(|a|). Le TRONCATURE sono
#  quelle del firmware: il cast (uint16_t) in C tronca, e arrotondare "per fare
#  meglio" darebbe una finestra diversa da quella del dispositivo.
GUARD_CALMA_S = 0.050
AMPIEZZA_CALMA_S = 0.200
GRAVITA = 9.81


def finestra_calma(b: Burst):
    fs = b.fs_nominale
    guard = int(GUARD_CALMA_S * fs)
    win = int(AMPIEZZA_CALMA_S * fs)
    we_max = b.trig - guard
    if we_max < win or win < 2:
        return None
    mag = np.sqrt(b.ax ** 2 + b.ay ** 2 + b.az ** 2)
    c1 = np.concatenate([[0.0], np.cumsum(mag)])
    c2 = np.concatenate([[0.0], np.cumsum(mag ** 2)])
    we = np.arange(win, we_max + 1)
    ws = we - win
    media = (c1[we] - c1[ws]) / win
    var = np.maximum(0.0, ((c2[we] - c2[ws]) - win * media ** 2) / (win - 1))
    k = int(np.argmin(np.abs(media - GRAVITA) + np.sqrt(var)))
    return int(ws[k]), int(we[k])


# ============================================================================
#  Traccia 2D — ricostruzione angolare del movimento dell'arco
# ============================================================================
#  Porta in Python la traccia dell'app 1.29/v0.3. Stessa fisica:
#
#    PITCH  filtro complementare fra giroscopio e accelerometro. La gravita'
#           osserva l'inclinazione, quindi l'integrazione del gyro si puo'
#           correggere e non deriva.
#    YAW    integrazione PURA del rate proiettato sulla verticale vera: nessun
#           accelerometro puo' vedere una rotazione attorno alla gravita'. E'
#           il motivo per cui il bias va stimato bene, ed e' l'unica difesa
#           contro la deriva.
#    BIAS   ZUPT sui campioni pre-scocco fermi (|gyro| sotto soglia).
#
#  IL VERSO DELLO YAW NON E' VALIDATO SULLA 1.83. Il -1 viene dal banco della
#  1.69. Con il montaggio a destra la geometria e' un'altra e il segno potrebbe
#  essere invertito: si risolve con un test statico a rotazione nota, non
#  guardando una traccia. Finche' non e' fatto, fidarsi della FORMA e non del
#  verso — per questo il parametro e' esposto, non nascosto in una costante.
# ----------------------------------------------------------------------------
#  ORIENTAMENTO DELLA TRACCIA — scelta empirica, dichiarata
# ----------------------------------------------------------------------------
#  La traccia calcola due serie:
#     pitch  = integrale di gy
#     yaw    = integrale del rate proiettato sulla verticale vera
#
#  Sui dati veri la serie "yaw" porta il movimento ampio (durante il rilascio
#  gx integra -4,3 gradi contro i -2,5 di gy; sul pre-scocco 5,6 contro 2,7),
#  mentre l'arciere osserva che il movimento ampio e' l'ALZO — la mira si fa
#  in alto e in basso, non a destra e sinistra.
#
#  Non ho potuto risolverlo con i dati di tiro: l'accelerometro in movimento
#  misura anche accelerazione lineare e non fa da riferimento, e la prova
#  statica delle pose valida l'accelerometro ma NON il giroscopio (una posa
#  ferma ha velocita' angolare zero). La prova dinamica scritta apposta non ha
#  dato esito.
#
#  Quindi qui si applica l'osservazione dell'arciere: la serie ampia va
#  sull'asse verticale. E' una SCELTA DI PRESENTAZIONE basata su
#  un'osservazione, non una misura — per questo sta in una costante con un
#  nome parlante invece che nascosta nel codice del disegno.
#
#  Per tornare indietro: mettere False. Cambia solo la traccia; hold e torsione
#  restano sugli assi che usa il firmware, e se un giorno si scoprisse che il
#  giroscopio e' permutato andrebbero riviste anche quelle.
TRACCIA_SCAMBIA_ASSI = True

SEGNO_YAW_DEFAULT = -1.0
ALPHA_COMPLEMENTARE = 0.98
SOGLIA_FERMO_DPS = 15.0


@dataclass
class Traccia:
    pitch: np.ndarray        # gradi, zero allo scocco
    yaw: np.ndarray          # gradi, zero allo scocco
    t_ms: np.ndarray
    trig: int
    n_bias: int              # su quanti campioni e' stato stimato il bias
    segno_yaw: float
    pitch_scocco: float      # pitch ASSOLUTO allo scocco = angolo di alzo


def traccia(b: Burst, segno_yaw: float = SEGNO_YAW_DEFAULT,
            off_alzo_deg: float = 0.0) -> Traccia:
    """Ricostruzione angolare per INTEGRAZIONE PURA del giroscopio.

    PERCHE' NON IL FILTRO COMPLEMENTARE DELLA 1.29
      Alpha = 0,98 (costante di tempo 0,22 s) sulla 1.83 NON funziona, e i dati
      lo dicono senza ambiguita'. Misurato sulla seduta del 27/07, fase di mira
      (da -220 a -40 ms), pitch riferito allo scocco:

          alpha = 0,98     media +8,09 gradi   escursione 2,69
          alpha = 0,995    media +2,30 gradi   escursione 0,65
          giroscopio puro  media +0,26 gradi   escursione 0,19

      L'arciere in mira e' FERMO, e il giroscopio integrato lo conferma (±1
      grado su 800 ms). L'accelerometro nello stesso intervallo oscilla di 14
      gradi: quella e' accelerazione LINEARE, non assetto, e il complementare
      la lascia passare. Peggio: viene trascinato dal rilascio PRIMA che il
      trigger scatti, quindi il riferimento allo scocco e' gia' spostato di 8
      gradi e la fase di mira finisce in alto sul grafico invece che al centro.
      E' il motivo per cui la traccia sembrava capovolta.

      Su 3 secondi, col bias preso dalla finestra calma, la deriva
      dell'integrazione pura e' confrontabile con quella del filtro (+2,87
      contro +3,29 gradi a +200 ms): non si perde nulla e si guadagna un
      riferimento onesto.

    BIAS DALLA FINESTRA CALMA, non dal criterio |gyro| < 15 dps della 1.29:
      quella soglia lascia dentro la trazione e la salita dell'arco, dove la
      rotazione e' vera e non e' bias. La finestra calma e' la stessa che il
      firmware usa per cant, alzo e hold: un solo criterio di "fermo" in tutto
      il progetto.
    """
    n, trig, fs = b.n, b.trig, b.fs
    dt_nom = 1.0 / fs

    cw = finestra_calma(b)
    if cw is not None:
        idx = np.arange(cw[0], cw[1])
    else:
        fine_fermo = max(1, trig - int(round(0.100 * fs)))
        mod = np.sqrt(b.gx[:fine_fermo] ** 2 + b.gy[:fine_fermo] ** 2
                      + b.gz[:fine_fermo] ** 2)
        idx = np.where(mod < SOGLIA_FERMO_DPS)[0]
        if len(idx) == 0:
            idx = np.arange(0, fine_fermo)

    bgx, bgy, bgz = b.gx[idx].mean(), b.gy[idx].mean(), b.gz[idx].mean()
    mx, my, mz = b.ax[idx].mean(), b.ay[idx].mean(), b.az[idx].mean()
    gn = math.sqrt(mx * mx + my * my + mz * mz) or 1.0
    ux, uy, uz = mx / gn, my / gn, mz / gn      # verticale VERA nel frame arco

    dts = np.diff(b.ts) / 1e6
    dts = np.where((dts > 0) & (dts < 0.1), dts, dt_nom)

    # pitch = rotazione attorno all'asse laterale; yaw = rate proiettato sulla
    # verticale vera, che resta corretto anche con l'arco cantato.
    pitch = np.concatenate([[0.0], np.cumsum((b.gy[1:] - bgy) * dts)])
    yaw_rate = (b.gx - bgx) * ux + (b.gy - bgy) * uy + (b.gz - bgz) * uz
    yaw = np.concatenate([[0.0], np.cumsum(segno_yaw * yaw_rate[1:] * dts)])

    # Alzo assoluto dalla FINESTRA CALMA: allo scocco l'accelerometro e'
    # dominato dall'impulso e darebbe -81 gradi invece dei -19 veri.
    #
    # E VA TOLTO L'OFFSET DI TARATURA. Il firmware lo sottrae prima di scrivere
    # nel CSV; se qui non lo si toglie, i due numeri differiscono esattamente di
    # off_alzo_deg — misurato sulla seduta del 27/07: -1,74 gradi su TUTTI e
    # venti i tiri, cioe' precisamente il valore in session.txt. Uno scarto
    # costante come quello non e' rumore: e' una correzione dimenticata.
    pitch_scocco = math.degrees(math.atan2(mz, mx)) - off_alzo_deg

    pitch -= pitch[trig]
    yaw -= yaw[trig]
    return Traccia(pitch=pitch, yaw=yaw, t_ms=b.t_ms, trig=trig,
                   n_bias=int(len(idx)), segno_yaw=segno_yaw,
                   pitch_scocco=pitch_scocco)


def serie_traccia(tr: Traccia):
    """Le due serie NELL'ORDINE IN CUI VANNO DISEGNATE: (orizzontale, verticale).

    Un solo punto in cui l'orientamento viene applicato. Chi disegna non deve
    sapere che esiste uno scambio, e chi calcola non deve sapere come si
    disegna: e' la stessa ragione per cui il montaggio si applica in
    mount_apply e non in venti posti diversi.
    """
    if TRACCIA_SCAMBIA_ASSI:
        return -tr.pitch, tr.yaw
    return tr.yaw, tr.pitch


# ----------------------------------------------------------------------------
#  Fasi e scala della traccia — specifica presa dalla 1.29 (renderTrace v1.4)
# ----------------------------------------------------------------------------
#  LE FASI SI MISURANO IN CAMPIONI, NON IN MILLISECONDI.
#    La mira sono gli ultimi 50 campioni prima dello scocco (~220 ms a 224 Hz);
#    la separazione freccia-corda 15 ms dopo. Sono le stesse costanti della 1.29.
#
#  LA SCALA SI CALCOLA SOLO DA trigger-50 IN POI, e questo e' il punto che avevo
#  sbagliato. Il tratto pre-mira contiene il movimento di PORTATA dell'arco in
#  posizione: escursioni di 8-10 gradi contro i decimi di grado della mira. Se
#  entra nel calcolo della scala, schiaccia tutto il resto in un punto e la
#  traccia sembra una diagonale — che e' esattamente l'effetto "ruotato".
#  Il blu puo' e deve uscire dal riquadro: non e' diagnostico.
CAMPIONI_MIRA = 50          # fase verde: gli ultimi 50 campioni prima dello scocco
SEPARAZIONE_MS = 15.0       # la freccia lascia la corda ~12-18 ms dopo il trigger

# Colori delle quattro fasi, identici alla 1.29.
FASI_TRACCIA = (
    ("pre-mira",        "#4488ff", 1.2, 0.60),
    ("mira",            "#00e87a", 1.8, 0.90),
    ("freccia in arco", "#ff4455", 2.6, 1.00),   # rosso ACCESO
    ("follow-through",  "#ff4455", 1.3, 0.45),   # rosso SPENTO
)


@dataclass
class GeometriaTraccia:
    i_mira: int          # inizio della fase verde
    i_sep: int           # separazione freccia-corda
    scala_deg: float     # semi-ampiezza del riquadro, gradi
    yaw_sep_deg: float   # deriva laterale fra scocco e separazione
    # NIENTE "aim" qui. L'angolo di alzo affidabile e' quello della FINESTRA
    # CALMA, che il firmware calcola su 200 ms di mira stabile e scrive nel CSV.
    # Il pitch del filtro complementare allo scocco e' gia' trascinato dalla
    # dinamica del rilascio: su questi dati dava -25/-34 gradi contro i -18/-21
    # veri. La traccia serve a mostrare gli SCOSTAMENTI, non l'assetto assoluto.


def geometria_traccia(b: Burst, tr: Traccia) -> GeometriaTraccia:
    i_mira = max(0, b.trig - CAMPIONI_MIRA)
    i_sep = min(b.n - 1, b.trig + max(1, int(round(SEPARAZIONE_MS / 1000 * b.fs))))

    oriz, vert = serie_traccia(tr)
    utile = slice(i_mira, b.n)
    scala = max(0.5,
                float(np.max(np.abs(oriz[utile]))),
                float(np.max(np.abs(vert[utile]))))

    return GeometriaTraccia(i_mira=i_mira, i_sep=i_sep,
                            scala_deg=math.ceil(scala),
                            yaw_sep_deg=float(oriz[i_sep]))


# ============================================================================
#  Sovrapposizione — mediana e banda interquartile su piu' tiri
# ============================================================================
def sovrapponi(bursts: list[Burst], campo: str = "az",
               da_ms: float = -300.0, a_ms: float = 600.0,
               passo_ms: float = 2.0):
    """Allinea piu' tiri sullo scocco e restituisce mediana e quartili.

    Perche' MEDIANA e QUARTILI e non media e deviazione standard: con nove tiri
    un singolo gesto anomalo sposta la media e gonfia la sd, e il risultato e'
    una banda che non descrive nessun tiro. La mediana ignora l'outlier, e i
    quartili dicono dove sta la meta' centrale dei tiri — che e' esattamente la
    domanda "come tiro di solito".

    I tiri vengono ricampionati su una griglia comune perche' non e' garantito
    che abbiano stessa lunghezza, stesso trigger_idx o stesso fs: sui dati del
    25/07 lo sono, ma un burst troncato o un ODR diverso romperebbero
    l'allineamento per indice. Si allinea per TEMPO, che e' il riferimento vero.
    """
    griglia = np.arange(da_ms, a_ms + passo_ms, passo_ms)
    righe = []
    for b in bursts:
        t = b.t_ms
        y = getattr(b, campo)
        dentro = (griglia >= t[0]) & (griglia <= t[-1])
        r = np.full_like(griglia, np.nan, dtype=np.float64)
        r[dentro] = np.interp(griglia[dentro], t, y)
        righe.append(r)
    if not righe:
        return griglia, None
    M = np.vstack(righe)
    validi = np.sum(~np.isnan(M), axis=0)
    with np.errstate(all="ignore"):
        q1 = np.nanpercentile(M, 25, axis=0)
        med = np.nanpercentile(M, 50, axis=0)
        q3 = np.nanpercentile(M, 75, axis=0)
    for a in (q1, med, q3):
        a[validi < 2] = np.nan
    return griglia, dict(matrice=M, q1=q1, mediana=med, q3=q3, n_validi=validi)


def scarto_dalla_mediana(M: np.ndarray, mediana: np.ndarray) -> np.ndarray:
    """Per ogni tiro, lo scarto assoluto medio dalla mediana di sessione.

    E' il numero che risponde a "quale tiro somiglia meno agli altri", ed e' il
    complemento temporale della distanza di Mahalanobis sulle metriche: quella
    dice CHE un tiro e' diverso, questa dice DOVE nel tempo lo diventa.
    """
    with np.errstate(all="ignore"):
        d = np.abs(M - mediana[None, :])
        return np.nanmean(d, axis=1)
