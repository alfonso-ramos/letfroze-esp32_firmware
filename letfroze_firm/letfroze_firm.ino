// =====================================================
// ColdGuard — ESP32 + DHT11 + OLED + Bluetooth
// v2 — LED azul (power), verde (viaje OK), rojo (alarma)
// =====================================================

#include "BluetoothSerial.h"
#include "DHT.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// =====================================================
// PINES
// =====================================================

#define DHT_PIN       4
#define DHT_TYPE      DHT11

#define OLED_SDA      21
#define OLED_SCL      22
#define OLED_WIDTH    128
#define OLED_HEIGHT   64
#define OLED_ADDR     0x3C

#define BTN_UP        32
#define BTN_DOWN      33
#define BTN_SELECT    25

#define LED_BLUE      2    // Azul  — dispositivo encendido (siempre ON)
#define LED_GREEN     26   // Verde — viaje activo y temp en rango
#define LED_RED       27   // Rojo  — alarma de temperatura
#define BUZZER        14   // Buzzer — alerta sonora

// =====================================================
// CONFIGURACIÓN
// =====================================================

#define INTERVALO_LECTURA  2500
#define MAX_REINTENTOS     5
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

bool blink_state = false;
unsigned long last_blink = 0;

// =====================================================
// OBJETOS
// =====================================================

BluetoothSerial SerialBT;
DHT dht(DHT_PIN, DHT_TYPE);
Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

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

  // LEDs y buzzer — primero para que el azul encienda lo antes posible
  pinMode(LED_BLUE,  OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED,   OUTPUT);
  pinMode(BUZZER,    OUTPUT);

  digitalWrite(LED_BLUE,  HIGH);  // enciende inmediatamente al arrancar
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_RED,   LOW);
  digitalWrite(BUZZER,    LOW);

  // OLED
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("ERROR: OLED no encontrada");
    // parpadea el rojo para indicar error de hardware
    while (true) {
      digitalWrite(LED_RED, HIGH); delay(200);
      digitalWrite(LED_RED, LOW);  delay(200);
    }
  }

  // Splash screen
  splash();

  // Bluetooth
  SerialBT.begin("ColdGuard");

  // DHT11
  dht.begin();
  delay(2000);  // estabilización obligatoria

  // Botones con pull-up interno
  pinMode(BTN_UP,     INPUT_PULLUP);
  pinMode(BTN_DOWN,   INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);

  Serial.println("ColdGuard v2 listo.");
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

  const char* estados[] = { "OLED OK", "TEMP OK", "BT OK", "READY" };
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
// LECTURA DHT11 CON REINTENTOS
// =====================================================

void leer_temperatura() {
  float t = NAN, h = NAN;

  for (int i = 0; i < MAX_REINTENTOS; i++) {
    t = dht.readTemperature();
    h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) break;
    delay(200);
  }

  if (isnan(t) || isnan(h)) {
    Serial.println("ERROR: DHT11 no responde");
    SerialBT.println("{\"error\":\"sensor\"}");
    return;
  }

  temp_actual = t;

  // JSON por Bluetooth
  String json = "{";
  json += "\"t\":"       + String(temp_actual, 1);
  json += ",\"h\":"      + String(h, 1);
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
// ESTADO DE TEMPERATURA (texto para OLED)
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
// =====================================================

void check_alarm() {

  // LED azul siempre encendido mientras haya alimentación
  digitalWrite(LED_BLUE, HIGH);

  if (!viaje_activo) {
    // Sin viaje activo — verde y rojo apagados, silencio
    digitalWrite(LED_GREEN, LOW);
    digitalWrite(LED_RED,   LOW);
    digitalWrite(BUZZER,    LOW);
    return;
  }

  unsigned long now = millis();

  if (temp_actual <= temp_max) {
    // ── NORMAL — verde fijo, rojo y buzzer apagados
    digitalWrite(LED_GREEN, HIGH);
    digitalWrite(LED_RED,   LOW);
    digitalWrite(BUZZER,    LOW);

  } else if (temp_actual < temp_alarm) {
    // ── ALERTA — verde apagado, rojo parpadeo lento 500ms, beep intermitente
    digitalWrite(LED_GREEN, LOW);
    if (now - last_blink > 500) {
      last_blink  = now;
      blink_state = !blink_state;
      digitalWrite(LED_RED, blink_state);
      digitalWrite(BUZZER,  blink_state);
    }

  } else {
    // ── CRÍTICA — verde apagado, rojo parpadeo rápido 150ms, buzzer continuo
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

  // Temperatura grande
  oled.setTextSize(2);
  oled.setCursor(0, 14);
  oled.print(temp_actual, 1);
  oled.print("C");

  // Tiempo transcurrido
  oled.setTextSize(1);
  oled.setCursor(0, 36);

  // formato hh:mm:ss si pasa de 60s
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
  oled.print("Min:"); oled.print(temp_min, 0);
  oled.print(" Max:"); oled.print(temp_max, 0);
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

  // Lectura periódica solo durante viaje
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
      if      (menu_index == 0) {
        viaje_activo = true;
        trip_start   = millis();
        lastLectura  = 0;        // fuerza lectura inmediata al entrar
        state        = STATE_VIAJE;
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
      digitalWrite(LED_GREEN, LOW);
      digitalWrite(LED_RED,   LOW);
      digitalWrite(BUZZER,    LOW);
      // azul sigue encendido — el dispositivo sigue ON
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