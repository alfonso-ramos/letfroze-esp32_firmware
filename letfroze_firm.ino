// =====================================================
// ColdGuard — ESP32 + DS18B20 + OLED + Bluetooth
// v3 — DS18B20 reemplaza DHT11
//      LED azul = menú, verde = viaje OK, rojo = alarma
// =====================================================

#include "BluetoothSerial.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// =====================================================
// PINES
// =====================================================

#define DS18B20_PIN   4    // DATA del DS18B20 (+ resistencia 4.7kΩ a 3.3V)

#define OLED_SDA      21
#define OLED_SCL      22
#define OLED_WIDTH    128
#define OLED_HEIGHT   64
#define OLED_ADDR     0x3C

#define BTN_UP        32
#define BTN_DOWN      33
#define BTN_SELECT    25

#define LED_BLUE      2    // Azul  — dispositivo en menú
#define LED_GREEN     26   // Verde — viaje activo, temp en rango
#define LED_RED       27   // Rojo  — alarma de temperatura
#define BUZZER        14

// =====================================================
// CONFIGURACIÓN
// =====================================================

#define INTERVALO_LECTURA  2000   // ms entre lecturas DS18B20
#define DEBOUNCE_MS        30

float temp_min    = 2.0;
float temp_max    = 8.0;
float temp_alarm  = 10.0;
float temp_actual = 0.0;

bool viaje_activo = false;
unsigned long trip_start = 0;

// =====================================================
// ESTADOS DEL MENÚ
// =====================================================

enum Estado {
  STATE_MENU,
  STATE_VIAJE,
  STATE_EDIT_MIN,
  STATE_EDIT_MAX,
  STATE_EDIT_ALARM
};

Estado state = STATE_MENU;

const char* menu_items[] = {
  "Iniciar Viaje",
  "Temp Min",
  "Temp Max",
  "Alarma"
};
const int MENU_SIZE = 4;
int menu_index = 0;

// =====================================================
// ALARMA
// =====================================================

bool blink_state  = false;
unsigned long last_blink = 0;

// =====================================================
// OBJETOS
// =====================================================

BluetoothSerial SerialBT;
OneWire           oneWire(DS18B20_PIN);
DallasTemperature ds18b20(&oneWire);
Adafruit_SSD1306  oled(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

// =====================================================
// PROTOTIPOS
// =====================================================

void splash();
void leer_temperatura();
const char* obtener_estado();
bool button_pressed(int pin);
void check_alarm();
void draw_menu();
void draw_edit(const char* title, float value);
void draw_viaje();

// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);

  // LEDs y buzzer — primero para feedback visual inmediato
  pinMode(LED_BLUE,  OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED,   OUTPUT);
  pinMode(BUZZER,    OUTPUT);

  // Al arrancar: solo azul encendido (dispositivo en menú)
  digitalWrite(LED_BLUE,  HIGH);
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_RED,   LOW);
  digitalWrite(BUZZER,    LOW);

  // OLED
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("ERROR: OLED no encontrada");
    while (true) {
      digitalWrite(LED_RED, HIGH); delay(200);
      digitalWrite(LED_RED, LOW);  delay(200);
    }
  }

  splash();

  // Bluetooth
  SerialBT.begin("ColdGuard");

  // DS18B20
  ds18b20.begin();
  ds18b20.setResolution(12);   // 12 bits = 0.0625°C de resolución
  ds18b20.setWaitForConversion(false);  // no bloqueante

  // Botones
  pinMode(BTN_UP,     INPUT_PULLUP);
  pinMode(BTN_DOWN,   INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);

  Serial.println("ColdGuard v3 listo.");
}

// =====================================================
// SPLASH SCREEN
// =====================================================

void splash() {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(18, 8);  oled.print("COLDGUARD");
  oled.setCursor(12, 26); oled.print("- Cadena de -");
  oled.setCursor(18, 38); oled.print("   Frio IoT");
  oled.display();
  delay(2000);

  const char* estados[] = { "OLED OK", "TEMP OK", "BT  OK", "READY " };
  for (int i = 0; i < 4; i++) {
    oled.clearDisplay();
    oled.setCursor(18, 8);  oled.print("COLDGUARD");
    oled.setCursor(25, 35); oled.print(estados[i]);
    oled.display();
    delay(600);
  }

  oled.clearDisplay();
  oled.display();
}

// =====================================================
// LECTURA DS18B20 — no bloqueante
// El DS18B20 necesita 750ms para conversión a 12 bits.
// Se solicita la conversión y en la siguiente llamada
// se lee el resultado ya listo.
// =====================================================

static bool conversion_solicitada = false;
static unsigned long tiempo_conversion = 0;

void leer_temperatura() {
  // Primera vez o conversión ya terminada: solicita nueva
  if (!conversion_solicitada) {
    ds18b20.requestTemperatures();
    conversion_solicitada = true;
    tiempo_conversion = millis();
    return;
  }

  // Espera los 750ms de conversión sin bloquear
  if (millis() - tiempo_conversion < 800) return;

  float t = ds18b20.getTempCByIndex(0);
  conversion_solicitada = false;  // lista para la próxima

  // DEVICE_DISCONNECTED_C = -127.0 — sensor desconectado
  if (t == DEVICE_DISCONNECTED_C || t < -55.0 || t > 125.0) {
    Serial.println("ERROR: DS18B20 no responde o fuera de rango");
    SerialBT.println("{\"error\":\"sensor\"}");
    return;
  }

  temp_actual = t;

  // JSON por Bluetooth
  String json = "{";
  json += "\"t\":"       + String(temp_actual, 2);  // 2 decimales — DS18B20 lo soporta
  json += ",\"min\":"    + String(temp_min, 1);
  json += ",\"max\":"    + String(temp_max, 1);
  json += ",\"alerta\":";
  json += (temp_actual > temp_max)   ? "true" : "false";
  json += ",\"critica\":";
  json += (temp_actual > temp_alarm) ? "true" : "false";
  json += "}";

  SerialBT.println(json);
  Serial.println(json);
}

// =====================================================
// ESTADO DE TEMPERATURA
// =====================================================

const char* obtener_estado() {
  if (temp_actual > temp_alarm) return "CRITICA";
  if (temp_actual > temp_max)   return "ALERTA";
  return "NORMAL";
}

// =====================================================
// BOTONES CON DEBOUNCE
// =====================================================

bool button_pressed(int pin) {
  if (digitalRead(pin) == LOW) {
    delay(DEBOUNCE_MS);
    if (digitalRead(pin) == LOW) {
      while (digitalRead(pin) == LOW);
      delay(DEBOUNCE_MS);
      return true;
    }
  }
  return false;
}

// =====================================================
// ALARMA — LEDs y buzzer
// Lógica de LEDs:
//   Sin viaje → solo azul encendido
//   Viaje NORMAL  → solo verde encendido
//   Viaje ALERTA  → rojo parpadeo lento, verde y azul apagados
//   Viaje CRITICA → rojo parpadeo rápido + buzzer continuo
// =====================================================

void check_alarm() {

  if (!viaje_activo) {
    // En menú — solo LED azul encendido
    digitalWrite(LED_BLUE,  HIGH);
    digitalWrite(LED_GREEN, LOW);
    digitalWrite(LED_RED,   LOW);
    digitalWrite(BUZZER,    LOW);
    return;
  }

  // Durante viaje — azul siempre apagado
  digitalWrite(LED_BLUE, LOW);

  unsigned long now = millis();

  if (temp_actual <= temp_max) {
    // ── NORMAL — solo verde fijo
    digitalWrite(LED_GREEN, HIGH);
    digitalWrite(LED_RED,   LOW);
    digitalWrite(BUZZER,    LOW);

  } else if (temp_actual < temp_alarm) {
    // ── ALERTA — verde apagado, rojo parpadeo lento 500ms + beep
    digitalWrite(LED_GREEN, LOW);
    if (now - last_blink > 500) {
      last_blink  = now;
      blink_state = !blink_state;
      digitalWrite(LED_RED, blink_state);
      digitalWrite(BUZZER,  blink_state);
    }

  } else {
    // ── CRÍTICA — rojo parpadeo rápido 150ms + buzzer continuo
    digitalWrite(LED_GREEN, LOW);
    digitalWrite(BUZZER,    HIGH);
    if (now - last_blink > 150) {
      last_blink  = now;
      blink_state = !blink_state;
      digitalWrite(LED_RED, blink_state);
    }
  }
}

// =====================================================
// PANTALLAS OLED
// =====================================================

void draw_menu() {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(20, 0);
  oled.print("ColdGuard");

  for (int i = 0; i < MENU_SIZE; i++) {
    if (i == menu_index) {
      oled.setCursor(0, 16 + i * 12);
      oled.print(">");
    }
    oled.setCursor(12, 16 + i * 12);
    oled.print(menu_items[i]);
  }
  oled.display();
}

void draw_edit(const char* title, float value) {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  oled.print(title);
  oled.setTextSize(2);
  oled.setCursor(30, 22);
  oled.print(value, 0);
  oled.print(" C");
  oled.setTextSize(1);
  oled.setCursor(8, 54);
  oled.print("SEL=Guardar");
  oled.display();
}

void draw_viaje() {
  unsigned long elapsed = (millis() - trip_start) / 1000;

  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);

  // Título + estado en la misma línea
  oled.setCursor(0, 0);
  oled.print("VIAJE");
  oled.setCursor(48, 0);
  oled.print(obtener_estado());

  // Temperatura en grande — 2 decimales gracias al DS18B20
  oled.setTextSize(2);
  oled.setCursor(0, 14);
  oled.print(temp_actual, 2);
  oled.print("C");

  // Tiempo transcurrido en MM:SS
  oled.setTextSize(1);
  oled.setCursor(0, 36);
  if (elapsed < 60) {
    oled.print("T: "); oled.print(elapsed); oled.print("s");
  } else {
    unsigned long m = elapsed / 60;
    unsigned long s = elapsed % 60;
    if (m < 10) oled.print("0");
    oled.print(m); oled.print(":");
    if (s < 10) oled.print("0");
    oled.print(s);
  }

  // Rangos configurados
  oled.setCursor(0, 48);
  oled.print("Mn:"); oled.print(temp_min, 0);
  oled.print(" Mx:"); oled.print(temp_max, 0);
  oled.print("C");

  // Botón salir
  oled.setCursor(78, 48);
  oled.print("SEL=Fin");

  oled.display();
}

// =====================================================
// LOOP PRINCIPAL
// =====================================================

static unsigned long lastLectura = 0;

void loop() {

  // Lectura periódica del DS18B20 solo durante viaje
  if (state == STATE_VIAJE) {
    if (millis() - lastLectura >= INTERVALO_LECTURA) {
      lastLectura = millis();
      leer_temperatura();
    }
  }

  check_alarm();

  // ── MENÚ PRINCIPAL ──────────────────────────────
  if (state == STATE_MENU) {
    draw_menu();

    if (button_pressed(BTN_UP)) {
      menu_index--;
      if (menu_index < 0) menu_index = MENU_SIZE - 1;
    }
    if (button_pressed(BTN_DOWN)) {
      menu_index++;
      if (menu_index >= MENU_SIZE) menu_index = 0;
    }
    if (button_pressed(BTN_SELECT)) {
      if (menu_index == 0) {
        viaje_activo        = true;
        trip_start          = millis();
        lastLectura         = 0;
        conversion_solicitada = false;  // resetea el estado del sensor
        state               = STATE_VIAJE;
      }
      else if (menu_index == 1) state = STATE_EDIT_MIN;
      else if (menu_index == 2) state = STATE_EDIT_MAX;
      else if (menu_index == 3) state = STATE_EDIT_ALARM;
    }
  }

  // ── VIAJE ACTIVO ─────────────────────────────────
  else if (state == STATE_VIAJE) {
    draw_viaje();

    if (button_pressed(BTN_SELECT)) {
      viaje_activo = false;
      // Al volver al menú: apaga verde/rojo/buzzer, enciende azul
      digitalWrite(LED_GREEN, LOW);
      digitalWrite(LED_RED,   LOW);
      digitalWrite(BUZZER,    LOW);
      digitalWrite(LED_BLUE,  HIGH);
      state = STATE_MENU;
    }
  }

  // ── EDITAR TEMP MIN ──────────────────────────────
  else if (state == STATE_EDIT_MIN) {
    draw_edit("Temp Min", temp_min);
    if (button_pressed(BTN_UP))     temp_min += 1;
    if (button_pressed(BTN_DOWN))   temp_min -= 1;
    if (button_pressed(BTN_SELECT)) state = STATE_MENU;
  }

  // ── EDITAR TEMP MAX ──────────────────────────────
  else if (state == STATE_EDIT_MAX) {
    draw_edit("Temp Max", temp_max);
    if (button_pressed(BTN_UP))     temp_max += 1;
    if (button_pressed(BTN_DOWN))   temp_max -= 1;
    if (button_pressed(BTN_SELECT)) state = STATE_MENU;
  }

  // ── EDITAR ALARMA ────────────────────────────────
  else if (state == STATE_EDIT_ALARM) {
    draw_edit("Alarma", temp_alarm);
    if (button_pressed(BTN_UP))     temp_alarm += 1;
    if (button_pressed(BTN_DOWN))   temp_alarm -= 1;
    if (button_pressed(BTN_SELECT)) state = STATE_MENU;
  }

  delay(50);
}
