// ============================================================================
//  ArchBB 1.83 — main_mount_test.cpp · TEST MONTAGGI (firmware separato)
//  Cesare Pagura · Padova/Noale IT · 22 luglio 2026
// ----------------------------------------------------------------------------
//  SCOPO — rispondere, coi DATI, a due domande sul montaggio dell'IMU:
//
//    A) CARATTERIZZAZIONE FISICA: dove punta OGNI asse del QMI8658 sulla 1.83?
//       A riposo l'accelerometro misura solo la GRAVITA' (~9.8 m/s^2 verso il
//       basso). Orientando la scheda in pose note, si legge quale asse fisico
//       (ax/ay/az) reagisce e con che segno -> si costruisce la mappa REALE del
//       chip, senza strumenti, con ground-truth perfetto.
//
//    B) VERIFICA MATRICI: applicando OGNI montaggio (0, -90, +90, 180, 0_FLIPZ)
//       allo STESSO campione grezzo, si vede quale da' cant/alzo col segno
//       concorde al gesto -> si conferma o si smentisce se serve il flip-Z, e se
//       basta UN flip per tutte le 4 posizioni o serve la quinta separata.
//
//  RIFERIMENTO DI MONTAGGIO (posizione 0, fissata dall'arciere):
//    - schermo verso l'ARCIERE
//    - il RETRO della scheda punta al BERSAGLIO (asse-freccia ESCE dal retro,
//      perpendicolare al PCB)
//    - USB-C e tasti a DESTRA
//    Posizione -90: schermo a destra, USB verso il bersaglio.
//
//  CONVENZIONE FUNZIONALE ATTESA DALLE FORMULE (shot_angles.cpp):
//    - X = asse gravita'/rollio (verticale ad arco dritto): mx in atan2
//    - Y = laterale -> CANT = atan2(ay, ax): + = inclinato a DESTRA
//    - Z = mira     -> ALZO = atan2(az, ax): + = punta in ALTO
//  Il test mostra assi FISICI e FUNZIONALI AFFIANCATI: se l'asse che risponde
//  all'alzo non e' 'az', o ha segno inatteso, il flip-Z stava mascherando uno
//  scambio di assi (non un semplice segno) -> lo si vede qui.
//
//  OUTPUT: seriale (tabelle complete, da loggare al PC) + display (colpo
//  d'occhio a banco, senza cavo). Nessun mount viene applicato in lettura:
//  usiamo imu_read_raw() (grezzi in frame SENSORE) e applichiamo noi le matrici.
//
//  USO:
//    pio run -e archbb_183_mount_test -t upload
//    poi: monitor seriale a 115200. Sul display, i comandi touch cambiano vista.
// ============================================================================
// ============================================================================
//  Attivo SOLO quando l'env definisce -D ARCHBB_MOUNT_TEST=1 (env
//  archbb_183_mount_test). Senza quel flag, questo file e' vuoto e compila il
//  main di produzione. Cosi' i due main coesistono in src/ senza conflitto.
// ============================================================================
#ifdef ARCHBB_MOUNT_TEST

#include <Arduino.h>
#include <math.h>
#include <string.h>   // strcmp (usato per l'asse dominante)

#include "config.h"
#include "display.h"
#include "touch.h"
#include "imu.h"
#include "mount.h"

// ----------------------------------------------------------------------------
//  Stato del test: due viste, commutabili col tocco.
// ----------------------------------------------------------------------------
enum class TestView : uint8_t { CHIP_RAW, MOUNTS };
static TestView s_view = TestView::CHIP_RAW;

// Media mobile leggera sui grezzi (a riposo la gravita' e' stabile; smorziamo
// il rumore per leggere numeri fermi). Non e' un filtro "furbo": solo una media
// esponenziale con alpha alto, cosi' i valori sul display non ballano.
static float fax = 0, fay = 0, faz = 0;
static float fgx = 0, fgy = 0, fgz = 0;
static bool  s_primed = false;

static void updateFiltered(float ax, float ay, float az,
                           float gx, float gy, float gz) {
  const float A = 0.15f;   // peso del nuovo campione
  if (!s_primed) { fax=ax; fay=ay; faz=az; fgx=gx; fgy=gy; fgz=gz; s_primed=true; return; }
  fax += A*(ax-fax); fay += A*(ay-fay); faz += A*(az-faz);
  fgx += A*(gx-fgx); fgy += A*(gy-fgy); fgz += A*(gz-fgz);
}

// ----------------------------------------------------------------------------
//  Helper: dato l'asse dominante (quello con |valore| massimo), ritorna una
//  etichetta tipo "+Z" / "-X" — cosi' a colpo d'occhio sai quale asse fisico
//  sta "sentendo" la gravita' nella posa corrente.
// ----------------------------------------------------------------------------
static const char* dominantAxis(float ax, float ay, float az) {
  float aax = fabsf(ax), aay = fabsf(ay), aaz = fabsf(az);
  if (aax >= aay && aax >= aaz) return ax >= 0 ? "+X" : "-X";
  if (aay >= aax && aay >= aaz) return ay >= 0 ? "+Y" : "-Y";
  return az >= 0 ? "+Z" : "-Z";
}

// ----------------------------------------------------------------------------
//  Calcolo cant/alzo per un dato montaggio, partendo dai grezzi filtrati.
//  Applichiamo la matrice del montaggio (mount_apply) e poi le STESSE formule
//  dello scoring. Cosi' testiamo il codice VERO, non una copia.
// ----------------------------------------------------------------------------
struct MountResult { float cant, alzo; };
static MountResult computeForMount(uint8_t orient) {
  // Copia locale: mount_apply modifica in-place.
  float ax = fax, ay = fay, az = faz;
  float gx = fgx, gy = fgy, gz = fgz;
  mount_apply(orient, ax, ay, az, gx, gy, gz);
  MountResult r;
  r.cant = atan2f(ay, ax) * 180.0f / (float)M_PI;   // = atan2(ay, ax)
  r.alzo = atan2f(az, ax) * 180.0f / (float)M_PI;   // = atan2(az, ax)
  return r;
}

// ============================================================================
//  OUTPUT SERIALE — tabelle complete (loggabili al PC)
// ============================================================================
static void serialReport() {
  if (!(bool)Serial || Serial.availableForWrite() <= 0) return;

  float g = sqrtf(fax*fax + fay*fay + faz*faz);   // modulo: ~9.8 se fermo

  Serial.println(F("=============================================================="));
  Serial.printf ("GREZZI (frame SENSORE, nessun mount)   |g|=%.2f m/s^2\n", g);
  Serial.printf ("  ax=%+6.2f  ay=%+6.2f  az=%+6.2f   [asse gravita' dominante: %s]\n",
                 fax, fay, faz, dominantAxis(fax, fay, faz));
  Serial.printf ("  gx=%+6.1f  gy=%+6.1f  gz=%+6.1f   dps\n", fgx, fgy, fgz);
  Serial.println(F("--------------------------------------------------------------"));
  Serial.println(F("CANT/ALZO per ogni montaggio (stesso grezzo):"));
  Serial.println(F("  mount            cant     alzo"));
  for (uint8_t m = 0; m < MOUNT_COUNT; m++) {
    MountResult r = computeForMount(m);
    Serial.printf ("  %-16s %+6.1f   %+6.1f\n", mount_name(m), r.cant, r.alzo);
  }
  Serial.println(F("=============================================================="));
  Serial.println(F("Guida: metti la scheda in POS 0 (schermo verso te, retro al"));
  Serial.println(F("bersaglio). A riposo l'asse dominante = gravita'. Poi inclina"));
  Serial.println(F("la PUNTA in su: guarda quale asse cambia e con che segno."));
  Serial.println();
}

// ============================================================================
//  OUTPUT DISPLAY — colpo d'occhio a banco
// ============================================================================
static void drawChipRaw() {
  tft.fillScreen(ArchColor::BG);
  tft.drawRect(0, 0, ARCHBB_W, ARCHBB_H, ArchColor::BORDER);

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(ArchColor::ACCENT, ArchColor::BG);
  tft.drawString("CHIP RAW", ARCHBB_W/2, 8, 4);

  float g = sqrtf(fax*fax + fay*fay + faz*faz);
  tft.setTextDatum(TL_DATUM);
  char line[40];

  // Assi fisici, coi valori grezzi. Evidenzio in ciano l'asse dominante.
  int y = 46; const int x = 16; const int dy = 30;
  const char* dom = dominantAxis(fax, fay, faz);

  auto drawAxis = [&](const char* name, float val, const char* fisico) {
    bool isDom = (strcmp(dom+1, fisico) == 0);   // dom = "+X", fisico = "X"
    tft.setTextColor(isDom ? ArchColor::ACCENT : ArchColor::TEXT2, ArchColor::BG);
    snprintf(line, sizeof(line), "%s %+7.2f", name, val);
    tft.drawString(line, x, y, 4);
    y += dy;
  };
  drawAxis("aX", fax, "X");
  drawAxis("aY", fay, "Y");
  drawAxis("aZ", faz, "Z");

  // Modulo gravita' (deve essere ~9.8 se la scheda e' ferma).
  y += 6;
  tft.setTextColor(fabsf(g-9.80f) < 0.4f ? ArchColor::GOOD : ArchColor::AMBER, ArchColor::BG);
  snprintf(line, sizeof(line), "|g| %.2f", g);
  tft.drawString(line, x, y, 4);

  // Asse dominante + interpretazione funzionale (X=grav, Y=cant, Z=alzo).
  y += dy + 4;
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  snprintf(line, sizeof(line), "domina: %s", dom);
  tft.drawString(line, x, y, 2);
  y += 22;
  tft.drawString("X=grav Y=cant Z=alzo", x, y, 2);

  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  tft.drawString("tocca -> vista MONTAGGI", ARCHBB_W/2, ARCHBB_H-8, 2);
}

static void drawMounts() {
  tft.fillScreen(ArchColor::BG);
  tft.drawRect(0, 0, ARCHBB_W, ARCHBB_H, ArchColor::BORDER);

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(ArchColor::ACCENT, ArchColor::BG);
  tft.drawString("MONTAGGI", ARCHBB_W/2, 8, 4);

  // Intestazione colonne
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  tft.drawString("mount",  10, 44, 2);
  tft.drawString("cant",  135, 44, 2);
  tft.drawString("alzo",  190, 44, 2);

  int y = 66; char line[24];
  for (uint8_t m = 0; m < MOUNT_COUNT; m++) {
    MountResult r = computeForMount(m);
    // Nome compatto
    tft.setTextColor(ArchColor::TEXT2, ArchColor::BG);
    const char* nm = mount_name(m);
    tft.drawString(nm, 10, y, 2);
    // cant / alzo: verde se |val|<3 (vicino a zero = assetto neutro atteso a
    // riposo su supporto piano), altrimenti ambra.
    tft.setTextColor(fabsf(r.cant) < 3 ? ArchColor::GOOD : ArchColor::TEXT, ArchColor::BG);
    snprintf(line, sizeof(line), "%+5.1f", r.cant);
    tft.drawString(line, 135, y, 2);
    tft.setTextColor(fabsf(r.alzo) < 3 ? ArchColor::GOOD : ArchColor::TEXT, ArchColor::BG);
    snprintf(line, sizeof(line), "%+5.1f", r.alzo);
    tft.drawString(line, 190, y, 2);
    y += 26;
  }

  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(ArchColor::TEXT3, ArchColor::BG);
  tft.drawString("tocca -> vista CHIP RAW", ARCHBB_W/2, ARCHBB_H-8, 2);
}

// ============================================================================
//  SETUP / LOOP
// ============================================================================
void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);

  displayInit();
  touchInit();     // avvia Wire (bus I2C condiviso) + crea il mutex I2C

  bool imuOk = imu_init(ODR_250HZ, 8, 512);

  tft.fillScreen(ArchColor::BG);
  tft.setTextDatum(MC_DATUM);
  if (imuOk) {
    tft.setTextColor(ArchColor::GOOD, ArchColor::BG);
    tft.drawString("IMU OK", ARCHBB_W/2, ARCHBB_H/2 - 20, 4);
    tft.setTextColor(ArchColor::TEXT2, ArchColor::BG);
    tft.drawString("TEST MONTAGGI 1.83", ARCHBB_W/2, ARCHBB_H/2 + 14, 2);
  } else {
    tft.setTextColor(ArchColor::BAD, ArchColor::BG);
    tft.drawString("IMU FAIL", ARCHBB_W/2, ARCHBB_H/2, 4);
  }
  delay(1500);

  if ((bool)Serial) {
    Serial.println();
    Serial.println(F("### ArchBB 1.83 — TEST MONTAGGI ###"));
    Serial.println(F("Pos 0 = schermo verso arciere, retro al bersaglio, USB a destra."));
    Serial.println(F("Report seriale ~1/s. Tocca il display per cambiare vista."));
    Serial.println();
  }
}

void loop() {
  // 1) Leggi i grezzi (frame SENSORE, nessun mount) e filtra.
  float ax, ay, az, gx, gy, gz;
  if (imu_read_raw(ax, ay, az, gx, gy, gz)) {
    updateFiltered(ax, ay, az, gx, gy, gz);
  }

  // 2) Tocco: commuta vista (con piccolo debounce implicito via td.valid).
  TouchData td = touchRead();
  static uint32_t lastTouchMs = 0;
  if (td.valid && millis() - lastTouchMs > 400) {
    lastTouchMs = millis();
    s_view = (s_view == TestView::CHIP_RAW) ? TestView::MOUNTS : TestView::CHIP_RAW;
  }

  // 3) Ridisegna il display ~10 Hz.
  static uint32_t lastDrawMs = 0;
  if (millis() - lastDrawMs > 100) {
    lastDrawMs = millis();
    if (s_view == TestView::CHIP_RAW) drawChipRaw();
    else                             drawMounts();
  }

  // 4) Report seriale ~1 Hz.
  static uint32_t lastSerMs = 0;
  if (millis() - lastSerMs > 1000) {
    lastSerMs = millis();
    serialReport();
  }

  delay(10);
}

#endif // ARCHBB_MOUNT_TEST
