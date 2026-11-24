// ============================================================
//  SINTETIZADOR AMBIENTAL — Placa 1 (ESP32-S3 SuperMini)
//  Suena cuando algo CAMBIA. Si el entorno está quieto, casi silencio (un respiro
//  suave cada ~20 s). Cada cambio es un «estímulo» con su propio sonido y su color,
//  siempre en Mi mayor hexatónica (Mi Fa# Sol# La Si Do#, sin Re#):
//
//    PERSONA (radar)    cada 30 cm que alguien se mueve suena una nota de piano
//                       eléctrico: la altura es la distancia (cerca = agudo), así que
//                       caminar hacia el equipo sube y alejarse baja.        NARANJA
//    LLEGA ALGUIEN      arpegio de gotas que sube.                          ROJO
//    SE VA              tres notas que bajan.                               VIOLETA
//    LUZ (VEML7700)     más luz = gotas que suben (ÁMBAR); menos luz = bajan (AZUL).
//                       Un cambio grande suena como una escala de tres notas.
//    PLANTAS (2 SHT40)  si cambia la humedad de una sonda, caen gotas agudas; regar
//                       = varias gotas. Planta 1 a la izquierda, 2 a la derecha. VERDE
//    TEMPERATURA        flauta grave.                                       FUCSIA
//    AIRE (ENS160)      flauta media cuando cambia el CO2.                  TURQUESA
//    UV (GUVA-S12SD)    destello agudo.                                     VIOLETA
//    PRESIÓN (BMP180)   nota muy grave.                                     ÍNDIGO
//    HUMEDAD (AHT21)    nota de piano eléctrico.                            CELESTE
//
//  Anillo 1: cada nota se enciende con el color de su fuente, donde cae en la escala.
//  Anillo 2: color de ambiente (azul de noche, celeste de día) que se tiñe suave con el
//  color del último estímulo; cada nota lanza una onda, y si hay alguien aparece un
//  arco naranjo que crece al acercarse.
//  Compilar con «USB CDC On Boot: Enabled».
// ============================================================
#pragma GCC optimize("O2")
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>     // solo para ColorHSV y gamma32
#include <ESP_I2S.h>
#include "driver/rmt_tx.h"
#include <math.h>

// ---- Pines (Placa 1 en KiCad) ----
#define PIN_SDA    4
#define PIN_SCL    7
#define PIN_SDA2   3
#define PIN_SCL2   2
#define PIN_LEDS   6
#define PIN_GUVA   1
#define PIN_RADAR_OUT 10
#define RADAR_A    44
#define RADAR_B    43
#define I2S_BCK    11
#define I2S_LCK    12
#define I2S_DIN    13

#define NUM_LEDS   48             // 2 anillos de 24: 0-23 melodía, 24-47 armonía
#define BRILLO_MAX 0.60f          // tope de brillo de los LED (48 LED en blanco a full piden ~3 A)
#define CON_FONDO  0              // 0 = suena solo la melodía; 1 = también el colchón de acordes

// ---- Audio ----
#define SR        32000
#define BLOQUE    64
#define NVOCES    10
#define VOLUMEN   0.8f            // el parlante amplificado satura con senos fuertes
#define UMBRAL_LIM 0.6f
#define TONO_HZ   6000.0f         // pasa-bajos suave a la salida: nada filudo
#define GRAVES    0.5f            // +3,5 dB bajo ~180 Hz

// ============================================================
//  ESCALA Y ACORDES
// ============================================================
const int8_t ESCALA[6] = {0, 2, 4, 5, 7, 9};      // Mi Fa# Sol# La Si Do#
#define MIDI_MI2 40
static inline int gradoAMidi(int g) { if (g < 0) g = 0; return MIDI_MI2 + 12 * (g / 6) + ESCALA[g % 6]; }
const char *NOMBRE_GRADO[6] = {"Mi", "Fa#", "Sol#", "La", "Si", "Do#"};

struct Acorde { const char *nombre; int8_t g[3]; };
const Acorde ACORDES[5] = {
  {"Mi",      {0, 2, 4}},   // Mi Sol# Si
  {"Fa#m",    {1, 3, 5}},   // Fa# La Do#
  {"La",      {3, 5, 0}},   // La Do# Mi
  {"Do#m",    {5, 0, 2}},   // Do# Mi Sol#
  {"Si sus4", {4, 0, 1}},   // Si Mi Fa#  (sin Re#, que no está en la escala)
};
const float TRANS[5][5] = {   // probabilidad de pasar de un acorde (fila) a otro (columna)
  {0.00f, 0.20f, 0.35f, 0.30f, 0.15f},
  {0.20f, 0.00f, 0.30f, 0.10f, 0.40f},
  {0.45f, 0.25f, 0.00f, 0.20f, 0.10f},
  {0.15f, 0.35f, 0.40f, 0.00f, 0.10f},
  {0.80f, 0.00f, 0.10f, 0.10f, 0.00f},
};

// ============================================================
//  SONIDOS
// ============================================================
struct Preset {
  const char *nombre;
  uint8_t fm;
  float desafino; uint8_t osc2Cuadrada; float mezcla2, sub;
  float corte, reso, envFiltro;
  float fA, fD, fS, fR;
  float ratio1, ind1Ini, ind1Fin, ind1Dec, ratio2, ind2, ind2Dec;
  float A, D, S, R;
  float ganancia;
  float nivel1, ratioOsc2;
  float lfoProf, lfoHz;
  float vibCents, vibHz;
  float form1, form2, formQ;
};
enum { P_FONDO, P_VOCES, P_GOTAS, P_EPIANO, P_FLAUTA, N_PRESETS };
const Preset PRESETS[N_PRESETS] = {
  // nombre   fm desaf osc2 mez2  sub    corte reso   envF  fA    fD    fS     fR   | FM r1 i1i   i1f   i1d    r2    i2    i2d  | A      D     S      R    | gan  | niv1 rat2 | lfoP  lfoHz | vib vHz | form
  {"FONDO",   0, 0.12f, 0, 1.0f, 0.45f, 480, 0.15f, 1.0f, 2.0f, 4.0f, 0.60f, 4.0f, 0, 0,    0,    0,     0,    0,    0,     2.50f, 3.0f, 0.85f, 4.5f, 0.55f, 1.0f, 1.0f, 0.30f, 0.07f, 0,  0,    0,   0,    0},
  {"VOCES",   0, 0.10f, 0, 0.7f, 0,     0,   0,     0,    0,    0,    0,     0,    0, 0,    0,    0,     0,    0,    0,     2.00f, 2.0f, 0.90f, 4.0f, 1.60f, 1.0f, 1.0f, 0,     0,     12, 4.5f, 650, 1100, 3.0f},
  {"GOTAS",   1, 0,     0, 0,    0,     0,   0,     0,    0,    0,    0,     0,    1, 1.0f, 0,    0.15f, 2.0f, 0.3f, 0.40f, 0.003f,3.5f, 0.00f, 2.0f, 0.50f, 0,    0,    0,     0,     0,  0,    0,   0,    0},
  {"EPIANO",  1, 0,     0, 0,    0,     0,   0,     0,    0,    0,    0,     0,    1, 1.4f, 0.35f,0.50f, 2.0f, 0.25f,0.15f, 0.003f,4.0f, 0.00f, 0.8f, 0.60f, 0,    0,    0,     0,     0,  0,    0,   0,    0},
  {"FLAUTA",  1, 0,     0, 0,    0,     0,   0,     0,    0,    0,    0,     0,    1, 0.5f, 0.25f,0.30f, 2.0f, 0,    0,     0.20f, 3.0f, 0.85f, 1.2f, 0.60f, 0,    0,    0,     0,     15, 5.0f, 0,   0,    0},
};

struct Coef { float aInc, dC, s, rC, faInc, fdC, fs, frC, m1, m2; } CC[N_PRESETS];
float coefTiempo(float t) { return 1.0f - expf(-6.9f / (fmaxf(t, 0.0005f) * SR)); }
void calcularCoef(int i) {
  const Preset &p = PRESETS[i]; Coef &C = CC[i];
  C.aInc = 1.0f / (fmaxf(p.A, 0.0005f) * SR);
  C.dC = coefTiempo(p.D); C.s = p.S; C.rC = coefTiempo(p.R);
  C.faInc = 1.0f / (fmaxf(p.fA, 0.0005f) * SR);
  C.fdC = coefTiempo(p.fD); C.fs = p.fS; C.frC = coefTiempo(p.fR);
  C.m1 = p.ind1Dec > 0 ? expf(-1.0f / (p.ind1Dec * SR)) : 0;
  C.m2 = p.ind2Dec > 0 ? expf(-1.0f / (p.ind2Dec * SR)) : 0;
}

float F1a1, F1a2, F1a3, F1k, F2a1, F2a2, F2a3, F2k;
void calcFormantes(float mult) {
  const Preset &p = PRESETS[P_VOCES];
  float k = 1.0f / p.formQ;
  float g = tanf(PI * fminf(p.form1 * mult, 12000.0f) / SR);
  F1k = k; F1a1 = 1.0f / (1.0f + g * (g + k)); F1a2 = g * F1a1; F1a3 = g * F1a2;
  g = tanf(PI * fminf(p.form2 * mult, 12000.0f) / SR);
  F2k = k; F2a1 = 1.0f / (1.0f + g * (g + k)); F2a2 = g * F2a1; F2a3 = g * F2a2;
}

// ============================================================
//  OSCILADORES
// ============================================================
#define TAB 1024
float SENO[TAB + 1];
static inline float seno(float fase) {
  int e = (int)fase; if (fase < 0) e--;
  fase -= e;
  float x = fase * TAB; int i = (int)x; if (i >= TAB) i = TAB - 1;
  float f = x - i;
  return SENO[i] + f * (SENO[i + 1] - SENO[i]);
}
static inline float blep(float t, float dt, float inv) {
  if (t < dt) { t *= inv; return t + t - t * t - 1.0f; }
  if (t > 1.0f - dt) { t = (t - 1.0f) * inv; return t * t + t + t + 1.0f; }
  return 0.0f;
}
static inline float sierra(float ph, float dt, float inv) { return 2.0f * ph - 1.0f - blep(ph, dt, inv); }

// ============================================================
//  VOCES
// ============================================================
struct Voz {
  bool activa; uint8_t pre, capa; int8_t nota; uint32_t orden; int32_t gate;
  float dt1, dt2, inv1, inv2, ph1, ph2, phSub;
  float pm1, pm2, dtm1, dtm2, ie1, ie2, kt;
  float env, envF; uint8_t et, etF;
  float ic1, ic2;
  float pL, pR;
  float lfoPh, vibPh;
  float f1a, f1b, f2a, f2b;
};
Voz voces[NVOCES];
void renderVoz(Voz &v, float *__restrict L, float *__restrict R, float multCorte, float multIndice, float multLfo);
uint32_t contadorOrden = 0;
volatile float cargaCPU = 0;
volatile int vocesSonando = 0;

void notaOn(int midi, uint8_t pre, uint8_t capa, float vel, float pan, int32_t gate) {
  const Preset &p = PRESETS[pre];
  int activas = 0; for (int i = 0; i < NVOCES; i++) activas += voces[i].activa;
  bool limitar = cargaCPU > 0.85f && activas >= 6;
  int elegida = -1;
  if (!limitar) for (int i = 0; i < NVOCES; i++) if (!voces[i].activa) { elegida = i; break; }
  if (elegida < 0) {                                   // la más baja en release
    float min = 2;
    for (int i = 0; i < NVOCES; i++) if (voces[i].et == 3 && voces[i].env < min) { min = voces[i].env; elegida = i; }
  }
  if (elegida < 0) {                                   // la más vieja de la melodía
    uint32_t viejo = UINT32_MAX;
    for (int i = 0; i < NVOCES; i++) if (voces[i].capa != 0 && voces[i].orden < viejo) { viejo = voces[i].orden; elegida = i; }
  }
  if (elegida < 0) elegida = 0;
  Voz &v = voces[elegida];
  v.ph1 = 0; v.ph2 = 0.37f; v.phSub = 0; v.pm1 = 0; v.pm2 = 0;
  v.ic1 = v.ic2 = 0; v.env = 0;
  v.lfoPh = (contadorOrden % 7) * 0.143f;
  v.vibPh = (contadorOrden % 5) * 0.2f;
  v.f1a = v.f1b = v.f2a = v.f2b = 0;
  float f = 440.0f * exp2f((midi - 69) / 12.0f);
  if (p.fm) {
    v.dt1 = f / SR; v.dtm1 = f * p.ratio1 / SR; v.dtm2 = f * p.ratio2 / SR;
    v.kt = powf(261.6f / f, 0.6f);
  } else {
    float d = exp2f(p.desafino / 24.0f);
    v.dt1 = f * d / SR; v.dt2 = f * p.ratioOsc2 / d / SR;
    v.kt = sqrtf(f / 261.6f);
  }
  v.inv1 = 1.0f / v.dt1; v.inv2 = v.dt2 > 0 ? 1.0f / v.dt2 : 0;
  v.ie1 = 1; v.ie2 = 1;
  v.envF = 0; v.et = 1; v.etF = 1;
  pan = fminf(fmaxf(pan, 0.1f), 0.9f);
  float nivelNota = f > 262.0f ? powf(262.0f / f, 0.55f) : 1.0f;   // agudos más suaves
  float g = nivelNota * vel * p.ganancia;
  v.pL = cosf(pan * PI / 2) * g; v.pR = sinf(pan * PI / 2) * g;
  v.pre = pre; v.capa = capa; v.gate = gate;
  v.nota = midi; v.activa = true; v.orden = contadorOrden++;
}

void soltarCapa(uint8_t capa) {
  for (int i = 0; i < NVOCES; i++)
    if (voces[i].activa && voces[i].capa == capa && voces[i].et != 3) { voces[i].et = 3; voces[i].etF = 3; }
}

static inline float pasoEnv(float &e, uint8_t &et, float aInc, float dC, float s, float rC) {
  if (et == 1)      { e += aInc; if (e >= 1) { e = 1; et = 2; } }
  else if (et == 2) { e += (s - e) * dC; }
  else if (et == 3) { e -= e * rC; }
  return e;
}

void renderVoz(Voz &v, float *__restrict L, float *__restrict R, float multCorte, float multIndice, float multLfo) {
  const Preset &p = PRESETS[v.pre];
  const Coef &C = CC[v.pre];
  const float aInc = C.aInc, dC = C.dC, sus = C.s, rC = C.rC;
  const float pL = v.pL, pR = v.pR;
  float env = v.env; uint8_t et = v.et;
  bool fin = false;

  if (!p.fm) {
    const float k = 2.0f - 2.0f * p.reso, mez2 = p.mezcla2, sub = p.sub, niv1 = p.nivel1;
    const float lfoProf = p.lfoProf * multLfo, lfoPaso = p.lfoHz * 8.0f / SR;
    float lfoPh = v.lfoPh, vibPh = v.vibPh;
    const bool formantes = p.form1 > 0, conOsc2 = mez2 > 0, conSub = sub > 0;
    const float vib = p.vibCents * 0.000578f, vibPaso = p.vibHz * 8.0f / SR;
    float f1a = v.f1a, f1b = v.f1b, f2a = v.f2a, f2b = v.f2b;
    const float faInc = C.faInc, fdC = C.fdC, fs = C.fs, frC = C.frC;
    const float dt1b = v.dt1, dt2b = v.dt2, inv1 = v.inv1, inv2 = v.inv2;
    float ph1 = v.ph1, ph2 = v.ph2, phSub = v.phSub, ic1 = v.ic1, ic2 = v.ic2, envF = v.envF;
    uint8_t etF = v.etF;
    for (int j = 0; j < BLOQUE && !fin; j += 8) {
      float vm = 1.0f;
      if (vib > 0) { vm += vib * seno(vibPh); vibPh += vibPaso; if (vibPh >= 1) vibPh -= 1; }
      const float dt1 = dt1b * vm, dt2 = dt2b * vm, dtSub = dt1 * 0.5f;
      float a1 = 0, a2 = 0, a3 = 0;
      if (!formantes) {
        float oct = p.envFiltro * envF;
        if (lfoProf > 0) { oct += lfoProf * seno(lfoPh); lfoPh += lfoPaso; if (lfoPh >= 1) lfoPh -= 1; }
        float fc = p.corte * multCorte * v.kt * exp2f(oct);
        fc = fminf(fmaxf(fc, 30.0f), 12000.0f);
        float g = tanf(PI * fc / SR);
        a1 = 1.0f / (1.0f + g * (g + k)); a2 = g * a1; a3 = g * a2;
      }
      for (int i = j; i < j + 8; i++) {
        pasoEnv(env, et, aInc, dC, sus, rC);
        if (env < 0.001f && et != 1) { fin = true; break; }
        pasoEnv(envF, etF, faInc, fdC, fs, frC);
        float o1 = sierra(ph1, dt1, inv1), o2 = conOsc2 ? sierra(ph2, dt2, inv2) : 0.0f;
        float x = (o1 * niv1 + o2 * mez2) * 0.5f + 1e-18f;
        if (conSub) x += seno(phSub) * sub;
        ph1 += dt1; if (ph1 >= 1) ph1 -= 1;
        ph2 += dt2; if (ph2 >= 1) ph2 -= 1;
        phSub += dtSub; if (phSub >= 1) phSub -= 1;
        float s;
        if (formantes) {
          float w3 = x - f1b, w1 = F1a1 * f1a + F1a2 * w3, w2 = f1b + F1a2 * f1a + F1a3 * w3;
          f1a = 2 * w1 - f1a; f1b = 2 * w2 - f1b;
          float u3 = x - f2b, u1 = F2a1 * f2a + F2a2 * u3, u2 = f2b + F2a2 * f2a + F2a3 * u3;
          f2a = 2 * u1 - f2a; f2b = 2 * u2 - f2b;
          s = (F1k * w1 + 0.7f * F2k * u1) * env;
        } else {
          float v3 = x - ic2;
          float v1 = a1 * ic1 + a2 * v3;
          float v2 = ic2 + a2 * ic1 + a3 * v3;
          ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2;
          s = v2 * env;
        }
        L[i] += s * pL; R[i] += s * pR;
      }
    }
    v.ph1 = ph1; v.ph2 = ph2; v.phSub = phSub; v.ic1 = ic1; v.ic2 = ic2; v.envF = envF; v.etF = etF;
    v.lfoPh = lfoPh; v.vibPh = vibPh;
    v.f1a = f1a; v.f1b = f1b; v.f2a = f2a; v.f2b = f2b;
  } else {
    const float i1f = p.ind1Fin, i1d = p.ind1Ini - p.ind1Fin, i2 = p.ind2;
    const float esc = v.kt * multIndice * (1.0f / (2.0f * PI));
    const float m1 = C.m1, m2 = C.m2, dt1 = v.dt1, dtm1 = v.dtm1, dtm2 = v.dtm2;
    float ph1 = v.ph1, pm1 = v.pm1, pm2 = v.pm2, ie1 = v.ie1, ie2 = v.ie2;
    const float vib = p.vibCents * 0.000578f, vibPaso = p.vibHz / SR;
    float vibPh = v.vibPh;
    for (int i = 0; i < BLOQUE; i++) {
      pasoEnv(env, et, aInc, dC, sus, rC);
      if (env < 0.001f && et != 1) { fin = true; break; }
      float vm = 1.0f;
      if (vib > 0) { vm += vib * seno(vibPh); vibPh += vibPaso; if (vibPh >= 1) vibPh -= 1; }
      float m = ((i1f + i1d * ie1) * seno(pm1) + i2 * ie2 * seno(pm2)) * esc;
      float s = seno(ph1 + m) * env;
      ie1 *= m1; ie2 *= m2;
      ph1 += dt1 * vm; if (ph1 >= 1) ph1 -= 1;
      pm1 += dtm1 * vm; if (pm1 >= 1) pm1 -= 1;
      pm2 += dtm2 * vm; if (pm2 >= 1) pm2 -= 1;
      L[i] += s * pL; R[i] += s * pR;
    }
    v.ph1 = ph1; v.pm1 = pm1; v.pm2 = pm2; v.ie1 = ie1; v.ie2 = ie2; v.vibPh = vibPh;
  }
  v.env = env; v.et = et;
  if (fin) { v.env = 0; v.et = 0; v.activa = false; }
}

// ============================================================
//  EFECTOS
// ============================================================
#define LCH 1024
float bufCh[LCH]; int posCh = 0;
float lfoS = 0, lfoC = 1;
const float LFO_W = 2 * PI * 0.30f / SR;
static inline float leerCh(float d) {
  float rp = posCh - d; if (rp < 0) rp += LCH;
  int i0 = (int)rp; float fr = rp - i0; int i1 = (i0 + 1) & (LCH - 1);
  return bufCh[i0] + fr * (bufCh[i1] - bufCh[i0]);
}
const int LARGO_REV[4] = {1117, 1361, 1559, 1801};
float lineaRev[4][1801]; int posRev[4]; float lpRev[4];
float ap1[225], ap2[556]; int pa1 = 0, pa2 = 0;
static inline float allpass(float x, float *buf, int len, int &pos) {
  float b = buf[pos]; float y = -0.6f * x + b;
  buf[pos] = x + 0.6f * y; if (++pos >= len) pos = 0;
  return y;
}
int16_t *bufEco = nullptr; int largoEco = 0; int posEco = 0; float lpEco = 0;
float limEnv = 0, limGan = 1, tonoL = 0, tonoR = 0, bajoL = 0, bajoR = 0;

// ============================================================
//  CONTROLES (los escribe la tarea de sensores y el radar, los lee el audio y los LED)
// ============================================================
static inline float lim01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
static inline float azar() { return (esp_random() >> 8) * (1.0f / 16777216.0f); }

struct Control {
  float luz = 0.5f, uv = 0, agua = 0.5f, temp = 20, aire = 0, humAmb = 0.5f;
  float presencia = 0, movimiento = 0, cercania = 0;
};
Control ctl;

#define RESPIRO_SEG 20            // sin cambios, una nota suave cada ~20 s (0 = silencio total)

// ============================================================
//  ESTÍMULOS: cada cambio del entorno es un estímulo, y cada estímulo suena
// ============================================================
enum Fuente { F_PERSONA, F_LLEGA, F_SEVA, F_LUZ, F_OSCURECE, F_PLANTA1, F_PLANTA2, F_TEMP, F_AIRE, F_UV,
              F_PRESION, F_HUMEDAD, F_RESPIRO, N_FUENTES };
const char *NOMBRE_FUENTE[N_FUENTES] = {"PERSONA", "LLEGA ALGUIEN", "SE VA", "MAS LUZ", "MENOS LUZ", "PLANTA 1",
                                        "PLANTA 2", "TEMPERATURA", "AIRE", "UV", "PRESION", "HUMEDAD", "respiro"};
// color de cada fuente en los anillos (RGB saturado, nunca blanco)
const uint8_t COLOR_FUENTE[N_FUENTES][3] = {
  {255, 70, 0},    // persona: naranja
  {255, 20, 0},    // llega: rojo anaranjado
  {130, 0, 255},   // se va: violeta
  {255, 160, 0},   // más luz: ámbar
  {0, 40, 255},    // menos luz: azul
  {0, 255, 30},    // planta 1: verde
  {90, 255, 0},    // planta 2: verde limón
  {255, 0, 100},   // temperatura: fucsia
  {0, 230, 190},   // aire: turquesa
  {180, 0, 255},   // UV: violeta
  {50, 0, 255},    // presión: índigo
  {0, 150, 255},   // humedad: celeste
  {0, 70, 255},    // respiro: azul
};

struct Estimulo { uint8_t fuente; int8_t dir; float valor, fuerza; };
bool sacarEstimulo(Estimulo &e);          // prototipos a mano: los automáticos del IDE quedan antes del struct
void interpretar(const Estimulo &e);
#define N_COLA_EST 16
Estimulo colaEst[N_COLA_EST]; volatile uint8_t estIn = 0, estOut = 0;
portMUX_TYPE muxEst = portMUX_INITIALIZER_UNLOCKED;
uint32_t ultimoPorFuente[N_FUENTES];
char ultimoTexto[28] = "esperando cambios";

// Lo llaman la tarea de sensores y el loop (radar). minMs evita que una fuente se repita muy seguido.
bool estimular(uint8_t f, int dir, float valor, float fuerza, uint32_t minMs, const char *detalle) {
  uint32_t ahora = millis();
  if (ultimoPorFuente[f] && ahora - ultimoPorFuente[f] < minMs) return false;
  ultimoPorFuente[f] = ahora;
  portENTER_CRITICAL(&muxEst);
  uint8_t n = (estIn + 1) % N_COLA_EST;
  if (n == estOut) estOut = (estOut + 1) % N_COLA_EST;   // cola llena: se pierde el más viejo
  colaEst[estIn] = {f, (int8_t)dir, valor, fuerza}; estIn = n;
  portEXIT_CRITICAL(&muxEst);
  snprintf(ultimoTexto, sizeof(ultimoTexto), "%s %s", NOMBRE_FUENTE[f], detalle);
  Serial.printf(">> %s %s\n", NOMBRE_FUENTE[f], detalle);
  return true;
}
bool sacarEstimulo(Estimulo &e) {
  bool hay = false;
  portENTER_CRITICAL(&muxEst);
  if (estOut != estIn) { e = colaEst[estOut]; estOut = (estOut + 1) % N_COLA_EST; hay = true; }
  portEXIT_CRITICAL(&muxEst);
  return hay;
}

// ---- cola de notas para los LED (audio → loop) ----
struct EventoLed { uint8_t fuente; int8_t grado; float vel; };
EventoLed colaLed[32]; volatile uint8_t colaIn = 0, colaOut = 0;
static inline void avisarLed(uint8_t fuente, int grado, float vel) {
  uint8_t n = (colaIn + 1) & 31;
  if (n != colaOut) { colaLed[colaIn] = {fuente, (int8_t)grado, vel}; colaIn = n; }
}

// ============================================================
//  INTÉRPRETE (corre dentro de la tarea de audio, a tiempo de muestra)
//  Cada estímulo se convierte en 1 a 4 notas agendadas sobre una grilla de semicorcheas.
// ============================================================
struct NotaPend { bool usada; uint32_t tick; int8_t grado; uint8_t pre, fuente; float vel, pan; int8_t largo; };
#define N_PEND 24
NotaPend pend[N_PEND];

struct Musica {
  Control c;                                   // copia suavizada de los controles
  float bpm = 70;
  int32_t faltaPaso = 0;
  uint32_t tick = 0, ultimaNotaTick = 0;
  float respiroEn = RESPIRO_SEG;
  // efectos
  float reverb = 0.5f, cola = 0.88f, apagado = 0.4f, chorus = 0.4f, eco = 0.2f, multLfo = 1;
  float multCorte = 1, multIndice = 1;
  bool ecoNegra = false;
} M;

int32_t muestrasPorPaso() { return (int32_t)(SR * 60.0f / (M.bpm * 4.0f)); }

void agendar(int offset, int grado, uint8_t pre, uint8_t fuente, float vel, float pan, int largo) {
  for (int i = 0; i < N_PEND; i++)
    if (!pend[i].usada) {
      pend[i] = {true, M.tick + offset, (int8_t)constrain(grado, 0, 26), pre, fuente, lim01(vel), pan, (int8_t)largo};
      return;
    }
}

// distancia del radar (cm) → grado: lejos (6 m) = Mi3, muy cerca = Do#5
int gradoDistancia(float cm) { return 6 + (int)lroundf(lim01(1.0f - cm / 600.0f) * 14); }

// ---- figuras: cada estímulo elige una al azar (sin repetir la anterior de esa fuente),
//      la invierte si el cambio es hacia abajo y a veces la toca al revés o más lenta ----
struct Figura { int8_t n; int8_t paso[4]; int8_t t[4]; };
const Figura FIGURAS[] = {
  {1, {0},           {0}},            //  0 una nota
  {2, {0, 1},        {0, 2}},         //  1 paso
  {2, {0, 2},        {0, 1}},         //  2 salto corto
  {2, {-1, 0},       {0, 1}},         //  3 nota de adorno
  {2, {1, 0},        {0, 2}},         //  4 apoyatura
  {2, {0, 0},        {0, 3}},         //  5 repetida
  {2, {0, 2},        {0, 3}},         //  6 con puntillo
  {2, {0, 6},        {0, 4}},         //  7 eco a la octava
  {3, {0, 1, 2},     {0, 2, 4}},      //  8 escala
  {3, {0, 2, 4},     {0, 1, 2}},      //  9 arpegio rápido
  {3, {0, 3, 2},     {0, 2, 3}},      // 10 salto y vuelta
  {3, {2, 1, 0},     {0, 2, 4}},      // 11 cae
  {3, {0, -1, 1},    {0, 2, 3}},      // 12 bordado
  {4, {0, 1, 0, -1}, {0, 1, 2, 3}},   // 13 giro
  {4, {0, 2, 1, 3},  {0, 2, 3, 5}},   // 14 zigzag
  {4, {0, 1, 2, 4},  {0, 1, 2, 4}},   // 15 sube y salta
};
const int N_FIGURAS = sizeof(FIGURAS) / sizeof(FIGURAS[0]);
int8_t ultimaFigura[N_FUENTES];
int ultimoGrado[N_FUENTES];
int deriva = 0;                       // centro tonal que se mueve solo cada 1–2 min
uint32_t proximaDeriva = 0;

// nMin..nMax = cuántas notas puede tener la figura
void tocarFigura(int gBase, int dir, uint8_t pre, uint8_t fuente, float vel, float pan, int nMin, int nMax, int largo) {
  int cand[N_FIGURAS], nc = 0;
  for (int i = 0; i < N_FIGURAS; i++)
    if (FIGURAS[i].n >= nMin && FIGURAS[i].n <= nMax && i != ultimaFigura[fuente]) cand[nc++] = i;
  if (!nc) cand[nc++] = 0;
  int fi = cand[esp_random() % nc];
  ultimaFigura[fuente] = fi;
  const Figura &F = FIGURAS[fi];
  bool alReves = F.n > 2 && azar() < 0.25f;
  int estirar = azar() < 0.3f ? 2 : 1;                     // a veces más lenta
  int s = dir < 0 ? -1 : 1;
  if (dir == 0 && azar() < 0.5f) s = -1;
  for (int k = 0; k < F.n; k++) {
    int j = alReves ? F.n - 1 - k : k;
    int g = gBase + s * F.paso[j];
    float v = vel * (k == 0 ? 1.0f : 0.75f + 0.25f * azar());
    int l = largo + (int)(azar() * largo * 0.6f) - largo / 4;
    agendar(F.t[k] * estirar, g, pre, fuente, v, pan + (azar() - 0.5f) * 0.15f, l < 2 ? 2 : l);
  }
  ultimoGrado[fuente] = gBase;
}

// a veces la fuente cambia de timbre, para que no suene siempre igual
uint8_t timbre(uint8_t principal) {
  float r = azar();
  if (r < 0.75f) return principal;
  const uint8_t OTROS[3] = {P_GOTAS, P_EPIANO, P_FLAUTA};
  uint8_t o = OTROS[esp_random() % 3];
  return o == principal ? principal : o;
}

// acerca el grado nuevo al anterior de la misma fuente (máx. 3 pasos): línea melódica continua
int seguir(uint8_t fuente, int objetivo) {
  int prev = ultimoGrado[fuente];
  if (prev <= 0) return objetivo;
  int d = objetivo - prev;
  if (d > 3) d = 3 + (int)(azar() * 2); else if (d < -3) d = -3 - (int)(azar() * 2);
  else d += (int)(azar() * 3) - 1;                          // un poco de libertad
  return prev + d;
}

void interpretar(const Estimulo &e) {
  const float f = e.fuerza;
  // el centro tonal se mueve solo (Mi → La → Do# → Fa# …), así las mismas distancias no dan las mismas notas
  if (millis() > proximaDeriva) {
    static const int8_t CENTROS[5] = {0, 3, 5, 1, 4};
    deriva = CENTROS[esp_random() % 5];
    proximaDeriva = millis() + 60000 + esp_random() % 60000;
  }
  switch (e.fuente) {
    case F_PERSONA: {
      int g = seguir(F_PERSONA, gradoDistancia(e.valor) + deriva / 2);
      int nMax = f > 0.6f ? 4 : (f > 0.25f ? 3 : 2);
      tocarFigura(g, e.dir, timbre(P_EPIANO), F_PERSONA, 0.45f + 0.4f * f, 0.5f + 0.15f * e.dir, 1, nMax, 6);
      break;
    }
    case F_LLEGA: {
      int g = gradoDistancia(e.valor) - 2 + deriva;
      tocarFigura(g, 1, timbre(P_GOTAS), F_LLEGA, 0.55f, 0.35f, 3, 4, 8);
      break;
    }
    case F_SEVA: {
      int g = ultimoGrado[F_PERSONA] > 0 ? ultimoGrado[F_PERSONA] : 12;
      tocarFigura(g, -1, timbre(P_EPIANO), F_SEVA, 0.42f, 0.65f, 2, 3, 10);
      break;
    }
    case F_LUZ: case F_OSCURECE: {
      int g = seguir(e.fuente, 8 + (int)lroundf(e.valor * 12) + deriva);
      int dir = e.fuente == F_LUZ ? 1 : -1;
      tocarFigura(g, dir, timbre(P_GOTAS), e.fuente, 0.4f + 0.3f * f, 0.5f, f > 0.5f ? 3 : 1, f > 0.5f ? 4 : 2, 8);
      break;
    }
    case F_PLANTA1: case F_PLANTA2: {
      float pan = e.fuente == F_PLANTA1 ? 0.2f : 0.8f;
      int n = e.dir > 0 ? 2 + (int)(f * 3) : 1;          // más humedad (regar) = varias gotas
      int g = 18 + (int)(azar() * 5);
      for (int k = 0; k < n; k++) {
        agendar(k == 0 ? 0 : k + (int)(azar() * 3), g, P_GOTAS, e.fuente, 0.3f + 0.25f * f * azar() + 0.1f, pan, 6);
        g -= 1 + (int)(azar() * 3);
      }
      break;
    }
    case F_TEMP:    tocarFigura(3 + (int)lroundf(e.valor * 9) + deriva / 2, e.dir, P_FLAUTA, F_TEMP, 0.5f, 0.5f, 1, 2, 12); break;
    case F_AIRE:    tocarFigura(seguir(F_AIRE, 9 + (int)lroundf(e.valor * 8) + deriva), e.dir, timbre(P_FLAUTA), F_AIRE, 0.45f, 0.35f + 0.3f * azar(), 1, 3, 10); break;
    case F_UV:      tocarFigura(17 + (int)lroundf(e.valor * 6), 1, P_GOTAS, F_UV, 0.33f, 0.7f, 2, 3, 6); break;
    case F_PRESION: agendar(0, (e.dir > 0 ? 4 : 0) + (deriva % 3), P_FLAUTA, F_PRESION, 0.55f, 0.5f, 16); break;
    case F_HUMEDAD: tocarFigura(7 + (int)lroundf(e.valor * 8) + deriva, e.dir, timbre(P_EPIANO), F_HUMEDAD, 0.4f, 0.6f, 1, 2, 8); break;
    case F_RESPIRO: tocarFigura(6 + (int)(azar() * 6) + deriva, 0, P_FLAUTA, F_RESPIRO, 0.3f, 0.4f + 0.2f * azar(), 1, 2, 16); break;
  }
}

// cada negra: el entorno ajusta el carácter del sonido (no dispara notas)
void actualizarAmbiente() {
  Control n = ctl;
  Control &c = M.c;
  const float k = 0.3f;
  c.luz += (n.luz - c.luz) * 0.6f; c.uv += (n.uv - c.uv) * k; c.agua += (n.agua - c.agua) * k;
  c.temp += (n.temp - c.temp) * k; c.aire += (n.aire - c.aire) * k; c.humAmb += (n.humAmb - c.humAmb) * k;
  c.presencia += (n.presencia - c.presencia) * 0.6f; c.movimiento += (n.movimiento - c.movimiento) * 0.6f;
  c.cercania += (n.cercania - c.cercania) * 0.6f;
  float pc = c.presencia * c.cercania;
  M.bpm = fminf(fmaxf(66 + (c.temp - 18) * 0.5f + 8 * c.presencia, 56), 84);
  M.reverb = 0.35f + 0.30f * c.agua + 0.20f * (1 - c.luz);    // oscuro y húmedo = más reverb
  M.cola = fminf(0.85f + 0.04f * c.agua + 0.04f * (1 - c.luz), 0.93f);
  M.apagado = 0.45f - 0.20f * c.uv;
  M.chorus = 0.25f + 0.50f * c.aire;
  M.eco = 0.15f + 0.20f * c.agua;
  M.ecoNegra = c.humAmb > 0.6f;
  M.multLfo = 0.6f + 1.4f * c.aire;
  M.multCorte = exp2f(-0.8f + 1.2f * c.luz + 0.5f * pc);       // con luz y alguien cerca, más brillante
  M.multIndice = exp2f(-0.6f + 0.8f * c.luz + 0.4f * pc);
}

void pasoMusica() {
  if ((M.tick & 3) == 0) actualizarAmbiente();
  // un estímulo nuevo por corchea como máximo, para que cada cambio se oiga por separado
  if ((M.tick & 1) == 0) { Estimulo e; if (sacarEstimulo(e)) interpretar(e); }
  for (int i = 0; i < N_PEND; i++) {
    NotaPend &p = pend[i];
    if (p.usada && (int32_t)(M.tick - p.tick) >= 0) {
      notaOn(gradoAMidi(p.grado), p.pre, 1, p.vel, p.pan, p.largo * muestrasPorPaso());
      avisarLed(p.fuente, p.grado, p.vel);
      p.usada = false;
      M.ultimaNotaTick = M.tick;
    }
  }
  // sin cambios por un rato: un respiro suave para mostrar que sigue vivo
  if (RESPIRO_SEG > 0 && (M.tick - M.ultimaNotaTick) * (float)muestrasPorPaso() > M.respiroEn * SR) {
    interpretar({F_RESPIRO, 0, 0, 0});
    M.ultimaNotaTick = M.tick;
    M.respiroEn = RESPIRO_SEG * (0.7f + 0.6f * azar());
  }
  M.tick++;
}

// ============================================================
//  TAREA DE AUDIO (núcleo 0)
// ============================================================
I2SClass i2s;

void tareaAudio(void *) {
  static float L[BLOQUE], R[BLOQUE];
  static int16_t salida[BLOQUE * 2];
  const float ciclosBloque = (float)BLOQUE / SR * getCpuFrequencyMhz() * 1e6f;
  static const float COEF_TONO = 1.0f - expf(-2.0f * PI * TONO_HZ / SR);
  static const float COEF_BAJO = 1.0f - expf(-2.0f * PI * 180.0f / SR);
  // efectos suavizados por muestra-bloque para que no salten
  float rev = M.reverb, cola = M.cola, apag = M.apagado, cho = M.chorus, eco = M.eco;

  for (;;) {
    uint32_t t0 = ESP.getCycleCount();

    M.faltaPaso -= BLOQUE;
    while (M.faltaPaso <= 0) { pasoMusica(); M.faltaPaso += muestrasPorPaso(); }

    for (int i = 0; i < NVOCES; i++) {
      Voz &v = voces[i];
      if (v.activa && v.et != 3) { v.gate -= BLOQUE; if (v.gate <= 0) { v.et = 3; v.etF = 3; } }
    }

    const float s = 0.002f;
    rev += (M.reverb - rev) * s; cola += (M.cola - cola) * s; apag += (M.apagado - apag) * s;
    cho += (M.chorus - cho) * s; eco += (M.eco - eco) * s;

    memset(L, 0, sizeof(L)); memset(R, 0, sizeof(R));
    calcFormantes(exp2f((M.c.luz - 0.5f) * 0.5f));
    int sonando = 0;
    for (int i = 0; i < NVOCES; i++) if (voces[i].activa) { renderVoz(voces[i], L, R, M.multCorte, M.multIndice, M.multLfo); sonando++; }
    vocesSonando = sonando;

    const float gan = 0.32f;
    const float normCh = 1.0f / (1.0f + 0.5f * cho);
    int T = M.ecoNegra ? (int)(SR * 60 / M.bpm) : (int)(SR * 45 / M.bpm);
    if (2 * T >= largoEco) T = (largoEco - 1) / 2;
    for (int i = 0; i < BLOQUE; i++) {
      float l = L[i] * gan, r = R[i] * gan;

      bufCh[posCh] = (l + r) * 0.5f;
      float ns = lfoS + LFO_W * lfoC; lfoC -= LFO_W * ns; lfoS = ns;
      float tl = leerCh(280 + 90 * lfoS), tr = leerCh(280 + 90 * lfoC);
      posCh = (posCh + 1) & (LCH - 1);
      l = (l + cho * tl) * normCh; r = (r + cho * tr) * normCh;

      if (bufEco) {
        int iL = posEco - T; if (iL < 0) iL += largoEco;
        int iR = posEco - 2 * T; if (iR < 0) iR += largoEco;
        float tapL = bufEco[iL] * (1.0f / 16384), tapR = bufEco[iR] * (1.0f / 16384);
        lpEco += (tapR - lpEco) * 0.30f;
        float y = (l + r) * 0.5f * eco + 0.5f * lpEco;
        y = fminf(fmaxf(y, -1.99f), 1.99f);
        bufEco[posEco] = (int16_t)(y * 16384);
        if (++posEco >= largoEco) posEco = 0;
        l += tapL; r += tapR;
      }

      float x = allpass(allpass((l + r) * 0.5f + 1e-18f, ap1, 225, pa1), ap2, 556, pa2);
      float o[4];
      for (int q = 0; q < 4; q++) { o[q] = lineaRev[q][posRev[q]]; lpRev[q] += (o[q] - lpRev[q]) * (1.0f - apag); }
      float a = lpRev[0], b = lpRev[1], c = lpRev[2], d = lpRev[3];
      float h[4] = {(a + b + c + d) * 0.5f, (a - b + c - d) * 0.5f, (a + b - c - d) * 0.5f, (a - b - c + d) * 0.5f};
      for (int q = 0; q < 4; q++) { lineaRev[q][posRev[q]] = x + cola * h[q]; if (++posRev[q] >= LARGO_REV[q]) posRev[q] = 0; }
      l += rev * 0.6f * (o[0] + o[2]);
      r += rev * 0.6f * (o[1] + o[3]);

      bajoL += (l - bajoL) * COEF_BAJO; bajoR += (r - bajoR) * COEF_BAJO;
      l += GRAVES * bajoL; r += GRAVES * bajoR;

      float pico = fmaxf(fabsf(l), fabsf(r));
      if (pico > limEnv) limEnv = pico; else limEnv += (pico - limEnv) * 0.0001f;
      float objetivo = limEnv > UMBRAL_LIM ? UMBRAL_LIM / limEnv : 1.0f;
      limGan += (objetivo - limGan) * (objetivo < limGan ? 0.04f : 0.0002f);
      l *= limGan; r *= limGan;

      l = fminf(fmaxf(l, -1.5f), 1.5f); r = fminf(fmaxf(r, -1.5f), 1.5f);
      l = l - 0.148148f * l * l * l;
      r = r - 0.148148f * r * r * r;
      tonoL += (l - tonoL) * COEF_TONO; tonoR += (r - tonoR) * COEF_TONO;
      salida[2 * i]     = (int16_t)(tonoL * 30000 * VOLUMEN);
      salida[2 * i + 1] = (int16_t)(tonoR * 30000 * VOLUMEN);
    }

    cargaCPU = (ESP.getCycleCount() - t0) / ciclosBloque;
    i2s.write((uint8_t *)salida, sizeof(salida));
  }
}

// ============================================================
//  SENSORES I2C
// ============================================================
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
bool hayOled, hayBmp, hayVeml, hayEns, hayAht, haySht1, haySht2;
uint8_t ensAddr = 0x53;

bool existe(TwoWire &w, uint8_t a) { w.beginTransmission(a); return w.endTransmission() == 0; }
bool leerReg(TwoWire &w, uint8_t a, uint8_t reg, uint8_t *buf, uint8_t n) {
  w.beginTransmission(a); w.write(reg);
  if (w.endTransmission(false) != 0) return false;
  if (w.requestFrom(a, n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = w.read();
  return true;
}
bool escribirReg(TwoWire &w, uint8_t a, uint8_t reg, const uint8_t *buf, uint8_t n) {
  w.beginTransmission(a); w.write(reg);
  for (uint8_t i = 0; i < n; i++) w.write(buf[i]);
  return w.endTransmission() == 0;
}
bool leerSHT40(TwoWire &w, float &t, float &h) {
  w.beginTransmission(0x44); w.write(0xFD);
  if (w.endTransmission() != 0) return false;
  delay(10);
  if (w.requestFrom((uint8_t)0x44, (uint8_t)6) != 6) return false;
  uint8_t b[6]; for (int i = 0; i < 6; i++) b[i] = w.read();
  t = -45 + 175.0f * ((b[0] << 8) | b[1]) / 65535.0f;
  h = constrain(-6 + 125.0f * ((b[3] << 8) | b[4]) / 65535.0f, 0, 100);
  return true;
}
bool leerAHT21(float &t, float &h) {
  Wire.beginTransmission(0x38); Wire.write(0xAC); Wire.write(0x33); Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;
  delay(85);
  if (Wire.requestFrom((uint8_t)0x38, (uint8_t)6) != 6) return false;
  uint8_t b[6]; for (int i = 0; i < 6; i++) b[i] = Wire.read();
  if (b[0] & 0x80) return false;
  uint32_t rh = ((uint32_t)b[1] << 12) | ((uint32_t)b[2] << 4) | (b[3] >> 4);
  uint32_t rt = ((uint32_t)(b[3] & 0x0F) << 16) | ((uint32_t)b[4] << 8) | b[5];
  h = rh * 100.0f / 1048576.0f; t = rt * 200.0f / 1048576.0f - 50;
  return true;
}
bool iniciarENS160() {
  for (uint8_t a : {0x53, 0x52}) {
    uint8_t id[2];
    if (leerReg(Wire, a, 0x00, id, 2) && id[0] == 0x60 && id[1] == 0x01) {
      ensAddr = a; uint8_t modo = 0x02; escribirReg(Wire, a, 0x10, &modo, 1); return true;
    }
  }
  return false;
}
bool leerENS160(uint16_t &tvoc, uint16_t &eco2) {
  uint8_t b[5];
  if (!leerReg(Wire, ensAddr, 0x21, b, 5)) return false;
  tvoc = b[1] | (b[2] << 8); eco2 = b[3] | (b[4] << 8);
  return true;
}
bool iniciarVEML7700() { uint8_t conf[2] = {0x00, 0x00}; return escribirReg(Wire, 0x10, 0x00, conf, 2); }
bool leerVEML7700(float &lux) {
  uint8_t b[2];
  if (!leerReg(Wire, 0x10, 0x04, b, 2)) return false;
  lux = (b[0] | (b[1] << 8)) * 0.0576f; return true;
}
int16_t AC1, AC2, AC3, B1c, B2c, MBc, MCc, MDc; uint16_t AC4, AC5, AC6;
bool iniciarBMP180() {
  uint8_t id;
  if (!leerReg(Wire, 0x77, 0xD0, &id, 1) || id != 0x55) return false;
  uint8_t c[22];
  if (!leerReg(Wire, 0x77, 0xAA, c, 22)) return false;
  auto s = [&](int i) { return (int16_t)((c[i] << 8) | c[i + 1]); };
  auto u = [&](int i) { return (uint16_t)((c[i] << 8) | c[i + 1]); };
  AC1 = s(0); AC2 = s(2); AC3 = s(4); AC4 = u(6); AC5 = u(8); AC6 = u(10);
  B1c = s(12); B2c = s(14); MBc = s(16); MCc = s(18); MDc = s(20);
  return true;
}
bool leerBMP180(float &t, float &hpa) {
  uint8_t cmd = 0x2E, b[3];
  if (!escribirReg(Wire, 0x77, 0xF4, &cmd, 1)) return false;
  delay(5);
  if (!leerReg(Wire, 0x77, 0xF6, b, 2)) return false;
  int32_t UT = (b[0] << 8) | b[1];
  cmd = 0x34;
  if (!escribirReg(Wire, 0x77, 0xF4, &cmd, 1)) return false;
  delay(5);
  if (!leerReg(Wire, 0x77, 0xF6, b, 3)) return false;
  int32_t UP = (((int32_t)b[0] << 16) | ((int32_t)b[1] << 8) | b[2]) >> 8;
  int32_t X1 = ((UT - (int32_t)AC6) * (int32_t)AC5) >> 15;
  int32_t X2 = ((int32_t)MCc << 11) / (X1 + MDc);
  int32_t B5 = X1 + X2;
  t = ((B5 + 8) >> 4) / 10.0f;
  int32_t B6 = B5 - 4000;
  X1 = (B2c * ((B6 * B6) >> 12)) >> 11; X2 = (AC2 * B6) >> 11;
  int32_t X3 = X1 + X2;
  int32_t B3 = (((int32_t)AC1 * 4 + X3) + 2) / 4;
  X1 = (AC3 * B6) >> 13; X2 = (B1c * ((B6 * B6) >> 12)) >> 16; X3 = ((X1 + X2) + 2) >> 2;
  uint32_t B4 = ((uint32_t)AC4 * (uint32_t)(X3 + 32768)) >> 15;
  uint32_t B7 = ((uint32_t)UP - B3) * 50000UL;
  int32_t p = (B7 < 0x80000000UL) ? (B7 * 2) / B4 : (B7 / B4) * 2;
  X1 = (p >> 8) * (p >> 8); X1 = (X1 * 3038) >> 16; X2 = (-7357 * p) >> 16;
  p += (X1 + X2 + 3791) >> 4;
  hpa = p / 100.0f; return true;
}

// lecturas crudas, para la OLED y el serie
struct Lecturas {
  float t1 = NAN, h1 = NAN, t2 = NAN, h2 = NAN, ta = NAN, ha = NAN, tb = NAN, hpa = NAN, lux = NAN, uvi = 0;
  uint16_t tvoc = 0, eco2 = 0;
} lec;

// ============================================================
//  RADAR HLK-LD2410C (UART 256000)
// ============================================================
struct Radar { bool datos = false; uint8_t estado = 0; uint16_t distMov = 0, distEst = 0, distDet = 0; uint8_t enMov = 0, enEst = 0; uint32_t tUlt = 0; } radar;
int radarRx = RADAR_A, radarTx = RADAR_B;

void leerRadar() {
  static uint8_t buf[72]; static int n = 0; static int largo = 0;
  static const uint8_t CAB[4] = {0xF4, 0xF3, 0xF2, 0xF1};
  while (Serial1.available()) {
    uint8_t b = Serial1.read();
    if (n < 4) { if (b == CAB[n]) buf[n++] = b; else n = (b == CAB[0]) ? 1 : 0; continue; }
    buf[n++] = b;
    if (n == 6) { largo = buf[4] | (buf[5] << 8); if (largo < 11 || largo > 60) { n = 0; continue; } }
    if (n > 6 && n == 6 + largo + 4) {
      const uint8_t *d = buf + 6;
      if ((d[0] == 0x02 || d[0] == 0x01) && d[1] == 0xAA) {
        radar.estado = d[2];
        radar.distMov = d[3] | (d[4] << 8); radar.enMov = d[5];
        radar.distEst = d[6] | (d[7] << 8); radar.enEst = d[8];
        radar.distDet = d[9] | (d[10] << 8);
        radar.datos = true; radar.tUlt = millis();
      }
      n = 0;
    }
    if (n >= (int)sizeof(buf)) n = 0;
  }
}

void iniciarRadar() {
  pinMode(PIN_RADAR_OUT, INPUT);
  for (int intento = 0; intento < 2 && !radar.datos; intento++) {
    radarRx = intento == 0 ? RADAR_A : RADAR_B; radarTx = intento == 0 ? RADAR_B : RADAR_A;
    Serial1.begin(256000, SERIAL_8N1, radarRx, radarTx);
    uint32_t t0 = millis();
    while (millis() - t0 < 1500 && !radar.datos) { leerRadar(); delay(5); }
    if (!radar.datos) Serial1.end();
  }
  if (radar.datos) Serial.printf("Radar: datos por UART (RX GPIO%d, TX GPIO%d)\n", radarRx, radarTx);
  else Serial.println("Radar: sin datos por UART; uso solo la salida OUT (GPIO10)");
}

// ============================================================
//  TAREA DE SENSORES (núcleo 1): lee, detecta cambios y dibuja la OLED
// ============================================================
void dibujarOled() {
  if (!hayOled) return;
  oled.clearDisplay(); oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1); oled.setCursor(0, 0);
  oled.println(ultimoTexto);                           // el último cambio que sonó
  oled.drawFastHLine(0, 10, 128, SSD1306_WHITE);
  oled.setCursor(0, 14);
  oled.printf("Luz %.0f lx  UV %.1f\n", isnan(lec.lux) ? 0.0f : lec.lux, lec.uvi);
  oled.printf("Planta %.1f%% %.1f%%\n", isnan(lec.h1) ? 0.0f : lec.h1, isnan(lec.h2) ? 0.0f : lec.h2);
  oled.printf("%.1fC %.1fhPa\n", ctl.temp, isnan(lec.hpa) ? 0.0f : lec.hpa);
  oled.printf("CO2 %u  hum %.0f%%\n", lec.eco2, isnan(lec.ha) ? 0.0f : lec.ha);
  if (ctl.presencia > 0.5f) oled.printf("Persona a %d cm", radar.distDet);
  else oled.print("Radar: nadie");
  oled.display();
}

// avisa +1 / -1 cuando el valor se movió más que el umbral desde el último aviso
struct Detector { float ref = NAN; };
int detectar(Detector &d, float v, float umbral, float &cambio);
int detectar(Detector &d, float v, float umbral, float &cambio) {
  if (isnan(v)) return 0;
  if (isnan(d.ref)) { d.ref = v; return 0; }
  cambio = v - d.ref;
  if (fabsf(cambio) < umbral) return 0;
  d.ref = v;
  return cambio > 0 ? 1 : -1;
}
Detector dLuz, dPl1, dPl2, dTemp, dAire, dUv, dPres, dHum;

void tareaSensores(void *) {
  char txt[24];
  float cambio;
  for (;;) {
    uint32_t t0 = millis();
    Control n = ctl;

    if (haySht1) leerSHT40(Wire, lec.t1, lec.h1);
    if (haySht2) leerSHT40(Wire1, lec.t2, lec.h2);
    if (hayAht) leerAHT21(lec.ta, lec.ha);
    if (hayBmp) leerBMP180(lec.tb, lec.hpa);
    if (hayVeml) leerVEML7700(lec.lux);
    if (hayEns) leerENS160(lec.tvoc, lec.eco2);
    uint32_t mv = 0; for (int i = 0; i < 16; i++) mv += analogReadMilliVolts(PIN_GUVA);
    lec.uvi = (mv / 16.0f) / 100.0f;                 // GUVA-S12SD: ~0,1 V por índice UV

    // valores normalizados (para el carácter del sonido y el color de ambiente)
    const float k = 0.3f;
    float luzLog = isnan(lec.lux) ? NAN : log10f(lec.lux + 1);
    if (!isnan(luzLog)) n.luz += (lim01(luzLog / 3.5f) - n.luz) * 0.8f;
    n.uv += (lim01(lec.uvi / 6.0f) - n.uv) * k;
    float hp = 0; int nh = 0;
    if (haySht1 && !isnan(lec.h1)) { hp += lec.h1; nh++; }
    if (haySht2 && !isnan(lec.h2)) { hp += lec.h2; nh++; }
    if (nh) n.agua += (lim01((hp / nh - 30) / 60) - n.agua) * k;
    float ts = 0; int nt = 0;
    for (float t : {lec.t1, lec.t2, lec.ta, lec.tb}) if (!isnan(t)) { ts += t; nt++; }
    if (nt) n.temp += (ts / nt - n.temp) * k;
    if (hayEns) n.aire += (0.7f * lim01((lec.eco2 - 400) / 1000.0f) + 0.3f * lim01(lec.tvoc / 500.0f) - n.aire) * k;
    if (!isnan(lec.ha)) n.humAmb += (lec.ha / 100 - n.humAmb) * k;
    ctl.luz = n.luz; ctl.uv = n.uv; ctl.agua = n.agua; ctl.temp = n.temp; ctl.aire = n.aire; ctl.humAmb = n.humAmb;

    // ---- cambios = estímulos ----   (umbral, y mínimo de tiempo entre avisos de la misma fuente)
    int d;
    if ((d = detectar(dLuz, luzLog, 0.05f, cambio))) {            // ~12 % más o menos luz
      snprintf(txt, sizeof(txt), "%.0f lx", lec.lux);
      estimular(d > 0 ? F_LUZ : F_OSCURECE, d, n.luz, lim01(fabsf(cambio) / 0.5f), 1200, txt);
    }
    if ((d = detectar(dPl1, lec.h1, 1.0f, cambio))) {
      snprintf(txt, sizeof(txt), "%.1f%%", lec.h1);
      estimular(F_PLANTA1, d, lim01((lec.h1 - 30) / 60), lim01(fabsf(cambio) / 5), 1500, txt);
    }
    if ((d = detectar(dPl2, lec.h2, 1.0f, cambio))) {
      snprintf(txt, sizeof(txt), "%.1f%%", lec.h2);
      estimular(F_PLANTA2, d, lim01((lec.h2 - 30) / 60), lim01(fabsf(cambio) / 5), 1500, txt);
    }
    if ((d = detectar(dTemp, nt ? ts / nt : NAN, 0.3f, cambio))) {
      snprintf(txt, sizeof(txt), "%.1f C", ts / nt);
      estimular(F_TEMP, d, lim01((ts / nt - 5) / 30), 0.5f, 3000, txt);
    }
    if (hayEns && (d = detectar(dAire, (float)lec.eco2, 30, cambio))) {
      snprintf(txt, sizeof(txt), "CO2 %u", lec.eco2);
      estimular(F_AIRE, d, n.aire, lim01(fabsf(cambio) / 200), 3000, txt);
    }
    if ((d = detectar(dUv, lec.uvi, 0.3f, cambio))) {
      snprintf(txt, sizeof(txt), "%.1f", lec.uvi);
      estimular(F_UV, d, n.uv, 0.5f, 2000, txt);
    }
    if ((d = detectar(dPres, lec.hpa, 0.3f, cambio))) {
      snprintf(txt, sizeof(txt), "%.1f hPa", lec.hpa);
      estimular(F_PRESION, d, 0, 0.5f, 5000, txt);
    }
    if ((d = detectar(dHum, lec.ha, 1.5f, cambio))) {
      snprintf(txt, sizeof(txt), "%.0f%%", lec.ha);
      estimular(F_HUMEDAD, d, n.humAmb, 0.5f, 3000, txt);
    }

    dibujarOled();
    uint32_t dt = millis() - t0;
    vTaskDelay(pdMS_TO_TICKS(dt < 700 ? 700 - dt : 10));
  }
}

// ============================================================
//  LEDS WS2812 por RMT propio
//  (el rmtInit() del núcleo 3.2.0 deja campos sin iniciar y falla al azar:
//   por eso los anillos se apagaban cuando el programa tenía audio)
// ============================================================
rmt_channel_handle_t canalLed = NULL;
rmt_encoder_handle_t codLed = NULL;
bool hayLeds = false;
uint8_t pixTx[NUM_LEDS * 3];

bool iniciarLeds() {
  for (int intento = 0; intento < 2 && !canalLed; intento++) {
    rmt_tx_channel_config_t cfg = {};
    cfg.gpio_num = (gpio_num_t)PIN_LEDS;
    cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    cfg.resolution_hz = 10000000;
    cfg.trans_queue_depth = 2;
    cfg.flags.with_dma = intento == 0;
    cfg.mem_block_symbols = intento == 0 ? 1024 : 192;
    if (rmt_new_tx_channel(&cfg, &canalLed) != ESP_OK) canalLed = NULL;
    else Serial.printf("LEDs: RMT %s\n", intento == 0 ? "con DMA" : "sin DMA");
  }
  if (!canalLed) return false;
  rmt_bytes_encoder_config_t ec = {};
  ec.bit0.level0 = 1; ec.bit0.duration0 = 4; ec.bit0.level1 = 0; ec.bit0.duration1 = 8;   // 0,4 µs / 0,8 µs
  ec.bit1.level0 = 1; ec.bit1.duration0 = 8; ec.bit1.level1 = 0; ec.bit1.duration1 = 4;   // 0,8 µs / 0,4 µs
  ec.flags.msb_first = 1;
  if (rmt_new_bytes_encoder(&ec, &codLed) != ESP_OK) return false;
  return rmt_enable(canalLed) == ESP_OK;
}

void mostrarLeds(const uint8_t *grb) {
  if (!hayLeds) return;
  if (rmt_tx_wait_all_done(canalLed, 30) != ESP_OK) return;
  memcpy(pixTx, grb, sizeof(pixTx));
  rmt_transmit_config_t tc = {};
  rmt_transmit(canalLed, codLed, pixTx, sizeof(pixTx), &tc);
}

// ------------------------------------------------------------
//  Dibujo de los anillos
//  anillo 1: cada nota se enciende con el color de su fuente donde cae en la escala
//            (una octava = una vuelta) y se apaga en ~1,5 s
//  anillo 2: color de ambiente (azul de noche, celeste de día) teñido suave por el
//            color del último estímulo; cada nota lanza una onda que recorre el anillo;
//            con alguien presente aparece un arco naranjo que crece al acercarse
// ------------------------------------------------------------
float notaV[24], notaR[24], notaG[24], notaB[24];
struct Onda { bool viva; float pos, edad, r, g, b, fuerza; } ondas[8];
float tinteR = 0, tinteG = 70, tinteB = 255, tintePeso = 0;   // color del último estímulo
float baseR = 0, baseG = 40, baseB = 255;                      // color del anillo 2 (ya mezclado)
float faseGiro = 0, brilloLed = 0.5f;
uint8_t pix[NUM_LEDS * 3];

const float NOCHE[3] = {20, 0, 255};     // azul profundo
const float DIA[3]   = {0, 150, 255};    // celeste
const float PLANTA[3] = {0, 255, 40};    // verde
float base1R = 0, base1G = 200, base1B = 40;                   // color del anillo 1 (ya mezclado)
const float ARCO[3]  = {255, 70, 0};     // persona: naranja

static inline int posEnAnillo(int grado) { return (((grado % 6) * 4) + (grado / 6)) % 24; }

static inline void ponerRGB(int i, float r, float g, float b, float brillo) {
  pix[i * 3 + 0] = (uint8_t)fminf(g * brillo, 255);
  pix[i * 3 + 1] = (uint8_t)fminf(r * brillo, 255);
  pix[i * 3 + 2] = (uint8_t)fminf(b * brillo, 255);
}

void dibujarLeds(float dt) {
  // notas nuevas
  while (colaOut != colaIn) {
    EventoLed e = colaLed[colaOut]; colaOut = (colaOut + 1) & 31;
    const uint8_t *c = COLOR_FUENTE[e.fuente];
    int p = posEnAnillo(e.grado);
    for (int d = -1; d <= 1; d++) {
      int q = (p + d + 24) % 24;
      float v = e.vel * (d == 0 ? 1.0f : 0.35f) * 1.4f;
      float tot = notaV[q] + v;
      if (tot <= 0) continue;
      notaR[q] = (notaR[q] * notaV[q] + c[0] * v) / tot;
      notaG[q] = (notaG[q] * notaV[q] + c[1] * v) / tot;
      notaB[q] = (notaB[q] * notaV[q] + c[2] * v) / tot;
      notaV[q] = fminf(tot, 1.0f);
    }
    for (auto &o : ondas) if (!o.viva) { o = {true, (float)p, 0, (float)c[0], (float)c[1], (float)c[2], fminf(e.vel * 1.5f, 1)}; break; }
    // las personas ya tienen su arco naranjo: tiñen poco, para que no tape el verde y el azul
    if (e.fuente != F_RESPIRO) {
      float peso = (e.fuente == F_PERSONA) ? 0.3f : 1.0f;
      if (peso >= tintePeso) { tinteR = c[0]; tinteG = c[1]; tinteB = c[2]; tintePeso = peso; }
    }
  }

  // ambiente: anillo 1 = la planta (verde, más intenso con más humedad);
  //           anillo 2 = el cielo (azul profundo de noche, celeste de día).
  // Los dos se tiñen suave con el color del último estímulo (se va en ~5 s).
  float luz = lim01(ctl.luz);
  float amb[3] = {NOCHE[0] + (DIA[0] - NOCHE[0]) * luz, NOCHE[1] + (DIA[1] - NOCHE[1]) * luz, NOCHE[2] + (DIA[2] - NOCHE[2]) * luz};
  float agua = lim01(ctl.agua);
  float verde[3] = {PLANTA[0], PLANTA[1] * (0.6f + 0.4f * agua), PLANTA[2]};
  tintePeso *= expf(-dt / 5.0f);
  float w = 0.7f * tintePeso;
  float obj[3] = {amb[0] + (tinteR - amb[0]) * w, amb[1] + (tinteG - amb[1]) * w, amb[2] + (tinteB - amb[2]) * w};
  float obj1[3] = {verde[0] + (tinteR - verde[0]) * w, verde[1] + (tinteG - verde[1]) * w, verde[2] + (tinteB - verde[2]) * w};
  float kc = 1.0f - expf(-dt / 0.6f);                        // transición suave
  baseR += (obj[0] - baseR) * kc; baseG += (obj[1] - baseG) * kc; baseB += (obj[2] - baseB) * kc;
  base1R += (obj1[0] - base1R) * kc; base1G += (obj1[1] - base1G) * kc; base1B += (obj1[2] - base1B) * kc;

  float pres = lim01(ctl.presencia), cerca = lim01(ctl.cercania);
  float objB = 0.55f + 0.25f * pres + 0.20f * pres * cerca;
  brilloLed += (objB - brilloLed) * (1.0f - expf(-dt / 0.4f));
  float brillo = BRILLO_MAX * brilloLed;

  faseGiro += dt * (0.02f + 0.3f * lim01(ctl.movimiento));
  float t = millis() / 1000.0f;
  float respira = 0.85f + 0.15f * sinf(2 * PI * t / 7.0f);
  float mitadArco = pres * (1.0f + 4.5f * cerca);            // 2 LED lejos → 11 LED pegado (deja ver el azul)
  float centroArco = 24.0f * faseGiro;
  float caida = powf(0.35f, dt);

  for (auto &o : ondas) if (o.viva) { o.edad += dt; if (o.edad > 1.8f) o.viva = false; }

  for (int i = 0; i < 24; i++) {
    // anillo 1: el verde de la planta + las notas encima
    float bv = (0.45f + 0.08f * sinf(2 * PI * (i / 24.0f - faseGiro))) * respira, nv = notaV[i];
    ponerRGB(i, base1R * bv * (1 - nv) + notaR[i] * nv, base1G * bv * (1 - nv) + notaG[i] * nv,
             base1B * bv * (1 - nv) + notaB[i] * nv, brillo);
    notaV[i] *= caida;

    // anillo 2
    float r = baseR, g = baseG, b = baseB;
    float v = (0.50f + 0.10f * sinf(2 * PI * (i / 24.0f + faseGiro))) * respira;
    if (mitadArco > 0.05f) {
      float dd = fabsf(fmodf(i - centroArco + 48.0f, 24.0f)); if (dd > 12) dd = 24 - dd;
      float a = lim01(mitadArco + 0.5f - dd) * pres;
      r += (ARCO[0] - r) * a; g += (ARCO[1] - g) * a; b += (ARCO[2] - b) * a; v += 0.4f * a;
    }
    for (auto &o : ondas) if (o.viva) {
      float frente = o.edad * 10.0f;                          // 10 LED por segundo hacia ambos lados
      float dd = fabsf(fmodf(i - o.pos + 48.0f, 24.0f)); if (dd > 12) dd = 24 - dd;
      float a = expf(-(dd - frente) * (dd - frente) / 1.5f) * o.fuerza * (1 - o.edad / 1.8f);
      r += (o.r - r) * a; g += (o.g - g) * a; b += (o.b - b) * a; v += 0.5f * a;
    }
    ponerRGB(24 + i, r * lim01(v), g * lim01(v), b * lim01(v), brillo);
  }
  mostrarLeds(pix);
}

// ============================================================
//  SETUP / LOOP
// ============================================================
void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) delay(10);
  Serial.println("\n=== SINTETIZADOR AMBIENTAL · suena con los cambios · Mi mayor hexatónica ===");

  for (int i = 0; i <= TAB; i++) SENO[i] = sinf(2 * PI * i / TAB);
  for (int i = 0; i < N_PRESETS; i++) calcularCoef(i);
  memset(ultimaFigura, -1, sizeof(ultimaFigura));

  hayLeds = iniciarLeds();
  if (!hayLeds) Serial.println("LEDs: no pude iniciar el RMT");
  memset(pix, 0, sizeof(pix)); mostrarLeds(pix);

  Wire.begin(PIN_SDA, PIN_SCL, 100000);
  Wire1.begin(PIN_SDA2, PIN_SCL2, 100000);
  delay(100);
  hayOled = existe(Wire, 0x3C) && oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  hayBmp = iniciarBMP180();
  hayVeml = existe(Wire, 0x10) && iniciarVEML7700();
  hayEns = iniciarENS160();
  hayAht = existe(Wire, 0x38);
  haySht1 = existe(Wire, 0x44);
  haySht2 = existe(Wire1, 0x44);
  Serial.printf("OLED %s | BMP180 %s | VEML7700 %s | ENS160 %s | AHT21 %s | SHT40 %s / %s\n",
                hayOled ? "OK" : "--", hayBmp ? "OK" : "--", hayVeml ? "OK" : "--", hayEns ? "OK" : "--",
                hayAht ? "OK" : "--", haySht1 ? "OK" : "--", haySht2 ? "OK" : "--");

  iniciarRadar();

  i2s.setPins(I2S_BCK, I2S_LCK, I2S_DIN);
  if (!i2s.begin(I2S_MODE_STD, SR, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO))
    Serial.println("DAC: no pude iniciar I2S");
  for (float seg : {0.9f, 0.6f, 0.4f}) {
    largoEco = (int)(2 * seg * SR) + 1;
    bufEco = (int16_t *)calloc(largoEco, sizeof(int16_t));
    if (bufEco) break;
  }
  Serial.printf("Eco: %s | memoria libre %u bytes\n", bufEco ? "OK" : "sin memoria", ESP.getFreeHeap());

  xTaskCreatePinnedToCore(tareaSensores, "sensores", 6144, nullptr, 1, nullptr, 1);
  delay(1600);
  M.c = ctl;
  baseR = NOCHE[0] + (DIA[0] - NOCHE[0]) * ctl.luz; baseG = NOCHE[1] + (DIA[1] - NOCHE[1]) * ctl.luz;
  baseB = NOCHE[2] + (DIA[2] - NOCHE[2]) * ctl.luz;
  xTaskCreatePinnedToCore(tareaAudio, "audio", 8192, nullptr, 10, nullptr, 0);
}

void loop() {
  static uint32_t tLed = 0, tLog = 0, tRadarRe = 0, tCambioPres = 0;
  static bool presente = false;
  static float distRef = 0;
  static float enMovSuave = 0;
  char txt[24];

  // ---- radar ----
  leerRadar();
  bool salidaOut = digitalRead(PIN_RADAR_OUT);
  bool datosFrescos = radar.datos && millis() - radar.tUlt < 2000;
  bool presCruda = datosFrescos ? radar.estado != 0 : salidaOut;
  float dist = radar.distDet;
  if (!datosFrescos && millis() - tRadarRe > 5000 && millis() - radar.tUlt > 5000) {   // sin datos: prueba RX/TX cruzados
    tRadarRe = millis();
    int t = radarRx; radarRx = radarTx; radarTx = t;
    Serial1.end(); Serial1.begin(256000, SERIAL_8N1, radarRx, radarTx);
  }

  // presencia con antirrebote: el estado nuevo tiene que durar 0,7 s
  if (presCruda == presente) tCambioPres = millis();
  else if (millis() - tCambioPres > 700) {
    presente = presCruda; tCambioPres = millis();
    if (presente) { snprintf(txt, sizeof(txt), "a %d cm", (int)dist); estimular(F_LLEGA, 1, dist, 1, 1500, txt); distRef = dist; }
    else          { estimular(F_SEVA, -1, distRef, 0.5f, 1500, ""); }
  }
  if (presente && datosFrescos) {
    float mov = (radar.estado & 1) ? radar.enMov : 0;
    // cada 30 cm de desplazamiento = una nota (acercarse sube, alejarse baja)
    if (fabsf(dist - distRef) >= 30) {
      int dir = dist < distRef ? 1 : -1;
      snprintf(txt, sizeof(txt), "%s %d cm", dir > 0 ? "se acerca" : "se aleja", (int)dist);
      if (estimular(F_PERSONA, dir, dist, lim01(mov / 100.0f), 450, txt)) distRef = dist;
    } else if (mov > enMovSuave + 35) {                   // un gesto brusco en el mismo lugar
      snprintf(txt, sizeof(txt), "se mueve %d cm", (int)dist);
      estimular(F_PERSONA, 0, dist, lim01(mov / 100.0f), 900, txt);
    }
    enMovSuave += (mov - enMovSuave) * 0.02f;
  }
  float cerca = presente ? lim01(1.0f - dist / 400.0f) : 0;
  float movN = presente && datosFrescos && (radar.estado & 1) ? radar.enMov / 100.0f : 0;
  ctl.presencia += ((presente ? 1.0f : 0.0f) - ctl.presencia) * 0.05f;
  ctl.movimiento += (movN - ctl.movimiento) * 0.05f;
  ctl.cercania += (cerca - ctl.cercania) * 0.05f;

  // ---- LEDs a ~40 cuadros por segundo ----
  uint32_t ahora = millis();
  if (ahora - tLed >= 25) {
    float dt = (ahora - tLed) / 1000.0f; if (dt > 0.2f) dt = 0.2f;
    tLed = ahora; dibujarLeds(dt);
  }

  // ---- informe por serie ----
  if (ahora - tLog > 5000) {
    tLog = ahora;
    Serial.printf("   [luz %.0f lx | plantas %.1f%% %.1f%% | %.1f C | CO2 %u | %s] voces %d CPU %d%%\n",
                  isnan(lec.lux) ? 0.0f : lec.lux, isnan(lec.h1) ? 0.0f : lec.h1, isnan(lec.h2) ? 0.0f : lec.h2,
                  ctl.temp, lec.eco2, presente ? "persona" : (datosFrescos ? "nadie" : "radar sin datos"),
                  (int)vocesSonando, (int)(cargaCPU * 100));
  }
  delay(2);
}
