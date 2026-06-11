// ===========================================
// LegmaMiteo Firmware — ESP32
// Modalità: dati simulati (no sensori)
// ===========================================

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// --- Configurazione ---
const char* WIFI_SSID     = "LAPTOP1234";
const char* WIFI_PASSWORD = "12345678";
const char* MQTT_HOST     = "192.168.137.1";  // es. 192.168.1.100
const int   MQTT_PORT     = 1883;
const char* STATION_ID    = "station-001";
const int   SEND_INTERVAL = 30000; // ms tra un invio e l'altro

// --- Oggetti ---
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// --- Connessione WiFi ---
void connectWifi() {
  Serial.print("Connessione WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connesso — IP: " + WiFi.localIP().toString());
}

// --- Connessione MQTT ---
void connectMqtt() {
  while (!mqtt.connected()) {
    Serial.print("Connessione MQTT...");
    if (mqtt.connect(STATION_ID)) {
      Serial.println("OK");
    } else {
      Serial.println("Fallita, retry tra 5s");
      delay(5000);
    }
  }
}

// --- Genera e invia dati simulati ---
void sendData() {
  // Valori random realistici
  float temp     = 15.0 + random(0, 150) / 10.0;   // 15-30°C
  float humidity = 40.0 + random(0, 500) / 10.0;   // 40-90%
  float pressure = 1000.0 + random(0, 300) / 10.0; // 1000-1030 hPa
  float lux      = random(0, 100000);               // 0-100k lux
  float wind     = random(0, 500) / 10.0;           // 0-50 km/h
  float wind_dir = random(0, 360);                  // 0-360°
  float rain     = random(0, 20) / 10.0;            // 0-2 mm

  // Costruisci JSON
  JsonDocument doc;
  doc["station_id"]      = STATION_ID;
  doc["temperature"]     = temp;
  doc["humidity"]        = humidity;
  doc["pressure"]        = pressure;
  doc["lux"]             = lux;
  doc["wind_speed"]      = wind;
  doc["wind_direction"]  = wind_dir;
  doc["rain_mm"]         = rain;

  char payload[256];
  serializeJson(doc, payload);

  // Topic MQTT
  char topic[64];
  snprintf(topic, sizeof(topic), "station/%s/base", STATION_ID);

  mqtt.publish(topic, payload);
  Serial.println("Inviato: " + String(payload));
}

void setup() {
  Serial.begin(115200);
  connectWifi();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  connectMqtt();
}

void loop() {
  if (!mqtt.connected()) connectMqtt();
  mqtt.loop();

  static unsigned long lastSend = 0;
  if (millis() - lastSend >= SEND_INTERVAL) {
    lastSend = millis();
    sendData();
  }
}