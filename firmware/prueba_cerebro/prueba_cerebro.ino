// ============================================================
//  PRUEBA PLACA CEREBRO v6 — ESP32-S3-DevKitC-1 (sintetizador de madera)
//  - Teclado de 15 cables por el MCP23017 (0x20): descubre solo qué par de
//    cables se une al apretar cada tecla (no hace falta saber filas/columnas)
//  - Encoder EC11 (A 40 · B 39 · pulsador 38)
//  - Pulsadores B1 (GPIO44) y B2 (GPIO45) bajo la OLED
//  - Todo se ve en la OLED y en el Monitor Serie (115200)
//  - Opcional: un pitido corto por el DAC con cada tecla, para probarlo
//    cuando ya tenga los 5 V
//
//  Compilar con «USB CDC On Boot: Enabled» y enchufar por el USB nativo:
//  GPIO44 es el RX del puerto serie de abajo y aquí lo usamos para B1.
// ============================================================
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP_I2S.h>

// ---- Pines (sacados de la placa en KiCad) ----
#define PIN_SDA   8
#define PIN_SCL   9
#define ENC_A     40
#define ENC_B     39
#define ENC_SW    38
#define BTN_1     44
#define BTN_2     45     // pin de arranque: solo pull-up interna, después de arrancar
#define I2S_BCK   5
#define I2S_LCK   6
#define I2S_DIN   7

uint8_t MCP_ADDR = 0x20;   // se busca en 0x20..0x27 por si A0-A2 no quedaron a GND

// 1 = el cable 1 del teclado quedó soldado en B6 y el 15 en A0 (al revés del dibujo).
// 0 = como el dibujo (cable 1 en A0 ... cable 15 en B6).
// Solo cambia el número de cable que se muestra; la lectura funciona igual.
#define TECLADO_INVERTIDO 1

#define PITIDO_DAC 1     // 1 = pitido por el DAC con cada tecla nueva; 0 = silencio

// Registros del MCP23017 (BANK = 0, el modo de fábrica)
#define R_IODIR 0x00
#define R_IPOL  0x02
#define R_GPPU  0x0C
#define R_GPIO  0x12
#define R_OLAT  0x14

#define N_LINEAS 15
#define MASCARA  0x7FFF   // A0..A7 + B0..B6 (B7 no se usa)

Adafruit_SSD1306 oled(128, 64, &Wire, -1);
I2SClass i2s;

bool hayOled = false, hayMcp = false, hayI2S = false;

// ---------- MCP23017 ----------
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
  if (!hay) return false;
  return mcpEscribir16(R_IPOL, 0x0000) &&
         mcpEscribir16(R_OLAT, 0x0000) &&      // la línea que se active como salida queda en LOW
         mcpEscribir16(R_GPPU, 0xFFFF) &&      // pull-up interna (100 k) en todas
         mcpEscribir16(R_IODIR, 0xFFFF);       // todas entradas en reposo
}

// índice 0..14 = A0..A7, B0..B6
const char *nombrePin(uint8_t i) {
  static const char *n[N_LINEAS] = {"A0","A1","A2","A3","A4","A5","A6","A7",
                                    "B0","B1","B2","B3","B4","B5","B6"};
  return n[i];
}

uint8_t cable(uint8_t i) { return TECLADO_INVERTIDO ? N_LINEAS - i : i + 1; }

// Matriz medida el 2026-11-17 tocando de la tecla más grave a la más aguda (49 teclas, Do a Do):
//  - columnas = cables 8,7,6,5,4,3,2,1 (de grave a agudo dentro de cada grupo)
//  - filas    = cables 10,11,12,13,14,15 (8 teclas cada una) y el 9 solo para la tecla 49 (8+9)
// Devuelve 0..48 (0 = la más grave) o -1 si ese par no es una tecla.
#define NOTA_BASE_HZ 130.81f   // Do3 en la tecla más grave
int notaDeTecla(uint8_t a, uint8_t b) {           // a < b, en números de cable
  if (a < 1 || a > 8 || b < 9 || b > 15) return -1;
  int col = 8 - a;
  int fila = (b == 9) ? 6 : b - 10;
  int n = fila * 8 + col;
  return n <= 48 ? n : -1;
}

void nombreNota(int n, char *s, size_t len) {
  static const char *NOM[12] = {"Do","Do#","Re","Re#","Mi","Fa","Fa#","Sol","Sol#","La","La#","Si"};
  snprintf(s, len, "%s%d", NOM[n % 12], 3 + n / 12);
}

uint16_t unidas[N_LINEAS];       // unidas[d] = líneas que bajan cuando d está en LOW
uint16_t unidasAnt[N_LINEAS];
uint16_t pegadasGnd = 0;         // líneas que están en LOW sin que nadie las active
uint16_t activas = 0;            // líneas que participan en alguna tecla apretada

// Activa una línea a la vez y mira cuáles otras bajan. Así encuentra cada par
// de cables que una tecla une, sin saber cuáles son filas y cuáles columnas.
bool escanearTeclado() {
  uint16_t g;
  if (!mcpEscribir16(R_IODIR, 0xFFFF)) return false;
  delayMicroseconds(50);
  if (!mcpLeer16(R_GPIO, g)) return false;
  pegadasGnd = ~g & MASCARA;

  activas = 0;
  for (uint8_t d = 0; d < N_LINEAS; d++) {
    unidas[d] = 0;
    if (pegadasGnd & (1 << d)) continue;
    mcpEscribir16(R_IODIR, 0xFFFF & ~(1 << d));
    delayMicroseconds(40);
    if (!mcpLeer16(R_GPIO, g)) return false;
    unidas[d] = ~g & MASCARA & ~(1 << d) & ~pegadasGnd;
  }
  mcpEscribir16(R_IODIR, 0xFFFF);

  // simétrico: si d baja a k o k baja a d, el par está unido (sirve también con diodos)
  for (uint8_t d = 0; d < N_LINEAS; d++)
    for (uint8_t k = d + 1; k < N_LINEAS; k++)
      if ((unidas[d] & (1 << k)) || (unidas[k] & (1 << d))) {
        unidas[d] |= 1 << k; unidas[k] |= 1 << d;
        activas |= (1 << d) | (1 << k);
      }
  return true;
}

// ---------- encoder ----------
volatile int32_t encCuentas = 0;
volatile uint8_t encEstado = 0;
static const int8_t TABLA_ENC[16] = {0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0};

void IRAM_ATTR isrEncoder() {
  uint8_t s = (digitalRead(ENC_A) << 1) | digitalRead(ENC_B);
  encEstado = ((encEstado << 2) | s) & 0x0F;
  encCuentas += TABLA_ENC[encEstado];
}

// ---------- DAC ----------
void iniciarAudio() {
  i2s.setPins(I2S_BCK, I2S_LCK, I2S_DIN);
  hayI2S = i2s.begin(I2S_MODE_STD, 44100, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  Serial.println(hayI2S ? "DAC: I2S iniciado (BCK 5, LCK 6, DIN 7)" : "DAC: no pude iniciar I2S");
}

void pitido(float hz) {
#if PITIDO_DAC
  if (!hayI2S) return;
  static int16_t buf[256 * 2];
  float fase = 0, paso = 2 * PI * hz / 44100.0f;
  for (int b = 0; b < 12; b++) {               // ~70 ms
    for (int i = 0; i < 256; i++) {
      int n = b * 256 + i;
      float env = n < 3072 - 400 ? 1.0f : (3072 - n) / 400.0f;   // sin clic al final
      int16_t v = (int16_t)(5000 * env * sinf(fase));
      buf[2 * i] = v; buf[2 * i + 1] = v;
      fase += paso; if (fase > 2 * PI) fase -= 2 * PI;
    }
    i2s.write((uint8_t *)buf, sizeof(buf));
  }
  memset(buf, 0, sizeof(buf));
  for (int k = 0; k < 4; k++) i2s.write((uint8_t *)buf, sizeof(buf));
#endif
}

// ---------- utilidades ----------
char listaI2C[40] = "";

int escanearI2C() {
  int n = 0;
  listaI2C[0] = 0;
  Serial.print("I2C (SDA 8 / SCL 9):");
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf(" 0x%02X", a); n++;
      size_t l = strlen(listaI2C);
      if (l < sizeof(listaI2C) - 4) snprintf(listaI2C + l, sizeof(listaI2C) - l, "%02X ", a);
    }
  }
  Serial.printf("  (%d dispositivos; se esperan 0x20 MCP23017 y 0x3C OLED)\n", n);
  return n;
}

char ultimo[24] = "-";

void dibujar(int32_t pasos, bool sw, bool b1, bool b2) {
  if (!hayOled) return;
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setCursor(0, 0);

  if (!hayMcp) {
    oled.println("MCP NO RESPONDE");
    oled.print("I2C: "); oled.println(listaI2C[0] ? listaI2C : "nada");
    oled.println("(3C = OLED)");
  } else {
    // teclas apretadas ahora, como pares de cables
    if (MCP_ADDR != 0x20) { oled.printf("MCP en %02X! ", MCP_ADDR); }
    oled.print("TECLA ");
    int n = 0;
    for (uint8_t d = 0; d < N_LINEAS && n < 3; d++)
      for (uint8_t k = d + 1; k < N_LINEAS && n < 3; k++)
        if (unidas[d] & (1 << k)) {
          uint8_t a = cable(d), b = cable(k);
          if (a > b) { uint8_t t = a; a = b; b = t; }
          oled.printf("%d+%d ", a, b); n++;
        }
    if (n == 0) oled.print("-");

    // 15 casillas, una por cable (1 a la izquierda, 15 a la derecha)
    for (uint8_t c = 1; c <= N_LINEAS; c++) {
      uint8_t i = TECLADO_INVERTIDO ? N_LINEAS - c : c - 1;
      int x = 4 + (c - 1) * 8, y = 11;
      if (pegadasGnd & (1 << i))      { oled.drawLine(x, y, x + 6, y + 6, SSD1306_WHITE); oled.drawLine(x + 6, y, x, y + 6, SSD1306_WHITE); }
      else if (activas & (1 << i))    oled.fillRect(x, y, 7, 7, SSD1306_WHITE);
      else                            oled.drawRect(x, y, 7, 7, SSD1306_WHITE);
    }
    oled.setCursor(4, 20);  oled.print("1");
    oled.setCursor(36, 20); oled.print("5");
    oled.setCursor(73, 20); oled.print("10");
    oled.setCursor(113, 20); oled.print("15");
  }

  oled.setCursor(0, 32);
  oled.printf("ENC %ld", (long)pasos);
  oled.setCursor(80, 32);
  if (sw) { oled.fillRect(78, 31, 26, 9, SSD1306_WHITE); oled.setTextColor(SSD1306_BLACK); }
  oled.print("SW");
  oled.setTextColor(SSD1306_WHITE);

  oled.setCursor(0, 44);
  oled.print("B1 "); if (b1) oled.fillRect(18, 44, 7, 7, SSD1306_WHITE); else oled.drawRect(18, 44, 7, 7, SSD1306_WHITE);
  oled.setCursor(44, 44);
  oled.print("B2 "); if (b2) oled.fillRect(62, 44, 7, 7, SSD1306_WHITE); else oled.drawRect(62, 44, 7, 7, SSD1306_WHITE);

  oled.setCursor(0, 56);
  oled.printf("ult: %s", ultimo);
  oled.display();
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) delay(10);
  Serial.println("\n=== PRUEBA PLACA CEREBRO ===");
  Serial.printf("Teclado %s (cable 1 en %s)\n", TECLADO_INVERTIDO ? "INVERTIDO" : "normal",
                TECLADO_INVERTIDO ? "B6" : "A0");

  pinMode(ENC_A, INPUT_PULLUP);
  pinMode(ENC_B, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);
  pinMode(BTN_1, INPUT_PULLUP);
  pinMode(BTN_2, INPUT_PULLUP);
  encEstado = (digitalRead(ENC_A) << 1) | digitalRead(ENC_B);
  attachInterrupt(ENC_A, isrEncoder, CHANGE);
  attachInterrupt(ENC_B, isrEncoder, CHANGE);

  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  delay(50);
  escanearI2C();

  for (uint8_t a : {0x3C, 0x3D}) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0 && oled.begin(SSD1306_SWITCHCAPVCC, a)) { hayOled = true; break; }
  }
  oled.setTextColor(SSD1306_WHITE);
  hayMcp = iniciarMcp();
  Serial.printf("OLED %s | MCP23017 %s\n", hayOled ? "OK" : "--", hayMcp ? "OK" : "NO RESPONDE");

  if (hayMcp && escanearTeclado() && pegadasGnd) {
    Serial.print("OJO: estas líneas están a GND sin apretar nada (corto o puente de estaño):");
    for (uint8_t i = 0; i < N_LINEAS; i++)
      if (pegadasGnd & (1 << i)) Serial.printf(" cable %d (%s)", cable(i), nombrePin(i));
    Serial.println();
  }

#if PITIDO_DAC
  iniciarAudio();
  pitido(660);
#endif
  Serial.println("Aprieta teclas, gira el encoder y prueba B1/B2.");
}

void loop() {
  static uint32_t tAnt = 0, tReintento = 0;
  if (millis() - tAnt < 10) return;
  tAnt = millis();

  // teclado
  if (!hayMcp && millis() - tReintento > 1000) {
    tReintento = millis();
    escanearI2C();
    hayMcp = iniciarMcp();
    if (hayMcp) Serial.println("MCP23017 apareció.");
  }
  if (hayMcp && !escanearTeclado()) {
    hayMcp = false;
    Serial.println("MCP23017 dejó de responder.");
  }
  if (hayMcp) {
    for (uint8_t d = 0; d < N_LINEAS; d++) {
      uint16_t nuevas = unidas[d] & ~unidasAnt[d], sueltas = unidasAnt[d] & ~unidas[d];
      for (uint8_t k = d + 1; k < N_LINEAS; k++) {
        uint8_t a = cable(d), b = cable(k);
        if (a > b) { uint8_t t = a; a = b; b = t; }
        if (nuevas & (1 << k)) {
          int n = notaDeTecla(a, b);
          char nom[8] = "?";
          if (n >= 0) nombreNota(n, nom, sizeof(nom));
          Serial.printf("TECLA %2d %-5s  cables %2d + %2d   (MCP %s + %s)\n", n + 1, nom, a, b, nombrePin(d), nombrePin(k));
          if (n >= 0) snprintf(ultimo, sizeof(ultimo), "tecla %d %s", n + 1, nom);
          else        snprintf(ultimo, sizeof(ultimo), "%d+%d ?", a, b);
          if (n >= 0) pitido(NOTA_BASE_HZ * powf(2.0f, n / 12.0f));
        }
        if (sueltas & (1 << k))
          Serial.printf("suelta cables %2d + %2d\n", a, b);
      }
      unidasAnt[d] = unidas[d];
    }
  }

  // encoder (EC11 normal: 4 cuentas por clic; si salta de a 2, el tuyo es de 2)
  noInterrupts(); int32_t c = encCuentas; interrupts();
  int32_t pasos = c / 4;
  static int32_t pasosAnt = 0;
  if (pasos != pasosAnt) {
    Serial.printf("ENCODER %ld (%s)  cuentas %ld\n", (long)pasos, pasos > pasosAnt ? "+" : "-", (long)c);
    pasosAnt = pasos;
  }

  bool sw = !digitalRead(ENC_SW), b1 = !digitalRead(BTN_1), b2 = !digitalRead(BTN_2);
  static bool swAnt = false, b1Ant = false, b2Ant = false;
  if (sw != swAnt) { Serial.printf("ENCODER pulsador %s\n", sw ? "APRETADO" : "suelto"); swAnt = sw; if (sw) { strcpy(ultimo, "ENC SW"); pitido(880); } }
  if (b1 != b1Ant) { Serial.printf("B1 %s\n", b1 ? "APRETADO" : "suelto"); b1Ant = b1; if (b1) { strcpy(ultimo, "B1"); pitido(523); } }
  if (b2 != b2Ant) { Serial.printf("B2 %s\n", b2 ? "APRETADO" : "suelto"); b2Ant = b2; if (b2) { strcpy(ultimo, "B2"); pitido(659); } }

  static uint32_t tOled = 0;
  if (millis() - tOled >= 50) { tOled = millis(); dibujar(pasos, sw, b1, b2); }
}
