/*
 * V2 - SOIL MOISTURE + MQTT (da caricare via OTA sopra V1)
 * ------------------------------------------------------------
 * Legge il Capacitive Soil Moisture Sensor v1.2 su GPIO14 (D14)
 * e pubblica la percentuale di umidità terreno su MQTT, oltre
 * a stamparla su Serial per debug da PC.
 *
 * *** ATTENZIONE HARDWARE ***
 * GPIO14 è un pin ADC2. Su ESP32, l'ADC2 NON è utilizzabile in
 * modo affidabile mentre il Wi-Fi è attivo (conflitto hardware
 * noto, documentato da Espressif) -> con Wi-Fi/MQTT attivi le
 * letture su D14 possono essere bloccate o sballate.
 * Se puoi, sposta il sensore su un pin ADC1 (es. GPIO32-GPIO39,
 * tipicamente D32-D39 sulle DevKit), cambiando solo SOIL_PIN.
 * Lascio D14 come richiesto, ma sappi che è la causa più
 * probabile di letture strane una volta collegato il Wi-Fi.
 *
 * Richiede libreria: PubSubClient (Nick O'Leary) via Library Manager
 * Richiede: secrets.h con WIFI_SSID, WIFI_PASSWORD, MQTT_BROKER,
 *   MQTT_PORT, (opzionali) MQTT_USER, MQTT_PASSWORD, OTA_PASSWORD
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include "secrets.h"

#define LED_BUILTIN_PIN 2
#define OTA_HOSTNAME "legmamiteo-soil-test"
#define SOIL_PIN 32

// Calibrazione sensore capacitivo basata sui tuoi test reali
const int ADC_DRY = 4095; // Valore letto all'aria aperta
const int ADC_WET = 2500; // Soglia minima stimata per terra zuppa / bagnata

// Topic nel formato usato dal resto di LegmaMiteo: station/{STATION_ID}/base
char mqttTopic[48];
const char* MQTT_CLIENT_ID = "legmamiteo-soil-test";

const unsigned long READ_INTERVAL_MS = 10000; // ogni 10s
unsigned long lastRead = 0;

WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(OTA_HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connessione al Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connesso. IP: ");
  Serial.println(WiFi.localIP());
}

void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
#ifdef OTA_PASSWORD
  ArduinoOTA.setPassword(OTA_PASSWORD);
#endif

  ArduinoOTA
    .onStart([]() {
      Serial.println("OTA: inizio aggiornamento");
    })
    .onEnd([]() {
      Serial.println("\nOTA: completato, riavvio...");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("OTA: %u%%\r", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      Serial.printf("OTA: errore [%u]\n", error);
    });

  ArduinoOTA.begin();
  Serial.println("OTA pronto.");
}

void mqttReconnect() {
  while (!mqttClient.connected()) {
    Serial.print("Connessione MQTT...");
    bool ok = mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD);
    if (ok) {
      Serial.println(" connesso.");
    } else {
      Serial.print(" fallito, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" riprovo tra 3s");
      delay(3000);
    }
  }
}

float readSoilPercent() {
  int raw = analogRead(SOIL_PIN);
  // Capacitivo: valore ADC alto = secco, valore ADC basso = bagnato
  float percent = (float)(ADC_DRY - raw) / (float)(ADC_DRY - ADC_WET) * 100.0;
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  Serial.printf("ADC grezzo: %d -> Umidità: %.1f%%\n", raw, percent);
  return percent;
}

void publishSoilData(float percent) {
  char payload[128];
  snprintf(payload, sizeof(payload),
           "{\"soil_moisture_percent\":%.1f}", percent);

  if (mqttClient.publish(mqttTopic, payload)) {
    Serial.print("Pubblicato su ");
    Serial.print(mqttTopic);
    Serial.print(": ");
    Serial.println(payload);
  } else {
    Serial.println("Pubblicazione MQTT fallita.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(LED_BUILTIN_PIN, OUTPUT);
  analogReadResolution(12); // 0-4095, come il resto del progetto LegmaMiteo

  setupWiFi();
  setupOTA();

  snprintf(mqttTopic, sizeof(mqttTopic), "station/%s/soil", STATION_ID);

  // Connessione TLS verso il broker via Tailscale.
  // setInsecure() = non verifica il certificato del server: va bene per
  // test rapidi, ma per produzione andrebbe caricata la CA con
  // espClient.setCACert(...) per verificare davvero il certificato.
  espClient.setInsecure();

  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
}

void loop() {
  ArduinoOTA.handle();

  if (!mqttClient.connected()) {
    mqttReconnect();
  }
  mqttClient.loop();

  unsigned long now = millis();
  if (now - lastRead >= READ_INTERVAL_MS) {
    lastRead = now;
    digitalWrite(LED_BUILTIN_PIN, HIGH); // lampeggio breve a ogni lettura
    float percent = readSoilPercent();
    publishSoilData(percent);
    delay(50);
    digitalWrite(LED_BUILTIN_PIN, LOW);
  }
}
