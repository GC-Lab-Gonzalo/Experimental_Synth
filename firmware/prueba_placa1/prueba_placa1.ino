// ============================================================
//  PRUEBA PLACA 1 — ESP32-S3 SuperMini (sintetizador de madera)
//  - Escanea los dos buses I2C y lee cada sensor que encuentre
//  - Muestra todo en la OLED y en el Monitor Serie (115200)
//  - Anima los 2 anillos WS2812 (24 + 24) en el GPIO6
//  - Hace sonar un tono corto por el DAC PCM5102A al partir
//  Radar y sensor UV no se usan.
// ============================================================
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>
#include <ESP_I2S.h>

// ---- Pines (sacados de la placa en KiCad) ----
#define PIN_SDA    4    // bus principal: OLED, BMP180, VEML7700, ENS160+AHT21, HUMEDAD
#define PIN_SCL    7
#define PIN_SDA2   3    // segundo bus: HUMEDAD2
#define PIN_SCL2   2
#define PIN_LEDS   6    // header "5 6 GND", pin 6
#define I2S_BCK    11
#define I2S_LCK    12
#define I2S_DIN    13

#define NUM_LEDS   48   // 2 anillos de 24 en cadena
#define BRILLO     40   // 0-255. Bajo a propósito: 48 LED a full piden ~3 A
#define TONO_INICIO 1   // 1 = pitido cada ~3 s por el DAC; 0 = sin audio

Adafruit_SSD1306 oled(128, 64, &Wire, -1);
Adafruit_NeoPixel leds(NUM_LEDS, PIN_LEDS, NEO_GRB + NEO_KHZ800);
I2SClass i2s;

bool hayOled = false, hayBmp = false, hayVeml = false, hayEns = false, hayAht = false, haySht1 = false, haySht2 = false;
uint8_t ensAddr = 0x53;
int sdaPrincipal = PIN_SDA, sclPrincipal = PIN_SCL;

// ---------- utilidades I2C ----------
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

int escanear(TwoWire &w, const char *nombre) {
  int n = 0;
  Serial.printf("Bus %s:", nombre);
  for (uint8_t a = 1; a < 127; a++) if (existe(w, a)) { Serial.printf(" 0x%02X", a); n++; }
  Serial.printf("  (%d dispositivos)\n", n);
  return n;
}

// ---------- SHT40 (0x44) ----------
bool leerSHT40(TwoWire &w, float &t, float &h) {
  w.beginTransmission(0x44); w.write(0xFD);
  if (w.endTransmission() != 0) return false;
  delay(10);
  if (w.requestFrom((uint8_t)0x44, (uint8_t)6) != 6) return false;
  uint8_t b[6]; for (int i = 0; i < 6; i++) b[i] = w.read();
  t = -45 + 175.0f * ((b[0] << 8) | b[1]) / 65535.0f;
  h = -6 + 125.0f * ((b[3] << 8) | b[4]) / 65535.0f;
  h = constrain(h, 0, 100);
  return true;
}

// ---------- AHT21 (0x38) ----------
bool leerAHT21(float &t, float &h) {
  Wire.beginTransmission(0x38); Wire.write(0xAC); Wire.write(0x33); Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;
  delay(85);
  if (Wire.requestFrom((uint8_t)0x38, (uint8_t)6) != 6) return false;
  uint8_t b[6]; for (int i = 0; i < 6; i++) b[i] = Wire.read();
  if (b[0] & 0x80) return false;  // todavía midiendo
  uint32_t rh = ((uint32_t)b[1] << 12) | ((uint32_t)b[2] << 4) | (b[3] >> 4);
  uint32_t rt = ((uint32_t)(b[3] & 0x0F) << 16) | ((uint32_t)b[4] << 8) | b[5];
  h = rh * 100.0f / 1048576.0f;
  t = rt * 200.0f / 1048576.0f - 50;
  return true;
}

// ---------- ENS160 (0x53 o 0x52) ----------
bool iniciarENS160() {
  for (uint8_t a : {0x53, 0x52}) {
    uint8_t id[2];
    if (leerReg(Wire, a, 0x00, id, 2) && id[0] == 0x60 && id[1] == 0x01) {
      ensAddr = a;
      uint8_t modo = 0x02;  // modo estándar
      escribirReg(Wire, a, 0x10, &modo, 1);
      return true;
    }
  }
  return false;
}

bool leerENS160(uint8_t &aqi, uint16_t &tvoc, uint16_t &eco2, uint8_t &estado) {
  uint8_t st, b[5];
  if (!leerReg(Wire, ensAddr, 0x20, &st, 1)) return false;
  if (!leerReg(Wire, ensAddr, 0x21, b, 5)) return false;
  estado = (st >> 2) & 0x03;      // 0 normal, 1 calentando, 2 arranque inicial
  aqi = b[0] & 0x07;
  tvoc = b[1] | (b[2] << 8);
  eco2 = b[3] | (b[4] << 8);
  return true;
}

// ---------- VEML7700 (0x10) ----------
bool iniciarVEML7700() {
  uint8_t conf[2] = {0x00, 0x00};  // ganancia x1, 100 ms, encendido
  return escribirReg(Wire, 0x10, 0x00, conf, 2);
}

bool leerVEML7700(float &lux) {
  uint8_t b[2];
  if (!leerReg(Wire, 0x10, 0x04, b, 2)) return false;
  lux = (b[0] | (b[1] << 8)) * 0.0576f;   // aproximado para ganancia x1 y 100 ms
  return true;
}

// ---------- BMP180 (0x77) ----------
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
  cmd = 0x34;  // presión, oss = 0
  if (!escribirReg(Wire, 0x77, 0xF4, &cmd, 1)) return false;
  delay(5);
  if (!leerReg(Wire, 0x77, 0xF6, b, 3)) return false;
  int32_t UP = (((int32_t)b[0] << 16) | ((int32_t)b[1] << 8) | b[2]) >> 8;

  int32_t X1 = ((UT - (int32_t)AC6) * (int32_t)AC5) >> 15;
  int32_t X2 = ((int32_t)MCc << 11) / (X1 + MDc);
  int32_t B5 = X1 + X2;
  t = ((B5 + 8) >> 4) / 10.0f;
  int32_t B6 = B5 - 4000;
  X1 = (B2c * ((B6 * B6) >> 12)) >> 11;
  X2 = (AC2 * B6) >> 11;
  int32_t X3 = X1 + X2;
  int32_t B3 = ((((int32_t)AC1 * 4 + X3)) + 2) / 4;
  X1 = (AC3 * B6) >> 13;
  X2 = (B1c * ((B6 * B6) >> 12)) >> 16;
  X3 = ((X1 + X2) + 2) >> 2;
  uint32_t B4 = ((uint32_t)AC4 * (uint32_t)(X3 + 32768)) >> 15;
  uint32_t B7 = ((uint32_t)UP - B3) * 50000UL;
  int32_t p = (B7 < 0x80000000UL) ? (B7 * 2) / B4 : (B7 / B4) * 2;
  X1 = (p >> 8) * (p >> 8);
  X1 = (X1 * 3038) >> 16;
  X2 = (-7357 * p) >> 16;
  p += (X1 + X2 + 3791) >> 4;
  hpa = p / 100.0f;
  return true;
}

// ---------- audio ----------
bool hayI2S = false;

void iniciarAudio() {
  i2s.setPins(I2S_BCK, I2S_LCK, I2S_DIN);
  hayI2S = i2s.begin(I2S_MODE_STD, 44100, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  Serial.println(hayI2S ? "DAC: I2S iniciado (BCK 11, LCK 12, DIN 13)" : "DAC: no pude iniciar I2S");
}

// Pitido de ~0,5 s: 440 Hz por el canal izquierdo y 880 Hz por el derecho.
void tonoPrueba() {
  if (!hayI2S) return;
  static int16_t buf[256 * 2];
  static float fL = 0, fR = 0;
  const float pL = 2 * PI * 440.0f / 44100.0f, pR = 2 * PI * 880.0f / 44100.0f;
  for (int bloque = 0; bloque < 86; bloque++) {
    for (int i = 0; i < 256; i++) {
      buf[2 * i]     = (int16_t)(6000 * sinf(fL));
      buf[2 * i + 1] = (int16_t)(6000 * sinf(fR));
      fL += pL; if (fL > 2 * PI) fL -= 2 * PI;
      fR += pR; if (fR > 2 * PI) fR -= 2 * PI;
    }
    i2s.write((uint8_t *)buf, sizeof(buf));
  }
  memset(buf, 0, sizeof(buf));
  for (int k = 0; k < 8; k++) i2s.write((uint8_t *)buf, sizeof(buf));
  Serial.println("DAC: pitido enviado (izq 440 Hz, der 880 Hz)");
}

// ---------- LEDs ----------
void diagnosticoAnillos() {
  // Anillo 1 (LED 0-23) en rojo, anillo 2 (LED 24-47) en azul.
  // Si los dos se ven ROJOS, están en paralelo y no en cadena.
  leds.clear();
  for (int i = 0; i < 24; i++) leds.setPixelColor(i, leds.Color(255, 0, 0));
  for (int i = 24; i < 48; i++) leds.setPixelColor(i, leds.Color(0, 0, 255));
  leds.show();
}

void animarAnillos() {
  static uint16_t base = 0;
  static uint32_t tAnt = 0;
  if (millis() - tAnt < 20) return;
  tAnt = millis();
  for (int i = 0; i < NUM_LEDS; i++) {
    int j = i % 24;
    uint16_t tono = base + j * 65536L / 24 + (i >= 24 ? 32768 : 0);
    leds.setPixelColor(i, leds.gamma32(leds.ColorHSV(tono, 255, 255)));
  }
  // un punto blanco que recorre los 48 para ver el orden de la cadena
  leds.setPixelColor((base / 512) % NUM_LEDS, leds.Color(255, 255, 255));
  leds.show();
  base += 256;
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) delay(10);
  Serial.println("\n=== PRUEBA PLACA 1 ===");

  leds.begin();
  leds.setBrightness(BRILLO);
  diagnosticoAnillos();

  Wire.begin(PIN_SDA, PIN_SCL, 100000);
  Wire1.begin(PIN_SDA2, PIN_SCL2, 100000);
  delay(100);

  if (escanear(Wire, "principal (SDA 4 / SCL 7)") == 0) {
    // por si SDA y SCL quedaron al revés en la placa
    Wire.end();
    Wire.begin(PIN_SCL, PIN_SDA, 100000);
    if (escanear(Wire, "principal invertido (SDA 7 / SCL 4)") > 0) {
      sdaPrincipal = PIN_SCL; sclPrincipal = PIN_SDA;
      Serial.println("OJO: el bus principal responde con SDA y SCL invertidos.");
    } else {
      Wire.end();
      Wire.begin(PIN_SDA, PIN_SCL, 100000);
    }
  }
  escanear(Wire1, "HUMEDAD2 (SDA 3 / SCL 2)");

  hayOled = existe(Wire, 0x3C) && oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  hayBmp  = iniciarBMP180();
  hayVeml = existe(Wire, 0x10) && iniciarVEML7700();
  hayEns  = iniciarENS160();
  hayAht  = existe(Wire, 0x38);
  haySht1 = existe(Wire, 0x44);
  haySht2 = existe(Wire1, 0x44);

  Serial.printf("OLED %s | BMP180 %s | VEML7700 %s | ENS160 %s | AHT21 %s | SHT40 HUMEDAD %s | SHT40 HUMEDAD2 %s\n",
                hayOled ? "OK" : "--", hayBmp ? "OK" : "--", hayVeml ? "OK" : "--", hayEns ? "OK" : "--",
                hayAht ? "OK" : "--", haySht1 ? "OK" : "--", haySht2 ? "OK" : "--");

  if (hayOled) {
    oled.clearDisplay(); oled.setTextColor(SSD1306_WHITE); oled.setTextSize(2);
    oled.setCursor(0, 0); oled.println("PLACA 1"); oled.setTextSize(1);
    oled.println("prueba de sensores"); oled.println();
    oled.println("Anillo 1 = rojo"); oled.println("Anillo 2 = azul");
    oled.display();
  }

#if TONO_INICIO
  iniciarAudio();
  tonoPrueba();
#endif
  delay(2000);   // deja ver el diagnóstico rojo/azul
}

void loop() {
  animarAnillos();

  static uint32_t tAnt = 0;
  if (millis() - tAnt < 1500) return;
  tAnt = millis();

  float t1 = NAN, h1 = NAN, t2 = NAN, h2 = NAN, ta = NAN, ha = NAN, tb = NAN, p = NAN, lux = NAN;
  uint8_t aqi = 0, est = 0; uint16_t tvoc = 0, eco2 = 0;
  bool okEns = false;

  if (haySht1) leerSHT40(Wire, t1, h1);
  if (haySht2) leerSHT40(Wire1, t2, h2);
  if (hayAht)  leerAHT21(ta, ha);
  if (hayBmp)  leerBMP180(tb, p);
  if (hayVeml) leerVEML7700(lux);
  if (hayEns)  okEns = leerENS160(aqi, tvoc, eco2, est);

#if TONO_INICIO
  static uint8_t ciclo = 0;
  if (++ciclo % 2 == 0) tonoPrueba();   // pitido cada ~3 s mientras pruebas el audio
#endif

  Serial.println("------------------------------");
  if (haySht1) Serial.printf("HUMEDAD  (SHT40) : %.1f C  %.1f %%\n", t1, h1);
  if (haySht2) Serial.printf("HUMEDAD2 (SHT40) : %.1f C  %.1f %%\n", t2, h2);
  if (hayAht)  Serial.printf("AHT21            : %.1f C  %.1f %%\n", ta, ha);
  if (hayBmp)  Serial.printf("BMP180           : %.1f C  %.1f hPa\n", tb, p);
  if (hayVeml) Serial.printf("VEML7700         : %.1f lux\n", lux);
  if (okEns)   Serial.printf("ENS160           : AQI %u  TVOC %u ppb  eCO2 %u ppm%s\n", aqi, tvoc, eco2,
                             est == 0 ? "" : "  (calentando, espera unos minutos)");
  if (!haySht1 && !haySht2 && !hayAht && !hayBmp && !hayVeml && !hayEns)
    Serial.println("No hay sensores. Conecta el ESP32 a la placa y reinícialo.");

  if (hayOled) {
    oled.clearDisplay(); oled.setTextSize(1); oled.setCursor(0, 0);
    if (haySht1) oled.printf("HUM1 %.1fC %.0f%%\n", t1, h1); else oled.println("HUM1 --");
    if (haySht2) oled.printf("HUM2 %.1fC %.0f%%\n", t2, h2); else oled.println("HUM2 --");
    if (hayAht)  oled.printf("AHT  %.1fC %.0f%%\n", ta, ha); else oled.println("AHT21 --");
    if (hayBmp)  oled.printf("BMP  %.1fC %.0fhPa\n", tb, p); else oled.println("BMP180 --");
    if (hayVeml) oled.printf("LUZ  %.0f lux\n", lux); else oled.println("VEML7700 --");
    if (okEns)   oled.printf("AQI %u  CO2 %u\nTVOC %u ppb\n", aqi, eco2, tvoc); else oled.println("ENS160 --");
    oled.display();
  }
}
