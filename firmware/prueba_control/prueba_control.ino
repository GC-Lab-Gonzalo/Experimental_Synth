// ============================================================
//  PRUEBA PLACA CONTROL v6 — se carga en el ESP32 de la Cerebro
//  La Control se une a la Cerebro con el cable de 10 hilos (J1 ↔ J2, 1:1).
//
//  Lee los 3 multiplexores CD74HC4051:
//    U1 → 8 potenciómetros (común POT, GPIO4, analógico)
//    U2 → switches S1–S8   (común BT1, GPIO14)
//    U3 → switches S9–S16  (común BT2, GPIO18)
//  S0/S1/S2 (GPIO15/16/17) eligen el canal en los tres a la vez.
//
//  OLED: 8 columnas; la barra es el potenciómetro y los dos cuadros de abajo
//  son el switch de la fila 1 y el de la fila 2 de esa columna.
//  Monitor serie (115200): cada cambio y un diagnóstico automático: si al mover
//  un potenciómetro se mueven dos columnas a la vez, dice qué línea S0/S1/S2 falla.
//
//  SIN_COMPONENTES = 1: para probar los multiplexores ANTES de soldar potenciómetros y
//  switches. Se simula cada uno con un cable o una pinza:
//    potenciómetro: une la pata del medio de sus 3 pads con una de las de los extremos
//                   (con una llega a 3V3 y la columna se llena; con la otra queda en GND)
//    switch:        une sus 2 pads redondos (es como apretarlo)
//  La prueba avisa qué columna se activó; si se activan dos a la vez, dice qué
//  línea S0/S1/S2 falla.
//
//  Compilar con «USB CDC On Boot: Enabled».
// ============================================================
#define SIN_COMPONENTES 1
#include "driver/gpio.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define PIN_SDA 8
#define PIN_SCL 9
#define PIN_POT 4
#define PIN_BT1 14
#define PIN_BT2 18
const uint8_t SEL[3] = {15, 16, 17};          // S0, S1, S2
const uint8_t COL_CH[8] = {3, 0, 1, 2, 4, 6, 7, 5};   // columna → canal del 4051 (medido en el KiCad)

Adafruit_SSD1306 oled(128, 64, &Wire, -1);
bool hayOled = false;

int pot[8], potAnt[8];
bool fila1[8], fila2[8], fila1Ant[8], fila2Ant[8];

void elegirCanal(uint8_t ch) { for (int i = 0; i < 3; i++) digitalWrite(SEL[i], (ch >> i) & 1); }

void leerTodo() {
  for (int c = 0; c < 8; c++) {
    elegirCanal(COL_CH[c]);
    delayMicroseconds(30);
    analogRead(PIN_POT);                       // primera lectura descartada (el ADC carga su condensador)
    int suma = 0;
    for (int k = 0; k < 4; k++) suma += analogRead(PIN_POT);
    pot[c] = suma / 4;
#if SIN_COMPONENTES
    gpio_pulldown_en((gpio_num_t)PIN_POT);     // canal al aire → 0 (si el ADC respeta la pull-down)
#endif
    fila1[c] = !digitalRead(PIN_BT1);
    fila2[c] = !digitalRead(PIN_BT2);
  }
}

// Si dos columnas cambian juntas con el mismo valor, sus canales comparten todo menos
// un bit de selección: esa línea (S0, S1 o S2) no está llegando a los multiplexores.
void diagnosticarSeleccion(const bool *movidas) {
  static uint32_t tAviso = 0;
  if (millis() - tAviso < 2000) return;
  for (int a = 0; a < 8; a++) for (int b = a + 1; b < 8; b++) {
    if (!movidas[a] || !movidas[b] || abs(pot[a] - pot[b]) > 60) continue;
    uint8_t x = COL_CH[a] ^ COL_CH[b];
    Serial.printf("OJO: las columnas %d y %d se mueven juntas (canales %d y %d).", a + 1, b + 1, COL_CH[a], COL_CH[b]);
    if (x == 1 || x == 2 || x == 4)
      Serial.printf(" Revisa la línea S%d: J1 pin %d → pata %d de U1, U2 y U3.\n", x == 1 ? 0 : (x == 2 ? 1 : 2),
                    x == 1 ? 7 : (x == 2 ? 8 : 9), x == 1 ? 11 : (x == 2 ? 10 : 9));
    else Serial.println(" Revisa S0, S1 y S2 (J1 pines 7, 8, 9).");
    tAviso = millis();
    return;
  }
}

void dibujar() {
  if (!hayOled) return;
  oled.clearDisplay();
  for (int c = 0; c < 8; c++) {
    int x = c * 16 + 2;
    int h = pot[c] * 38 / 4095;
    oled.drawRect(x, 0, 12, 40, SSD1306_WHITE);
    oled.fillRect(x, 40 - h, 12, h, SSD1306_WHITE);
    if (fila1[c]) oled.fillRect(x + 1, 44, 10, 8, SSD1306_WHITE); else oled.drawRect(x + 1, 44, 10, 8, SSD1306_WHITE);
    if (fila2[c]) oled.fillRect(x + 1, 55, 10, 8, SSD1306_WHITE); else oled.drawRect(x + 1, 55, 10, 8, SSD1306_WHITE);
  }
  oled.display();
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) delay(10);
  Serial.println("\n=== PRUEBA PLACA CONTROL (multiplexores) ===");

  for (int i = 0; i < 3; i++) pinMode(SEL[i], OUTPUT);
  pinMode(PIN_BT1, INPUT_PULLUP);
  pinMode(PIN_BT2, INPUT_PULLUP);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_POT, ADC_11db);   // 0 a ~3,1 V

  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  hayOled = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);

  leerTodo();
  for (int c = 0; c < 8; c++) { potAnt[c] = pot[c]; fila1Ant[c] = fila1[c]; fila2Ant[c] = fila2[c]; }

  // revisión inicial
  Serial.print("Potenciómetros (0-100 %):");
  for (int c = 0; c < 8; c++) Serial.printf(" P%d=%d", c + 1, pot[c] * 100 / 4095);
  Serial.println();
  int iguales = 0;
  for (int c = 1; c < 8; c++) if (abs(pot[c] - pot[0]) < 40) iguales++;
  bool todoArriba = true, todoAbajo = true;
  for (int c = 0; c < 8; c++) { if (pot[c] < 4000) todoArriba = false; if (pot[c] > 60) todoAbajo = false; }
  if (SIN_COMPONENTES) Serial.println("Modo sin componentes: une el pad del medio de cada potenciómetro con un extremo, y los 2 pads de cada switch.");
  else if (todoAbajo) Serial.println("OJO: los 8 leen 0. Revisa POT (J1 pin 5 → U1 pata 3) y la alimentación de U1 (pata 16 = 3V3, 6/7/8 = GND).");
  else if (todoArriba) Serial.println("OJO: los 8 leen el máximo. Revisa GND de los potenciómetros y de U1 (patas 6, 7, 8).");
  else if (iguales == 7) Serial.println("Los 8 leen lo mismo: si los tienes en posiciones distintas, S0/S1/S2 no están llegando.");

  int f1 = 0, f2 = 0;
  for (int c = 0; c < 8; c++) { f1 += fila1[c]; f2 += fila2[c]; }
  if (f1 == 8) Serial.println("OJO: la fila 1 (S1-S8) se lee toda apretada. Revisa BT1 (J1 pin 6 → U2 pata 3) y GND de U2.");
  if (f2 == 8) Serial.println("OJO: la fila 2 (S9-S16) se lee toda apretada. Revisa BT2 (J1 pin 10 → U3 pata 3) y GND de U3.");
  Serial.println("Gira cada potenciómetro y aprieta cada switch: solo debe moverse su columna.");
}

#if SIN_COMPONENTES
// Columnas activas a la vez: si son dos y sus canales difieren en un solo bit, esa S no llega.
void diagnosticarGrupo(const bool *activas, const char *que) {
  static uint32_t tAviso = 0;
  int n = 0; for (int c = 0; c < 8; c++) n += activas[c];
  if (n < 2 || millis() - tAviso < 2000) return;
  for (int a = 0; a < 8; a++) for (int b = a + 1; b < 8; b++) {
    if (!activas[a] || !activas[b]) continue;
    uint8_t x = COL_CH[a] ^ COL_CH[b];
    Serial.printf("OJO (%s): las columnas %d y %d se activan juntas (canales %d y %d).", que, a + 1, b + 1, COL_CH[a], COL_CH[b]);
    if (x == 1 || x == 2 || x == 4)
      Serial.printf(" Revisa la línea S%d: J1 pin %d → pata %d de U1, U2 y U3.\n", x == 1 ? 0 : (x == 2 ? 1 : 2),
                    x == 1 ? 7 : (x == 2 ? 8 : 9), x == 1 ? 11 : (x == 2 ? 10 : 9));
    else Serial.println(" Revisa S0, S1 y S2 (J1 pines 7, 8, 9).");
    tAviso = millis();
    return;
  }
}

void loop() {
  static uint32_t tAnt = 0;
  static bool altoAnt[8] = {false};
  if (millis() - tAnt < 20) return;
  tAnt = millis();
  leerTodo();

  bool alto[8];
  for (int c = 0; c < 8; c++) {
    alto[c] = pot[c] > 3900;                       // pad del medio unido a 3V3
    if (alto[c] != altoAnt[c]) {
      Serial.printf("P%d %s\n", c + 1, alto[c] ? "en 3V3  (U1 canal OK)" : "suelto");
      altoAnt[c] = alto[c];
    }
    if (fila1[c] != fila1Ant[c]) { Serial.printf("S%d %s\n", c + 1, fila1[c] ? "unido  (U2 canal OK)" : "suelto"); fila1Ant[c] = fila1[c]; }
    if (fila2[c] != fila2Ant[c]) { Serial.printf("S%d %s\n", c + 9, fila2[c] ? "unido  (U3 canal OK)" : "suelto"); fila2Ant[c] = fila2[c]; }
  }
  diagnosticarGrupo(alto, "potenciómetros/U1");
  diagnosticarGrupo(fila1, "fila 1/U2");
  diagnosticarGrupo(fila2, "fila 2/U3");

  static uint32_t tOled = 0;
  if (millis() - tOled > 60) { tOled = millis(); dibujar(); }
}
#else
void loop() {
  static uint32_t tAnt = 0;
  if (millis() - tAnt < 20) return;
  tAnt = millis();
  leerTodo();

  bool movidas[8] = {false};
  int nMovidas = 0;
  for (int c = 0; c < 8; c++) {
    if (abs(pot[c] - potAnt[c]) > 80) {
      Serial.printf("P%d = %d %%\n", c + 1, pot[c] * 100 / 4095);
      movidas[c] = true; nMovidas++;
      potAnt[c] = pot[c];
    }
    if (fila1[c] != fila1Ant[c]) { Serial.printf("S%d %s\n", c + 1, fila1[c] ? "APRETADO" : "suelto"); fila1Ant[c] = fila1[c]; }
    if (fila2[c] != fila2Ant[c]) { Serial.printf("S%d %s\n", c + 9, fila2[c] ? "APRETADO" : "suelto"); fila2Ant[c] = fila2[c]; }
  }
  if (nMovidas >= 2) diagnosticarSeleccion(movidas);

  static uint32_t tOled = 0;
  if (millis() - tOled > 60) { tOled = millis(); dibujar(); }
}
#endif
