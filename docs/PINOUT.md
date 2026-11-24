# Experimental-Synth: placas Cerebro y Control (v6)

Las dos placas miden 180 × 75 mm y llevan agujeros M3 de 3,2 mm a 4 mm de cada borde, igual que la placa 1.
Van una al lado de la otra: el J2 de la Cerebro (borde derecho) y el J1 de la Control (borde izquierdo) quedan
enfrentados a la misma altura y se unen con un cable dupont 1:1 de 10 hilos.

Las tres placas están en KiCad 9, en `hardware/kicad/`, y los archivos para fabricar en `hardware/fabricacion/`
(un ZIP de Gerber + taladros por placa, más renders de frente y atrás):

| Placa | KiCad | Fabricación | Cambios |
|---|---|---|---|
| Control | `hardware/kicad/control_v6.1.kicad_pcb` | `hardware/fabricacion/control_v6.1/` | **v6.1 (2026-11-24):** huella de los 16 LED corregida y zona de los LED re-ruteada |
| Cerebro | `hardware/kicad/cerebro_v6.1.kicad_pcb` | `hardware/fabricacion/cerebro_v6.1/` | **v6.1 (2026-11-24):** huella de los 8 LED corregida y fila de LED re-ruteada |
| Placa 1 | `hardware/kicad/placa1_v4.kicad_pcb` | `hardware/fabricacion/placa1_v4/` | Radar HLK-LD2410C y sensores de humedad SHT40 (I2C). No lleva LED 6028: sin cambios |

> **Fallo de la v6 (ya fabricada):** la huella `LED_WS2812B_6028_REV` de Control y Cerebro v6 quedó en espejo
> respecto del LED real: GND y 5V caían en la diagonal equivocada, así que ningún giro del LED calzaba. En las placas
> v6 hay que soldar el LED mirando hacia atrás o con puentes de cable esmaltado. La v6.1 usa la huella corregida
> (`LED_WS2812B-6028_Inverso`, en `hardware/biblioteca/GCLab.pretty/`, verificada contra la hoja de datos XINGLIGHT XL-6028RGBC-WS2812B y contra el PercuSynth V2),
> con un triángulo de serigrafía junto al pad de GND. Los archivos v6 no se
> publican en este repositorio para que nadie los fabrique por error.

## Cable entre placas

| Pin | Señal | GPIO (ESP32-S3) |
|---|---|---|
| 1 | GND | — |
| 2 | 5V (LEDs) | — |
| 3 | 3V3 | — |
| 4 | LED (datos WS2812B) | 21, pasando antes por los 8 LEDs de la Cerebro |
| 5 | POT (común del mux de potenciómetros) | 4 (ADC1) |
| 6 | BT1 (común del mux de la fila 1 de switches) | 14 |
| 7 | S0 | 15 |
| 8 | S1 | 16 |
| 9 | S2 | 17 |
| 10 | BT2 (común del mux de la fila 2 de switches) | 18 |

## Pines del ESP32-S3 (placa Cerebro)

| Función | GPIO |
|---|---|
| OLED SDA / SCL (mismo bus I2C que el MCP23017) | 8 / 9 |
| microSD CS / MOSI / CLK / MISO (módulo de 3,3 V) | 10 / 11 / 12 / 13 |
| DAC PCM5102A BCK / LCK / DIN | 5 / 6 / 7 |
| Encoder EC11 (ENC1) A / B / pulsador (a GND, usar pull-up interna) | 40 / 39 / 38 |
| Pulsadores B1 / B2 (bajo la OLED, a GND, usar pull-up interna) | 44 / 45 |
| Interrupción del teclado (MCP23017 INTA) | 47 |
| Datos LEDs | 21 |
| EXP | 1, 2, 41, 42 + 3V3 y GND |

- **GPIO45:** es pin de arranque. No le pongas pull-up externa; usa la interna después de arrancar, como en el PercuSynth.
- **GPIO44:** es el RX del USB-serial. Programa por el puerto USB nativo.

## Teclado (15 líneas)

Los 15 cables del teclado van al MCP23017, en la dirección I2C 0x20:
- J5 pines 1–8 → GPA0–GPA7.
- J5 pines 9–15 → GPB0–GPB6.

El firmware decide qué líneas son filas y cuáles columnas.

**Matriz medida el 2026-11-17** (49 teclas, 4 octavas de Do a Do, tocadas de la más grave a la más aguda):

- Columnas: cables 8, 7, 6, 5, 4, 3, 2, 1 (en ese orden, de grave a agudo dentro de cada grupo).
- Filas: cables 10, 11, 12, 13, 14 y 15, con 8 teclas cada una (teclas 1–48), y el cable 9 solo para la tecla 49 (cables 8 + 9).
- Tecla (0 = la más grave) = fila × 8 + (8 − columna), con fila = 0 para el cable 10 … 5 para el 15 y 6 para el 9.
- El teclado **tiene diodos**: los acordes no generan notas fantasma (medido con `firmware/synth_cerebro/`).

En la Cerebro de prueba el módulo MCP quedó con el teclado al revés (cable 1 en B6, cable 15 en A0).
`firmware/prueba_cerebro/` lo corrige con `TECLADO_INVERTIDO 1` y toca la nota de cada tecla por el DAC.

## LEDs

Son 24 WS2812B 6028 de montaje inverso, los mismos del PercuSynth V2, en una sola cadena:

| Índices | Ubicación |
|---|---|
| 0–7 | Placa Cerebro, de izquierda a derecha |
| 8–15 | Bajo los switches S1–S8, de izquierda a derecha |
| 16–23 | Bajo los switches S16–S9, de derecha a izquierda |

El orden de la cadena no cambió en la v6.1, así que el firmware sirve tal cual. Al soldar (por detrás), el
triángulo de serigrafía marca el pad de GND; la pata en punta del 6028 (la de la esquina cortada) es GND. En la
fila de abajo de la Control los LED van girados 180° respecto de los de arriba (el triángulo queda abajo del LED).

## Multiplexores (placa Control: 3 × CD74HC4051 SOIC-16, por detrás)

S0..S2 seleccionan el canal (0–7) en los tres chips a la vez.

| Columna | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| Canal | 3 | 0 | 1 | 2 | 4 | 6 | 7 | 5 |

Con un canal seleccionado se leen tres cosas: POT (analógico), BT1 (switch de esa columna en la fila 1) y BT2 (switch de esa columna en la fila 2). Un switch apretado se lee como LOW.

```cpp
const uint8_t COL_CH[8] = {3, 0, 1, 2, 4, 6, 7, 5};   // columna -> canal
const uint8_t SEL[3] = {15, 16, 17};
void muxSelect(uint8_t ch) { for (int i = 0; i < 3; i++) digitalWrite(SEL[i], (ch >> i) & 1); }
// setup: pinMode(SEL[i], OUTPUT); pinMode(14, INPUT_PULLUP); pinMode(18, INPUT_PULLUP);
// por columna c: muxSelect(COL_CH[c]); delayMicroseconds(10);
//   pot = analogRead(4); fila1 = !digitalRead(14); fila2 = !digitalRead(18);
```

Con 10 µs el ADC del ESP32-S3 arrastra ruido del canal anterior: en `firmware/synth_cerebro/` funcionó estable
esperar 60 µs, descartar dos lecturas, promediar cuatro y aplicar un cambio solo si pasa de ~1,7 % (70 de 4095).

Pruebas (2026-11-18): `firmware/prueba_control/` revisa los 24 canales. Con `SIN_COMPONENTES 1` se simulan los
potenciómetros y switches uniendo sus pads con una pinza. Las patas que fallaron al soldar fueron U2 pata 15 (S4) y
U3 pata 13 (S10).

Lo que hace cada control en el sintetizador está en la cabecera de `firmware/synth_cerebro/synth_cerebro.ino`.

## Placa 1: radar HLK-LD2410C

El antiguo header UART (RX · TX · GND) de la fila superior pasó a ser el header RADAR de 5 pines: RX · TX · GND · 5V · OUT.
El radar se conecta con cables dupont sueltos.

| Header RADAR | Va al SuperMini | Pin del radar |
|---|---|---|
| RX | pin RX/TX del UART (GPIO43/44) | TX |
| TX | pin RX/TX del UART (GPIO43/44) | RX |
| GND | GND | GND |
| 5V | 5V (el radar no funciona con 3,3 V) | VCC |
| OUT | GPIO10 (pad libre del lado de 5V/GND/3V3) | OUT |

- El radar habla a 256000 baudios, 8N1. El ESP32-S3 deja asignar el UART a cualquier pin: si no llegan datos,
  intercambia rx y tx en `Serial1.begin(256000, SERIAL_8N1, rx, tx)`.
- OUT queda en alto mientras detecta una persona. Sirve sin usar el UART.
- Revisa en la serigrafía de tu SuperMini que el pad libre que usamos sea GPIO10 antes de escribir el firmware.

## Placa 1: sensores de humedad SHT40 (sondas IP68 de 4 cables)

Los headers HUMEDAD y HUMEDAD2 pasaron de 3 pines analógicos (S · Vcc · GND) a 4 pines I2C: **3V3 · GND · SDA · SCL**.
Los dos SHT40 responden en la misma dirección (0x44), así que cada uno va en su propio bus:

| Header | Bus | SDA | SCL | Pull-ups |
|---|---|---|---|---|
| HUMEDAD | I2C principal (el de la OLED, VEML7700, ENS160/AHT21 y BMP180) | SDA de siempre | SCL de siempre | Las del módulo OLED |
| HUMEDAD2 | Segundo bus | GPIO3 (antes S2) | GPIO2 (antes H) | R1 y R2, 4,7 kΩ 0805, junto al header |

```cpp
Wire.begin(SDA_PRINCIPAL, SCL_PRINCIPAL);   // los mismos pines que ya usa la OLED
Wire1.begin(3, 2);                          // HUMEDAD2: SDA2 = GPIO3, SCL2 = GPIO2
// sensor 1 en Wire (0x44), sensor 2 en Wire1 (0x44)
```

Los SHT40 se alimentan con 3,3 V (no les pongas 5 V).

## Antes de fabricar

- **LEDs:** compra la versión de montaje inverso («reverse mount») del WS2812B 6028. Uno común queda alumbrando hacia atrás.
  Fabrica solo la v6.1: la v6 tiene la huella del LED en espejo.
- **Pulsadores:** usan dos pines en diagonal, así que funcionan sin importar cómo vengan unidos por dentro.
- **Encoder:** EC11 con pulsador (5 pines + 2 patas de anclaje), por ejemplo el Alps EC11E o un clon.
- **Malla de GND:** ya va en las dos caras de la Control y la Cerebro. Si mueves algo en KiCad, vuelve a rellenar las zonas con B.
