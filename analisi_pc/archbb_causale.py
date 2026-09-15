#!/usr/bin/env python3
# ============================================================================
#  ArchBB — archbb_causale.py
#  Le metriche della FINESTRA CAUSALE, piu' la loro traduzione in centimetri.
#  Cesare Pagura · Padova/Noale IT · agosto 2026
# ----------------------------------------------------------------------------
#  IL FATTO CHE FONDA QUESTO MODULO (briefing 31/07)
#
#    La freccia lascia la corda ~17 ms dopo il rilascio. Il trigger scatta
#    sull'urto, cioe' ~29 ms DOPO che la freccia e' gia' partita.
#
#    Tutto quello che accade dopo −29 ms e' FOLLOW-THROUGH: diagnostica del
#    gesto, non causa dell'impatto. hold_deg e release_jerk stanno li' dentro.
#    Restano utili — un braccio che si abbassa subito dice qualcosa — ma non
#    possono comparire in una catena causale verso l'impatto.
#
#    Le fasi sono percio' tre e non due:
#        MIRA           fino al rilascio rilevato per tiro
#        CAUSALE        17 ms dal rilascio
#        FOLLOW-THROUGH il resto, etichettato come diagnostica
#
#  PERCHE' NON C'E' DERIVA SU 17 ms
#    incertezza sul bias dopo ZUPT (2,9 dps, 44 campioni)   0,007 gradi
#    fattore di scala (5%) su 0,6 gradi                     0,03
#    deriva termica                                         trascurabile
#    ------------------------------------------------------------------
#    totale < 0,04 contro un segnale di 0,58: SNR ~ 14.
#    Il problema di deriva che ha bloccato il progetto riguardava
#    integrazioni da 900 e 2000 ms. Qui non esiste.
#
#  CONVENZIONI — MISURATE al banco il 31/07, non dedotte. Valgono per mount=2.
#    alzo = atan2(az, ax)   positivo = punta alta
#    cant = atan2(ay, ax)   positivo = inclinato a destra
#    rate_alzo = −gy        rate_cant = +gz       rate_yaw = −gx (positivo=destra)
#  Se cambia mount_orientation vanno RIMISURATE, non ricalcolate.
# ============================================================================
from __future__ import annotations

VERSIONE = "2026.08.25-01"

import math
from dataclasses import dataclass

import numpy as np
import pandas as pd

from archbb_dsp import Burst

# ----------------------------------------------------------------------------
#  Parametri, tutti in un posto
# ----------------------------------------------------------------------------
G                 = 9.80665
ZUPT_A, ZUPT_B    = -280.0, -80.0   # finestra di mira: assetto E bias del gyro
T_CAUSALE_MS      = 17.0            # corsa utile: t = 2L/v ~ 2*0,50/54
K_RILASCIO        = 5.0             # soglia in unita' di rumore della mira
# ----------------------------------------------------------------------------
#  DOVE CERCARE IL RILASCIO — e perche' il limite destro non e' zero
# ----------------------------------------------------------------------------
#  Il rilascio DEVE precedere il trigger: la freccia parte 17 ms dopo il
#  rilascio e il trigger scatta sull'urto, ~29 ms dopo la partenza. Un rilascio
#  "rilevato" dopo il trigger non e' un rilascio, e' il contraccolpo.
#
#  La prima stesura cercava fino a +50 ms, e sul tiro 1 del 16/08 ha restituito
#  +13 ms: finestra causale interamente DOPO il trigger, cioe' la negazione del
#  modello. Il difetto si vedeva a occhio nel grafico del tiro — banda gialla a
#  destra della riga rossa — e per niente nei numeri, che sembravano normali.
#  Un vincolo fisico noto e non imposto al codice e' un errore che aspetta.
RIL_LO, RIL_HI    = -300.0, -20.0
G_TOLL, SD_MAX    = 0.60, 0.50      # criterio di finestra statica

# Guadagno di trasferimento riser -> freccia. NON e' 1.
#  (a) La freccia acquista velocita' MENTRE l'arco ruota: la direzione finale
#      e' la media dell'angolo pesata sugli incrementi di quantita' di moto,
#      cioe' ~Δθ/2. Guadagno 0,5 gia' da qui.
#  (b) La rotazione e' attorno al pivot della mano, non alla cocca.
#  Riscontro esterno: 0,1 gradi di torque -> ~50 mm a 70 m (Bow International),
#  contro 122 mm geometrici: rapporto 0,41.
#  QUESTO VALORE E' UNA STIMA, non una misura: va sostituito dalla pendenza che
#  esce dalla campagna di taratura. Finche' e' una stima, i centimetri che ne
#  derivano sono indicativi e l'interfaccia deve dirlo.
K_TRASFERIMENTO   = 0.45

# Parallasse del mirino per la presa mediterranea (briefing 31/07 §5):
#   errore_laterale = d · (P/L) · sin(cant),  P/L = E/R − tan(δ)
#   con E = 11,7 cm, R = 60 cm  ->  P/L ~ 0,136
P_SU_L_MEDITERRANEA = 0.1356


@dataclass
class Causale:
    """Le grandezze della finestra causale per un tiro.

    IL SUFFISSO `_causale` NON E' DECORATIVO. Il CSV porta gia' `alzo` e `cant`
    — l'assetto di MIRA, ricavato da alzo_cdeg/cant_cdeg — che sono grandezze
    diverse, di fase diversa, con unita' uguale. Usare gli stessi nomi faceva
    si' che il merge li rinominasse in alzo_x/alzo_y senza dire nulla, e la
    scheda causale falliva con un KeyError dieci schermate piu' in la' rispetto
    alla causa. Il nome porta la fase: e' l'unica difesa che non si dimentica.
    """
    shot: int
    t_rilascio_ms: float | None
    yaw_causale: float | None    # gradi, positivo = a destra
    alzo_causale: float | None   # gradi, positivo = punta alta
    cant_causale: float | None   # gradi, positivo = a destra
    rumore_dps: float | None     # oscillazione di mira: contesto per la soglia
    mira_statica: bool
    alzo_mira: float | None      # assetto assoluto in mira, dall'accelerometro
    cant_mira: float | None
    n_campioni: int              # quanti campioni dentro i 17 ms


def _statica(b: Burst, m: np.ndarray) -> tuple[float, float, float, bool]:
    ax, ay, az = b.ax[m].mean(), b.ay[m].mean(), b.az[m].mean()
    mod = np.sqrt(b.ax[m]**2 + b.ay[m]**2 + b.az[m]**2)
    ok = (abs(mod.mean() - G) <= G_TOLL) and (mod.std(ddof=1) <= SD_MAX)
    return ax, ay, az, ok


def analizza_burst(b: Burst) -> Causale:
    """Rilascio, ZUPT, integrazione sui 17 ms. Una sola passata per burst."""
    vuoto = Causale(b.shot, None, None, None, None, None, False, None, None, 0)
    t = b.t_ms
    mA = (t >= ZUPT_A) & (t <= ZUPT_B)
    if mA.sum() < 20:
        return vuoto

    axA, ayA, azA, statica = _statica(b, mA)
    alzo_mira = math.degrees(math.atan2(azA, axA))
    cant_mira = math.degrees(math.atan2(ayA, axA))

    # --- ZUPT: l'arco e' fermo, quindi il giroscopio legge OFFSET, non moto --
    #  Per tiro, non per seduta: compensa anche la deriva termica.
    bx, by, bz = b.gx[mA].mean(), b.gy[mA].mean(), b.gz[mA].mean()
    yaw_rate  = -(b.gx - bx)
    alzo_rate = -(b.gy - by)
    cant_rate = +(b.gz - bz)

    # --- rilascio: primo istante oltre 5x il rumore della PROPRIA mira ------
    #  SUL MODULO DEI TRE ASSI, non sul solo yaw. La prima stesura usava lo yaw
    #  e nascondeva una selezione insidiosa: un tiro con rilascio pulito ha poca
    #  torsione laterale, quindi non supera la soglia e viene ESCLUSO. Sono
    #  esattamente i tiri con yaw_causale piccolo — toglierli sposta il campione
    #  verso i valori grandi, e una selezione legata proprio alla grandezza che
    #  si vuole misurare puo' da sola produrre correlazioni che non esistono.
    #
    #  Il modulo vede lo stesso evento fisico da qualunque parte vada l'arco, e
    #  non ha quel legame. Sui dati del 16/08 concorda con lo yaw entro pochi
    #  millisecondi dove entrambi funzionano.
    #
    #  La soglia resta relativa al tiro e non assoluta: un arciere teso e uno
    #  rilassato hanno rumori di mira diversi, e una soglia fissa misurerebbe
    #  la tensione invece del rilascio.
    rumore = float(np.std(yaw_rate[mA], ddof=1))       # riportato per contesto
    omega  = np.sqrt((b.gx - bx)**2 + (b.gy - by)**2 + (b.gz - bz)**2)
    rum_om = float(np.std(omega[mA], ddof=1))
    med_om = float(np.mean(omega[mA]))
    dev_om = np.abs(omega - med_om)
    soglia = K_RILASCIO * rum_om

    #  SI CERCA ALL'INDIETRO, non in avanti. Il rilascio e' l'ULTIMA salita
    #  prima dello scocco, non la prima agitazione della finestra.
    #
    #  La prima stesura prendeva il primo attraversamento e sul 25/08 ha
    #  agganciato due tiri a −298 e −299 ms, cioe' al bordo stesso della
    #  finestra: erano assestamenti di mira, non rilasci. Il tiro 1 aveva rumore
    #  di mira 1,26 dps — il piu' basso della seduta — quindi soglia 6,3 dps, e
    #  qualunque piccolo aggiustamento la superava. La soglia adattiva protegge
    #  dai tiri rumorosi e rende ipersensibili quelli tranquilli: cercando in
    #  avanti, quella sensibilita' diventa un errore.
    #
    #  Cercando all'indietro l'ultimo ritorno SOTTO soglia, si trova l'inizio
    #  della salita che porta allo scocco. Sulla stessa seduta la dispersione
    #  dell'istante passa da 73,7 a 16,4 ms; sulle sedute dove il metodo in
    #  avanti gia' funzionava non cambia nulla (16/08: 14,0 -> 15,3; 22/08:
    #  15,8 -> 16,5), e recupera un tiro che prima restava senza rilascio.
    idx = np.where((t >= RIL_LO) & (t <= RIL_HI))[0]
    sotto = idx[dev_om[idx] <= soglia]
    if len(sotto) == 0 or sotto[-1] + 1 >= len(t):
        # NON si inventa un istante. Si dichiara mancante, e chi legge decide.
        return Causale(b.shot, None, None, None, None, rumore, statica,
                       alzo_mira, cant_mira, 0)
    t_ril = float(t[sotto[-1] + 1])

    mi = (t >= t_ril) & (t <= t_ril + T_CAUSALE_MS)
    if mi.sum() < 3:
        return Causale(b.shot, t_ril, None, None, None, rumore, statica,
                       alzo_mira, cant_mira, int(mi.sum()))
    ti = t[mi] / 1000.0
    return Causale(
        shot=b.shot, t_rilascio_ms=t_ril,
        yaw_causale=float(np.trapezoid(yaw_rate[mi], ti)),
        alzo_causale=float(np.trapezoid(alzo_rate[mi], ti)),
        cant_causale=float(np.trapezoid(cant_rate[mi], ti)),
        rumore_dps=rumore, mira_statica=statica,
        alzo_mira=alzo_mira, cant_mira=cant_mira, n_campioni=int(mi.sum()),
    )


# ============================================================================
#  Tenuta e rilascio — RICALCOLO dal burst
# ============================================================================
#  Il firmware li calcola a bordo e li scrive nel CSV; qui servono solo per le
#  sedute la cui frame e' stata corretta a posteriori, dove i valori registrati
#  si riferiscono agli assi sbagliati.
#
#  Definizioni identiche a shot_angles.cpp — finestre, soglie e classi. Non
#  sono state "riscritte in altro modo": due vocabolari per la stessa grandezza
#  sarebbero un modo garantito di litigare col proprio dispositivo.
HOLD_MS            = 900.0    # SHOT_HOLD_MS
HOLD_TENUTO_DEG    = 4.0      # ~2x il p95 del rumore di mira
HOLD_ABBASSATO_DEG = 8.0      # ~4x
RELEASE_MS         = 40.0     # SHOT_RELEASE_MS
RELEASE_PULITO     = 330.0
RELEASE_STRAPPO    = 510.0


def _classe_hold(v: float) -> int:
    if v != v:                       return 4      # HOLD_NA
    if abs(v) <= HOLD_TENUTO_DEG:    return 0      # TENUTO
    if abs(v) <= HOLD_ABBASSATO_DEG: return 1      # LIEVE
    return 2 if v < 0 else 3                       # ABBASSATO / ALZATO


def _classe_release(v: float) -> int:
    if v != v:                  return 3           # RELEASE_NA
    if v <= RELEASE_PULITO:     return 0
    if v <= RELEASE_STRAPPO:    return 1
    return 2


def hold_e_jerk(b: Burst) -> tuple[float, float]:
    """(hold_deg, release_jerk) ricalcolati dal burst, come li fa il firmware.

    hold  = integrale di −gy per 900 ms dal trigger, bias ZUPT dalla mira.
    jerk  = massimo |d(ay)/dt| nei 40 ms dal trigger. Nessuna baseline da
            togliere: la derivata elimina da sola il termine gravitazionale
            costante.
    """
    t = b.t_ms
    mA = (t >= ZUPT_A) & (t <= ZUPT_B)
    hold = float("nan")
    if mA.sum() >= 20:
        mh = (t >= 0) & (t <= HOLD_MS)
        if mh.sum() >= 10:
            hold = float(np.trapezoid(-(b.gy[mh] - b.gy[mA].mean()), t[mh] / 1000.0))

    jerk = float("nan")
    mr = (t >= 0) & (t <= RELEASE_MS)
    if mr.sum() >= 4:
        dt = np.diff(t[mr]) / 1000.0
        dv = np.diff(b.ay[mr])
        buoni = dt > 0
        if buoni.any():
            jerk = float(np.max(np.abs(dv[buoni] / dt[buoni])))
    return hold, jerk


def tabella(sessione) -> pd.DataFrame:
    """Le metriche causali di tutti i burst di una seduta, unite ai tiri."""
    righe = [analizza_burst(b).__dict__ for b in sessione.bursts_ordinati()]
    if not righe:
        return pd.DataFrame()
    c = pd.DataFrame(righe)
    if sessione.tiri.empty:
        return c
    # Guardia esplicita contro le collisioni di nome. Un merge che rinomina in
    # _x/_y e' silenzioso, e il guasto si manifesta molto lontano dalla causa:
    # meglio fermarsi qui con un messaggio che dice quale colonna.
    comuni = (set(c.columns) & set(sessione.tiri.columns)) - {"shot"}
    if comuni:
        raise ValueError(
            "collisione di nomi fra CSV e metriche causali: "
            + ", ".join(sorted(comuni))
            + " — rinominare nel modulo causale, non nel CSV")
    fusa = sessione.tiri.merge(c, on="shot", how="left")

    # Se la seduta e' stata corretta nel montaggio, hold e jerk sono stati
    # azzerati da correggi_mount() perche' non derivabili: si rifanno qui, sul
    # burst gia' ruotato.
    if sessione.meta.get("mount_corretto") == "si":
        vh, vj, vch, vcj = [], [], [], []
        for _, r in fusa.iterrows():
            b = sessione.bursts.get(r.get("burst_file") or "")
            h, j = hold_e_jerk(b) if b is not None else (float("nan"), float("nan"))
            vh.append(h); vj.append(j)
            vch.append(_classe_hold(h)); vcj.append(_classe_release(j))
        fusa["hold"] = vh
        fusa["hold_cdeg"] = [x * 100 if x == x else np.nan for x in vh]
        fusa["hold_class"] = vch
        fusa["release_jerk"] = vj
        fusa["release_class"] = vcj
    return fusa


# ============================================================================
#  RIANCORAGGIO DEL t=0 — l'istante del trigger non e' un buon riferimento
# ============================================================================
#  Il trigger scatta su | ‖a‖ − g | oltre una soglia, per due campioni
#  consecutivi. Con la soglia bassa (4 m/s², il default a cui il firmware e'
#  tornato dopo un bump di CONFIG_VERSION) si aggancia al PRIMO sussulto della
#  coda dello scocco: un evento poco marcato, la cui posizione dentro la curva
#  varia molto da tiro a tiro.
#
#  Il rimedio ovvio — «rifacciamo i conti come se la soglia fosse 20» — non
#  funziona su questi dati: nelle sedute di agosto il picco mediano vale 22
#  m/s² il 16 e 10-12 il 15, quindi a soglia 20 meta' dei tiri non esisterebbe.
#  Una soglia ASSOLUTA e' legata all'ampiezza dell'urto, che cambia con arco,
#  frecce e montaggio.
#
#  Il 50% del picco di quel tiro invece e' adimensionale: sta sempre nello
#  stesso punto della curva, forte o debole che sia. Misurato sui 17 tiri del
#  16/08, l'intervallo rilascio→ancora ha sd 12,3 ms contro i 14,0 dell'ancora
#  attuale — e 12,1 della soglia 20, che pero' perde 7 tiri su 17.
#
#  ONESTA': il guadagno e' modesto. La dispersione dei 46 ms NON e' dominata
#  dall'ancoraggio: resta soprattutto variabilita' vera del gesto piu'
#  incertezza sul rilevamento del rilascio.
G_RIF = 9.81


def _misura_trigger(b: Burst) -> np.ndarray:
    """La grandezza su cui il trigger decide: | ‖a‖ − g |.

    NON e' l'accelerazione lungo l'asse freccia, come verrebbe da pensare. E'
    il modulo, che e' invariante per rotazione — ed e' proprio la ragione per
    cui e' stato scelto: immune ai falsi da inclinazione della scheda.
    """
    return np.abs(np.sqrt(b.ax**2 + b.ay**2 + b.az**2) - G_RIF)


def riancora(b: Burst, regola: str = "meta_picco") -> float | None:
    """Nuovo istante di riferimento, in ms sulla scala attuale del burst.

    'meta_picco'  primo attraversamento del 50% del picco dello scocco
    'picco'       il massimo
    'thNN'        prima coppia di campioni consecutivi sopra NN m/s²
    """
    t = b.t_ms
    dev = _misura_trigger(b)
    m = (t >= -250) & (t <= 120)
    tt, dd = t[m], dev[m]
    if len(dd) < 6:
        return None
    ip = int(np.argmax(dd))
    if regola == "picco":
        return float(tt[ip])
    if regola == "meta_picco":
        # si cerca la SALITA verso il picco, non un attraversamento qualunque:
        # dopo il picco la curva ripassa dallo stesso valore scendendo
        idx = np.where(dd[:ip + 1] > 0.5 * dd[ip])[0]
        return float(tt[idx[0]]) if len(idx) else None
    if regola == "nessuna":
        return 0.0
    if regola.startswith("th"):
        soglia = float(regola[2:])
        sopra = dd > soglia
        for i in range(len(sopra) - 1):
            if sopra[i] and sopra[i + 1]:
                return float(tt[i + 1])
    return None


def applica_riancoraggio(sessione, regola: str = "meta_picco") -> int:
    """Sposta il t=0 di tutti i burst della seduta. Ritorna quanti ne ha spostati.

    IDEMPOTENTE: lo scostamento gia' applicato viene memorizzato sul burst e
    sottratto, cosi' richiamarla non accumula. Senza, ogni rerun di Streamlit
    sposterebbe la seduta un po' piu' in la' e i grafici scivolerebbero via
    lentamente — un guasto lentissimo e quasi impossibile da attribuire.
    """
    n = 0
    for b in sessione.bursts.values():
        b.off_ms = 0.0                   # si riparte sempre dall'originale
        off = riancora(b, regola)
        if off is None:
            continue
        b.off_ms = off                   # assoluto, non cumulativo
        n += 1
    return n


# ============================================================================
#  TRACCIA 2D — dove va la punta dell'arco, in gradi
# ============================================================================
#  Era stata tolta dalla prima versione dell'analizzatore, e la motivazione
#  scritta era: «costruita sull'integrazione da 900 e 2000 ms, dove la deriva e'
#  il problema dominante». Vero per QUELLA traccia, che copriva tutta la
#  finestra. Falso in generale — e buttare via lo strumento invece del suo uso
#  sbagliato e' costato uno strumento buono.
#
#  Il bilancio d'errore del briefing 31/07, per finestre corte:
#      incertezza sul bias dopo ZUPT              0,007°
#      fattore di scala (5%) su 0,6°              0,03°
#      totale                                    < 0,04°  contro segnale 0,58°
#  cioe' SNR ~ 14. Sulla finestra causale la deriva NON esiste.
#
#  Fin dove ci si puo' spingere. L'errore residuo di bias dopo lo ZUPT vale
#  circa 0,44 °/s e cresce lineare col tempo:
#      17 ms  -> 0,007°   (1,5% del segnale)
#     200 ms  -> 0,088°   (18%)
#     500 ms  -> 0,22°    (44%)
#     900 ms  -> 0,40°    (80%)
#    2000 ms  -> 0,88°    (180%: la deriva supera il segnale)
#  La soglia ragionevole sta intorno ai 250 ms. Fin li' la traccia e' una
#  MISURA; oltre e' un disegno, e va detto invece di lasciarlo credere.
TRACCIA_FINE_MS = 250.0        # oltre non si disegna: sarebbe deriva
DERIVA_DPS      = 0.44         # errore residuo di bias dopo ZUPT


def traccia(b: Burst, t_rilascio: float | None,
            fine_ms: float = TRACCIA_FINE_MS) -> dict:
    """Percorso angolare della punta dell'arco, integrato dal rilascio.

    Ritorna array in gradi, con l'origine nell'istante del rilascio: cio' che
    conta e' lo SPOSTAMENTO da li' in poi, non l'assetto assoluto.

    I due tratti sono separati perche' hanno statuto diverso:
      `causale`  dal rilascio a +17 ms — puo' ancora influenzare la freccia
      `dopo`     da +17 ms a `fine_ms` — follow-through, diagnostica del gesto
    """
    vuoto = dict(ok=False)
    if t_rilascio is None or t_rilascio != t_rilascio:
        return vuoto
    t = b.t_ms
    mA = (t >= ZUPT_A) & (t <= ZUPT_B)
    if mA.sum() < 20:
        return vuoto
    bx, by, bz = b.gx[mA].mean(), b.gy[mA].mean(), b.gz[mA].mean()

    m = (t >= t_rilascio) & (t <= t_rilascio + fine_ms)
    if m.sum() < 4:
        return vuoto
    ti = t[m] / 1000.0

    def cumula(rate):
        """Integrale cumulato TRAPEZOIDALE, non rettangolare.

        Non e' un dettaglio: nei 17 ms causali ci stanno appena 4 campioni, su
        un segnale che sale ripidissimo. La somma rettangolare dava +1,63° dove
        np.trapezoid — quello che produce yaw_causale — ne da' +0,75. Il
        grafico avrebbe contraddetto il numero nella stessa schermata, e a
        quel punto non si sa piu' a quale dei due credere.
        Il passo e' quello REALE fra campioni: l'ODR non e' costante (il
        battimento dei tick FreeRTOS alterna 4005 e 5005 us) e un dt medio
        introdurrebbe un errore che cresce come la traccia stessa.
        """
        passo = np.diff(ti)
        medie = (rate[1:] + rate[:-1]) / 2.0
        return np.concatenate([[0.0], np.cumsum(medie * passo)])

    yaw = cumula(-(b.gx[m] - bx))
    alzo = cumula(-(b.gy[m] - by))
    rel = t[m] - t_rilascio
    # Ultimo campione DENTRO i 17 ms — non il primo fuori. searchsorted senza
    # side='right' puntava al campione successivo, e con appena 4 campioni nella
    # finestra quell'uno in piu' raddoppiava il valore letto sul grafico
    # rispetto a quello della tabella. Deve coincidere con la maschera usata da
    # analizza_burst: (t >= t_ril) & (t <= t_ril + T_CAUSALE_MS).
    i_fine = int(np.searchsorted(rel, T_CAUSALE_MS, side="right")) - 1
    return dict(ok=True, t_rel=rel, yaw=yaw, alzo=alzo,
                i_fine_causale=max(i_fine, 1),
                deriva_max=DERIVA_DPS * fine_ms / 1000.0)


# ============================================================================
#  Centratura — il passaggio che ha raddrizzato l'analisi del 1° agosto
# ============================================================================
#  La mediana dello yaw e' SISTEMATICA: l'arco torce sempre nello stesso verso,
#  e la taratura del mirino la assorbe. Quello che puo' spostare l'impatto tiro
#  per tiro e' solo lo SCARTO DALLA PROPRIA MEDIANA.
#
#  Usare il valore assoluto come predittore inietta una costante che il
#  bersaglio non vede — e nella prima passata dell'analisi B produsse una
#  correlazione fasulla con la distanza, perche' d·tan(0,58°) e' di fatto una
#  funzione della distanza.
#
#  Vale identico per il tempo di mira: la letteratura lo descrive come
#  strategia INDIVIDUALE, quindi nessun valore assoluto ha significato e conta
#  solo lo scarto dal proprio abituale.
def centra(df: pd.DataFrame, colonne: tuple, per: str | None = None) -> pd.DataFrame:
    """Aggiunge d_<col> = col − mediana(col). Se `per` e' dato, per gruppo."""
    d = df.copy()
    for c in colonne:
        if c not in d.columns:
            continue
        if per and per in d.columns:
            d["d_" + c] = d[c] - d.groupby(per)[c].transform("median")
        else:
            d["d_" + c] = d[c] - d[c].median()
    return d


def in_centimetri(df: pd.DataFrame, k: float = K_TRASFERIMENTO) -> pd.DataFrame:
    """Traduce le metriche causali centrate in centimetri sul bersaglio.

    yaw   -> laterale, per pura geometria attenuata dal guadagno k
    cant  -> laterale, dominato dalla PARALLASSE del mirino e non dal termine
             balistico: il punto mirino e' solidale al riser e ruota, l'occhio
             no. I due termini si sottraggono, ed e' il motivo per cui gli
             stringwalker cantano con disinvoltura (E piccolo -> quasi si
             annullano, esattamente intorno ai 25 m).
    alzo  -> verticale.
    """
    d = df.copy()
    if "dist_m" not in d.columns:
        return d
    dist_cm = d["dist_m"].astype(float) * 100.0
    if "d_yaw_causale" in d.columns:
        d["cm_yaw"] = k * dist_cm * np.tan(np.radians(d["d_yaw_causale"]))
    if "d_cant_causale" in d.columns:
        d["cm_cant"] = dist_cm * P_SU_L_MEDITERRANEA * np.sin(np.radians(d["d_cant_causale"]))
    if {"cm_yaw", "cm_cant"} <= set(d.columns):
        d["cm_lat"] = d["cm_yaw"] + d["cm_cant"]
    if "d_alzo_causale" in d.columns:
        d["cm_vert"] = k * dist_cm * np.tan(np.radians(d["d_alzo_causale"]))
    return d


# ============================================================================
#  Potenza statistica — perche' un risultato nullo non e' un'informazione
# ============================================================================
#  Un risultato nullo vale solo quanto la potenza che lo ha prodotto. C'e'
#  un'asimmetria che si dimentica sempre: un positivo con potenza bassa e'
#  comunque un risultato, un NULLO con potenza bassa non e' niente — non dice
#  "l'effetto non c'e'", dice "non ero in grado di vederlo".
#
#  L'analisi del 1° agosto aveva n=35 contro un effetto atteso r~0,30, cioe'
#  potenza 0,38: se la tesi fosse stata vera, il test avrebbe fallito comunque
#  nel 62% dei casi. Convenzione: sotto 0,80 un nullo e' INCONCLUDENTE.
def potenza(n: int, r_vero: float, alfa: float = 0.05) -> float:
    """Potenza del test di correlazione, trasformazione z di Fisher."""
    from scipy import stats
    if n < 4 or abs(r_vero) >= 1:
        return float("nan")
    z = 0.5 * math.log((1 + r_vero) / (1 - r_vero))
    se = 1.0 / math.sqrt(n - 3)
    zc = stats.norm.ppf(1 - alfa / 2)
    return float(stats.norm.cdf(abs(z) / se - zc) + stats.norm.cdf(-abs(z) / se - zc))


def n_necessario(r_vero: float, obiettivo: float = 0.80, alfa: float = 0.05) -> int:
    for n in range(6, 4001):
        if potenza(n, r_vero, alfa) >= obiettivo:
            return n
    return -1


def diagnosi_potenza(df: pd.DataFrame) -> dict:
    """Quanta parte della dispersione laterale puo' spiegare lo yaw, e con
    quanti tiri sarebbe visibile. E' il calcolo da fare PRIMA di raccogliere,
    non dopo aver concluso che non c'e' correlazione."""
    out = {}
    if not {"cm_yaw", "imp_x_cm", "dist_m"} <= set(df.columns):
        return out
    d = df.dropna(subset=["cm_yaw", "imp_x_cm", "dist_m"])
    if len(d) < 4:
        return out
    sd_tot = float(d["imp_x_cm"].std(ddof=1))
    sd_yaw = float(d["cm_yaw"].std(ddof=1))
    dist_media = float(d["dist_m"].mean())
    out["n"] = len(d)
    out["sd_impatto_cm"] = sd_tot
    out["sd_yaw_cm"] = sd_yaw
    # dispersione angolare totale: il numero che dice se il gesto e' quello
    # giusto. A 5 m in giardino esce ~0,65 gradi, il doppio di una seduta vera.
    out["sd_angolare_deg"] = math.degrees(math.atan(sd_tot / (dist_media * 100.0))) \
        if dist_media > 0 else float("nan")
    f = (sd_yaw / sd_tot) ** 2 if sd_tot > 0 else float("nan")
    out["f"] = f
    out["r_atteso"] = math.sqrt(f) if f == f and f >= 0 else float("nan")
    if out["r_atteso"] == out["r_atteso"]:
        out["potenza_attuale"] = potenza(len(d), out["r_atteso"])
        out["n_per_80"] = n_necessario(out["r_atteso"])
    return out


# ============================================================================
#  Regressione — MISURA k, non lo verifica
# ============================================================================
#  Il briefing del 31/07 proponeva pendenza attesa 1. E' sbagliato, per i due
#  motivi in testa a K_TRASFERIMENTO. Riformulazione:
#
#    La regressione non VERIFICA una pendenza attesa. MISURA il guadagno di
#    trasferimento k. Cio' che conferma la tesi e' r, non la pendenza. Un r
#    alto con k~0,45 e' il risultato migliore possibile: valida la metrica e
#    consegna la costante di taratura.
#
#  Se si esegue il test aspettandosi 1 e ne esce 0,45, il rischio e' archiviare
#  la grandezza giusta. E' l'errore speculare a quello dei 5 gradi: la' il
#  valore era vero e sbagliata la finestra, qui la grandezza e' giusta e
#  sbagliato il fattore di conversione.
def regressione(df: pd.DataFrame, x: str, y: str) -> dict:
    from scipy import stats
    if x not in df.columns or y not in df.columns:
        return {}
    d = df[[x, y]].dropna()
    if len(d) < 4:
        return {"n": len(d)}
    xv, yv = d[x].to_numpy(float), d[y].to_numpy(float)
    r, p = stats.pearsonr(xv, yv)
    rs, ps = stats.spearmanr(xv, yv)
    lr = stats.linregress(xv, yv)
    # jackknife: un r che dipende da un solo tiro non e' un r
    if len(d) > 6:
        jr = [stats.pearsonr(np.delete(xv, i), np.delete(yv, i))[0] for i in range(len(d))]
        jmin, jmax = float(min(jr)), float(max(jr))
    else:
        jmin = jmax = float("nan")
    return dict(n=len(d), r=float(r), p=float(p), rho=float(rs), p_rho=float(ps),
                pendenza=float(lr.slope), intercetta=float(lr.intercept),
                err_pendenza=float(lr.stderr), jack_min=jmin, jack_max=jmax,
                potenza=potenza(len(d), abs(float(r))))
