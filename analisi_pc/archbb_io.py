#!/usr/bin/env python3
# ============================================================================
#  ArchBB — archbb_io.py
#  Lettura delle sedute: cartelle sulla microSD, zip, upload.
#  Cesare Pagura · Padova/Noale IT · agosto 2026
# ----------------------------------------------------------------------------
#  PRINCIPIO EREDITATO DAL FIRMWARE: la card *e'* lo stato.
#    sd_storage.cpp non tiene un indice delle sessioni: le enumera dal
#    filesystem ogni volta. Qui si fa lo stesso. Nessun database, nessuna cache
#    da tenere allineata: si guarda la card e si vede la verita'.
#
#  NOVITA' 16/08 — GLI ZIP SI APRONO COME LE CARTELLE
#    Comprimere una seduta direttamente sulla card e' il modo naturale di
#    portarla via, e fino a ieri l'analizzatore leggeva gli zip solo se caricati
#    dall'interfaccia. Adesso la scansione di una cartella trova ANCHE i .zip
#    che contiene, e li apre. Una cartella che contiene sia SESS_x/ sia
#    SESS_x.zip mostra una sola seduta: vince la cartella, perche' e' l'unica
#    su cui si possa scrivere una correzione.
#
#  ATTENZIONE ai burst SENZA riga CSV e viceversa
#    Il firmware scrive prima il .bin e poi la riga CSV (il CSV e' il dato
#    prezioso, il burst e' sacrificabile). Un crash fra i due lascia un burst
#    orfano: non e' un errore da nascondere, e' un tiro di cui restano i
#    campioni ma non lo scoring, e va mostrato come tale.
# ============================================================================
from __future__ import annotations

VERSIONE = "2026.08.25-01"

import csv as csvmod
import io
import re
import zipfile
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import pandas as pd

from archbb_dsp import Burst, leggi_burst

RE_SESSIONE = re.compile(r"^SESS_[0-9_]+$", re.I)
RE_BURST    = re.compile(r"^burst_\d+\.bin$", re.I)

# ----------------------------------------------------------------------------
#  Colonne del CSV, per versione di formato
# ----------------------------------------------------------------------------
#  Il firmware estende SEMPRE in coda, quindi leggere per NOME rende tutte le
#  versioni compatibili senza rami condizionali: una colonna che non c'e'
#  semplicemente non compare, e chi la usa trova NaN.
COL_INTERE = (
    "shot", "ts_ms", "colpito", "zona", "dist_m", "elev_deg", "arcs",
    "angoli_stabili", "hold_class", "release_jerk", "release_class",
    # v2 (F19) — coordinate d'impatto in centimetri sul piano del bersaglio
    "imp_x_cm", "imp_y_cm", "imp_raggio_cm",
    # v3 (F20) — tempi del gesto
    "tempo_mira_ms", "tempo_alzata_ms",
    # v4 (F20b) — plausibilita' dell'assetto
    "assetto_ok",
    # v5 (F21b) — picco dell'urto di scocco, in CENTESIMI di m/s^2
    "picco_cms2",
)
COL_CENTESIMI = ("cant_cdeg", "alzo_cdeg", "hold_cdeg")   # -> gradi


@dataclass
class Sessione:
    nome: str
    percorso: str                      # descrittivo (path, "zip:...", "up:...")
    meta: dict = field(default_factory=dict)
    tiri: pd.DataFrame = field(default_factory=pd.DataFrame)
    bursts: dict = field(default_factory=dict)     # nome file -> Burst
    avvisi: list = field(default_factory=list)
    scrivibile: bool = False           # solo le cartelle su disco lo sono

    @property
    def n_tiri(self) -> int:
        return len(self.tiri)

    @property
    def n_onde(self) -> int:
        return len(self.bursts)

    @property
    def csv_versione(self) -> int:
        """Versione del formato CSV, dedotta dalle colonne presenti.

        Si deduce e non si legge dal commento: un file corretto a mano puo'
        avere il commento di una versione e le colonne di un'altra, e a valle
        contano le colonne.
        """
        c = set(self.tiri.columns)
        if "picco_cms2" in c:      return 5
        if "assetto_ok" in c:      return 4
        if "tempo_mira_ms" in c:   return 3
        if "imp_x_cm" in c:        return 2
        return 1

    @property
    def quando(self) -> str:
        """Chiave di ordinamento cronologico, la migliore disponibile."""
        for k in ("datetime", "ended_datetime", "last_shot_datetime"):
            q = self.meta.get(k)
            if q:
                return q
        m = re.match(r"SESS_(\d{8})_(\d{4})", self.nome)
        if m:
            d, t = m.group(1), m.group(2)
            return f"{d[0:4]}-{d[4:6]}-{d[6:8]}T{t[0:2]}:{t[2:4]}:00"
        return "0000-" + self.nome        # i numerati finiscono in fondo

    @property
    def mount(self) -> int | None:
        v = self.meta.get("mount")
        try:
            return int(v)
        except (TypeError, ValueError):
            return None

    def burst_del_tiro(self, shot: int) -> Burst | None:
        if self.tiri.empty:
            return None
        r = self.tiri[self.tiri["shot"] == shot]
        if r.empty:
            return None
        f = r.iloc[0].get("burst_file") or ""
        return self.bursts.get(f)

    def bursts_ordinati(self) -> list[Burst]:
        return [self.bursts[k] for k in sorted(self.bursts)]


# ============================================================================
#  Parsing dei singoli file
# ============================================================================
def leggi_session_txt(testo: str) -> dict:
    meta = {}
    for riga in testo.splitlines():
        if "=" in riga:
            k, v = riga.split("=", 1)
            meta[k.strip()] = v.strip()
    return meta


def leggi_shots_csv(testo: str) -> tuple[pd.DataFrame, str]:
    """shots.csv -> DataFrame. Ritorna anche la riga di commento.

    IL PUNTO DELICATO: il campo VUOTO significa NON MISURATO, che non e' zero.
    Un hold non valido letto come 0 diventerebbe "tenuta perfetta" — l'esatto
    contrario del vero. Un imp_x_cm vuoto letto come 0 diventerebbe "centro
    esatto", cioe' il tiro migliore possibile. pandas legge i vuoti come NaN, e
    NaN e' la rappresentazione giusta: si propaga nei calcoli invece di
    falsarli in silenzio. Non si riempiono MAI con fillna(0).
    """
    righe = [r for r in testo.splitlines() if r.strip()]
    commento = ""
    i = 0
    while i < len(righe) and righe[i].lstrip().startswith("#"):
        commento = righe[i].lstrip("# ").strip()
        i += 1
    if i >= len(righe):
        return pd.DataFrame(), commento

    lettore = csvmod.DictReader(io.StringIO("\n".join(righe[i:])))
    dati = list(lettore)
    if not dati:
        return pd.DataFrame(), commento
    df = pd.DataFrame(dati)
    df.columns = [c.strip() for c in df.columns]

    for c in COL_INTERE:
        if c in df.columns:
            df[c] = pd.to_numeric(df[c].replace("", np.nan), errors="coerce")
    for c in COL_CENTESIMI:
        if c in df.columns:
            v = pd.to_numeric(df[c].replace("", np.nan), errors="coerce")
            # La colonna in centesimi va riscritta NUMERICA, non solo letta per
            # derivarne i gradi: lasciandola stringa, qualunque calcolo che la
            # tocchi piu' avanti esplode con un errore che parla di dtype e non
            # di quello che e' successo davvero.
            df[c] = v
            df[c.replace("_cdeg", "")] = v / 100.0      # cant, alzo, hold in gradi
    if "burst_file" in df.columns:
        df["burst_file"] = df["burst_file"].fillna("").astype(str).str.strip()
    if "colpito" in df.columns:
        df["colpito"] = df["colpito"] == 1

    # --- comodita' derivate, tutte NaN-safe ---------------------------------
    if "tempo_mira_ms" in df.columns:
        df["tempo_mira_s"] = df["tempo_mira_ms"] / 1000.0
    if "tempo_alzata_ms" in df.columns:
        df["tempo_alzata_s"] = df["tempo_alzata_ms"] / 1000.0
    if "picco_cms2" in df.columns:
        # in m/s^2, che e' l'unita' in cui se ne parla ovunque. Il firmware lo
        # scrive in centesimi per stare in un intero senza perdere risoluzione.
        df["picco"] = df["picco_cms2"] / 100.0
    if {"imp_x_cm", "imp_y_cm"} <= set(df.columns):
        # Distanza dal PUNTO DI MIRA, non dal centro del gruppo: sono due cose
        # diverse e quella dal centro del gruppo si calcola dopo, quando il
        # gruppo e' noto.
        df["imp_r_cm"] = np.hypot(df["imp_x_cm"], df["imp_y_cm"])
    return df, commento


# ============================================================================
#  Composizione di una seduta da una mappa nome->bytes
# ============================================================================
def _componi_sessione(nome: str, percorso: str, file_map: dict,
                      scrivibile: bool = False) -> Sessione:
    s = Sessione(nome=nome, percorso=percorso, scrivibile=scrivibile)

    if "session.txt" in file_map:
        s.meta = leggi_session_txt(file_map["session.txt"].decode("utf-8", "replace"))
    else:
        s.avvisi.append("session.txt assente: mancano fw, ODR e montaggio dichiarati")

    if "shots.csv" in file_map:
        s.tiri, commento = leggi_shots_csv(file_map["shots.csv"].decode("utf-8", "replace"))
        if commento:
            s.meta.setdefault("csv_commento", commento)
    else:
        s.avvisi.append("shots.csv assente: nessuna metrica, solo forme d'onda")

    for nomefile, dati in file_map.items():
        if RE_BURST.match(nomefile):
            b = leggi_burst(dati, nomefile)
            if b is None:
                s.avvisi.append(f"{nomefile}: non e' un burst valido (magic errato)")
            else:
                s.bursts[nomefile] = b
                if not b.seq_continua:
                    s.avvisi.append(f"{nomefile}: sequenza con buchi, fs stimata dalla media")

    # --- coerenza fra le due meta' del dato ---------------------------------
    if not s.tiri.empty and "burst_file" in s.tiri.columns:
        attesi = {f for f in s.tiri["burst_file"] if f}
        presenti = set(s.bursts)
        for f in sorted(attesi - presenti):
            s.avvisi.append(f"{f}: dichiarato nel CSV ma assente")
        for f in sorted(presenti - attesi):
            s.avvisi.append(f"{f}: burst orfano, nessuna riga CSV")

    # --- avvisi sul montaggio, che e' il parametro piu' facile da sbagliare --
    if s.mount is not None and s.mount == 0:
        s.avvisi.append(
            "mount=0 (frontale): se la scheda era di lato, cant e alzo sono "
            "scambiati. Vedi la scheda «Assetto» per la verifica.")
    return s


# ============================================================================
#  Scansione delle sorgenti
# ============================================================================
def _mappa_da_cartella(d: Path) -> dict:
    fm = {}
    for f in d.iterdir():
        if f.is_file():
            try:
                fm[f.name.lower()] = f.read_bytes()
            except OSError:
                pass
    return fm


def _gruppi_da_zip(dati: bytes) -> dict[str, dict]:
    """Raggruppa il contenuto di uno zip per seduta.

    Due strutture possibili, entrambe comuni:
      archivio.zip/SESS_2026.../shots.csv     zippata la cartella
      archivio.zip/shots.csv                  zippato il CONTENUTO
    Nel secondo caso il nome della seduta lo da' l'archivio, e va passato dal
    chiamante: qui si usa la chiave vuota "" come segnaposto.
    """
    gruppi: dict[str, dict] = {}
    with zipfile.ZipFile(io.BytesIO(dati)) as z:
        for info in z.infolist():
            if info.is_dir():
                continue
            parti = Path(info.filename).parts
            sess = next((p for p in parti if RE_SESSIONE.match(p)), None)
            chiave = sess if sess is not None else ""
            gruppi.setdefault(chiave, {})[Path(info.filename).name.lower()] = z.read(info)
    # Lo zip senza cartella vale solo se contiene davvero una seduta.
    if "" in gruppi and not ({"session.txt", "shots.csv"} & set(gruppi[""])):
        del gruppi[""]
    return gruppi


def scansiona_zip(dati: bytes, etichetta: str = "zip") -> list[Sessione]:
    """Sedute contenute in un archivio zip (caricato o su disco)."""
    fuori = []
    for nome, fm in _gruppi_da_zip(dati).items():
        n = nome or etichetta
        fuori.append(_componi_sessione(n, f"zip:{etichetta}", fm, scrivibile=False))
    return ordina(fuori)


def scansiona_cartella(percorso: str) -> list[Sessione]:
    """Sedute in una cartella: sottocartelle SESS_*, zip, e la cartella stessa.

    Tre casi, nell'ordine in cui si presentano nella pratica:
      1. la radice della card, con dentro molte SESS_*
      2. un archivio .zip accanto alle cartelle (novita' del 16/08)
      3. la cartella di UNA seduta, puntata direttamente
    """
    p = Path(percorso).expanduser()
    if not p.exists():
        raise FileNotFoundError(f"non esiste: {p}")
    if not p.is_dir():
        # Puntato un file: se e' uno zip lo si apre, altrimenti e' un errore.
        if p.suffix.lower() == ".zip":
            return scansiona_zip(p.read_bytes(), p.stem)
        raise NotADirectoryError(f"non e' una cartella: {p}")

    sessioni: list[Sessione] = []
    visti: set[str] = set()

    # (3) la cartella E' una seduta
    if (p / "session.txt").exists() or (p / "shots.csv").exists():
        s = _componi_sessione(p.name, str(p), _mappa_da_cartella(p), scrivibile=True)
        sessioni.append(s); visti.add(p.name.upper())

    for d in sorted(p.iterdir()):
        # (1) sottocartelle
        if d.is_dir() and RE_SESSIONE.match(d.name):
            fm = _mappa_da_cartella(d)
            if fm:
                sessioni.append(_componi_sessione(d.name, str(d), fm, scrivibile=True))
                visti.add(d.name.upper())
        # (2) archivi
        elif d.is_file() and d.suffix.lower() == ".zip":
            try:
                dati = d.read_bytes()
            except OSError:
                continue
            for nome, fm in _gruppi_da_zip(dati).items():
                n = nome or d.stem
                # La cartella vince sullo zip: e' l'unica su cui si possa
                # scrivere una correzione, e mostrare due volte la stessa
                # seduta e' peggio che non mostrarne una.
                if n.upper() in visti:
                    continue
                sessioni.append(_componi_sessione(n, f"zip:{d}", fm, scrivibile=False))
                visti.add(n.upper())

    return ordina(sessioni)


# ============================================================================
#  CORREZIONE DEL MONTAGGIO — recupero di sedute nate nel frame sbagliato
# ============================================================================
#  IL FATTO (15 agosto 2026)
#    Tre sedute sono state registrate con `mount=0` mentre la scheda era
#    fisicamente montata a destra (`mount=2`). Nessuno aveva toccato niente:
#    prima un bump di CONFIG_VERSION aveva riportato la configurazione ai
#    default, poi la scrittura BLE che doveva rimetterla a posto veniva
#    scartata in silenzio perche' l'app spediva un pacchetto di 4 byte piu'
#    corto di quanto il firmware si aspettasse.
#
#    Il sintomo era invisibile a occhio: il sistema continuava a produrre
#    numeri plausibili, solo sull'asse sbagliato. Un «cant di 8 gradi
#    costante» che in realta' era l'elevazione del bersaglio.
#
#  PERCHE' E' RECUPERABILE
#    La trasformazione mancante e' una ROTAZIONE PURA, quindi invertibile
#    senza perdita. Da mount.h, MOUNT_SX_P90:
#        cay = az    caz = -ay        cgy = gz    cgz = -gy
#    (ax e gx sono invarianti: la rotazione avviene attorno a quell'asse; il
#    flip-Z hardware e' un pre-passo gia' applicato in entrambi i casi, quindi
#    non entra nel conto.)
#
#  COSA VIENE CORRETTO, E COSA VA RICALCOLATO
#    burst           permutazione diretta dei sei assi
#    cant / alzo     seguono dalla permutazione:
#                        alzo_giusto = −cant_registrato
#                        cant_giusto = +alzo_registrato
#    hold / jerk     NON derivabili dai valori registrati: hold integra gy e
#                    jerk deriva ay, e sotto la permutazione quei due assi
#                    diventano gz e az. Si RICALCOLANO dal burst corretto.
#    impatti/tempi   non toccati: non dipendono dagli assi.
#
#  NON DISTRUTTIVO: i file su disco restano come sono. La correzione vive in
#  memoria, per la sessione di analisi. Una seduta corretta lo dichiara.
MOUNT_ATTESO = 2


def _permuta(b) -> None:
    """Applica in-place la rotazione mancante a un burst. Vedi sopra."""
    ay, az = b.ay.copy(), b.az.copy()
    gy, gz = b.gy.copy(), b.gz.copy()
    b.ay, b.az = az, -ay
    b.gy, b.gz = gz, -gy


def correggi_mount(s: Sessione) -> Sessione:
    """Riporta nel frame canonico una seduta registrata con mount=0.

    Ritorna la sessione modificata (in-place: e' gia' una copia in memoria).
    Non fa nulla se il montaggio dichiarato e' gia' quello atteso.
    """
    if s.mount == MOUNT_ATTESO:
        return s

    for b in s.bursts.values():
        _permuta(b)

    if not s.tiri.empty and {"cant", "alzo"} <= set(s.tiri.columns):
        #   alzo_giusto = −cant_registrato        cant_giusto = +alzo_registrato
        # Si copiano ENTRAMBI prima di scrivere: assegnare in sequenza usando la
        # colonna appena sovrascritta e' il modo classico di scambiare due
        # variabili ottenendone una sola.
        cant_v = s.tiri["cant"].copy()
        alzo_v = s.tiri["alzo"].copy()
        s.tiri["alzo"] = -cant_v
        s.tiri["cant"] = alzo_v
        # le colonne in centesimi seguono, cosi' chi legge quelle non trova
        # due verita' diverse nella stessa riga
        if {"alzo_cdeg", "cant_cdeg"} <= set(s.tiri.columns):
            cc = s.tiri["cant_cdeg"].copy()
            ac_ = s.tiri["alzo_cdeg"].copy()
            s.tiri["alzo_cdeg"] = -cc
            s.tiri["cant_cdeg"] = ac_

    # hold e jerk non sono derivabili: si azzerano qui e si ricalcolano a valle
    # dal burst corretto. Metterli a NaN e non lasciarli sbagliati e' la stessa
    # disciplina della sentinella: un dato mancante e' meglio di uno falso.
    for c in ("hold", "hold_cdeg", "hold_class", "release_jerk", "release_class"):
        if c in s.tiri.columns:
            s.tiri[c] = np.nan

    s.meta["mount"] = str(MOUNT_ATTESO)
    s.meta["mount_lato"] = "destro (corretto in analisi)"
    s.meta["mount_corretto"] = "si"
    s.avvisi = [a for a in s.avvisi if not a.startswith("mount=0")]
    s.avvisi.append(
        "montaggio corretto in analisi: assi ruotati di +90°, cant e alzo "
        "scambiati, tenuta e rilascio ricalcolati dal burst")
    return s


def mount_sospetto(s: Sessione) -> bool:
    """Vero se i dati dicono che il montaggio dichiarato non e' quello vero.

    Il criterio e' quello della scheda «Assetto»: puntando un bersaglio a
    elevazione nota, l'alzo deve valere l'elevazione. Se invece e' il CANT a
    seguirla, gli assi sono scambiati. Si guarda ai DATI e non solo al valore
    dichiarato, perche' una seduta puo' dichiarare mount=0 ed essere stata
    davvero raccolta con la scheda frontale.
    """
    if s.mount == MOUNT_ATTESO or s.tiri.empty:
        return False
    if not {"cant", "alzo", "elev_deg"} <= set(s.tiri.columns):
        return False
    e = s.tiri["elev_deg"].median()
    if pd.isna(e):
        return False
    # ATTENZIONE AL SEGNO. La rotazione da' alzo_giusto = −cant_registrato,
    # quindi il cant registrato vale MENO l'elevazione, non piu'. Confrontare
    # |cant − elev| invece di |−cant − elev| fa fallire il rilevamento proprio
    # sulle sedute che lo motivano: con elevazione −8° e cant registrato +8°,
    # il primo criterio da' 16 e il secondo 0.
    da_alzo = abs(s.tiri["alzo"].median() - e)
    da_cant = abs(-s.tiri["cant"].median() - e)
    return bool(da_cant + 2.0 < da_alzo)


def ordina(sessioni: list[Sessione]) -> list[Sessione]:
    """Dalla piu' recente. Vedi la nota sui due schemi di nome in testa."""
    return sorted(sessioni, key=lambda s: s.quando, reverse=True)


def unisci(sessioni: list[Sessione]) -> pd.DataFrame:
    """Tabella unica dei tiri di piu' sedute, con la provenienza in colonna.

    Serve perche' la potenza statistica arriva da n, e n arriva accumulando
    sedute. La colonna `seduta` resta sempre, cosi' si puo' controllare che un
    effetto non sia solo la differenza fra due giornate.
    """
    pezzi = []
    for s in sessioni:
        if s.tiri.empty:
            continue
        d = s.tiri.copy()
        d.insert(0, "seduta", s.nome)
        d["mount"] = s.mount
        d["fw"] = s.meta.get("fw", "")
        pezzi.append(d)
    if not pezzi:
        return pd.DataFrame()
    return pd.concat(pezzi, ignore_index=True)
