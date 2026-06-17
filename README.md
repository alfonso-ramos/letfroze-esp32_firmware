# Letfroze — Firmware ESP32

Firmware para el dispositivo Letfroze, un monitor de cadena de frío basado en ESP32. Lee temperatura y humedad con un sensor DHT11 (temporal), muestra los datos en una pantalla OLED de 128×64, emite alertas visuales y sonoras, y transmite lecturas en tiempo real a una app Android nativa vía Bluetooth SPP.

---

## Características

- Lectura de temperatura y humedad cada 2.5 segundos con reintentos automáticos
- Pantalla OLED SSD1306 128×64 con menú navegable por botones
- 3 niveles de alerta: Normal, Alerta y Crítica con LED y buzzer
- Transmisión de datos JSON por Bluetooth clásico (SPP) a la app Android
- Inicio y fin de viaje controlado desde el menú del dispositivo
- Umbrales de temperatura configurables desde el propio dispositivo

---

## Hardware requerido

| Componente | Especificación | Pin(es) |
|---|---|---|
| Microcontrolador | ESP32 DevKit (cualquier variante con BT clásico) | — |
| Sensor | DHT11 (módulo de 3 pines) | GPIO 4 |
| Pantalla | OLED SSD1306 128×64 I2C — dirección 0x3C | SDA: GPIO 21 / SCL: GPIO 22 |
| Botón arriba | Pulsador NO con pull-up interno | GPIO 32 |
| Botón abajo | Pulsador NO con pull-up interno | GPIO 33 |
| Botón seleccionar | Pulsador NO con pull-up interno | GPIO 25 |
| LED verde | LED estándar + resistencia 220Ω en serie | GPIO 26 |
| LED rojo | LED estándar + resistencia 220Ω en serie | GPIO 27 |
| Buzzer | Buzzer pasivo | GPIO 14 |
| Alimentación | USB-C / 5V al ESP32 | — |

> Los botones usan `INPUT_PULLUP` interno del ESP32. Conectar un pin al GPIO y el otro a GND — no se necesitan resistencias externas.

---

## Diagrama de conexiones

```
DHT11          ESP32           OLED SSD1306
------         -----           ------------
VCC    ──────► 3.3V ◄───────── VCC
DATA   ──────► GPIO 4
GND    ──────► GND  ◄───────── GND
                │
               GPIO 21 (SDA) ─► SDA
               GPIO 22 (SCL) ─► SCL

Botón UP       GPIO 32 ── [BTN] ── GND
Botón DOWN     GPIO 33 ── [BTN] ── GND
Botón SELECT   GPIO 25 ── [BTN] ── GND

LED verde      GPIO 26 ── 220Ω ── LED(+) ── GND
LED rojo       GPIO 27 ── 220Ω ── LED(+) ── GND
Buzzer         GPIO 14 ── (+)Buzzer(-) ── GND
```

---

## Dependencias (librerías Arduino)

Instalar desde **Herramientas → Administrar librerías** en Arduino IDE:

| Librería | Autor | Versión mínima |
|---|---|---|
| DHT sensor library | Adafruit | 1.4.x |
| Adafruit Unified Sensor | Adafruit | 1.1.x |
| Adafruit SSD1306 | Adafruit | 2.5.x |
| Adafruit GFX Library | Adafruit | 1.11.x |

`BluetoothSerial` viene incluida en el soporte oficial de ESP32 para Arduino — no requiere instalación adicional.

---

## Instalación y carga

### 1. Configurar Arduino IDE

1. Abrir Arduino IDE 2.x
2. Ir a **Archivo → Preferencias → URLs adicionales** y agregar:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Ir a **Herramientas → Placa → Gestor de placas**, buscar `esp32` e instalar el paquete de Espressif

### 2. Seleccionar la placa

- **Herramientas → Placa → ESP32 Arduino → ESP32 Dev Module**
- **Puerto:** seleccionar el puerto COM del dispositivo conectado

### 3. Subir el firmware

Abrir `termostato_esp32.ino` y presionar **Subir** (Ctrl+U).

Verificar en **Monitor Serie** a 115200 baud que aparezca:

```
Listo. Enviando datos por Bluetooth.
{"t":24.5,"h":60.0,"alerta":false,"critica":false}
```

---

## Protocolo Bluetooth

El ESP32 se anuncia como `ESP32_Termostato` usando el perfil SPP (Serial Port Profile). Envía una línea JSON cada 2.5 segundos por el socket Bluetooth.

### Formato del mensaje

```json
{
  "t": 24.5,
  "h": 60.0,
  "min": 2.0,
  "max": 8.0,
  "alerta": false,
  "critica": false
}
```

| Campo | Tipo | Descripción |
|---|---|---|
| `t` | float | Temperatura en °C (1 decimal) |
| `h` | float | Humedad relativa en % (1 decimal) |
| `min` | float | Umbral mínimo configurado |
| `max` | float | Umbral máximo configurado |
| `alerta` | bool | `true` si `t > max` |
| `critica` | bool | `true` si `t > temp_alarm` |

En caso de fallo del sensor:

```json
{"error": "sensor"}
```

El UUID SPP estándar es `00001101-0000-1000-8000-00805F9B34FB`.

---

## Lógica de alertas

| Condición | LED verde | LED rojo | Buzzer |
|---|---|---|---|
| Sin viaje activo | Apagado | Apagado | Silencio |
| `t ≤ temp_max` | Fijo | Apagado | Silencio |
| `temp_max < t < temp_alarm` | Apagado | Parpadeo lento (500ms) | Beep intermitente |
| `t ≥ temp_alarm` | Apagado | Parpadeo rápido (150ms) | Continuo |

---

## Menú del dispositivo

Navegar con los botones **▲ arriba**, **▼ abajo** y **● seleccionar**.

```
┌─────────────────┐
│   ColdGuard     │
│ > Iniciar Viaje │
│   Temp Min      │
│   Temp Max      │
│   Alarma        │
└─────────────────┘
```

| Opción | Función |
|---|---|
| Iniciar Viaje | Activa el monitoreo continuo y empieza la sesión de datos |
| Temp Min | Edita el umbral mínimo esperado (referencia informativa) |
| Temp Max | Edita el umbral máximo — activa alerta si se supera |
| Alarma | Edita el umbral crítico — activa buzzer continuo si se supera |

Durante un viaje, presionar **● seleccionar** finaliza la sesión y regresa al menú.

### Valores por defecto

| Parámetro | Valor | Descripción |
|---|---|---|
| `temp_min` | 2.0 °C | Mínimo esperado de cadena de frío |
| `temp_max` | 8.0 °C | Máximo permitido |
| `temp_alarm` | 10.0 °C | Temperatura crítica |
| `INTERVALO` | 2500 ms | Tiempo entre lecturas |

---

## Estructura del repositorio

```
coldguard-firmware/
├── termostato_esp32.ino   # Firmware principal
└── README.md              # Este archivo
```

---

## Solución de problemas

**Monitor Serie muestra `nan` o `ERROR: DHT11 no responde`**
Verificar que el cable DATA esté firmemente en GPIO 4. Si el error persiste, cambiar `DHT_PIN` a GPIO 15 o GPIO 5 y reconectar el cable. El módulo de 3 pines ya incluye resistencia pull-up; el sensor de 4 pines requiere una de 10kΩ entre VCC y DATA.

**El ESP32 no aparece en Bluetooth del celular**
Confirmar que el firmware fue subido correctamente y que el ESP32 está encendido. El nombre del dispositivo es `ESP32_Termostato`. Reiniciar el ESP32 si no aparece tras 30 segundos.

**OLED no muestra nada**
Verificar SDA en GPIO 21 y SCL en GPIO 22. Confirmar que la dirección I2C es `0x3C` — algunos módulos usan `0x3D`. Cambiar `OLED_ADDR` en el código si es necesario.

**La app Android se desconecta inmediatamente**
Emparejar el ESP32 desde **Ajustes → Bluetooth** del celular antes de abrir la app. La app requiere el dispositivo previamente vinculado para conectarse.

---

## Roadmap

- [ ] Reconexión Bluetooth automática tras pérdida de señal
- [ ] Almacenamiento de lecturas en tarjeta SD
- [ ] Historial de viajes consultable desde el menú OLED
- [ ] Soporte Wi-Fi + MQTT para integración con nube
- [ ] Sensor DS18B20 como alternativa al DHT11 (mayor precisión)

---

## Licencia

MIT — ver archivo `LICENSE` para más detalles.
