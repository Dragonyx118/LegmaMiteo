// ============================================================================
// LegmaMiteo Firmware — ESP32 (BMP180 Reale — Backup Continuo su Modulo SD)
// Modalità: Test continuo con Power Bank, salvataggio su SD e svuotamento automatico
// FIX: SD.open con FILE_APPEND invece di FILE_WRITE (che su ESP32 = "w+" e
//       tronca il file ogni volta, causando la perdita di tutti i dati tranne
//       l'ultimo). Aggiunto anche un tentativo immediato di reconnect MQTT
//       appena il WiFi torna disponibile, per svuotare la coda più in fretta.
// FIX NTP: Attesa attiva della sincronizzazione orario nel setup.
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_BMP085.h>
#include <SPI.h>
#include <SD.h>
#include "time.h"

// --- Configurazione Rete e MQTT ---
const char* WIFI_SSID     = "LAPTOP1234";
const char* WIFI_PASSWORD = "12345678";
const char* MQTT_HOST     = "192.168.137.1";
const int   MQTT_PORT     = 1883;
const char* STATION_ID    = "station-001";
const char* MQTT_USER     = "station-esp32";
const char* MQTT_PASSWORD = "LegmaMiteo2026!";

// Server NTP per l'orario esatto
const char* NTP_SERVER    = "pool.ntp.org";
const long  GMT_OFFSET_SEC = 3600;        // Italia: UTC+1
const int   DAYLIGHT_OFFSET_SEC = 3600;   // Ora legale attivata

// --- Pin del Modulo SD ---
const int chipSelect = 5; 

// --- Temporizzatori (in millisecondi) ---
const unsigned long SEND_INTERVAL       = 10000; // Lettura/Salvataggio ogni 10 secondi
const unsigned long WIFI_CHECK_INTERVAL = 30000; // Controllo Wi-Fi ogni 30 secondi

unsigned long lastSend = 0;
unsigned long lastWifiCheck = 0;
bool timeSynchronized = false;

// --- Oggetti ---
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
Adafruit_BMP085 bmp;

// --- Funzione per generare la stringa con l'orario corrente ---
String getTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "0000-00-00 00:00:00"; 
  }
  char timeStringBuff[50];
  strftime(timeStringBuff, sizeof(timeStringBuff), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(timeStringBuff);
}

// --- Connessione Wi-Fi asincrona con attesa NTP ---
void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.print("\n[WiFi] Tentativo di connessione...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 8) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connesso! IP: " + WiFi.localIP().toString());
    
    if (!timeSynchronized) {
      configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
      
      // --- MODIFICA OPZIONE 1: Attesa attiva del server NTP ---
      Serial.print("[TIME] Sincronizzazione orario con NTP in corso...");
      struct tm timeinfo;
      int ntpAttempts = 0;
      
      // Attende che getLocalTime restituisca true (max 5 secondi di timeout)
      while (!getLocalTime(&timeinfo) && ntpAttempts < 10) {
        delay(500);
        Serial.print(".");
        ntpAttempts++;
      }
      
      if (ntpAttempts < 10) {
        Serial.println("\n[TIME] Orologio interno sincronizzato con successo!");
        timeSynchronized = true;
      } else {
        Serial.println("\n[TIME] Errore: Timeout sincronizzazione NTP. Server non raggiungibile.");
      }
      // --------------------------------------------------------
    }
  } else {
    Serial.println("\n[WiFi] Rete offline. Uso la scheda SD.");
  }
}

// --- Invia i dati accumulati sulla SD e ripulisce il file ---
void svuotaCodaSD() {
  if (!SD.exists("/backup.jsonl")) return;

  Serial.println("\n[SD-RECOVERY] Rete ripristinata! Lettura ed invio dati memorizzati su SD...");
  
  File file = SD.open("/backup.jsonl", FILE_READ);
  if (!file) {
    Serial.println("[SD-RECOVERY] Errore critico nell'apertura del file di backup.");
    return;
  }

  char topic[64];
  snprintf(topic, sizeof(topic), "station/%s/base", STATION_ID);

  while (file.available()) {
    String rigaPayload = file.readStringUntil('\n');
    rigaPayload.trim();
    if (rigaPayload.length() > 0) {
      mqtt.publish(topic, rigaPayload.c_str());
      Serial.println("[MQTT] Inviato storico da SD: " + rigaPayload);
      delay(100); 
    }
  }
  file.close();

  SD.remove("/backup.jsonl");
  Serial.println("[SD-RECOVERY] Coda SD svuotata. File di backup rimosso.");
}

// --- Connessione Broker MQTT ---
void connectMqtt() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  if (!mqtt.connected()) {
    Serial.print("[MQTT] Connessione in corso...");
    if (mqtt.connect(STATION_ID, MQTT_USER, MQTT_PASSWORD)) {
      Serial.println("Connesso!");
      svuotaCodaSD();
    } else {
      Serial.print("Fallita, errore=");
      Serial.println(mqtt.state());
    }
  }
}

// --- Scrittura fisica su file in MicroSD ---
void scriviSuSD(String payload) {
  File file = SD.open("/backup.jsonl", FILE_APPEND);
  if (!file) {
    Serial.println("[SD-ERROR] Impossibile scrivere sulla scheda MicroSD!");
    return;
  }
  file.println(payload);
  file.close();
  Serial.println("[SD-STORE] Dati salvati localmente su SD: " + payload);
}

// --- Acquisizione e smistamento ---
void handleDataCycle() {
  float real_temp = bmp.readTemperature();
  float real_press = bmp.readPressure() / 100.0F;

  float humidity = 0.0;   
  float lux      = 0.0;   
  float wind     = 0.0;   
  float wind_dir = 0.0;   
  float rain     = 0.0;   

  JsonDocument doc;
  doc["station_id"]      = STATION_ID;
  doc["timestamp"]       = getTimestamp(); 
  doc["temperature"]     = real_temp;
  doc["humidity"]        = humidity;
  doc["pressure"]        = real_press;
  doc["lux"]             = lux;
  doc["wind_speed"]      = wind;
  doc["wind_direction"]  = wind_dir;
  doc["rain_mm"]         = rain;

  char payload[256];
  serializeJson(doc, payload);

  if (WiFi.status() == WL_CONNECTED && mqtt.connected()) {
    char topic[64];
    snprintf(topic, sizeof(topic), "station/%s/base", STATION_ID);
    if (mqtt.publish(topic, payload)) {
      Serial.println("[MQTT] Inviato Live: " + String(payload));
      return; 
    }
  }

  scriviSuSD(String(payload));
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n--- Avvio Stazione LegmaMiteo + Modulo SD ---");

  if (!bmp.begin()) {
    Serial.println("[HARDWARE] BMP180 non rilevato!");
  } else {
    Serial.println("[HARDWARE] BMP180 pronto.");
  }

  Serial.print("[HARDWARE] Inizializzazione modulo SD... ");
  if (!SD.begin(chipSelect)) {
    Serial.println("FALLITA! Controlla pin, contatti o formattazione FAT32.");
  } else {
    Serial.println("OK, MicroSD rilevata.");
  }

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  
  connectWifi();
  connectMqtt();

  lastSend = millis();
  lastWifiCheck = millis();
}

void loop() {
  if (WiFi.status() == WL_CONNECTED && mqtt.connected()) {
    mqtt.loop();
  }

  unsigned long currentMillis = millis();

  if (currentMillis - lastWifiCheck >= WIFI_CHECK_INTERVAL) {
    lastWifiCheck = currentMillis;
    if (WiFi.status() != WL_CONNECTED) {
      connectWifi();
      if (WiFi.status() == WL_CONNECTED) {
        connectMqtt();
      }
    } else {
      connectMqtt();
    }
  }

  if (currentMillis - lastSend >= SEND_INTERVAL) {
    lastSend = currentMillis;
    handleDataCycle();
  }
}