// ============================================================
//  SYNTH POLIFÓNICO — placa Cerebro v6 (sintetizador de madera)
//  Teclado de 49 teclas (Do3–Do7) por el MCP23017, 8 voces y 5 sonidos, cada uno
//  hecho con un mecanismo distinto:
//    PAD     dos sierras desafinadas + sub, filtro que se abre lento
//    CORO    sierras por dos filtros de formantes («aah»): voces lejanas, con vibrato
//    FLAUTA  seno casi puro con vibrato, sin soplo
//    GOTAS   FM armónica suave: notas redondas que se apagan solas (ideal con arpegio)
//    EPIANO  piano eléctrico tibio: FM solo con armónicos naturales (1:1 y 2:1), sin
//            «campanita»; ataque blando, se apaga lento y ondula con el chorus
//  Todo pasa por chorus, eco ping-pong a tempo (puntillo de corchea) y reverb.
//
//  Controles de la Cerebro:
//              B1 = siguiente sonido
//              B2 (toque) = arpegiador: apagado → sube → sube-baja → azar
//              B2 apretado + encoder = tempo (40–160 BPM)
//              encoder = brillo (filtro, índice FM o vocal del coro)
//              pulsador del encoder = sostenido; con arpegio es «latch»
//
//  Placa Control (cable de 10 hilos a J2), de izquierda a derecha:
//    P1 BRILLO   P2 ATAQUE   P3 COLA   P4 CHORUS   P5 ECO   P6 REVERB   P7 TEMPO   P8 VOLUMEN
//    (ataque y cola multiplican los tiempos del sonido, ×0,25 a ×4; chorus, eco y
//     reverb van de 0 al doble de lo que trae el sonido; con la perilla al medio,
//     el sonido queda como viene)
//    S1–S5  PAD · CORO · FLAUTA · GOTAS · EPIANO
//    S6–S8  arpegio SUBE · SUBE-BAJA · AZAR (otra vez el mismo = apagado)
//    S9  sostenido / latch      S10 octava −     S11 octava +
//    S12 octavas del arpegio (1 → 2 → 3)          S13 división (corchea → tresillo → semicorchea)
//    S14 congelar la reverb     S15 tap tempo    S16 pánico (silencio total)
//  Cada cambio aparece un segundo en grande en la OLED.
//
//  Compilar con «USB CDC On Boot: Enabled». El audio corre en el núcleo 0
//  y el teclado, el encoder y la OLED en el loop (núcleo 1).
// ============================================================
#pragma GCC optimize("O2")   // el audio necesita velocidad; el IDE compila con -Os
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP_I2S.h>
#include <math.h>

// ---- Pines (placa Cerebro en KiCad) ----
#define PIN_SDA   8
#define PIN_SCL   9
#define ENC_A     40
#define ENC_B     39
#define ENC_SW    38
#define BTN_1     44
#define BTN_2     45
#define I2S_BCK   5
#define I2S_LCK   6
#define I2S_DIN   7

// ---- Placa Control (multiplexores CD74HC4051) ----
#define PIN_POT   4      // común de U1 (potenciómetros), ADC1
#define PIN_BT1   14     // común de U2 (switches S1–S8)
#define PIN_BT2   18     // común de U3 (switches S9–S16)
const uint8_t SEL_MUX[3] = {15, 16, 17};                  // S0, S1, S2
const uint8_t COL_CH[8]  = {3, 0, 1, 2, 4, 6, 7, 5};      // columna → canal del 4051
// 0 mientras los potenciómetros no estén soldados: un canal al aire lee ruido y
// movería los parámetros solo. Los switches funcionan igual.
#define POTS_SOLDADOS 1

// 1 = cable 1 del teclado soldado en B6 y el 15 en A0 (la placa de Gonzalo)
#define TECLADO_INVERTIDO 1

// ---- Audio ----
#define SR        44100
#define BLOQUE    64
#define NVOCES    8
#define NOTA_BASE_HZ 130.81f    // Do3 en la tecla más grave
#define N_TECLAS  49

// ============================================================
//  SONIDOS
// ============================================================
struct Preset {
  const char *nombre;
  uint8_t fm;                       // 0 = sustractivo, 1 = FM
  float desafino;                   // semitonos entre las dos sierras
  uint8_t osc2Cuadrada;             // 0 = sierra, 1 = cuadrada
  float mezcla2, sub;               // nivel del oscilador 2 y del sub (una octava abajo)
  float corte, reso, envFiltro;     // Hz, 0..0.9, octavas que abre la envolvente
  float fA, fD, fS, fR;             // envolvente del filtro (s, s, 0..1, s)
  float ratio1, ind1Ini, ind1Fin, ind1Dec, ratio2, ind2, ind2Dec;   // FM
  float A, D, S, R;                 // envolvente de volumen
  float chorus, reverb, cola, apagado, ganancia;
  float nivel1, ratioOsc2;          // nivel de la sierra 1; frecuencia del osc 2 (1 = unísono, 2 = octava)
  float aire;                       // ruido blanco antes del filtro (soplido)
  float lfoProf, lfoHz;             // el filtro «respira»: octavas y velocidad
  float vibCents, vibHz;            // vibrato de altura
  float form1, form2, formQ;        // formantes (Hz); 0 = filtro pasa-bajos normal
  float eco;                        // envío al eco ping-pong
};

const Preset PRESETS[] = {
  // nombre   fm desaf osc2 mez2   sub    corte reso   envF  fA     fD    fS     fR   | FM: r1   i1i   i1f   i1d    r2    i2    i2d  | A      D     S      R    | chor  rev    cola   apag   gan  | niv1  rat2  aire   lfoP lfoHz| vib  vHz | form1 form2 Q   | eco
  {"PAD",     0, 0.12f, 0, 1.00f, 0.40f, 700, 0.20f, 1.8f, 0.80f, 2.5f, 0.35f, 1.8f, 0,    0,    0,    0,     0,    0,    0,     0.40f, 1.5f, 0.80f, 2.5f, 0.55f, 0.50f, 0.87f, 0.30f, 0.70f, 1.0f, 1.0f, 0.00f, 0, 0,       0,   0,    0,    0,    0,    0.20f},
  {"CORO",    0, 0.10f, 0, 0.70f, 0,     0,   0,     0,    0,     0,    0,     0,    0,    0,    0,    0,     0,    0,    0,     0.80f, 2.0f, 0.90f, 2.5f, 0.80f, 0.70f, 0.91f, 0.40f, 2.20f, 1.0f, 1.0f, 0.05f, 0, 0,       12,  4.5f, 650,  1100, 3.0f, 0.15f},
  {"FLAUTA",  1, 0,     0, 0,     0,     0,   0,     0,    0,     0,    0,     0,    1.0f, 0.5f, 0.25f,0.30f, 2.0f, 0,    0,     0.12f, 3.0f, 0.85f, 0.6f, 0.30f, 0.60f, 0.90f, 0.45f, 0.60f, 0,    0,    0,     0, 0,       15,  5.0f, 0,    0,    0,    0.25f},
  {"GOTAS",   1, 0,     0, 0,     0,     0,   0,     0,    0,     0,    0,     0,    1.0f, 1.0f, 0,    0.15f, 2.0f, 0.3f, 0.40f, 0.003f,3.5f, 0.00f, 2.0f, 0.40f, 0.65f, 0.91f, 0.40f, 0.50f, 0,    0,    0,     0, 0,       0,   0,    0,    0,    0,    0.40f},
  {"EPIANO",  1, 0,     0, 0,     0,     0,   0,     0,    0,     0,    0,     0,    1.0f, 1.4f, 0.35f,0.50f, 2.0f, 0.25f,0.15f, 0.003f,4.0f, 0.00f, 0.6f, 0.60f, 0.40f, 0.87f, 0.45f, 0.60f, 0,    0,    0,     0, 0,       0,   0,    0,    0,    0,    0.10f},
};
const int N_PRESETS = sizeof(PRESETS) / sizeof(PRESETS[0]);

// coeficientes por muestra del sonido actual
struct Coef { float aInc, dC, s, rC, faInc, fdC, fs, frC, m1, m2; } C;

// t = segundos hasta que la envolvente cae a -60 dB (1/1000). Antes t era la constante de tiempo,
// y una nota soltada seguía sonando (y gastando CPU) ~7 veces más de lo que decía el sonido.
float coefTiempo(float t) { return 1.0f - expf(-6.9f / (fmaxf(t, 0.0005f) * SR)); }

float multAtaqueUsado = 1, multColaUsado = 1;
void calcularCoef(const Preset &p) {
  float ma = multAtaqueUsado, mc = multColaUsado;           // perillas ATAQUE y COLA
  C.aInc = 1.0f / (fmaxf(p.A * ma, 0.0005f) * SR);
  C.dC = coefTiempo(p.D * mc); C.s = p.S; C.rC = coefTiempo(p.R * mc);
  C.faInc = 1.0f / (fmaxf(p.fA * ma, 0.0005f) * SR);
  C.fdC = coefTiempo(p.fD * mc); C.fs = p.fS; C.frC = coefTiempo(p.fR * mc);
  C.m1 = p.ind1Dec > 0 ? expf(-1.0f / (p.ind1Dec * SR)) : 0;
  C.m2 = p.ind2Dec > 0 ? expf(-1.0f / (p.ind2Dec * SR)) : 0;
}

// coeficientes de los dos filtros de formantes (pasa-banda), se recalculan con el brillo
float F1a1, F1a2, F1a3, F1k, F2a1, F2a2, F2a3, F2k;
void calcFormantes(const Preset &p, float mult) {
  if (p.form1 <= 0) return;
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

static inline float seno(float fase) {             // fase en ciclos, cualquier valor
  int e = (int)fase; if (fase < 0) e--;              // floor sin llamar a la librería
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
  bool activa; int8_t nota; uint32_t orden; bool pendiente;   // pendiente = soltada con el pedal puesto
  float dt1, dt2, inv1, inv2, ph1, ph2, phSub;
  float pm1, pm2, dtm1, dtm2, ie1, ie2, kt;
  float env, envF; uint8_t et, etF;                            // etapa: 0 libre, 1 ataque, 2 decay/sustain, 3 release
  float ic1, ic2, a1, a2, a3;
  float pL, pR;
  uint32_t ruido; float lfoPh, vibPh, nlp;
  float f1a, f1b, f2a, f2b;                                    // estados de los formantes
};
Voz voces[NVOCES];
void renderVoz(Voz &v, float *__restrict L, float *__restrict R, float multCorte, float multIndice);   // evita el prototipo automático del IDE
uint32_t contadorOrden = 0;

volatile int presetPedido = 0;
int presetActual = -1;
volatile int brillo = 0;            // -12..12
volatile bool sostenido = false;
volatile int vocesSonando = 0;
volatile float cargaCPU = 0;

// parámetros que mueve la placa Control (los lee la tarea de audio)
volatile float multAtaque = 1, multCola = 1;              // ×0,25 .. ×4
volatile float multChorus = 1, multEco = 1, multReverb = 1;
volatile float volumen = 1;                                // 0 .. 1 sobre VOLUMEN
volatile int   octava = 0;                                 // −2 .. +2
volatile int   arpOctavas = 2;                             // 1 .. 3
volatile int   arpDivision = 0;                            // 0 corchea, 1 tresillo, 2 semicorchea
volatile bool  congelar = false, panico = false;
const int DIV_POR_PULSO[3] = {2, 3, 4};
const char *NOMBRE_DIV[3] = {"CORCHEA", "TRESILLO", "SEMICORCHEA"};
#define UMBRAL_LIM 0.6f               // pico máximo de salida (≈ −4,4 dBFS)
#define VOLUMEN    0.8f               // nivel final hacia el DAC (−2 dB): el PCM5102 entrega 2,1 Vrms a
                                      // escala completa y eso satura muchos amplificadores pequeños
#define TONO_HZ    6500.0f            // pasa-bajos suave en la salida: quita lo filudo de arriba
#define GRAVES     0.5f               // refuerzo bajo ~180 Hz (+3,5 dB) para que no se apaguen los graves
float limEnv = 0, limGan = 1, limMinBloque = 1, tonoL = 0, tonoR = 0, bajoL = 0, bajoR = 0;
volatile float limMin = 1;            // la reducción más fuerte desde el último informe por serie

void notaOn(int n) {
  const Preset &p = PRESETS[presetActual];
  int elegida = -1;
  for (int i = 0; i < NVOCES; i++) if (voces[i].activa && voces[i].nota == n) { elegida = i; break; }
  // protección: si el audio ya va sobre 85 % del núcleo, no se suman voces; se reemplaza una
  int activas = 0; for (int i = 0; i < NVOCES; i++) activas += voces[i].activa;
  bool limitar = cargaCPU > 0.85f && activas >= 5;
  if (elegida < 0 && !limitar) for (int i = 0; i < NVOCES; i++) if (!voces[i].activa) { elegida = i; break; }
  if (elegida < 0) {                                   // robar: la más baja en release, si no la más vieja
    float min = 2;
    for (int i = 0; i < NVOCES; i++) if (voces[i].et == 3 && voces[i].env < min) { min = voces[i].env; elegida = i; }
  }
  if (elegida < 0) {
    uint32_t viejo = UINT32_MAX;
    for (int i = 0; i < NVOCES; i++) if (voces[i].orden < viejo) { viejo = voces[i].orden; elegida = i; }
  }
  Voz &v = voces[elegida];
  if (!v.activa) {                                     // voz nueva: estado limpio
    v.ph1 = 0; v.ph2 = 0.37f; v.phSub = 0; v.pm1 = 0; v.pm2 = 0;
    v.ic1 = v.ic2 = 0; v.env = 0;
    v.ruido = 0x9E3779B9u * (contadorOrden + 1);
    v.lfoPh = (contadorOrden % 7) * 0.143f;             // cada voz respira a destiempo
    v.vibPh = (contadorOrden % 5) * 0.2f; v.nlp = 0;
    v.f1a = v.f1b = v.f2a = v.f2b = 0;
  }
  float f = NOTA_BASE_HZ * exp2f(n / 12.0f);
  if (p.fm) {
    v.dt1 = f / SR; v.dtm1 = f * p.ratio1 / SR; v.dtm2 = f * p.ratio2 / SR;
    v.kt = powf(261.6f / f, 0.6f);                     // bastante menos índice FM en los agudos
  } else {
    float d = exp2f(p.desafino / 24.0f);
    v.dt1 = f * d / SR; v.dt2 = f * p.ratioOsc2 / d / SR;
    v.kt = sqrtf(f / 261.6f);                          // el filtro sigue a la nota
  }
  v.inv1 = 1.0f / v.dt1; v.inv2 = v.dt2 > 0 ? 1.0f / v.dt2 : 0;
  v.ie1 = 1; v.ie2 = 1;
  v.envF = 0; v.et = 1; v.etF = 1;
  float pan = fminf(fmaxf(0.3f + 0.4f * n / 48.0f, 0.25f), 0.75f);   // graves un poco a la izquierda
  // el oído es muy sensible entre 2 y 4 kHz: las notas sobre Do4 bajan de a poco
  // (Do5 −3 dB, Do6 −6,6 dB, Do7 −10 dB, Do8 del arpegio −13 dB)
  float nivelNota = f > 262.0f ? powf(262.0f / f, 0.55f) : 1.0f;
  v.pL = cosf(pan * PI / 2) * nivelNota; v.pR = sinf(pan * PI / 2) * nivelNota;
  v.nota = n; v.activa = true; v.pendiente = false; v.orden = contadorOrden++;
}

void liberarNota(int n) {                              // suelta sin mirar el pedal (lo usa el arpegiador)
  for (int i = 0; i < NVOCES; i++)
    if (voces[i].activa && voces[i].nota == n && voces[i].et != 3) { voces[i].et = 3; voces[i].etF = 3; voces[i].pendiente = false; }
}

void notaOff(int n) {
  for (int i = 0; i < NVOCES; i++) {
    Voz &v = voces[i];
    if (v.activa && v.nota == n && v.et != 3) {
      if (sostenido) v.pendiente = true;
      else { v.et = 3; v.etF = 3; }
    }
  }
}

// Las voces se calculan con copias locales del estado: así el compilador las deja en
// registros en vez de releerlas de memoria en cada muestra (L y R podrían «pisarlas»).
static inline float pasoEnv(float &e, uint8_t &et, float aInc, float dC, float s, float rC) {
  if (et == 1)      { e += aInc; if (e >= 1) { e = 1; et = 2; } }
  else if (et == 2) { e += (s - e) * dC; }
  else if (et == 3) { e -= e * rC; }
  return e;
}

void renderVoz(Voz &v, float *__restrict L, float *__restrict R, float multCorte, float multIndice) {
  const Preset &p = PRESETS[presetActual];
  const float aInc = C.aInc, dC = C.dC, sus = C.s, rC = C.rC;
  const float pL = v.pL, pR = v.pR;
  float env = v.env; uint8_t et = v.et;
  bool fin = false;

  if (!p.fm) {
    const float k = 2.0f - 2.0f * p.reso, mez2 = p.mezcla2, sub = p.sub, niv1 = p.nivel1, aire = p.aire;
    const float lfoProf = p.lfoProf, lfoPaso = p.lfoHz * 8.0f / SR;
    uint32_t rnd = v.ruido; float lfoPh = v.lfoPh, vibPh = v.vibPh;
    const bool formantes = p.form1 > 0, conOsc2 = mez2 > 0, conSub = sub > 0;
    const float vib = p.vibCents * 0.000578f, vibPaso = p.vibHz * 8.0f / SR;
    float f1a = v.f1a, f1b = v.f1b, f2a = v.f2a, f2b = v.f2b;
    const float faInc = C.faInc, fdC = C.fdC, fs = C.fs, frC = C.frC;
    const float dt1b = v.dt1, dt2b = v.dt2, inv1 = v.inv1, inv2 = v.inv2;
    const bool cuadrada = p.osc2Cuadrada;
    float ph1 = v.ph1, ph2 = v.ph2, phSub = v.phSub, ic1 = v.ic1, ic2 = v.ic2, envF = v.envF;
    uint8_t etF = v.etF;
    for (int j = 0; j < BLOQUE && !fin; j += 8) {
      float vm = 1.0f;                                   // vibrato, cada 8 muestras
      if (vib > 0) { vm += vib * seno(vibPh); vibPh += vibPaso; if (vibPh >= 1) vibPh -= 1; }
      const float dt1 = dt1b * vm, dt2 = dt2b * vm, dtSub = dt1 * 0.5f;
      float a1 = 0, a2 = 0, a3 = 0;
      if (!formantes) {                                  // filtro pasa-bajos cada 8 muestras
        float oct = p.envFiltro * envF;
        if (lfoProf > 0) { oct += lfoProf * seno(lfoPh); lfoPh += lfoPaso; if (lfoPh >= 1) lfoPh -= 1; }
        float fc = p.corte * multCorte * v.kt * exp2f(oct);
        fc = fminf(fmaxf(fc, 30.0f), 15000.0f);
        float g = tanf(PI * fc / SR);
        a1 = 1.0f / (1.0f + g * (g + k)); a2 = g * a1; a3 = g * a2;
      }
      for (int i = j; i < j + 8; i++) {
        pasoEnv(env, et, aInc, dC, sus, rC);
        if (env < 0.001f && et != 1) { fin = true; break; }
        pasoEnv(envF, etF, faInc, fdC, fs, frC);
        float o1 = niv1 > 0 ? sierra(ph1, dt1, inv1) : 0.0f, o2 = 0;   // sin sierra 1 se ahorra CPU
        if (conOsc2) {
          if (cuadrada) {
            float ph2b = ph2 + 0.5f; if (ph2b >= 1) ph2b -= 1;
            o2 = sierra(ph2, dt2, inv2) - sierra(ph2b, dt2, inv2);
          } else o2 = sierra(ph2, dt2, inv2);
        }
        float x = (o1 * niv1 + o2 * mez2) * 0.5f + 1e-18f;
        if (conSub) x += seno(phSub) * sub;
        if (aire > 0) { rnd = rnd * 1664525u + 1013904223u; x += aire * (int32_t)rnd * 4.6566e-10f; }
        ph1 += dt1; if (ph1 >= 1) ph1 -= 1;
        ph2 += dt2; if (ph2 >= 1) ph2 -= 1;
        phSub += dtSub; if (phSub >= 1) phSub -= 1;
        float s;
        if (formantes) {                                 // dos pasa-banda en paralelo: la vocal
          float w3 = x - f1b, w1 = F1a1 * f1a + F1a2 * w3, w2 = f1b + F1a2 * f1a + F1a3 * w3;
          f1a = 2 * w1 - f1a; f1b = 2 * w2 - f1b;
          float u3 = x - f2b, u1 = F2a1 * f2a + F2a2 * u3, u2 = f2b + F2a2 * f2a + F2a3 * u3;
          f2a = 2 * u1 - f2a; f2b = 2 * u2 - f2b;
          s = (F1k * w1 + 0.7f * F2k * u1) * env;
        } else {
          float v3 = x - ic2;                            // filtro de estado variable (TPT), pasa-bajos
          float v1 = a1 * ic1 + a2 * v3;
          float v2 = ic2 + a2 * ic1 + a3 * v3;
          ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2;
          s = v2 * env;
        }
        L[i] += s * pL; R[i] += s * pR;
      }
    }
    v.ph1 = ph1; v.ph2 = ph2; v.phSub = phSub; v.ic1 = ic1; v.ic2 = ic2; v.envF = envF; v.etF = etF;
    v.ruido = rnd; v.lfoPh = lfoPh; v.vibPh = vibPh;
    v.f1a = f1a; v.f1b = f1b; v.f2a = f2a; v.f2b = f2b;
  } else {
    const float i1f = p.ind1Fin, i1d = p.ind1Ini - p.ind1Fin, i2 = p.ind2;
    const float esc = v.kt * multIndice * (1.0f / (2.0f * PI));
    const float m1 = C.m1, m2 = C.m2, dt1 = v.dt1, dtm1 = v.dtm1, dtm2 = v.dtm2;
    float ph1 = v.ph1, pm1 = v.pm1, pm2 = v.pm2, ie1 = v.ie1, ie2 = v.ie2;
    const float vib = p.vibCents * 0.000578f, vibPaso = p.vibHz / SR, aire = p.aire * 2.0f;
    float vibPh = v.vibPh, nlp = v.nlp; uint32_t rnd = v.ruido;
    for (int i = 0; i < BLOQUE; i++) {
      pasoEnv(env, et, aInc, dC, sus, rC);
      if (env < 0.001f && et != 1) { fin = true; break; }
      float vm = 1.0f;
      if (vib > 0) { vm += vib * seno(vibPh); vibPh += vibPaso; if (vibPh >= 1) vibPh -= 1; }
      float m = ((i1f + i1d * ie1) * seno(pm1) + i2 * ie2 * seno(pm2)) * esc;
      float tono = seno(ph1 + m);
      if (aire > 0) {                                    // soplo: ruido suavizado
        rnd = rnd * 1664525u + 1013904223u;
        nlp += ((int32_t)rnd * 4.6566e-10f - nlp) * 0.3f;
        tono += aire * nlp;
      }
      float s = tono * env;
      ie1 *= m1; ie2 *= m2;
      ph1 += dt1 * vm; if (ph1 >= 1) ph1 -= 1;
      pm1 += dtm1 * vm; if (pm1 >= 1) pm1 -= 1;
      pm2 += dtm2 * vm; if (pm2 >= 1) pm2 -= 1;
      L[i] += s * pL; R[i] += s * pR;
    }
    v.ph1 = ph1; v.pm1 = pm1; v.pm2 = pm2; v.ie1 = ie1; v.ie2 = ie2;
    v.vibPh = vibPh; v.nlp = nlp; v.ruido = rnd;
  }
  v.env = env; v.et = et;
  if (fin) { v.env = 0; v.et = 0; v.activa = false; }
}

// ============================================================
//  EFECTOS
// ============================================================
#define LCH 1024
float bufCh[LCH]; int posCh = 0;
float lfoS = 0, lfoC = 1;                            // LFO por rotación: seno y coseno sin tabla
const float LFO_W = 2 * PI * 0.35f / SR;

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

// eco ping-pong en una sola línea: y = entrada + fb·y[n−2T]; izquierda oye y[n−T], derecha y[n−2T]
int16_t *bufEco = nullptr; int largoEco = 0; int posEco = 0; float lpEco = 0;

// ============================================================
//  ARPEGIADOR
// ============================================================
volatile int arpModo = 0;           // 0 apagado, 1 sube, 2 sube-baja, 3 azar
volatile int bpm = 72;
const char *NOMBRE_ARP[4] = {"", "SUBE", "SUBE-BAJA", "AZAR"};

// ============================================================
//  TAREA DE AUDIO (núcleo 0)
// ============================================================
I2SClass i2s;
portMUX_TYPE muxTeclas = portMUX_INITIALIZER_UNLOCKED;
uint64_t teclasCompartidas = 0;

void publicarTeclas(uint64_t m) { portENTER_CRITICAL(&muxTeclas); teclasCompartidas = m; portEXIT_CRITICAL(&muxTeclas); }
uint64_t tomarTeclas() { portENTER_CRITICAL(&muxTeclas); uint64_t m = teclasCompartidas; portEXIT_CRITICAL(&muxTeclas); return m; }

void tareaAudio(void *) {
  static float L[BLOQUE], R[BLOQUE];
  static int16_t salida[BLOQUE * 2];
  uint64_t antes = 0;
  bool sostAntes = false;
  int modoAntes = 0, arpNota = -1; uint64_t latch = 0; bool teniaTeclas = false;
  int32_t faltaPaso = 0, faltaGate = 0; uint32_t arpPos = 0, rndArp = 12345;
  int octAntes = 0;
  const float ciclosBloque = (float)BLOQUE / SR * getCpuFrequencyMhz() * 1e6f;

  for (;;) {
    uint32_t t0 = ESP.getCycleCount();

    int pp = presetPedido;
    if (pp != presetActual) {                          // cambio de sonido: corta las voces
      presetActual = pp; calcularCoef(PRESETS[pp]);
      for (int i = 0; i < NVOCES; i++) { voces[i].activa = false; voces[i].et = 0; }
      antes = 0;
    }
    if (multAtaque != multAtaqueUsado || multCola != multColaUsado) {   // perillas ATAQUE / COLA
      multAtaqueUsado = multAtaque; multColaUsado = multCola; calcularCoef(PRESETS[presetActual]);
    }
    const Preset &p = PRESETS[presetActual];

    if (panico) {                                      // S16: silencio total, también eco y reverb
      for (int i = 0; i < NVOCES; i++) { voces[i].activa = false; voces[i].et = 0; }
      memset(lineaRev, 0, sizeof(lineaRev)); memset(lpRev, 0, sizeof(lpRev));
      memset(ap1, 0, sizeof(ap1)); memset(ap2, 0, sizeof(ap2)); memset(bufCh, 0, sizeof(bufCh));
      if (bufEco) memset(bufEco, 0, largoEco * sizeof(int16_t));
      lpEco = 0; arpNota = -1; latch = 0; antes = 0;
      panico = false;
    }

    uint64_t ahora = tomarTeclas();
    bool sost = sostenido;
    const int pasoArp = SR * 60 / (bpm * DIV_POR_PULSO[arpDivision]);
    const int oct = octava;
    if (oct != octAntes) {                             // cambio de octava: suelta lo que suena
      for (int i = 0; i < NVOCES; i++) if (voces[i].activa) { voces[i].et = 3; voces[i].etF = 3; voces[i].pendiente = false; }
      arpNota = -1; antes = 0; octAntes = oct;
    }

    int modo = arpModo;
    if (modo != modoAntes) {                           // al entrar o salir del arpegio, todo suelta
      for (int i = 0; i < NVOCES; i++) if (voces[i].activa) { voces[i].et = 3; voces[i].etF = 3; voces[i].pendiente = false; }
      arpNota = -1; arpPos = 0; faltaPaso = 0; latch = 0; antes = 0; modoAntes = modo;
    }

    if (modo == 0) {
      uint64_t cambios = ahora ^ antes;
      while (cambios) {
        int n = __builtin_ctzll(cambios);
        cambios &= cambios - 1;
        if (ahora & (1ULL << n)) notaOn(n + 12 * oct); else notaOff(n + 12 * oct);
      }
      antes = ahora;
    } else {
      uint64_t notas;
      if (sost) {                                      // latch: un acorde nuevo reemplaza al anterior
        if (ahora) latch = teniaTeclas ? (latch | ahora) : ahora;
        notas = latch;
      } else { latch = 0; notas = ahora; }
      teniaTeclas = ahora != 0;

      if (!notas) {
        if (arpNota >= 0) { liberarNota(arpNota); arpNota = -1; }
        faltaPaso = 0; arpPos = 0;
      } else {
        if (faltaGate > 0) { faltaGate -= BLOQUE; if (faltaGate <= 0 && arpNota >= 0) liberarNota(arpNota); }
        faltaPaso -= BLOQUE;
        if (faltaPaso <= 0) {
          static int sec[3 * N_TECLAS]; int largo = 0;
          const int nOct = arpOctavas;
          for (int o = 0; o < nOct; o++) {               // el acorde y 0, 1 o 2 octavas arriba
            uint64_t m = notas;
            while (m) { int n = __builtin_ctzll(m); m &= m - 1; sec[largo++] = n + 12 * (o + oct); }
          }
          int idx;
          if (modo == 1) idx = arpPos % largo;
          else if (modo == 2) { int per = largo > 1 ? 2 * largo - 2 : 1; int k = arpPos % per; idx = k < largo ? k : per - k; }
          else {
            rndArp = rndArp * 1664525u + 1013904223u; idx = (rndArp >> 8) % largo;
            if (largo > 1 && sec[idx] == arpNota) idx = (idx + 1) % largo;
          }
          arpPos++;
          if (arpNota >= 0) liberarNota(arpNota);
          arpNota = sec[idx]; notaOn(arpNota);
          faltaPaso += pasoArp; if (faltaPaso <= 0) faltaPaso = pasoArp;
          faltaGate = pasoArp * 4 / 5;
        }
      }
    }

    if (sostAntes && !sost)                            // se soltó el pedal
      for (int i = 0; i < NVOCES; i++)
        if (voces[i].activa && voces[i].pendiente) { voces[i].et = 3; voces[i].etF = 3; voces[i].pendiente = false; }
    sostAntes = sost;

    memset(L, 0, sizeof(L)); memset(R, 0, sizeof(R));
    float multCorte = exp2f(brillo / 6.0f), multIndice = exp2f(brillo / 8.0f);
    calcFormantes(p, exp2f(brillo / 24.0f));         // en el coro, el brillo mueve la vocal
    int sonando = 0;
    for (int i = 0; i < NVOCES; i++) if (voces[i].activa) { renderVoz(voces[i], L, R, multCorte, multIndice); sonando++; }
    vocesSonando = sonando;

    const float gan = p.ganancia * 0.32f;
    static const float COEF_TONO = 1.0f - expf(-2.0f * PI * TONO_HZ / SR);
    static const float COEF_BAJO = 1.0f - expf(-2.0f * PI * 180.0f / SR);
    const float chor = fminf(p.chorus * multChorus, 1.0f);
    const float envioEco = fminf(p.eco * multEco, 1.0f);
    const float humedo = fminf(p.reverb * multReverb, 1.2f) * 0.6f;
    const bool helado = congelar;
    const float colaRev = helado ? 0.999f : p.cola, filtroRev = helado ? 1.0f : 1.0f - p.apagado;
    const float volFinal = 30000 * VOLUMEN * volumen;
    const float normCh = 1.0f / (1.0f + 0.5f * chor);
    for (int i = 0; i < BLOQUE; i++) {
      float l = L[i] * gan, r = R[i] * gan;

      // chorus estéreo
      bufCh[posCh] = (l + r) * 0.5f;
      float ns = lfoS + LFO_W * lfoC; lfoC -= LFO_W * ns; lfoS = ns;
      float tl = leerCh(400 + 120 * lfoS), tr = leerCh(400 + 120 * lfoC);
      posCh = (posCh + 1) & (LCH - 1);
      l = (l + chor * tl) * normCh; r = (r + chor * tr) * normCh;

      // eco ping-pong a puntillo de corchea, con el retorno oscurecido
      if (bufEco) {
        int T = SR * 45 / bpm; if (2 * T >= largoEco) T = (largoEco - 1) / 2;
        int iL = posEco - T; if (iL < 0) iL += largoEco;
        int iR = posEco - 2 * T; if (iR < 0) iR += largoEco;
        float tapL = bufEco[iL] * (1.0f / 16384), tapR = bufEco[iR] * (1.0f / 16384);
        lpEco += (tapR - lpEco) * 0.35f;
        float y = (l + r) * 0.5f * envioEco + 0.5f * lpEco;
        y = fminf(fmaxf(y, -1.99f), 1.99f);
        bufEco[posEco] = (int16_t)(y * 16384);
        if (++posEco >= largoEco) posEco = 0;
        l += tapL; r += tapR;
      }

      // reverb: 2 allpass de difusión + red de 4 retardos con matriz de Hadamard
      // congelada: no entra nada nuevo y la cola queda sonando sin apagarse
      float x = allpass(allpass((helado ? 0.0f : (l + r) * 0.5f) + 1e-18f, ap1, 225, pa1), ap2, 556, pa2);
      float o[4];
      for (int q = 0; q < 4; q++) {
        o[q] = lineaRev[q][posRev[q]];
        lpRev[q] += (o[q] - lpRev[q]) * filtroRev;
      }
      float a = lpRev[0], b = lpRev[1], c = lpRev[2], d = lpRev[3];
      float h[4] = {(a + b + c + d) * 0.5f, (a - b + c - d) * 0.5f, (a + b - c - d) * 0.5f, (a - b - c + d) * 0.5f};
      for (int q = 0; q < 4; q++) {
        lineaRev[q][posRev[q]] = x + colaRev * h[q];
        if (++posRev[q] >= LARGO_REV[q]) posRev[q] = 0;
      }
      l += humedo * (o[0] + o[2]);
      r += humedo * (o[1] + o[3]);

      // refuerzo de graves antes del limitador (así nunca se pasa de la escala)
      bajoL += (l - bajoL) * COEF_BAJO; bajoR += (r - bajoR) * COEF_BAJO;
      l += GRAVES * bajoL; r += GRAVES * bajoR;

      // limitador: sigue el pico y baja el volumen general en vez de distorsionar.
      // Ataque ~1 ms, recuperación ~0,3 s; deja la salida bajo UMBRAL_LIM.
      float pico = fmaxf(fabsf(l), fabsf(r));
      if (pico > limEnv) limEnv = pico; else limEnv += (pico - limEnv) * 0.00008f;
      float objetivo = limEnv > UMBRAL_LIM ? UMBRAL_LIM / limEnv : 1.0f;
      limGan += (objetivo - limGan) * (objetivo < limGan ? 0.03f : 0.00015f);
      l *= limGan; r *= limGan;
      if (limGan < limMinBloque) limMinBloque = limGan;

      // saturación suave (cúbica, llega a ±1 en ±1,5) solo para lo que se escape del limitador
      l = fminf(fmaxf(l, -1.5f), 1.5f); r = fminf(fmaxf(r, -1.5f), 1.5f);
      l = l - 0.148148f * l * l * l;
      r = r - 0.148148f * r * r * r;
      tonoL += (l - tonoL) * COEF_TONO; tonoR += (r - tonoR) * COEF_TONO;
      salida[2 * i]     = (int16_t)(tonoL * volFinal);
      salida[2 * i + 1] = (int16_t)(tonoR * volFinal);
    }

    if (limMinBloque < limMin) limMin = limMinBloque;
    limMinBloque = 1;
    cargaCPU = (ESP.getCycleCount() - t0) / ciclosBloque;
    i2s.write((uint8_t *)salida, sizeof(salida));     // bloquea hasta que haya espacio: marca el tiempo
  }
}

// ============================================================
//  TECLADO (MCP23017)
// ============================================================
#define R_IODIR 0x00
#define R_IPOL  0x02
#define R_GPPU  0x0C
#define R_GPIO  0x12
#define R_OLAT  0x14
#define N_LINEAS 15
#define MASCARA  0x7FFF

uint8_t MCP_ADDR = 0x20;
bool hayOled = false, hayMcp = false;

bool mcpEscribir16(uint8_t reg, uint16_t v) {
  Wire.beginTransmission(MCP_ADDR);
  Wire.write(reg); Wire.write(v & 0xFF); Wire.write(v >> 8);
  return Wire.endTransmission() == 0;
}
bool mcpLeer16(uint8_t reg, uint16_t &v) {
  Wire.beginTransmission(MCP_ADDR); Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)MCP_ADDR, (uint8_t)2) != 2) return false;
  v = Wire.read(); v |= (uint16_t)Wire.read() << 8;
  return true;
}
bool iniciarMcp() {
  bool hay = false;
  for (uint8_t a = 0x20; a <= 0x27 && !hay; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { MCP_ADDR = a; hay = true; }
  }
  return hay && mcpEscribir16(R_IPOL, 0) && mcpEscribir16(R_OLAT, 0) &&
         mcpEscribir16(R_GPPU, 0xFFFF) && mcpEscribir16(R_IODIR, 0xFFFF);
}

uint16_t unidas[N_LINEAS];     // unidas[d] = líneas que bajan cuando d está en LOW (sin simetrizar)
uint16_t pegadasGnd = 0;

bool escanearTeclado() {
  uint16_t g;
  if (!mcpEscribir16(R_IODIR, 0xFFFF)) return false;
  delayMicroseconds(30);
  if (!mcpLeer16(R_GPIO, g)) return false;
  pegadasGnd = ~g & MASCARA;
  for (uint8_t d = 0; d < N_LINEAS; d++) {
    unidas[d] = 0;
    if (pegadasGnd & (1 << d)) continue;
    mcpEscribir16(R_IODIR, 0xFFFF & ~(1 << d));
    delayMicroseconds(30);
    if (!mcpLeer16(R_GPIO, g)) return false;
    unidas[d] = ~g & MASCARA & ~(1 << d) & ~pegadasGnd;
  }
  mcpEscribir16(R_IODIR, 0xFFFF);
  return true;
}

// Matriz medida el 2026-11-17: filas = cables 10..15 (8 teclas cada una) y 9 (tecla 49);
// columnas = cables 8,7,6,5,4,3,2,1 de grave a agudo.
const uint8_t CABLE_FILA[7] = {10, 11, 12, 13, 14, 15, 9};
const uint8_t CABLE_COL[8]  = {8, 7, 6, 5, 4, 3, 2, 1};
uint8_t lineaDeCable(uint8_t c) { return TECLADO_INVERTIDO ? N_LINEAS - c : c - 1; }

uint32_t lecturasUnaDir = 0, lecturasDosDir = 0;

uint64_t leerMatriz() {
  uint64_t m = 0;
  for (int f = 0; f < 7; f++) {
    uint8_t lf = lineaDeCable(CABLE_FILA[f]);
    for (int cc = 0; cc < 8; cc++) {
      int n = f * 8 + cc;
      if (n >= N_TECLAS) break;
      uint8_t lc = lineaDeCable(CABLE_COL[cc]);
      bool a = unidas[lf] & (1 << lc), b = unidas[lc] & (1 << lf);
      if (a || b) { m |= 1ULL << n; if (a && b) lecturasDosDir++; else lecturasUnaDir++; }
    }
  }
  return m;
}

void nombreNota(int n, char *s, size_t len) {
  static const char *NOM[12] = {"Do","Do#","Re","Re#","Mi","Fa","Fa#","Sol","Sol#","La","La#","Si"};
  snprintf(s, len, "%s%d", NOM[n % 12], 3 + n / 12);
}

// ============================================================
//  PLACA CONTROL
// ============================================================
char aviso[20] = ""; uint32_t tAviso = 0;
void avisar(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt); vsnprintf(aviso, sizeof(aviso), fmt, ap); va_end(ap);
  tAviso = millis();
  Serial.printf("> %s\n", aviso);
}

const char *NOMBRE_POT[8] = {"BRILLO", "ATAQUE", "COLA", "CHORUS", "ECO", "REVERB", "TEMPO", "VOLUMEN"};

void aplicarPot(int c, float u, bool mostrar) {          // u = 0..1
  float x2 = 2 * u - 1;                                    // −1..1
  switch (c) {
    case 0: brillo = (int)lroundf(x2 * 12); if (mostrar) avisar("BRILLO %+d", (int)brillo); break;
    case 1: multAtaque = powf(4.0f, x2); if (mostrar) avisar("ATAQUE %.2fs", PRESETS[presetPedido].A * multAtaque); break;
    case 2: multCola = powf(4.0f, x2);
            if (mostrar) avisar("COLA %.1fs", (PRESETS[presetPedido].S > 0 ? PRESETS[presetPedido].R : PRESETS[presetPedido].D) * multCola); break;
    case 3: multChorus = 2 * u; if (mostrar) avisar("CHORUS %d%%", (int)(u * 200)); break;
    case 4: multEco = 2.5f * u; if (mostrar) avisar("ECO %d%%", (int)(u * 250)); break;
    case 5: multReverb = 2 * u; if (mostrar) avisar("REVERB %d%%", (int)(u * 200)); break;
    case 6: bpm = 40 + (int)lroundf(u * 120); if (mostrar) avisar("TEMPO %d", (int)bpm); break;
    case 7: volumen = u < 0.02f ? 0 : powf(10.0f, (u - 1) * 2); if (mostrar) avisar("VOLUMEN %d%%", (int)(u * 100)); break;
  }
}

uint32_t tapAnt = 0, tapIntervalo[3] = {0, 0, 0}; int nTaps = 0;
void tapTempo() {
  uint32_t ahora = millis(), d = ahora - tapAnt; tapAnt = ahora;
  if (d < 250 || d > 1500) { nTaps = 0; avisar("TAP..."); return; }
  tapIntervalo[nTaps % 3] = d; nTaps++;
  int n = nTaps < 3 ? nTaps : 3; uint32_t suma = 0;
  for (int i = 0; i < n; i++) suma += tapIntervalo[i];
  int t = (int)lroundf(60000.0f * n / suma); bpm = t < 40 ? 40 : (t > 160 ? 160 : t);
  avisar("TEMPO %d", (int)bpm);
}

void apretarSwitch(int s) {                                // s = 1..16
  if (s <= N_PRESETS) { presetPedido = s - 1; avisar("%s", PRESETS[s - 1].nombre); return; }
  switch (s) {
    case 6: case 7: case 8: {
      int m = s - 5;                                        // 1 sube, 2 sube-baja, 3 azar
      arpModo = (arpModo == m) ? 0 : m;
      if (arpModo) avisar("ARP %s", NOMBRE_ARP[arpModo]); else avisar("ARP APAGADO");
      break;
    }
    case 9:  sostenido = !sostenido; avisar("%s %s", arpModo ? "LATCH" : "SOST", sostenido ? "SI" : "NO"); break;
    case 10: if (octava > -2) octava = octava - 1; avisar("OCTAVA %+d", (int)octava); break;
    case 11: if (octava < 2) octava = octava + 1; avisar("OCTAVA %+d", (int)octava); break;
    case 12: arpOctavas = arpOctavas % 3 + 1; avisar("ARP %d OCT", (int)arpOctavas); break;
    case 13: arpDivision = (arpDivision + 1) % 3; avisar("%s", NOMBRE_DIV[arpDivision]); break;
    case 14: congelar = !congelar; avisar(congelar ? "CONGELADO" : "DESCONGELA"); break;
    case 15: tapTempo(); break;
    case 16: sostenido = false; panico = true; avisar("SILENCIO"); break;
  }
}

// Lee los 8 potenciómetros y los 16 switches; aplica solo lo que cambió.
void leerControl(bool inicio) {
  static float potFilt[8]; static float potAplicado[8];
  static uint8_t f1Est[8], f2Est[8], f1Cuenta[8], f2Cuenta[8];
  for (int c = 0; c < 8; c++) {
    for (int i = 0; i < 3; i++) digitalWrite(SEL_MUX[i], (COL_CH[c] >> i) & 1);
    delayMicroseconds(60);                                  // deja asentar el canal nuevo
#if POTS_SOLDADOS
    analogRead(PIN_POT); analogRead(PIN_POT);              // descarta dos (carga del ADC)
    float v = 0; for (int k = 0; k < 4; k++) v += analogRead(PIN_POT); v *= 0.25f;
    if (inicio) { potFilt[c] = v; potAplicado[c] = v; aplicarPot(c, v / 4095.0f, false); }
    else {
      potFilt[c] += (v - potFilt[c]) * 0.25f;
      if (fabsf(potFilt[c] - potAplicado[c]) > 70) {      // > 1,7 %: la perilla se movió (el ruido del ADC no llega)
        potAplicado[c] = potFilt[c];
        aplicarPot(c, fminf(fmaxf((potFilt[c] - 20) / 4050.0f, 0.0f), 1.0f), true);
      }
    }
#endif
    // switches con antirrebote: el estado nuevo tiene que repetirse 2 lecturas seguidas
    uint8_t a = !digitalRead(PIN_BT1), b = !digitalRead(PIN_BT2);
    if (inicio) { f1Est[c] = a; f2Est[c] = b; continue; }
    if (a != f1Est[c]) { if (++f1Cuenta[c] >= 2) { f1Est[c] = a; f1Cuenta[c] = 0; if (a) apretarSwitch(c + 1); } } else f1Cuenta[c] = 0;
    if (b != f2Est[c]) { if (++f2Cuenta[c] >= 2) { f2Est[c] = b; f2Cuenta[c] = 0; if (b) apretarSwitch(c + 9); } } else f2Cuenta[c] = 0;
  }
}

// ============================================================
//  ENCODER
// ============================================================
volatile int32_t encCuentas = 0;
volatile uint8_t encEstado = 0;
static const int8_t TABLA_ENC[16] = {0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0};
void IRAM_ATTR isrEncoder() {
  uint8_t s = (digitalRead(ENC_A) << 1) | digitalRead(ENC_B);
  encEstado = ((encEstado << 2) | s) & 0x0F;
  encCuentas += TABLA_ENC[encEstado];
}

// ============================================================
//  OLED
// ============================================================
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
char ultimaNota[8] = "";
bool mostrarTempo = false;           // B2 apretado: la pantalla muestra el tempo

void dibujar() {
  if (!hayOled) return;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(2); oled.setCursor(0, 0);
  oled.print(PRESETS[presetPedido].nombre);
  oled.setTextSize(1);
  oled.setCursor(104, 0); oled.printf("%d/%d", presetPedido + 1, N_PRESETS);

  // brillo: barra con el centro marcado
  oled.drawRect(0, 22, 128, 8, SSD1306_WHITE);
  oled.drawFastVLine(64, 20, 12, SSD1306_WHITE);
  int x = 64 + brillo * 62 / 12;
  if (x >= 64) oled.fillRect(64, 24, x - 64 + 1, 4, SSD1306_WHITE);
  else         oled.fillRect(x, 24, 64 - x, 4, SSD1306_WHITE);

  oled.setCursor(0, 38);
  if (!hayMcp) oled.print("TECLADO NO RESPONDE");
  else if (arpModo) oled.printf("ARP %s", NOMBRE_ARP[arpModo]);
  else oled.printf("%s", ultimaNota);
  oled.setCursor(84, 38);
  if (arpModo || mostrarTempo) oled.printf("%3d BPM", (int)bpm);
  else oled.printf("voces %d", (int)vocesSonando);

  const char *pedal = arpModo ? "LATCH" : "SOST";
  int ancho = strlen(pedal) * 6 + 5;
  if (sostenido) { oled.fillRect(0, 51, ancho, 11, SSD1306_WHITE); oled.setTextColor(SSD1306_BLACK); }
  oled.setCursor(3, 53); oled.print(pedal);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(72, 53); oled.printf("CPU %2d%%", (int)(cargaCPU * 100));

  if (aviso[0] && millis() - tAviso < 1200) {             // el último cambio, en grande
    oled.fillRect(0, 18, 128, 30, SSD1306_BLACK);
    oled.drawRect(0, 18, 128, 30, SSD1306_WHITE);
    int largo = strlen(aviso), tam = largo * 12 <= 120 ? 2 : 1;
    oled.setTextSize(tam);
    oled.setCursor((128 - largo * 6 * tam) / 2, tam == 2 ? 26 : 29);
    oled.print(aviso);
    oled.setTextSize(1);
  }
  oled.display();
}

// Mide cuánto del núcleo usan las 8 voces de cada sonido (sin efectos) y lo escribe por serie.
void medirSonidos() {
  static float L[BLOQUE], R[BLOQUE];
  const float ciclosBloque = (float)BLOQUE / SR * getCpuFrequencyMhz() * 1e6f;
  for (int p = 0; p < N_PRESETS; p++) {
    presetActual = p; calcularCoef(PRESETS[p]); calcFormantes(PRESETS[p], 1);
    for (int i = 0; i < NVOCES; i++) voces[i].activa = false;
    for (int i = 0; i < NVOCES; i++) notaOn(12 + i * 4);
    uint32_t t0 = ESP.getCycleCount();
    for (int b = 0; b < 40; b++) {
      memset(L, 0, sizeof(L)); memset(R, 0, sizeof(R));
      for (int i = 0; i < NVOCES; i++) if (voces[i].activa) renderVoz(voces[i], L, R, 1, 1);
    }
    uint32_t t = ESP.getCycleCount() - t0;
    Serial.printf("Medición %-8s: 8 voces = %d%% del núcleo (sin efectos)\n", PRESETS[p].nombre, (int)(100.0f * t / 40 / ciclosBloque));
  }
  for (int i = 0; i < NVOCES; i++) { voces[i].activa = false; voces[i].et = 0; }
  presetActual = -1;
}

// ============================================================
//  SETUP / LOOP
// ============================================================
void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) delay(10);
  Serial.println("\n=== SYNTH POLIFONICO CEREBRO ===");

  for (int i = 0; i <= TAB; i++) SENO[i] = sinf(2 * PI * i / TAB);
  medirSonidos();

  pinMode(ENC_A, INPUT_PULLUP); pinMode(ENC_B, INPUT_PULLUP); pinMode(ENC_SW, INPUT_PULLUP);
  pinMode(BTN_1, INPUT_PULLUP); pinMode(BTN_2, INPUT_PULLUP);
  for (int i = 0; i < 3; i++) pinMode(SEL_MUX[i], OUTPUT);
  pinMode(PIN_BT1, INPUT_PULLUP); pinMode(PIN_BT2, INPUT_PULLUP);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_POT, ADC_11db);
  leerControl(true);                                       // las perillas fijan los parámetros desde el arranque
  encEstado = (digitalRead(ENC_A) << 1) | digitalRead(ENC_B);
  attachInterrupt(ENC_A, isrEncoder, CHANGE);
  attachInterrupt(ENC_B, isrEncoder, CHANGE);

  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  for (uint8_t a : {0x3C, 0x3D}) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0 && oled.begin(SSD1306_SWITCHCAPVCC, a)) { hayOled = true; break; }
  }
  hayMcp = iniciarMcp();
  Serial.printf("OLED %s | MCP23017 %s\n", hayOled ? "OK" : "--", hayMcp ? "OK" : "NO RESPONDE");

  i2s.setPins(I2S_BCK, I2S_LCK, I2S_DIN);
  if (!i2s.begin(I2S_MODE_STD, SR, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO))
    Serial.println("DAC: no pude iniciar I2S");

  for (float seg : {0.6f, 0.4f, 0.25f}) {             // eco: 2 × 0,6 s si hay memoria, si no menos
    largoEco = (int)(2 * seg * SR) + 1;
    bufEco = (int16_t *)calloc(largoEco, sizeof(int16_t));
    if (bufEco) break;
  }
  Serial.printf("Eco: %s (%d muestras) | memoria libre %u bytes\n", bufEco ? "OK" : "sin memoria", bufEco ? largoEco : 0, ESP.getFreeHeap());

  xTaskCreatePinnedToCore(tareaAudio, "audio", 8192, nullptr, 10, nullptr, 0);
  Serial.println("B1 = sonido · B2 = arpegio (apretado + encoder = tempo) · encoder = brillo · pulsador = sostenido/latch");
  Serial.println("Control: P1 brillo · P2 ataque · P3 cola · P4 chorus · P5 eco · P6 reverb · P7 tempo · P8 volumen");
}

void loop() {
  static uint64_t estable = 0, bajosAnt = 0;
  static uint32_t tReintento = 0, tOled = 0, tLog = 0;
  static bool sucio = true, diodosInformado = false;

  // ---- teclado ----
  if (!hayMcp) {
    if (millis() - tReintento > 1000) { tReintento = millis(); hayMcp = iniciarMcp(); sucio = true; }
  } else if (!escanearTeclado()) {
    hayMcp = false; estable = 0; publicarTeclas(0); sucio = true;
    Serial.println("MCP23017 dejó de responder.");
  } else {
    uint64_t raw = leerMatriz();
    uint64_t nuevas = raw & ~estable;
    uint64_t bajos = estable & ~raw;
    uint64_t sueltas = bajos & bajosAnt;            // soltar solo tras 2 lecturas seguidas (antirrebote)
    bajosAnt = bajos;
    estable = (estable | nuevas) & ~sueltas;
    if (nuevas | sueltas) {
      publicarTeclas(estable);
      for (int n = 0; n < N_TECLAS; n++) if (nuevas & (1ULL << n)) {
        nombreNota(n, ultimaNota, sizeof(ultimaNota));
        Serial.printf("+ %s\n", ultimaNota);
      }
      sucio = true;
    }
    if (!diodosInformado && lecturasUnaDir + lecturasDosDir > 200) {
      diodosInformado = true;
      if (lecturasDosDir > lecturasUnaDir)
        Serial.println("Teclado SIN diodos: con 3 teclas en ángulo puede sonar una 4ª nota fantasma.");
      else
        Serial.println("Teclado CON diodos: acordes sin notas fantasma.");
    }
  }

  // ---- botones y encoder ----
  static bool swAnt = false, b1Ant = false, b2Ant = false, b2Giro = false;
  static uint32_t tBoton = 0;
  bool sw = !digitalRead(ENC_SW), b1 = !digitalRead(BTN_1), b2 = !digitalRead(BTN_2);

  noInterrupts(); int32_t c = encCuentas; interrupts();
  static int32_t pasosAnt = 0;
  int32_t pasos = c / 4;
  if (pasos != pasosAnt) {
    int d = pasos - pasosAnt; pasosAnt = pasos;
    if (b2) {                                        // B2 apretado: tempo
      int t = bpm + 2 * d; bpm = t < 40 ? 40 : (t > 160 ? 160 : t); b2Giro = true;
    } else {
      int b = brillo + d; brillo = b < -12 ? -12 : (b > 12 ? 12 : b);
    }
    sucio = true;
  }

  if (millis() - tBoton > 30) {
    if (sw && !swAnt) { sostenido = !sostenido; sucio = true; tBoton = millis(); }
    if (b1 && !b1Ant) { presetPedido = (presetPedido + 1) % N_PRESETS; sucio = true; tBoton = millis(); }
    if (b2 && !b2Ant) { b2Giro = false; mostrarTempo = true; sucio = true; tBoton = millis(); }
    if (!b2 && b2Ant) {                              // B2 soltado sin girar: cambia el modo de arpegio
      if (!b2Giro) arpModo = (arpModo + 1) % 4;
      mostrarTempo = false; sucio = true; tBoton = millis();
    }
    swAnt = sw; b1Ant = b1; b2Ant = b2;
  }

  // ---- placa Control ----
  static uint32_t tControl = 0;
  if (millis() - tControl >= 15) {
    tControl = millis();
    uint32_t avisoAntes = tAviso;
    leerControl(false);
    if (tAviso != avisoAntes) sucio = true;
  }
  static bool avisoVisible = false;
  bool av = aviso[0] && millis() - tAviso < 1200;
  if (av != avisoVisible) { avisoVisible = av; sucio = true; }

  // ---- pantalla (cada vez que cambia algo, máx. 8 veces por segundo) ----
  if (millis() - tOled > 500) sucio = true;          // refresca voces y CPU
  if (sucio && millis() - tOled > 120) { tOled = millis(); sucio = false; dibujar(); }

  if (millis() - tLog > 2000) {
    tLog = millis();
    float lm = limMin; limMin = 1;
    Serial.printf("[%s] arp %s %d BPM | brillo %d | voces %d  CPU audio %d%% | limitador %.1f dB\n", PRESETS[presetPedido].nombre,
                  arpModo ? NOMBRE_ARP[arpModo] : "--", (int)bpm, (int)brillo, (int)vocesSonando, (int)(cargaCPU * 100),
                  20 * log10f(lm));
  }
}
