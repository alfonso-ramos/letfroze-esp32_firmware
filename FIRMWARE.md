# LetFloze — Firmware ESP32

Firmware para dispositivo de monitoreo de cadena de frío basado en ESP32. Se conecta vía BLE a una app móvil que reenvía los datos al backend.

## Arquitectura de conexión

```
ESP32 ── BLE ──> App Móvil ── HTTP ──> Backend LetFloze
                  │
              Internet
           (celular/WiFi)
```

El ESP32 **no se conecta directamente a internet**. Envía datos por BLE a la app móvil, y esta se encarga de subirlos al backend usando su conexión (celular o WiFi del router).

## Flujo de operación

### 1. Pairing (BLE)

1. App escanea dispositivos BLE cercanos
2. Usuario selecciona el ESP32 en la app
3. App lee `serial_number` del servicio BLE del dispositivo
4. App envía `POST /api/v1/devices/:id/pair` al backend
5. Backend registra el device con `bluetooth_address` y `status: "paired"`
6. App guarda la referencia local (deviceId + bleId)

### 2. Inicio de viaje

1. App envía `POST /api/v1/trips/:id/start`
2. App notifica al ESP32 vía BLE que el viaje inició (escribe en una characteristic)
3. ESP32 comienza a tomar lecturas periódicas (cada 30s por defecto)

### 3. Lectura de sensores

1. ESP32 lee temperatura (DS18B20/DHT22/BME280) y humedad
2. Almacena lectura en buffer local con `seq_number` incremental
3. Cada lectura está disponible para la app vía BLE:
   - **Lectura individual**: app lee characteristic -> ESP32 responde con última lectura
   - **Lectura batch**: app solicita lote de lecturas no enviadas -> ESP32 responde con array

### 4. Transmisión al backend

La app móvil periódicamente (o cuando tiene conexión):

1. **Streaming** (tiempo real):
   ```
   POST /api/v1/sensor-data/stream
   { "tripId": "...", "deviceId": "...", "temperature": 4.5, "humidity": 65.2,
     "batteryLevel": 85, "seqNumber": 1423, "time": "2026-07-12T14:30:00Z" }
   ```

2. **Sync batch** (offline recuperado):
   ```
   POST /api/v1/sensor-data/sync
   { "tripId": "...", "deviceId": "...", "clientSyncId": "...",
     "readings": [
       { "time": "...", "temperature": 4.5, "humidity": 65.2,
         "batteryLevel": 85, "signalStrength": -60, "seqNumber": 1423 }
     ],
     "events": [
       { "time": "...", "eventType": "door_open", "payload": {}, "seqNumber": 50 }
     ],
     "syncedAt": "2026-07-12T15:00:00Z" }
   ```


## Servicios BLE

### UUIDs

```
Servicio principal:     0000FFE0-0000-1000-8000-00805F9B34FB
Characteristic lectura: 0000FFE1-0000-1000-8000-00805F9B34FB  (notify/read)
Characteristic control: 0000FFE2-0000-1000-8000-00805F9B34FB  (write)
Characteristic config:  0000FFE3-0000-1000-8000-00805F9B34FB  (read/write)
```

### Formato de datos

**Characteristic de lectura (FFE1) — notify/read**

```json
{
  "t": 4.52,
  "h": 65.1,
  "b": 83,
  "s": -65,
  "seq": 1423,
  "ts": "2026-07-12T14:30:00Z"
}
```

| Campo | Tipo | Descripción |
|-------|------|-------------|
| `t` | float | Temperatura en °C |
| `h` | float | Humedad relativa % |
| `b` | int | Nivel batería 0-100% |
| `s` | int | RSSI señal Bluetooth |
| `seq` | int | Número de secuencia (monolítico, nunca se reinicia) |
| `ts` | string | Timestamp ISO 8601 |

**Characteristic de control (FFE2) — write (app → ESP32)**

```json
{ "cmd": "start_sampling" }
{ "cmd": "stop_sampling" }
{ "cmd": "set_interval", "interval": 10 }
{ "cmd": "sync_request", "from_seq": 1000 }
{ "cmd": "sync_ack", "seq": 1500 }
{ "cmd": "calibrate", "offset": 0.5 }
{ "cmd": "reboot" }
```

**Characteristic de config (FFE3) — read/write**

```json
{
  "serial": "LFZ-001A",
  "fw": "1.0.0",
  "interval": 30,
  "calibration": 0.0
}
```

## Almacenamiento local (ESP32)

El firmware debe mantener un buffer circular en RTC memory (preserva en deep sleep):

```
struct SensorReading {
  float temperature;
  float humidity;
  uint8_t battery_level;
  int8_t signal_strength;
  uint32_t seq_number;
  uint64_t timestamp;  // epoch ms
};

// Buffer circular en RTC memory
RTC_DATA_ATTR SensorReading buffer[BUFFER_SIZE];  // ej: 1024 lecturas
RTC_DATA_ATTR uint32_t write_index = 0;
RTC_DATA_ATTR uint32_t global_seq = 0;
```

Cada nueva lectura incrementa `global_seq` (nunca se reinicia, ni al resetear el dispositivo). Esto permite al backend deduplicar usando `(deviceId, seqNumber, time)`.

## Modos de conectividad

### Modo 1: Internet por celular (recomendado)

```
ESP32 ── BLE ──> App Android/iOS ── LTE/5G ──> Backend
```

- El ESP32 nunca necesita WiFi configurado
- La app móvil del conductor lleva internet
- Ideal para transporte en ruta (no depende de router externo)

### Modo 2: Router WiFi externo (almacén/depósito)

```
ESP32 ── WiFi ──> Router ──> Internet ──> Backend (MQTT)
```

- El ESP32 se conecta directamente a WiFi del almacén
- Usa MQTT sobre EMQX para enviar lecturas
- La app móvil sigue usándose para gestión (iniciar viaje, pair, etc.)
- Requiere credenciales WiFi configuradas

Opción alternativa: ESP32 se conecta a WiFi pero sigue enviando vía BLE a la app (que luego reenvía). Esto permite que la app funcione como caché cuando el WiFi falla.

## Estados del ESP32

```
         ┌──────────┐
         │  PAIRED  │
         └────┬─────┘
              │ cmd: start_sampling
              v
       ┌─────────────┐
       │  SAMPLING   │◄──── cmd: set_interval
       └──────┬──────┘
              │ cmd: stop_sampling
              v
         ┌──────────┐
         │  IDLE    │
         └──────────┘
```

En **SAMPLING** el ESP32 despierta cada N segundos, toma lectura, la guarda en buffer y la publica en la BLE characteristic (notify). Entre lecturas puede entrar en deep sleep para ahorrar batería.

## Configuración inicial (primer uso)

1. App escanea BLE → encuentra ESP32
2. App lee characteristic config → obtiene `serial` y `fw`
3. App envía `POST /api/v1/devices` al backend con el serial
4. App escribe en characteristic config el `deviceId` asignado por backend
5. App envía `POST /api/v1/devices/:id/pair` → estado `paired`
6. App escribe `cmd: start_sampling` → ESP32 comienza a muestrear

## Especificaciones técnicas

| Parámetro | Valor |
|-----------|-------|
| MCU | ESP32-S3 (recomendado) o ESP32 |
| Sensor temp | DS18B20 (impermeable, -55 a +125°C) |
| Sensor hum | DHT22 (opcional, 0-100% RH) |
| Batería | Li-Ion 18650 + TP4056 cargador |
| Autonomía | ~30 días (deep sleep entre lecturas) |
| Intervalo default | 30 segundos |
| Buffer size | 1024 lecturas mínimo |

## Pines (recomendación)

| Pin | Conexión |
|-----|----------|
| GPIO4 | DS18B20 DATA |
| GPIO27 | DHT22 DATA |
| GPIO32 | LED status (verde) |
| GPIO33 | LED error (rojo) |
| GPIO34 | Battery voltage (ADC) |
| EN | Reset button |

## API Reference (para la app móvil)

La app móvil usa estos endpoints del backend para interactuar con el dispositivo:

| Endpoint | Propósito |
|----------|-----------|
| `POST /api/v1/devices` | Registrar nuevo dispositivo |
| `POST /api/v1/devices/:id/pair` | Vincular ESP32 con viaje |
| `POST /api/v1/sensor-data/stream` | Lectura individual tiempo real |
| `POST /api/v1/sensor-data/sync` | Batch de lecturas offline |
| `GET /api/v1/devices/:id/latest` | Última lectura conocida |

## Debugging

El ESP32 tiene un LED de estado:
- **Parpadeo rápido (100ms)**: Inicializando / buscando BLE
- **Parpadeo lento (1s)**: Paired, esperando comando
- **Sólido**: Sampeando activamente
- **3 parpadeos, pausa**: Error de sensor
- **5 parpadeos, pausa**: Buffer lleno (la app no está leyendo)

Para debug serial: `115200 baud`, salida JSON:

```json
{"event":"reading","temp":4.5,"hum":65,"seq":1423,"bat":83}
{"event":"ble_write","cmd":"start_sampling"}
{"event":"buffer","used":512,"total":1024}
```

## Ejemplo de código mínimo (Arduino)

```cpp
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#define SERVICE_UUID        "0000FFE0-0000-1000-8000-00805F9B34FB"
#define CHAR_READING_UUID   "0000FFE1-0000-1000-8000-00805F9B34FB"
#define CHAR_CONTROL_UUID   "0000FFE2-0000-1000-8000-00805F9B34FB"
#define CHAR_CONFIG_UUID    "0000FFE3-0000-1000-8000-00805F9B34FB"
#define ONE_WIRE_BUS        4
#define BATTERY_PIN         34
#define SAMPLING_INTERVAL   30  // segundos

RTC_DATA_ATTR uint32_t global_seq = 0;
RTC_DATA_ATTR bool sampling = false;
BLECharacteristic *pReadingChar;

void setup() {
    Serial.begin(115200);
    initBLE();
    initSensors();
}

void loop() {
    if (sampling) {
        float temp = readTemperature();
        float hum = readHumidity();
        uint8_t bat = readBattery();
        uint32_t seq = ++global_seq;
        
        String json = "{\"t\":" + String(temp) + ",\"h\":" + String(hum)
                    + ",\"b\":" + String(bat) + ",\"seq\":" + String(seq)
                    + ",\"ts\":\"" + getISO8601() + "\"}";
        
        pReadingChar->setValue(json.c_str());
        pReadingChar->notify();
        
        Serial.printf("{\"event\":\"reading\",\"temp\":%.2f,\"seq\":%u}\n", temp, seq);
    }
    delay(SAMPLING_INTERVAL * 1000);
}
```
