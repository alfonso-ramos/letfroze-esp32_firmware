#include "BluetoothSerial.h"
#include "DHT.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define DHT_PIN       4    // DATA del DHT11
#define DHT_TYPE      DHT11

#define OLED_SDA      21
#define OLED_SCL      22
#define OLED_WIDTH    128
#define OLED_HEIGHT   64
#define OLED_ADDR     0x3C

#define BTN_UP        32   // PULL_UP — activo en LOW
#define BTN_DOWN      33
#define BTN_SELECT    25

#define LED_GREEN     26
#define LED_RED       27
#define BUZZER        14

// =====================================================
// CONFIGURACIÓN
// =====================================================

#define INTERVALO_LECTURA  2500   // ms entre lecturas DHT11
#define MAX_REINTENTOS     5
#define DEBOUNCE_MS        30

float temp_min   = 2.0;
float temp_max   = 8.0;
float temp_alarm = 10.0;
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
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);

  // OLED
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("ERROR: OLED no encontrada");
    while (true);
  }

  // Splash screen — igual que en MicroPython
  splash();

  // Bluetooth
  SerialBT.begin("ESP32_Termostato");

  // DHT11
  dht.begin();
  delay(2000);  // estabilización obligatoria del DHT11

  // Botones con pull-up interno
  pinMode(BTN_UP,     INPUT_PULLUP);
  pinMode(BTN_DOWN,   INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);

  // LEDs y buzzer
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED,   OUTPUT);
  pinMode(BUZZER,    OUTPUT);

  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_RED,   LOW);
  digitalWrite(BUZZER,    LOW);

  Serial.println("ColdGuard listo.");
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

  // Enviar JSON por Bluetooth
  String json = "{";
  json += "\"t\":"      + String(temp_actual, 1);
  json += ",\"h\":"     + String(h, 1);
  json += ",\"min\":"   + String(temp_min, 1);
  json += ",\"max\":"   + String(temp_max, 1);
  json += ",\"alerta\":";
  json += (temp_actual > temp_max)  ? "true"  : "false";
  json += ",\"critica\":";
  json += (temp_actual > temp_alarm) ? "true" : "false";
  json += "}";

  SerialBT.println(json);
  Serial.println(json);
}

// =====================================================
// ESTADO DE TEMPERATURA (texto)
// =====================================================

const char* obtener_estado() {
  if (temp_actual > temp_alarm) return "CRITICA";
  if (temp_actual > temp_max)   return "ALERTA";
  return "NORMAL";
}

// =====================================================
// BOTONES CON DEBOUNCE — igual que en MicroPython
// =====================================================

bool button_pressed(int pin) {
  if (digitalRead(pin) == LOW) {
    delay(DEBOUNCE_MS);
    if (digitalRead(pin) == LOW) {
      while (digitalRead(pin) == LOW);  // espera soltar
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
  if (!viaje_activo) {
    digitalWrite(LED_GREEN, LOW);
    digitalWrite(LED_RED,   LOW);
    digitalWrite(BUZZER,    LOW);
    return;
  }

  unsigned long now = millis();

  if (temp_actual <= temp_max) {
    // NORMAL — verde fijo
    digitalWrite(LED_GREEN, HIGH);
    digitalWrite(LED_RED,   LOW);
    digitalWrite(BUZZER,    LOW);

  } else if (temp_actual < temp_alarm) {
    // ALERTA — rojo parpadeante lento + beep
    digitalWrite(LED_GREEN, LOW);
    if (now - last_blink > 500) {
      last_blink  = now;
      blink_state = !blink_state;
      digitalWrite(LED_RED, blink_state);
      digitalWrite(BUZZER,  blink_state);
    }

  } else {
    // CRÍTICA — rojo parpadeante rápido + buzzer continuo
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

  // Título
  oled.setCursor(0, 0);
  oled.print("VIAJE ACTIVO");

  // Temperatura grande
  oled.setTextSize(2);
  oled.setCursor(0, 14);
  oled.print(temp_actual, 1);
  oled.print("C");

  // Info pequeña
  oled.setTextSize(1);
  oled.setCursor(0, 36);
  oled.print("T: ");
  oled.print(elapsed);
  oled.print("s");

  oled.setCursor(0, 48);
  oled.print("Min:");
  oled.print(temp_min, 0);
  oled.print(" Max:");
  oled.print(temp_max, 0);

  // Estado en esquina derecha
  oled.setCursor(70, 36);
  oled.print(obtener_estado());

  // Indicador "SEL=Fin" solo cuando hay viaje
  oled.setCursor(60, 48);
  oled.print("SEL=Fin");

  oled.display();
}

// =====================================================
// LOOP PRINCIPAL
// =====================================================

static unsigned long lastLectura = 0;

void loop() {
  // Lectura periódica del sensor
  if (state == STATE_VIAJE) {
    if (millis() - lastLectura >= INTERVALO_LECTURA) {
      lastLectura = millis();
      leer_temperatura();
    }
  }

  check_alarm();

  // --- MENÚ PRINCIPAL ---
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
      if      (menu_index == 0) { viaje_activo = true; trip_start = millis(); state = STATE_VIAJE; }
      else if (menu_index == 1) state = STATE_EDIT_MIN;
      else if (menu_index == 2) state = STATE_EDIT_MAX;
      else if (menu_index == 3) state = STATE_EDIT_ALARM;
    }
  }

  // --- VIAJE ACTIVO ---
  else if (state == STATE_VIAJE) {
    draw_viaje();

    if (button_pressed(BTN_SELECT)) {
      viaje_activo = false;
      digitalWrite(LED_GREEN, LOW);
      digitalWrite(LED_RED,   LOW);
      digitalWrite(BUZZER,    LOW);
      state = STATE_MENU;
    }
  }

  // --- EDITAR TEMP MIN ---
  else if (state == STATE_EDIT_MIN) {
    draw_edit("Temp Min", temp_min);
    if (button_pressed(BTN_UP))     temp_min += 1;
    if (button_pressed(BTN_DOWN))   temp_min -= 1;
    if (button_pressed(BTN_SELECT)) state = STATE_MENU;
  }

  // --- EDITAR TEMP MAX ---
  else if (state == STATE_EDIT_MAX) {
    draw_edit("Temp Max", temp_max);
    if (button_pressed(BTN_UP))     temp_max += 1;
    if (button_pressed(BTN_DOWN))   temp_max -= 1;
    if (button_pressed(BTN_SELECT)) state = STATE_MENU;
  }

  // --- EDITAR ALARMA ---
  else if (state == STATE_EDIT_ALARM) {
    draw_edit("Alarma", temp_alarm);
    if (button_pressed(BTN_UP))     temp_alarm += 1;
    if (button_pressed(BTN_DOWN))   temp_alarm -= 1;
    if (button_pressed(BTN_SELECT)) state = STATE_MENU;
  }

  delay(50);
}
