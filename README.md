# ArchBB 1.83 — Fase 21

Scatola nera per l'arco nudo: una scheda sul riser registra con un IMU cosa fa
l'arco attorno al rilascio, l'arciere dichiara a schermo dove è andata la
freccia, tutto finisce su microSD.

```
firmware   1.83-F22b-pretiro
CSV        v5     (nuova colonna in coda: picco_cms2)
Config     v7     (invariata)
App        v0.8
Scheda     Waveshare ESP32-S3-Touch-LCD-1.83
```

## Cosa c'è di nuovo in questa fase

**La schermata IMPATTO non si fa più coprire dal dito.** Quattro tasti grandi
ai bordi — sinistra, destra, sopra, sotto — spostano il marcatore di un
centimetro per tocco, con il punto sempre in chiaro. Il tocco diretto sul
cerchio resta per il posizionamento grossolano. Il cerchio scende da R=100 a
R=50 px per far posto ai tasti: la risoluzione del tocco diretto dimezza, ma
quei mezzi centimetri erano teorici perché il punto stava sotto il dito.

**Niente più temporizzatore.** La conferma è un OK esplicito: INDIETRO · NON SO
· OK. La lettura in centimetri è in font 4, l'anello dei 20 cm è giallo, il
marcatore bianco, e un tocco fuori dal cerchio non salta più al bordo.

**Il riepilogo mostra il picco d'urto** in m/s², la stessa grandezza che il
trigger confronta con la soglia. Senza semaforo colorato: è un numero da
mettere in serie storica, non da giudicare a colpo d'occhio, finché il
cronografo non avrà detto a cosa corrisponde.

**La radice è pulita.** I ventitré `.md` che si erano accumulati sono confluiti
in `DOCUMENTAZIONE.md`; gli originali stanno in `storico/`.

## Struttura

```
DOCUMENTAZIONE.md   tutto: hardware, architettura, metriche, contratti, errori
platformio.ini      la build. Due righe critiche, spiegate dentro
src/                firmware
app/                app companion (HTML, Web Bluetooth)
tools/              diagnostica offline: controllo sintassi, lettori Python
test_mount/         procedura di caratterizzazione dell'IMU
storico/            i documenti di fase superati, per la cronologia
```

## Compilare

```powershell
pio run -e archbb_183_touchcal -t upload
```

Prima di ogni commit, il controllo di sintassi senza toolchain — due secondi:

```sh
sh tools/sintassi/controlla.sh
```

Il resto sta in [`DOCUMENTAZIONE.md`](DOCUMENTAZIONE.md).
