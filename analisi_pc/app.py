#!/usr/bin/env python3
# ============================================================================
#  ArchBB — Analizzatore
#  Cesare Pagura · Padova/Noale IT · agosto 2026
# ----------------------------------------------------------------------------
#  COSA E' CAMBIATO RISPETTO ALLA VERSIONE DEL 30/07, e perche'
#
#  Quella versione era costruita attorno a hold, jerk e traccia 2D. Il briefing
#  del 31/07 ha spostato il terreno sotto: la freccia parte ~29 ms PRIMA che il
#  trigger scatti, quindi hold e jerk misurano FOLLOW-THROUGH. Restano utili
#  come diagnostica del gesto; non possono comparire in una catena causale
#  verso l'impatto, ed e' un errore di categoria presentarli come tali.
#
#  Questa versione e' organizzata attorno a tre cose che allora non esistevano:
#    - l'IMPATTO in centimetri (F19), che e' la grandezza DIPENDENTE — l'unica
#      con cui si possa giudicare tutte le altre;
#    - i TEMPI del gesto (F20/F20b);
#    - la FINESTRA CAUSALE di 17 ms, con la centratura sulla mediana.
#
#  E attorno a un principio che il progetto ha pagato caro: prima di guardare
#  una correlazione si guarda la POTENZA. Un nullo con potenza 0,38 non e'
#  un'informazione, e l'interfaccia deve dirlo prima, non dopo.
# ============================================================================
import sys
from pathlib import Path

import numpy as np
import pandas as pd
import plotly.graph_objects as go
import streamlit as st

sys.path.insert(0, str(Path(__file__).parent))
import archbb_io as aio
import archbb_causale as ac
import archbb_dsp as dsp

VERSIONE_ATTESA = "2026.08.25-01"

st.set_page_config(page_title="ArchBB — Analizzatore", layout="wide",
                   initial_sidebar_state="expanded")

# ----------------------------------------------------------------------------
#  Coerenza dei moduli. I file sono un insieme: estrarre lo zip sopra una
#  cartella esistente e sostituirne solo alcuni lascia un miscuglio che si
#  manifesta come AttributeError dentro una scheda, dieci schermate piu' in la'
#  rispetto alla causa. Con la marca, si dice subito e si dice quale file.
# ----------------------------------------------------------------------------
_disallineati = [n for n, m in (("archbb_io", aio), ("archbb_causale", ac),
                                ("archbb_dsp", dsp))
                 if getattr(m, "VERSIONE", "?") != VERSIONE_ATTESA]
if _disallineati:
    st.error(f"Moduli disallineati: {', '.join(_disallineati)} — attesa "
             f"{VERSIONE_ATTESA}. Riestrai l'archivio in una cartella pulita.")
    st.stop()

# ----------------------------------------------------------------------------
#  Colori — EREDITATI dal tema, non cablati
# ----------------------------------------------------------------------------
#  La prima versione scriveva i colori nel CSS dando per scontato il tema
#  scuro. Streamlit di default e' CHIARO: card nere su fondo bianco e etichette
#  grigie dentro, cioe' illeggibili. Il difetto vero non era la scelta dei
#  colori, era averla cablata — cosi' qualunque tema l'utente scegliesse non
#  aveva effetto.
#
#  Adesso i colori di sfondo e testo vengono da st.get_option(), quindi
#  seguono il tema. Il tema scuro resta il default, ma sta in
#  .streamlit/config.toml dove si puo' cambiare.
def _opt(nome, fallback):
    try:
        v = st.get_option(nome)
        return v if v else fallback
    except Exception:
        return fallback

_SFONDO   = _opt("theme.backgroundColor", "#0D1117")
_CARD     = _opt("theme.secondaryBackgroundColor", "#161B22")
_TESTO    = _opt("theme.textColor", "#E6E8EB")
_PRIMARIO = _opt("theme.primaryColor", "#4EA8DE")


def _luminanza(hexcol: str) -> float:
    """Luminanza relativa (WCAG), per decidere i toni secondari."""
    h = hexcol.lstrip("#")
    if len(h) == 3:
        h = "".join(c * 2 for c in h)
    try:
        r, g, b = (int(h[i:i+2], 16) / 255 for i in (0, 2, 4))
    except ValueError:
        return 0.0
    f = lambda c: c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
    return 0.2126 * f(r) + 0.7152 * f(g) + 0.0722 * f(b)


def _mescola(a: str, b: str, q: float) -> str:
    """a mescolato verso b della frazione q. Serve per ricavare i toni
    secondari DAL testo invece di inventarli: cosi' il contrasto resta
    proporzionato qualunque sia il tema."""
    pa, pb = a.lstrip("#"), b.lstrip("#")
    out = []
    for i in (0, 2, 4):
        va, vb = int(pa[i:i+2], 16), int(pb[i:i+2], 16)
        out.append(f"{round(va + (vb - va) * q):02X}")
    return "#" + "".join(out)


_SCURO = _luminanza(_SFONDO) < 0.4


def _contrasto(a: str, b: str) -> float:
    la, lb = _luminanza(a), _luminanza(b)
    return (max(la, lb) + 0.05) / (min(la, lb) + 0.05)


def _attenua(testo: str, fondo: str, minimo: float) -> str:
    """Il tono piu' tenue che rispetta ancora il contrasto richiesto.

    Le frazioni fisse NON funzionano: mescolare il testo verso lo sfondo del
    30% da' 7,2:1 su fondo scuro e 4,4:1 su fondo chiaro — sotto la soglia.
    Il contrasto non e' lineare nella miscela, quindi la frazione va CERCATA,
    non scelta. Costa dieci iterazioni e vale per qualunque tema, compresi
    quelli che l'utente si inventa.

    Soglie WCAG 2.1: 4,5:1 per il testo normale, 3:1 per quello di supporto.
    """
    scelto = testo
    for i in range(0, 61, 5):
        c = _mescola(testo, fondo, i / 100.0)
        if _contrasto(c, fondo) < minimo:
            break
        scelto = c
    return scelto


# Il contrasto si verifica contro la CARD e non contro lo sfondo: le etichette
# vivono dentro le card, ed e' li' che devono leggersi. Era anche la ragione
# per cui il difetto e' passato inosservato — il grigio funzionava sul fondo,
# non sul riquadro.
_TESTO2 = _attenua(_TESTO, _CARD, 4.5)
_TESTO3 = _attenua(_TESTO, _CARD, 3.0)
_BORDO  = _mescola(_CARD, _TESTO, 0.14)
_GRIGLIA = _mescola(_SFONDO, _TESTO, 0.16)

PAL = dict(acc=_PRIMARIO,
           grn="#3FB68B" if not _SCURO else "#5DCAA5",
           amb="#C97F10" if not _SCURO else "#EF9F27",
           red="#C7392F" if not _SCURO else "#E2564D",
           txt=_TESTO, tx2=_TESTO2, tx3=_TESTO3, grid=_GRIGLIA,
           bg="rgba(0,0,0,0)")

st.markdown(f"""<style>
  .block-container{{padding-top:2.2rem;max-width:1500px}}
  h1,h2,h3{{letter-spacing:-.01em}}
  .kpi{{background:{_CARD};border:1px solid {_BORDO};border-radius:12px;
       padding:12px 14px;margin-bottom:8px}}
  .kpi .v{{font-size:25px;font-weight:700;line-height:1.15;color:{_TESTO};
       font-family:ui-monospace,SFMono-Regular,Menlo,monospace}}
  .kpi .k{{font-size:11px;color:{_TESTO2};text-transform:uppercase;
       letter-spacing:.08em;margin-top:4px;font-weight:600}}
  .kpi .s{{font-size:11px;color:{_TESTO3};margin-top:3px;line-height:1.35}}
  .nota{{background:{_CARD};border-left:3px solid {_PRIMARIO};
        border-radius:0 8px 8px 0;padding:11px 14px;font-size:13px;
        color:{_TESTO2};margin:10px 0;line-height:1.55}}
  .nota b{{color:{_TESTO}}}
  .nota code{{background:{_mescola(_CARD, _TESTO, 0.10)};color:{_TESTO};
        padding:1px 5px;border-radius:4px;font-size:12px}}
</style>""", unsafe_allow_html=True)


def kpi(v, k, s=""):
    st.markdown(f"<div class='kpi'><div class='v'>{v}</div><div class='k'>{k}</div>"
                f"<div class='s'>{s}</div></div>", unsafe_allow_html=True)


def nota(html):
    st.markdown(f"<div class='nota'>{html}</div>", unsafe_allow_html=True)


# ============================================================================
#  Tenuta e rilascio — i due semafori che il display mostra sul riser
# ============================================================================
#  Erano stati tolti dalla prima versione perche' il briefing del 31/07 ha
#  stabilito che misurano FOLLOW-THROUGH: la freccia parte ~29 ms prima che il
#  trigger scatti, quindi non hanno spostato *quella* freccia.
#
#  Era un errore, e di categoria: «non causale» non vuol dire «non utile». Il
#  gesto e' ripetibile — se la tenuta e' rossa otto volte su dieci c'e' un
#  problema di forma che si ripercuote su tutti i tiri successivi. E sono
#  l'unica cosa che l'arciere vede sul riser subito dopo il tiro: non trovarle
#  qui rompeva il collegamento fra i due strumenti.
#
#  I NOMI E LE SOGLIE SONO QUELLI DEL FIRMWARE, non ricopiati a mano in altre
#  parole: shot_angles.h, hold_class_name() e release_class_name(). Se un
#  giorno il firmware cambia una soglia, qui cambia l'etichetta e basta —
#  due vocabolari diversi per la stessa cosa sarebbero un modo garantito di
#  litigare col proprio dispositivo.
CLASSI_TENUTA = {
    0: ("TENUTO",    "verde",  "punta ferma entro 4°"),
    1: ("LIEVE",     "giallo", "fra 4° e 8°"),
    2: ("ABBASSATO", "rosso",  "braccio caduto oltre 8°"),
    3: ("ALZATO",    "rosso",  "braccio sollevato oltre 8°"),
    4: ("n/d",       "grigio", "finestra non calcolabile"),
}
CLASSI_RILASCIO = {
    0: ("PULITO",  "verde",  "strappo sotto 330"),
    1: ("MEDIO",   "giallo", "fra 330 e 510"),
    2: ("STRAPPO", "rosso",  "sopra 510"),
    3: ("n/d",     "grigio", "non calcolabile"),
}


def col_classe(nome: str) -> str:
    """Il verde e' TENUE di proposito.

    Su venti tiri la maggior parte sara' verde: un verde pieno e saturo
    trasforma il grafico in un prato dove i rossi si perdono. L'occhio deve
    cadere su cio' che merita attenzione — e' il senso di un semaforo.
    """
    return {"verde": PAL["grn"], "giallo": PAL["amb"],
            "rosso": PAL["red"], "grigio": PAL["tx3"]}[nome]


def opacita_classe(nome: str) -> float:
    return {"verde": 0.42, "giallo": 0.85, "rosso": 1.0, "grigio": 0.30}[nome]


def _cl(v, tabella):
    """Classe -> (etichetta, colore, spiegazione). NaN -> grigio, MAI verde:
    un dato mancante che sembra buono e' peggio di un buco dichiarato."""
    if v is None or (isinstance(v, float) and v != v):
        return tabella[max(tabella)]
    return tabella.get(int(v), tabella[max(tabella)])


def fig(h=320, x="", y=""):
    f = go.Figure()
    f.update_layout(height=h, paper_bgcolor=PAL["bg"], plot_bgcolor=PAL["bg"],
                    font=dict(color=PAL["tx2"], size=12),
                    margin=dict(l=8, r=8, t=28, b=8),
                    xaxis_title=x, yaxis_title=y,
                    legend=dict(bgcolor="rgba(0,0,0,0)", orientation="h",
                                y=1.12, x=0))
    f.update_xaxes(gridcolor=PAL["grid"], zerolinecolor=PAL["grid"])
    f.update_yaxes(gridcolor=PAL["grid"], zerolinecolor=PAL["grid"])
    return f


# ============================================================================
#  Sorgente
# ============================================================================
@st.cache_data(show_spinner=False)
def _da_cartella(percorso: str, _firma: float):
    return aio.scansiona_cartella(percorso)


@st.cache_data(show_spinner=False)
def _da_zip(dati: bytes, nome: str):
    return aio.scansiona_zip(dati, nome)


def firma(p: str) -> float:
    """Impronta della cartella: se cambia, la cache si invalida.

    Include i .zip perche' adesso sono sorgenti a pieno titolo — senza,
    sostituire uno zip non aggiornerebbe nulla e sembrerebbe un difetto.
    """
    try:
        base = Path(p).expanduser()
        if base.is_file():
            return base.stat().st_mtime
        t = 0.0
        for d in base.iterdir():
            if d.is_dir() or d.suffix.lower() == ".zip":
                t += d.stat().st_mtime
        return t
    except OSError:
        return 0.0


with st.sidebar:
    st.markdown("### ArchBB · Analizzatore")
    st.caption(VERSIONE_ATTESA)
    modo = st.radio("Sorgente", ["Cartella o zip su disco", "Carica archivio"],
                    label_visibility="collapsed")
    sessioni = []
    if modo == "Cartella o zip su disco":
        p = st.text_input("Percorso", value=st.session_state.get("perc", ""),
                          placeholder=r"D:\ARCHBB  oppure  D:\ARCHBB\SESS_...zip")
        st.session_state["perc"] = p
        if p:
            try:
                sessioni = _da_cartella(p, firma(p))
            except Exception as e:
                st.error(str(e))
        st.caption("Trova cartelle SESS_* **e** archivi .zip. Puoi anche "
                   "puntare direttamente a una seduta o a un singolo zip.")
    else:
        up = st.file_uploader("Archivio .zip", type=["zip"])
        if up:
            try:
                sessioni = _da_zip(up.getvalue(), up.name)
            except Exception as e:
                st.error(str(e))

    if not sessioni:
        st.info("Nessuna seduta caricata.")
        st.stop()

    nomi = [s.nome for s in sessioni]
    st.markdown("---")
    scelte = st.multiselect("Sedute", nomi, default=nomi[:1])
    if not scelte:
        st.warning("Seleziona almeno una seduta.")
        st.stop()
    sel = [s for s in sessioni if s.nome in scelte]

    # --- correzione del montaggio -------------------------------------------
    #  Si PROPONE, non si applica da sola. Ruotare gli assi di una seduta e' una
    #  affermazione su come era montata la scheda quel giorno, e quella cosa la
    #  sa l'arciere, non il programma. Il rilevamento automatico serve a non
    #  fargliela cercare.
    sospette = [x.nome for x in sel if aio.mount_sospetto(x)]
    if sospette:
        st.markdown("---")
        st.warning(f"**{len(sospette)} sedute con gli assi scambiati.** In "
                   "queste, il *cant* segue l'elevazione del bersaglio invece "
                   "dell'alzo: la scheda era montata di lato ma il dispositivo "
                   "credeva di essere frontale.")
        correggi = st.multiselect(
            "Correggi il montaggio in", sospette, default=sospette,
            help="Ruota gli assi di +90° e rimette a posto cant e alzo. La "
                 "rotazione è invertibile senza perdita, quindi non si perde "
                 "nulla. Tenuta e rilascio vengono ricalcolati dai dati "
                 "grezzi, perché quei due non si possono ricavare dai valori "
                 "già scritti. I file sul disco non vengono toccati.")
        sel = [aio.correggi_mount(x) if x.nome in correggi else x for x in sel]

    st.markdown("---")
    solo_ok = st.checkbox("Nascondi i falsi scocchi", value=True, help=(
        "A volte il trigger scatta mentre l'arco viene preso su, girato o "
        "appoggiato: il dispositivo lo registra come se fosse un tiro. Si "
        "riconoscono perché l'arco risulta inclinato di 30° o 40°, posizioni "
        "in cui non si tira mai.\n\n"
        "Con la spunta, quei record spariscono. I tiri veri non vengono "
        "toccati. Da togliere solo se si sospetta che stia scartando anche "
        "qualcosa di buono."))

    solo_statica = st.checkbox("Solo i tiri con mira ferma", value=False, help=(
        "Il giroscopio non misura la posizione: misura la **velocità** di "
        "rotazione. Per sapere di quanto l'arco ha ruotato bisogna sommare "
        "quelle velocità nel tempo — e ogni giroscopio ha un piccolo errore "
        "costante, dice di ruotare anche da fermo.\n\n"
        "La correzione: guardarlo nell'attimo in cui si **sa** che è fermo, "
        "cioè in mira, misurare quanto sbaglia, e togliere quell'errore. Ogni "
        "tiro ha la sua correzione, presa dalla sua mira.\n\n"
        "Se in mira non si era fermi, quella correzione è sbagliata e peggiora "
        "le cose. Con la spunta restano solo i tiri con l'arco davvero "
        "immobile: numeri più puliti, qualche tiro in meno. Con pochi tiri "
        "conviene lasciarla spenta."))

    rianc = st.selectbox(
        "Riferimento dei tempi (t = 0)",
        ["Il trigger, com'è registrato", "Metà dell'urto dello scocco"],
        index=0, help=(
            "Il trigger scatta quando l'accelerazione supera una soglia. Con "
            "soglia bassa si aggancia al **primo sussulto** dello scocco, che "
            "è un punto poco definito: cade in posti diversi della curva da "
            "tiro a tiro, e ogni millisecondo di errore sposta le metriche "
            "causali.\n\n"
            "La seconda opzione riporta lo zero a **metà della salita** "
            "dell'urto, che sta sempre nello stesso punto della curva anche "
            "quando lo scocco è più debole. Sui 18 tiri del 16/08 la "
            "dispersione del rilascio scende da 14,0 a 13,1 ms.\n\n"
            "Non cambia i dati: cambia solo da dove si contano i tempi."))

    k_tras = st.slider("Quanto della rotazione arriva alla freccia", 0.10, 1.00,
                       ac.K_TRASFERIMENTO, 0.05, format="%.2f", help=(
        "Se al rilascio l'arco ruota di mezzo grado a destra, di quanto va a "
        "destra la freccia? La risposta ingenua è: mezzo grado. È sbagliata.\n\n"
        "La freccia non parte tutta in una volta: ci mette 17 millisecondi a "
        "lasciare la corda, e in quei millisecondi accelera da ferma a 54 m/s. "
        "La sua direzione si decide soprattutto **alla fine**, quando ha già "
        "preso velocità — il primo istante, quando l'arco non ha ancora "
        "ruotato, pesa quasi nulla. Risultato: la freccia eredita circa la "
        "**metà** della rotazione.\n\n"
        "Si aggiunge che l'arco ruota attorno al pugno che lo impugna, non "
        "attorno alla cocca, e anche questo attenua.\n\n"
        "0,45 vuol dire il 45%. È una stima ragionata, non una misura: si "
        "conferma con una campagna dedicata. Intanto muovendo il cursore si "
        "vede quanto le conclusioni dipendono da quel numero."))


# ============================================================================
#  Preparazione dei dati — un solo posto
# ============================================================================
@st.cache_data(show_spinner="Analisi dei burst…")
def prepara(nomi: tuple, corrette: tuple, _sessioni, k: float) -> pd.DataFrame:
    pezzi = []
    for s in _sessioni:
        d = ac.tabella(s)
        if d.empty:
            continue
        d.insert(0, "seduta", s.nome)
        d["mount"] = s.mount
        d["fw"] = s.meta.get("fw", "")
        pezzi.append(d)
    if not pezzi:
        return pd.DataFrame()
    df = pd.concat(pezzi, ignore_index=True)
    # La centratura e' PER SEDUTA: la componente sistematica che il mirino
    # assorbe puo' cambiare fra una giornata e l'altra (vento, taratura,
    # attrezzo). Centrare su tutto insieme confonderebbe la differenza fra
    # sedute con la dispersione dentro una seduta.
    df = ac.centra(df, ("yaw_causale", "alzo_causale", "cant_causale",
                        "tempo_mira_s", "tempo_alzata_s"), per="seduta")
    return ac.in_centimetri(df, k=k)


# `corrette` entra nella chiave della cache: senza, togliendo la spunta a una
# seduta l'analizzatore restituirebbe il risultato di prima, che e' il modo
# piu' rapido di perdere fiducia in uno strumento.
if rianc.startswith("Metà"):
    for _x in sel:
        ac.applica_riancoraggio(_x, "meta_picco")
else:
    for _x in sel:
        ac.applica_riancoraggio(_x, "nessuna")   # azzera l'offset

D = prepara(tuple(scelte),
            tuple(x.nome for x in sel if x.meta.get("mount_corretto") == "si")
            + (rianc,),
            sel, k_tras)
if D.empty:
    st.warning("Nessun tiro leggibile nelle sedute selezionate.")
    st.stop()

N_GREZZO = len(D)
if solo_ok and "assetto_ok" in D.columns:
    D = D[D["assetto_ok"].fillna(0) != 1]
if solo_statica and "mira_statica" in D.columns:
    D = D[D["mira_statica"].fillna(False)]

TAB = st.tabs(["Panoramica", "Impatti", "Il gesto", "Tempi", "Finestra causale",
               "Assetto", "Tiro per tiro", "Dati"])

# ============================================================================
#  1 · PANORAMICA
# ============================================================================
with TAB[0]:
    c = st.columns(5)
    with c[0]:
        kpi(len(D), "tiri", f"su {N_GREZZO} letti")
    with c[1]:
        n = int(D["imp_x_cm"].notna().sum()) if "imp_x_cm" in D else 0
        kpi(f"{n}/{len(D)}", "con impatto",
            "grandezza dipendente" if n else "nessuna: niente da giudicare")
    with c[2]:
        n = int(D["tempo_mira_s"].notna().sum()) if "tempo_mira_s" in D else 0
        kpi(f"{n}/{len(D)}", "con tempo di mira")
    with c[3]:
        n = int(D["mira_statica"].sum()) if "mira_statica" in D else 0
        kpi(f"{n}/{len(D)}", "con mira ferma", "serve anche a correggere il giroscopio")
    with c[4]:
        v = D["t_rilascio_ms"].median() if "t_rilascio_ms" in D else np.nan
        kpi(f"{v:+.0f} ms" if v == v else "—", "rilascio",
            "dal trigger, mediana")

    st.markdown("#### Le fasi, e cosa può stare in ciascuna")
    nota("La freccia lascia la corda <b>~17 ms</b> dopo il rilascio, e il "
         "trigger scatta ~29 ms dopo che è già partita. Ne segue una divisione "
         "che non è di comodo:<br><br>"
         "<b>Mira</b> — fino al rilascio rilevato per tiro. Assetto assoluto "
         "dall'accelerometro, nessuna integrazione, nessuna deriva.<br>"
         "<b>Causale</b> — 17 ms. È l'unica finestra in cui l'arco può ancora "
         "influenzare la freccia.<br>"
         "<b>Follow-through</b> — il resto. <b>hold</b> e <b>release_jerk</b> "
         "vivono qui: sono diagnostica del gesto, non cause dell'impatto, e "
         "presentarli come tali è un errore di categoria.")

    if "assetto_ok" in D.columns:
        n_scarti = int((D["assetto_ok"] == 1).sum())
        if n_scarti:
            st.warning(f"{n_scarti} record con assetto implausibile: arco "
                       "maneggiato col trigger armato, non tiri.")
    corr = [x.nome for x in sel if x.meta.get("mount_corretto") == "si"]
    if corr:
        st.info("**Montaggio corretto in analisi** per: " + ", ".join(corr)
                + ". Gli assi sono stati ruotati di +90°, cant e alzo "
                "scambiati, tenuta e rilascio ricalcolati dai dati grezzi. "
                "I file sul disco non sono stati toccati.")
    avvisi = [(s.nome, a) for s in sel for a in s.avvisi]
    if avvisi:
        with st.expander(f"Avvisi delle sedute ({len(avvisi)})"):
            for n_, a in avvisi:
                st.caption(f"**{n_}** — {a}")

# ============================================================================
#  2 · IMPATTI  (la grandezza dipendente)
# ============================================================================
with TAB[1]:
    if "imp_x_cm" not in D.columns or D["imp_x_cm"].notna().sum() == 0:
        st.info("Nessun impatto registrato: senza la grandezza dipendente non "
                "c'è modo di giudicare nessuna metrica del gesto.")
    else:
        P = D.dropna(subset=["imp_x_cm", "imp_y_cm"])
        c = st.columns(4)
        with c[0]: kpi(f"{P.imp_x_cm.std(ddof=1):.1f} cm", "σ laterale",
                       "è quello che conta")
        with c[1]: kpi(f"{P.imp_y_cm.std(ddof=1):.1f} cm", "σ verticale")
        with c[2]: kpi(f"{P.imp_x_cm.mean():+.1f} cm", "media laterale",
                       "assorbita dalla taratura")
        with c[3]:
            dm = P["dist_m"].astype(float).mean()
            ang = np.degrees(np.arctan(P.imp_x_cm.std(ddof=1) / (dm * 100)))
            kpi(f"{ang:.2f}°", "dispersione angolare",
                f"a {dm:.0f} m — confrontabile fra distanze")

        f = fig(460, "laterale [cm]  (+ destra)", "verticale [cm]  (+ alto)")
        lim = float(max(20, np.ceil(max(P.imp_r_cm.max(), 20) / 10) * 10))
        for r in np.linspace(lim / 4, lim, 4):
            f.add_shape(type="circle", x0=-r, y0=-r, x1=r, y1=r,
                        line=dict(color=PAL["grid"], width=1))
        f.add_hline(y=0, line_color=PAL["grid"]); f.add_vline(x=0, line_color=PAL["grid"])
        # --- ogni tiro e' DUE SEMIDISCHI: sinistra tenuta, destra rilascio ---
        #  Disegnati come TRACCE poligonali (fill="toself"), non come shape.
        #  La prima stesura usava add_shape: la figura si costruiva ma restava
        #  nera, perche' senza nemmeno una traccia visibile Plotly non ha dati
        #  su cui ancorare la vista e scaleanchor con due range espliciti la
        #  fa collassare. Le tracce risolvono tutto insieme — vista, legenda e
        #  hover — e non dipendono dall'ordine dei layer.
        #
        #  La divisione e' VERTICALE di proposito: sul grafico l'alto/basso e
        #  il destra/sinistra hanno gia' un significato loro (dove e' finita la
        #  freccia), e dividere in orizzontale suggerirebbe un legame che non
        #  c'e'.
        raggio = lim * 0.045
        archi = np.linspace(-np.pi / 2, np.pi / 2, 14)   # semiarco destro

        for colonna, tabella, verso, etichetta in (
                ("hold_class", CLASSI_TENUTA, -1, "tenuta"),
                ("release_class", CLASSI_RILASCIO, +1, "rilascio")):
            # un poligono per tiro, tutti i tiri della stessa classe in UNA
            # traccia: separati da None, cosi' la legenda ha una voce sola per
            # classe invece di una per tiro.
            per_classe: dict = {}
            for _, q in P.iterrows():
                et, colore, _ = _cl(q.get(colonna), tabella)
                x0, y0 = float(q.imp_x_cm), float(q.imp_y_cm)
                xs = [x0 + verso * raggio * np.cos(a) for a in archi] + [x0, None]
                ys = [y0 + raggio * np.sin(a) for a in archi] + [y0 - raggio, None]
                v = per_classe.setdefault((et, colore), ([], []))
                v[0].extend(xs); v[1].extend(ys)
            for (et, colore), (xs, ys) in per_classe.items():
                f.add_trace(go.Scatter(
                    x=xs, y=ys, mode="lines", fill="toself",
                    fillcolor=col_classe(colore),
                    opacity=opacita_classe(colore),
                    line=dict(width=0), hoverinfo="skip",
                    name=f"{etichetta} {et}",
                    legendgroup=etichetta, legendgrouptitle_text=etichetta))

        # contorno dei mancati: distingue l'esito senza rubare il colore ai due
        # semafori, che adesso lo usano tutto
        if "colpito" in P.columns:
            q = P[~P["colpito"].astype(bool)]
            if len(q):
                f.add_trace(go.Scatter(
                    x=q.imp_x_cm, y=q.imp_y_cm, mode="markers", name="mancato",
                    marker=dict(size=raggio * 3.4, color="rgba(0,0,0,0)",
                                line=dict(width=1.4, color=PAL["txt"])),
                    hoverinfo="skip"))

        # punto trasparente per il tooltip: le aree riempite non lo danno
        eti = []
        for _, q in P.iterrows():
            t_, _, _ = _cl(q.get("hold_class"), CLASSI_TENUTA)
            r_, _, _ = _cl(q.get("release_class"), CLASSI_RILASCIO)
            eti.append(f"tiro {int(q.shot)}<br>tenuta {t_} · rilascio {r_}")
        f.add_trace(go.Scatter(
            x=P.imp_x_cm, y=P.imp_y_cm, mode="markers", showlegend=False,
            marker=dict(size=16, color="rgba(0,0,0,0)"), text=eti,
            hovertemplate="%{text}<br>%{x:+.0f} / %{y:+.0f} cm<extra></extra>"))

        f.update_yaxes(scaleanchor="x", scaleratio=1)
        f.update_layout(xaxis_range=[-lim*1.1, lim*1.1], yaxis_range=[-lim*1.1, lim*1.1])
        st.plotly_chart(f, use_container_width=True)
        st.caption("Ogni tiro è un bollino diviso: **sinistra la tenuta, "
                   "destra il rilascio**, con gli stessi colori del display. "
                   "Il contorno bianco segna i tiri fuori bersaglio.")

        nota("L'origine è il <b>punto di mira</b>, non il centro della sagoma: "
             "è ciò che rende il dato indipendente dalla forma dell'animale 3D "
             "e lo mette nello stesso riferimento in cui la fisica fa le "
             "previsioni.<br><br>"
             "La <b>dispersione angolare</b> è il numero da guardare quando si "
             "confrontano sedute a distanze diverse — e dice anche se il gesto "
             "è quello giusto: in giardino a 5 m esce intorno a 0,65°, circa il "
             "doppio di una seduta con mira vera.")

# ============================================================================
#  3 · IL GESTO — tenuta e rilascio, i due semafori del display
# ============================================================================
with TAB[2]:
    ha_cl = ("hold_class" in D.columns and D["hold_class"].notna().sum() > 0)
    if not ha_cl:
        st.info("Nessuna classificazione di tenuta o rilascio in queste sedute.")
    else:
        st.markdown("##### Come è andato il gesto, tiro per tiro")
        nota("Questi due semafori sono gli stessi che il dispositivo mostra sul "
             "riser subito dopo lo scocco.<br><br>"
             "<b>Non dicono dove è andata la freccia</b>, e non potrebbero: si "
             "misurano dopo lo scocco, e a quel punto la freccia è già partita "
             "da circa 29 millesimi di secondo. Dicono com'è stato il "
             "<b>gesto</b> — e il gesto si ripete. Un rosso isolato non "
             "significa molto; otto rossi su dieci sono un problema di forma "
             "che si porta dietro tutti i tiri successivi.")

        c = st.columns(2)
        for col, (colonna, tab, titolo, spiega) in zip(c, (
            ("hold_class", CLASSI_TENUTA, "Tenuta",
             "Di quanto si abbassa la punta dell'arco nei 9 decimi di secondo "
             "dopo lo scocco. Le soglie non sono inventate: <b>4°</b> è il "
             "doppio del tremolio normale in mira, <b>8°</b> il quadruplo. Un "
             "arco che scende molto vuol dire che si stava già rilassando "
             "prima del tempo."),
            ("release_class", CLASSI_RILASCIO, "Rilascio",
             "Quanto è stato brusco lo scocco, misurato come strappo laterale "
             "nei primi 4 centesimi di secondo. Qui le soglie sono "
             "<b>relative</b>: non esiste un valore buono in assoluto, dipende "
             "da arco, corda e presa."))):
            with col:
                st.markdown(f"**{titolo}**")
                v = D[colonna] if colonna in D.columns else pd.Series(dtype=float)
                conta = {}
                for _, q in D.iterrows():
                    et, colore, _ = _cl(q.get(colonna), tab)
                    conta[(et, colore)] = conta.get((et, colore), 0) + 1
                tot = max(1, sum(conta.values()))
                f = fig(190, "", "")
                for (et, colore), n in sorted(conta.items(), key=lambda kv: -kv[1]):
                    f.add_trace(go.Bar(y=[titolo], x=[n], orientation="h",
                                       name=f"{et} ({n})",
                                       marker_color=col_classe(colore),
                                       marker_opacity=max(opacita_classe(colore), .5),
                                       text=f"{et} {n}", textposition="inside",
                                       insidetextanchor="middle"))
                f.update_layout(barmode="stack", showlegend=False, height=120,
                                margin=dict(l=0, r=0, t=6, b=0))
                f.update_xaxes(visible=False); f.update_yaxes(visible=False)
                st.plotly_chart(f, use_container_width=True)
                buoni = sum(n for (et, colore), n in conta.items() if colore == "verde")
                st.caption(f"{buoni} su {tot} nella fascia buona "
                           f"({100*buoni/tot:.0f}%)")
                nota(spiega)

        # --- picco dell'urto: controllo del MONTAGGIO, non del gesto --------
        if "picco" in D.columns and D["picco"].notna().sum() >= 4:
            st.markdown("##### L'urto dello scocco — controllo del montaggio")
            P = D.dropna(subset=["picco"])
            cv = 100 * P.picco.std(ddof=1) / P.picco.mean()
            c = st.columns(4)
            with c[0]: kpi(f"{P.picco.median():.1f}", "mediana [m/s²]")
            with c[1]: kpi(f"{P.picco.min():.1f}", "minimo",
                           f"{P.picco.min()/6:.1f}× la soglia")
            with c[2]:
                kpi(f"{cv:.0f}%", "dispersione",
                    "montaggio saldo" if cv < 30 else "controlla il serraggio")
            with c[3]: kpi(f"{P.picco.max():.1f}", "massimo")

            f = fig(220, "tiro", "urto [m/s²]")
            f.add_trace(go.Scatter(x=P.shot, y=P.picco, mode="lines+markers",
                                   line=dict(color=PAL["red"], width=1.6),
                                   marker=dict(size=7), showlegend=False))
            f.add_hline(y=6.0, line_color=PAL["amb"], line_dash="dash",
                        annotation_text="soglia del trigger")
            st.plotly_chart(f, use_container_width=True)

            if cv >= 40:
                st.warning(
                    f"**Dispersione {cv:.0f}%: il montaggio si muove.** Su una "
                    "staffa allentata l'urto trasmesso cambia da tiro a tiro. "
                    "Con il fissaggio a tre grani M3 del 25/08 si è scesi dal "
                    "39–48% al 25%.")
            nota("Quanta energia residua dei flettenti arriva al riser dopo che "
                 "la freccia è partita. <b>Non misura l'apertura</b>: il "
                 "confronto col cronografo lo esclude — la velocità della "
                 "freccia varia dell'1,8% (energia 3,5%) mentre l'urto varia del "
                 "25–48%, cioè fino a quattordici volte tanto.<br><br>"
                 "Misura invece <b>quanto la scheda è solidale all'arco</b>, ed "
                 "è per questo che serve: se la dispersione risale sopra il 40% "
                 "un grano si è allentato. Il minimo dice il margine sulla "
                 "soglia del trigger: sotto 1,5× si comincia a perdere tiri.")

        st.markdown("##### Nel corso della seduta")
        nota("Il singolo tiro dice poco. Una fila di gialli o di rossi verso la "
             "fine è il segno più comune della stanchezza: è quello che questa "
             "striscia serve a far vedere.")
        f = fig(220, "tiro", "")
        for riga, (colonna, tab, et_riga) in enumerate((
                ("release_class", CLASSI_RILASCIO, "rilascio"),
                ("hold_class", CLASSI_TENUTA, "tenuta"))):
            xs, cs, tx = [], [], []
            for _, q in D.iterrows():
                et, colore, _ = _cl(q.get(colonna), tab)
                xs.append(int(q.shot)); cs.append(col_classe(colore)); tx.append(et)
            f.add_trace(go.Scatter(
                x=xs, y=[et_riga]*len(xs), mode="markers", name=et_riga,
                marker=dict(size=17, color=cs, symbol="square",
                            line=dict(width=1, color=_SFONDO)),
                text=tx, showlegend=False,
                hovertemplate="tiro %{x}<br>%{text}<extra></extra>"))
        f.update_layout(height=180, margin=dict(l=0, r=0, t=10, b=30))
        st.plotly_chart(f, use_container_width=True)


# ============================================================================
#  4 · TEMPI
# ============================================================================
with TAB[3]:
    if "tempo_mira_s" not in D.columns or D["tempo_mira_s"].notna().sum() == 0:
        st.info("Nessun tempo registrato (serve firmware F20 o successivo).")
    else:
        T = D.dropna(subset=["tempo_mira_s"])
        c = st.columns(4)
        with c[0]: kpi(f"{T.tempo_mira_s.median():.2f} s", "mira, mediana")
        with c[1]: kpi(f"{T.tempo_mira_s.std(ddof=1):.2f} s", "σ mira",
                       "serve dispersione")
        with c[2]: kpi(f"{T.tempo_mira_s.min():.1f}–{T.tempo_mira_s.max():.1f} s",
                       "intervallo")
        with c[3]:
            v = T["tempo_alzata_s"].median() if "tempo_alzata_s" in T else np.nan
            kpi(f"{v:.2f} s" if v == v else "—", "alzata, mediana",
                "indicativa, non misura")

        f = fig(300, "tiro", "secondi")
        f.add_trace(go.Scatter(x=T.shot, y=T.tempo_mira_s, mode="lines+markers",
                               name="mira", line=dict(color=PAL["acc"], width=2),
                               marker=dict(size=7)))
        if "tempo_alzata_s" in T:
            f.add_trace(go.Scatter(x=T.shot, y=T.tempo_alzata_s, mode="lines+markers",
                                   name="alzata", line=dict(color=PAL["tx3"], width=1,
                                                            dash="dot"),
                                   marker=dict(size=5)))
        f.add_hline(y=float(T.tempo_mira_s.median()), line_color=PAL["grn"],
                    line_dash="dash", annotation_text="mediana")
        st.plotly_chart(f, use_container_width=True)

        if "imp_r_cm" in T.columns and T["imp_r_cm"].notna().sum() >= 6:
            Q = T.dropna(subset=["imp_r_cm"]).copy()
            # Raggio dal CENTRO DEL GRUPPO, non dal punto di mira: la distanza
            # dal punto di mira include l'errore di taratura del mirino, che
            # non c'entra col tempo di mira.
            Q["rad"] = np.hypot(Q.imp_x_cm - Q.imp_x_cm.mean(),
                                Q.imp_y_cm - Q.imp_y_cm.mean())
            nq = min(3, max(2, len(Q) // 4))
            Q["gruppo"] = pd.qcut(Q.tempo_mira_s, nq, duplicates="drop")
            g = Q.groupby("gruppo", observed=True).agg(
                n=("rad", "size"), tempo=("tempo_mira_s", "median"),
                raggio=("rad", "mean")).reset_index(drop=True)
            f2 = fig(280, "tempo di mira [s], mediana del gruppo",
                     "raggio medio dal centro del gruppo [cm]")
            f2.add_trace(go.Bar(x=[f"{t:.1f} s" for t in g.tempo], y=g.raggio,
                                marker_color=PAL["acc"],
                                text=[f"n={int(v)}" for v in g.n],
                                textposition="outside"))
            st.plotly_chart(f2, use_container_width=True)
            reg = ac.regressione(Q, "tempo_mira_s", "rad")
            if reg.get("n", 0) >= 6:
                st.caption(f"tempo vs raggio: r = {reg['r']:+.3f} (p = {reg['p']:.3f}) · "
                           f"ρ = {reg['rho']:+.3f} · potenza = {reg['potenza']:.2f}")

        nota("La letteratura è netta su un punto e contraddittoria su un altro. "
             "La durata di mira risulta una <b>strategia individuale</b>, non un "
             "determinante di gruppo; e mentre diverse ricerche associano durate "
             "brevi a prestazioni migliori, altre riportano il contrario.<br><br>"
             "Due conseguenze: <b>nessun valore assoluto ha senso come soglia</b> "
             "— conta lo scarto dalla propria mediana; e un dispositivo personale "
             "è lo strumento giusto per questa domanda, perché se l'effetto è "
             "individuale uno studio su otto atleti d'élite non può trovarlo e "
             "cento tiri di un arciere solo sì.")

# ============================================================================
#  5 · FINESTRA CAUSALE
# ============================================================================
with TAB[4]:
    if "yaw_causale" not in D.columns or D["yaw_causale"].notna().sum() == 0:
        st.info("Nessuna metrica causale: servono i burst.")
    else:
        C = D.dropna(subset=["yaw_causale"])
        c = st.columns(4)
        for col, (k_, nome) in zip(c[:3], [("yaw_causale", "yaw"),
                                           ("alzo_causale", "alzo"),
                                           ("cant_causale", "cant")]):
            with col:
                kpi(f"{C[k_].median():+.3f}°", f"{nome} causale",
                    f"σ {C[k_].std(ddof=1):.3f}°")
        with c[3]:
            kpi(f"{C.rumore_dps.median():.1f} dps", "rumore di mira",
                "soglia rilascio = 5×")

        st.markdown("##### Il test: lo yaw causale predice lo scarto laterale?")
        dg = ac.diagnosi_potenza(D)
        if dg:
            c = st.columns(4)
            with c[0]: kpi(f"{dg['f']:.3f}", "quota di varianza f",
                           "spiegabile dallo yaw")
            with c[1]: kpi(f"{dg['r_atteso']:.2f}", "r atteso")
            with c[2]:
                pw = dg["potenza_attuale"]
                kpi(f"{pw:.2f}", "potenza attuale",
                    "sotto 0,80 → inconcludente" if pw < .8 else "sufficiente")
            with c[3]: kpi(dg["n_per_80"], "n per l'80%", f"ne hai {dg['n']}")

            if dg.get("sd_angolare_deg", 0) > 1.0:
                st.error(
                    f"**Dispersione angolare {dg['sd_angolare_deg']:.2f}°: questa "
                    "seduta non può testare nessuna metrica causale.** Non è un "
                    "problema dello strumento — è che il gesto misurato non è "
                    "quello che la metrica descrive. Una seduta con mira "
                    "attenta sta intorno a 0,3–0,7°. Sopra 1° servono migliaia "
                    "di tiri per vedere qualunque cosa: usa la seduta per "
                    "validare il dispositivo, non per misurare il tiro.")
            if dg["potenza_attuale"] < 0.80:
                st.warning(
                    f"**Potenza {dg['potenza_attuale']:.2f}: qualunque risultato "
                    "nullo qui sotto è inconcludente, non negativo.** Serve "
                    f"n ≈ {dg['n_per_80']}. La dispersione angolare è "
                    f"{dg['sd_angolare_deg']:.2f}°: se supera ~0,4° il gesto non "
                    "è quello che la metrica vuole misurare, e allargare il "
                    "campione non basta — serve mira vera, a distanze diverse.")

        reg = ac.regressione(D, "cm_yaw", "imp_x_cm")
        if reg.get("n", 0) >= 4:
            c = st.columns(4)
            with c[0]: kpi(f"{reg['r']:+.3f}", "r", f"p = {reg['p']:.3f}")
            with c[1]: kpi(f"{reg['rho']:+.3f}", "ρ di Spearman")
            with c[2]: kpi(f"{reg['pendenza']:+.2f}", "pendenza misurata",
                           f"± {reg['err_pendenza']:.2f}")
            with c[3]: kpi(f"{reg['jack_min']:+.2f} … {reg['jack_max']:+.2f}",
                           "r al jackknife", "dipende da un solo tiro?")

            P = D.dropna(subset=["cm_yaw", "imp_x_cm"])
            f = fig(360, "scarto laterale previsto dallo yaw [cm]",
                    "scarto laterale osservato [cm]")
            f.add_trace(go.Scatter(x=P.cm_yaw, y=P.imp_x_cm, mode="markers",
                                   marker=dict(size=11, color=PAL["acc"], opacity=.85),
                                   name="tiri",
                                   text=[f"tiro {int(s)}" for s in P.shot],
                                   hovertemplate="%{text}<br>previsto %{x:+.1f}<br>"
                                                 "osservato %{y:+.1f}<extra></extra>"))
            xs = np.linspace(P.cm_yaw.min(), P.cm_yaw.max(), 10)
            f.add_trace(go.Scatter(x=xs, y=reg["intercetta"] + reg["pendenza"] * xs,
                                   mode="lines", name="misurata",
                                   line=dict(color=PAL["amb"], width=2)))
            f.add_trace(go.Scatter(x=xs, y=P.imp_x_cm.mean() + (xs - P.cm_yaw.mean()),
                                   mode="lines", name="pendenza 1 (attesa se k è giusto)",
                                   line=dict(color=PAL["tx3"], width=1, dash="dot")))
            st.plotly_chart(f, use_container_width=True)

        nota("<b>Questa regressione non verifica una pendenza attesa: misura il "
             "guadagno di trasferimento k.</b> Ciò che conferma la tesi è "
             "<b>r</b>, non la pendenza.<br><br>"
             "La freccia non eredita tutto l'angolo del riser — acquista "
             "velocità mentre l'arco ruota, quindi conta la media dell'angolo "
             "pesata sugli incrementi di quantità di moto, circa Δθ/2; e la "
             "rotazione è attorno al pivot della mano, non alla cocca. Atteso "
             "k ≈ 0,4–0,5. Con k impostato correttamente nella barra laterale, "
             "la pendenza qui sopra dovrebbe uscire vicina a 1.<br><br>"
             "Un r alto con k ≈ 0,45 è il risultato migliore possibile: valida "
             "la metrica <i>e</i> consegna la costante di taratura. Aspettarsi "
             "1 e archiviare uno 0,45 come fallimento sarebbe l'errore "
             "speculare a quello dei 5 gradi.")

        with st.expander("Tutti i predittori, con la potenza accanto"):
            righe = []
            for x, y, fase, nome in [
                ("cm_yaw", "imp_x_cm", "causale", "yaw → laterale"),
                ("cm_cant", "imp_x_cm", "causale", "cant → laterale"),
                ("cm_lat", "imp_x_cm", "causale", "yaw+cant → laterale"),
                ("cm_vert", "imp_y_cm", "causale", "alzo → verticale"),
                ("d_tempo_mira_s", "imp_x_cm", "mira", "tempo → laterale"),
                ("d_tempo_mira_s", "imp_y_cm", "mira", "tempo → verticale"),
                ("hold", "imp_y_cm", "follow-through", "hold → verticale"),
                ("release_jerk", "imp_x_cm", "follow-through", "jerk → laterale"),
                ("picco", "imp_y_cm", "montaggio", "urto → verticale"),
                ("cant", "imp_x_cm", "mira", "cant di mira → laterale"),
                ("alzo", "imp_y_cm", "mira", "alzo di mira → verticale"),
            ]:
                r = ac.regressione(D, x, y)
                if r.get("n", 0) >= 4:
                    righe.append(dict(relazione=nome, fase=fase, n=r["n"],
                                      r=round(r["r"], 3), p=round(r["p"], 3),
                                      rho=round(r["rho"], 3),
                                      potenza=round(r["potenza"], 2)))
            if righe:
                t = pd.DataFrame(righe)
                st.dataframe(t, use_container_width=True, hide_index=True)
                st.caption(
                    f"Con {len(righe)} test simultanei, la soglia di Bonferroni "
                    f"è p < {0.05/len(righe):.4f}. Le righe *follow-through* non "
                    "possono essere causali: se una risulta forte, è un indizio "
                    "diagnostico o un confondente, non un meccanismo.")

# ============================================================================
#  6 · ASSETTO — la verifica del montaggio
# ============================================================================
with TAB[5]:
    st.markdown("##### Il montaggio è quello giusto?")
    nota("Le convenzioni di segno sono <b>misurate al banco</b> per "
         "<code>mount=2</code>: se cambia l'orientamento vanno rimisurate, non "
         "ricalcolate. La verifica più economica è questa: puntando un "
         "bersaglio a elevazione nota, <b>l'alzo deve valere l'elevazione</b> e "
         "il cant deve restare piccolo. Se sono scambiati, il frame è sbagliato "
         "— è successo il 15/08, dove un «cant di 8° costante» era l'elevazione "
         "del bersaglio travestita.")
    if {"alzo", "cant", "elev_deg"} <= set(D.columns):
        c = st.columns(4)
        with c[0]: kpi(f"{D['alzo'].median():+.2f}°", "alzo di mira, mediana")
        with c[1]: kpi(f"{D['cant'].median():+.2f}°", "cant di mira, mediana")
        with c[2]: kpi(f"{D['elev_deg'].median():+.0f}°", "elevazione dichiarata")
        with c[3]:
            mm = D["mount"].dropna()
            kpi(int(mm.iloc[0]) if len(mm) else "—", "mount")
        # Il confronto sul cant e' col segno INVERTITO: se gli assi sono
        # scambiati, il cant registrato vale −elevazione. Vedi mount_sospetto().
        ea = abs(D["alzo"].median() - D["elev_deg"].median())
        ec = abs(-D["cant"].median() - D["elev_deg"].median())
        if ea <= 3:
            st.success(f"L'alzo segue l'elevazione (scarto {ea:.1f}°): frame coerente.")
        elif ec <= 3:
            st.error(f"È il **cant** a seguire l'elevazione, col segno invertito "
                     f"(scarto {ec:.1f}°), non "
                     "l'alzo: montaggio sbagliato, gli assi sono scambiati. "
                     "I dati restano recuperabili — la trasformazione è una "
                     "permutazione reversibile.")
        else:
            st.warning("Né alzo né cant seguono l'elevazione dichiarata: "
                       "controlla che l'elevazione inserita sia quella vera.")

        f = fig(320, "tiro", "gradi")
        for k_, col, nome in (("alzo", PAL["acc"], "alzo"), ("cant", PAL["grn"], "cant")):
            f.add_trace(go.Scatter(x=D.shot, y=D[k_], mode="markers+lines", name=nome,
                                   line=dict(color=col, width=1), marker=dict(size=7)))
        f.add_trace(go.Scatter(x=D.shot, y=D.elev_deg, mode="lines",
                               name="elevazione dichiarata",
                               line=dict(color=PAL["amb"], width=2, dash="dash")))
        st.plotly_chart(f, use_container_width=True)

# ============================================================================
#  7 · TIRO PER TIRO
# ============================================================================
with TAB[6]:
    tiri = sorted(D["shot"].dropna().astype(int).unique())
    if not tiri:
        st.info("Nessun tiro.")
    else:
        sh = st.select_slider("Tiro", tiri, value=tiri[0])
        r = D[D["shot"] == sh].iloc[0]
        sed = next((s for s in sel if s.nome == r["seduta"]), None)
        b = sed.burst_del_tiro(sh) if sed else None

        c = st.columns(5)
        with c[0]: kpi(f"{r.get('tempo_mira_s', np.nan):.2f} s"
                       if pd.notna(r.get("tempo_mira_s")) else "—", "mira")
        with c[1]: kpi(f"{r.get('yaw_causale', np.nan):+.3f}°"
                       if pd.notna(r.get("yaw_causale")) else "—", "yaw causale")
        with c[2]: kpi(f"{r.get('imp_x_cm', np.nan):+.0f} cm"
                       if pd.notna(r.get("imp_x_cm")) else "n/d", "laterale")
        with c[3]: kpi(f"{r.get('imp_y_cm', np.nan):+.0f} cm"
                       if pd.notna(r.get("imp_y_cm")) else "n/d", "verticale")
        with c[4]: kpi(f"{r.get('t_rilascio_ms', np.nan):+.0f} ms"
                       if pd.notna(r.get("t_rilascio_ms")) else "—", "rilascio")

        if b is not None:
            t = b.t_ms
            m = (t >= -400) & (t <= 200)
            f = fig(380, "ms dal trigger", "velocità angolare [dps]")
            for k_, col, nome in ((-b.gx, PAL["acc"], "yaw (−gx)"),
                                  (-b.gy, PAL["grn"], "alzo (−gy)"),
                                  (b.gz, PAL["tx3"], "cant (+gz)")):
                f.add_trace(go.Scatter(x=t[m], y=k_[m], mode="lines", name=nome,
                                       line=dict(width=1.6, color=col)))
            tr = r.get("t_rilascio_ms")
            if pd.notna(tr):
                f.add_vrect(x0=tr, x1=tr + ac.T_CAUSALE_MS, fillcolor=PAL["amb"],
                            opacity=.18, line_width=0,
                            annotation_text="i 17 ms che contano")
                f.add_vline(x=tr, line_color=PAL["amb"], line_dash="dot",
                            annotation_text="rilascio")
                # la freccia se ne va QUI: e' l'istante che separa causa da
                # diagnostica, e prima non era disegnato da nessuna parte
                f.add_vline(x=tr + ac.T_CAUSALE_MS, line_color=PAL["grn"],
                            line_dash="dot", annotation_text="freccia via")
            # La grandezza su cui il trigger decide, sull'asse DESTRO.
            # NON e' l'accelerazione lungo l'asse freccia: e' | ‖a‖ − g |, il
            # modulo, invariante per rotazione. E' la ragione per cui e' stata
            # scelta (immune ai falsi da inclinazione) ed e' anche il motivo
            # per cui mostrare az al suo posto sarebbe fuorviante.
            f.add_trace(go.Scatter(
                x=t[m], y=ac._misura_trigger(b)[m], mode="lines", yaxis="y2",
                name="urto  | ‖a‖ − g |",
                line=dict(width=1.4, color=PAL["red"], dash="dot")))
            f.update_layout(yaxis2=dict(
                title="urto [m/s²]", overlaying="y", side="right",
                showgrid=False, color=PAL["red"], rangemode="tozero"))
            f.add_vline(x=0, line_color=PAL["red"], line_dash="dash",
                        annotation_text="trigger")
            f.add_vrect(x0=ac.ZUPT_A, x1=ac.ZUPT_B, fillcolor=PAL["grn"],
                        opacity=.10, line_width=0, annotation_text="mira ferma")
            st.plotly_chart(f, use_container_width=True)

            # --- traccia 2D: dove va la punta dell'arco --------------------
            tz = ac.traccia(b, tr) if pd.notna(tr) else dict(ok=False)
            if tz["ok"]:
                i = tz["i_fine_causale"]
                f2 = fig(430, "spostamento laterale [°]  (+ destra)",
                         "spostamento verticale [°]  (+ alto)")
                # tratto SCURO = follow-through, disegnato per primo cosi' il
                # tratto causale gli sta sopra e resta leggibile
                f2.add_trace(go.Scatter(
                    x=tz["yaw"][i:], y=tz["alzo"][i:], mode="lines",
                    name=f"follow-through (fino a {ac.TRACCIA_FINE_MS:.0f} ms)",
                    line=dict(color=PAL["tx3"], width=1.6),
                    hovertemplate="%{customdata:.0f} ms<br>%{x:+.2f}° / %{y:+.2f}°"
                                  "<extra></extra>",
                    customdata=tz["t_rel"][i:]))
                # tratto PIENO = i 17 ms che contano
                f2.add_trace(go.Scatter(
                    x=tz["yaw"][:i+1], y=tz["alzo"][:i+1], mode="lines",
                    name="i 17 ms che contano",
                    line=dict(color=PAL["amb"], width=4),
                    hovertemplate="%{customdata:.0f} ms<br>%{x:+.2f}° / %{y:+.2f}°"
                                  "<extra></extra>",
                    customdata=tz["t_rel"][:i+1]))
                f2.add_trace(go.Scatter(
                    x=[0], y=[0], mode="markers", name="rilascio",
                    marker=dict(size=11, color=PAL["grn"],
                                line=dict(width=2, color=_SFONDO))))
                f2.add_trace(go.Scatter(
                    x=[tz["yaw"][i]], y=[tz["alzo"][i]], mode="markers",
                    name="freccia via",
                    marker=dict(size=13, color=PAL["amb"], symbol="x")))
                f2.add_hline(y=0, line_color=PAL["grid"])
                f2.add_vline(x=0, line_color=PAL["grid"])
                # Scala UGUALE sui due assi: con scale diverse la forma della
                # traccia mente, e la forma e' l'unica cosa che si guarda.
                f2.update_yaxes(scaleanchor="x", scaleratio=1)
                st.plotly_chart(f2, use_container_width=True)
                st.caption(
                    f"Il percorso della punta dell'arco a partire dal rilascio. "
                    f"Il **tratto spesso** sono i 17 ms in cui la freccia è "
                    f"ancora sulla corda: quello determina dove va. Il tratto "
                    f"sottile è il follow-through, che racconta il gesto ma non "
                    f"lo ha causato. I due assi hanno la **stessa scala**, "
                    f"quindi la forma è fedele. "
                    f"Oltre {ac.TRACCIA_FINE_MS:.0f} ms non si disegna: l'errore "
                    f"del giroscopio arriverebbe a {tz['deriva_max']:.2f}° e "
                    f"sarebbe indistinguibile dal movimento vero.")

            if pd.notna(tr):
                st.caption(
                    f"**Da sinistra a destra:** l'arco è fermo in mira (fascia "
                    f"verde, dove si misura anche l'errore del giroscopio) → "
                    f"il rilascio a {tr:+.0f} ms → i **17 ms** in cui l'arco può "
                    f"ancora influenzare la freccia (fascia gialla) → la freccia "
                    f"se ne va (riga verde) → **da lì in poi è follow-through**, "
                    f"cioè il gesto dopo, non la causa. "
                    f"Il trigger (riga rossa) arriva per ultimo, ~29 ms più "
                    f"tardi: scatta sull'urto, quando la freccia è già via.")
            else:
                st.warning(
                    "**Rilascio non rilevabile su questo tiro.** L'arco non si "
                    "è mosso abbastanza da distinguersi dal tremolio di mira "
                    "prima dello scocco. Senza l'istante del rilascio non c'è "
                    "finestra causale, e le metriche relative restano vuote — "
                    "meglio di un numero inventato.")

# ============================================================================
#  8 · DATI
# ============================================================================
with TAB[7]:
    st.markdown("##### Tabella completa")
    st.dataframe(D, use_container_width=True, height=480)
    st.download_button("Scarica CSV unito", D.to_csv(index=False).encode(),
                       "archbb_analisi.csv", "text/csv")
    nota("<code>picco</code> è in m/s² (il firmware lo scrive in centesimi "
         "nella colonna <code>picco_cms2</code>).<br><br>"
         "Le colonne <code>d_*</code> sono <b>centrate sulla mediana della "
         "seduta</b>: la componente sistematica è assorbita dalla taratura del "
         "mirino e non può spostare l'impatto tiro per tiro. Le colonne "
         "<code>cm_*</code> sono la traduzione in centimetri con il k della "
         "barra laterale — indicative finché k è una stima e non una misura.")
