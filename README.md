# Experimental Synth — Sintetizador de Madera

Sintetizador experimental de **GC Lab Chile**: una caja de madera con un teclado de 49 teclas reciclado de un Casio,
perillas, teclas mecánicas y LEDs RGB. Funciona con dos ESP32-S3 y tres placas de 180 × 75 mm diseñadas en KiCad 9.

| Placa | Qué hace |
|---|---|
| **Cerebro** | ESP32-S3, teclado (MCP23017), DAC PCM5102A, OLED, microSD, encoder EC11 y 8 LEDs WS2812B |
| **Control** | 8 perillas y 16 teclas leídas con 3 multiplexores CD74HC4051, más 16 LEDs WS2812B |
| **Placa 1** | Plantas y entorno: ESP32-S3, sensores ambientales, dos SHT40, radar HLK-LD2410C, DAC y pantalla |

## ⚠️ Fallos conocidos (corregidos en los archivos actuales)

**v6 → v6.1 (2026-11-24): huella de los LED WS2812B 6028 en espejo.**
En la revisión v6 de las placas **Cerebro** y **Control**, la huella del LED de montaje inverso
(`LED_WS2812B_6028_REV`) quedó en espejo respecto del componente real. GND y 5V caían en la diagonal equivocada,
así que el LED no calzaba en ninguna orientación. Afectó a los 24 LEDs.

- **Los archivos de este repositorio (v6.1) ya están corregidos.** La huella nueva `LED_WS2812B-6028_Inverso`
  (en `hardware/biblioteca/GCLab.pretty/`) se verificó contra la hoja de datos XINGLIGHT XL-6028RGBC-WS2812B
  (pág. 11, vista desde la lente) y contra la placa PercuSynth V2, que usa el mismo LED y funciona.
- La zona de los LED se re-ruteó en las dos placas. El DRC de KiCad da 0 errores y 0 conexiones pendientes,
  igual que antes. El orden de la cadena de LEDs no cambió, así que el firmware sirve tal cual.
- Cada LED tiene ahora un **triángulo de serigrafía junto al pad de GND**. La pata en punta del 6028
  (la de la esquina cortada) es GND. En la fila de abajo de la Control los LED van girados 180° respecto de
  los de arriba.
- **Si tienes placas v6 ya fabricadas:** suelda los LED mirando hacia atrás o corrige con puentes de cable
  esmaltado. No vuelvas a fabricar la v6: sus archivos no se publican aquí.

La Placa 1 no lleva estos LED y no cambió.

## Contenido

```
hardware/
  kicad/          Placas en KiCad 9 (cerebro_v6.1, control_v6.1, placa1_v4)
  fabricacion/    Un ZIP de Gerber + taladros por placa, listo para JLCPCB, y renders de frente y atrás
  biblioteca/     GCLab.pretty con la huella corregida del WS2812B 6028 inverso
firmware/         Sketches de Arduino para ESP32-S3 (pruebas por placa y los dos sintetizadores)
docs/
  PINOUT.md       Conexiones, pines del ESP32-S3, cable entre placas, teclado, LEDs y multiplexores
  montaje/        Diagramas de conexión del módulo MCP23017 con el teclado
```

## Fabricar

- Sube el ZIP de `hardware/fabricacion/<placa>/` tal cual. Son placas de 2 capas, de 180 × 75 mm.
- Las ventanas rectangulares de los LED son cortes de la placa (van en el contorno, capa Edge.Cuts).
- Compra el WS2812B 6028 en versión **de montaje inverso** («reverse mount»), por ejemplo el XINGLIGHT
  XL-6028RGBC-WS2812B. Uno común queda alumbrando hacia atrás.

## Antes de armar

- **5V del ESP32-S3 (Cerebro):** el DAC, los LEDs y el cable a la Control se alimentan del pin 5V del
  ESP32-S3-DevKitC-1. Muchas de esas placas traen ese pin desconectado hasta que se cierra un puente de soldadura
  (marcado IN-OUT o similar). Si nada de 5V prende, revisa ese puente.
- **Orientación del ESP32 en la Cerebro:** guíate por la serigrafía (pin 1 = 3V3). Para cables provisorios usa los
  pads rotulados de la placa (OLED, J3) y no cuentes pines del módulo.
- **Firmware de LEDs:** con el núcleo Arduino-ESP32 3.2.0, `Adafruit_NeoPixel` puede fallar al azar (los LED se
  apagan). `firmware/synth_ambiente_placa1/` maneja el RMT directo con la configuración en cero para evitarlo;
  `firmware/prueba_placa1/` todavía usa `Adafruit_NeoPixel`. Otra opción es usar el núcleo 3.3.x.
- Compila con «USB CDC On Boot: Enabled» y programa por el USB nativo del ESP32-S3.

## Licencia

Todo el contenido de este repositorio (placas, Gerber, huellas, firmware y documentación) está bajo la licencia
[Creative Commons Atribución-NoComercial-CompartirIgual 4.0 Internacional (CC BY-NC-SA 4.0)](LICENSE).

- **Puedes** usarlo, estudiarlo, fabricar placas para ti, para talleres o para enseñar, y modificarlo.
- **Debes** dar crédito a GC Lab Chile y enlazar este repositorio: `https://github.com/GC-Lab-Gonzalo/Experimental_Synth`.
- **No puedes** venderlo ni usarlo con fines comerciales (por ejemplo, vender placas, kits o instrumentos basados
  en este diseño) sin permiso escrito de GC Lab Chile.
- Si lo modificas y lo compartes, tiene que ser con esta misma licencia.

¿Te interesa un uso comercial? Escríbenos desde [gclabchile.com](https://www.gclabchile.com).

Copyright © 2026 GC Lab Chile.

---

Hecho en Chile por [GC Lab Chile](https://www.gclabchile.com): ingeniería electrónica, IA y artes creativas.
