#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <SparkFun_BMP581_Arduino_Library.h>
#include <Adafruit_SHT4x.h>
#include <Adafruit_VEML7700.h>
#include <Adafruit_MCP9808.h>
#include <SparkFun_AS3935.h>
#include <SPI.h>
#include <SD.h>
#include "time.h"
#include "secrets.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "esp_sleep.h"
#include <Adafruit_NeoPixel.h>
#include "esp_task_wdt.h"
#include <ArduinoOTA.h>
#include <ESPmDNS.h>

const int RGB_LED_PIN = 48;
Adafruit_NeoPixel rgbLed(1, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);

void mostraQualitaSegnaleLed(int rssi) {
  uint32_t colore;
  if (rssi >= -55) {
    colore = rgbLed.Color(0, 100, 255);
  } else if (rssi >= -65) {
    colore = rgbLed.Color(0, 180, 0);
  } else if (rssi >= -75) {
    colore = rgbLed.Color(200, 180, 0);
  } else {
    colore = rgbLed.Color(200, 0, 0);
  }
  rgbLed.setPixelColor(0, colore);
  rgbLed.show();
}

void mostraMqttNonRaggiungibile() {
  uint32_t arancione = rgbLed.Color(220, 90, 0);
  rgbLed.setPixelColor(0, arancione);
  rgbLed.show();
}

void mostraRelaNonTrovata() {
  uint32_t colore = rgbLed.Color(180, 0, 200);
  rgbLed.setPixelColor(0, colore);
  rgbLed.show();
}

bool ssidVisibileInAria(const char* ssidCercato) {
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == ssidCercato) {
      WiFi.scanDelete();
      return true;
    }
  }
  WiFi.scanDelete();
  return false;
}

const char* NTP_SERVER    = "time.google.com";
const long  GMT_OFFSET_SEC = 3600;
const int   DAYLIGHT_OFFSET_SEC = 0;    // Winter: DST OFF (UTC+1)

const char* TIME_API_URL = "https://timeapi.io/api/time/current/zone?timeZone=Europe/Rome";

const int I2C_SDA = 6;
const int I2C_SCL = 5;

const int SPI_MISO = 16;
const int SPI_CLK  = 17;
const int SPI_MOSI = 15;
const int chipSelect = 18;
const int AS3935_CS = 9;
const int AS3935_IRQ = 7;

// --- Campionamento adattivo ---
const uint64_t CICLO_STABILE_US = 30000000ULL;   // 30s
const uint64_t CICLO_VELOCE_US  = 10000000ULL;   // 10s
const float SOGLIA_CALO_PRESSIONE_HPA_MIN = 1.0f; // hPa/min
const float SOGLIA_CALO_TEMPERATURA_C_MIN = 1.0f; // °C/min
const unsigned long DURATA_MODALITA_VELOCE_MS = 15UL * 60UL * 1000UL; // 15 minuti

uint64_t cicloAttualeUs = CICLO_VELOCE_US;
unsigned long modalitaVeloceFinoAMillis = 0;

bool primaLetturaAdattiva = true;
float precTemperaturaAdattiva = 0.0f;
float precPressioneAdattivaHpa = 0.0f;
unsigned long precTimestampAdattivoMs = 0;

void valutaCampionamentoAdattivo(float temperaturaAttuale, float pressioneAttualeHpa) {
  unsigned long oraMs = millis();

  if (!primaLetturaAdattiva) {
    float deltaMinuti = (float)(oraMs - precTimestampAdattivoMs) / 60000.0f;
    if (deltaMinuti > 0.01f) {
      float ratePressione = (pressioneAttualeHpa - precPressioneAdattivaHpa) / deltaMinuti;
      float rateTemperatura = fabs(temperaturaAttuale - precTemperaturaAdattiva) / deltaMinuti;

      bool caloPressioneRapido = ratePressione <= -SOGLIA_CALO_PRESSIONE_HPA_MIN;
      bool caloTemperaturaRapido = rateTemperatura >= SOGLIA_CALO_TEMPERATURA_C_MIN;

      if (caloPressioneRapido || caloTemperaturaRapido) {
        if (modalitaVeloceFinoAMillis == 0) {
          Serial.printf("[ADATTIVO] Evento rapido rilevato (pressione: %.2f hPa/min, temperatura: %.2f C/min) - passo a campionamento veloce (10s).\n",
                        ratePressione, rateTemperatura);
        }
        modalitaVeloceFinoAMillis = oraMs + DURATA_MODALITA_VELOCE_MS;
      }
    }
  }

  precTemperaturaAdattiva = temperaturaAttuale;
  precPressioneAdattivaHpa = pressioneAttualeHpa;
  precTimestampAdattivoMs = oraMs;
  primaLetturaAdattiva = false;

  if (modalitaVeloceFinoAMillis != 0 && (long)(oraMs - modalitaVeloceFinoAMillis) >= 0) {
    Serial.println("[ADATTIVO] Nessun evento rapido da un po', torno al campionamento stabile (30s).");
    modalitaVeloceFinoAMillis = 0;
  }

  cicloAttualeUs = (modalitaVeloceFinoAMillis != 0) ? CICLO_VELOCE_US : CICLO_STABILE_US;
}

bool timeSynchronized = false;
uint32_t sdReadOffset = 0;

const uint32_t SD_COMPACT_THRESHOLD_BYTES = 5000000;

const int MAX_RICONNESSIONI_CONSECUTIVE = 6;
int riconnessioniConsecutiveFallite = 0;

const uint32_t WATCHDOG_TIMEOUT_SEC = 100;

const int MAX_RESET_WIFI_FALLITI_CONSECUTIVI = 3;
int resetWifiFallitiConsecutivi = 0;

// ============================================================
// OTA (aggiornamento firmware via Wi-Fi)
// ============================================================
// ATTENZIONE CONSUMO: questa stazione passa la maggior parte del tempo
// con il WiFi completamente spento (vedi vaiInLightSleepReale()).
// Per permettere un upload OTA bisogna tenere il WiFi acceso e in
// ascolto per una finestra di tempo ad OGNI ciclo attivo, prima di
// andare in sleep — altrimenti un'eventuale richiesta OTA in arrivo
// non verrebbe mai vista. Questo allunga leggermente ogni ciclo e
// quindi il consumo medio, anche quando non si sta facendo nessun
// aggiornamento. La finestra (OTA_LISTEN_WINDOW_MS) è tenuta breve
// apposta per questo. Se la stazione resta sul campo per lunghi
// periodi senza bisogno di aggiornamenti, conviene mettere
// OTA_ABILITATO a false per eliminare del tutto questo overhead
// (richiede comunque un upload via USB per rimetterlo a true in
// futuro, visto che a OTA disattivata il WiFi non resta mai acceso
// abbastanza per riceverne uno).
const bool OTA_ABILITATO = true;
const unsigned long OTA_LISTEN_WINDOW_MS = 2000; // finestra di ascolto OTA per ogni ciclo

// NOTA PRATICA: con un ciclo da 10-30s e una finestra di soli 2s,
// è facile che l'invito OTA (UDP) mandato da PlatformIO arrivi
// mentre la stazione sta già dormendo. Se "pio run -t upload" dà
// "No response from device", è normale: riprova, il prossimo
// tentativo ha buone probabilità di centrare la finestra successiva.

volatile bool otaInCorso = false; // true durante un aggiornamento vero e proprio: in quel caso NON si va mai in sleep, qualunque sia il tempo trascorso, per non interrompere il flash a metà (interromperlo non brickerebbe la scheda — l'update scrive sulla partizione OTA inattiva — ma farebbe comunque fallire l'aggiornamento).

void setupOTA() {
  ArduinoOTA.setHostname(STATION_ID);
#ifdef OTA_PASSWORD
  ArduinoOTA.setPassword(OTA_PASSWORD);
#endif

  ArduinoOTA
    .onStart([]() {
      otaInCorso = true;
      String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
      Serial.println("[OTA] Inizio aggiornamento " + type + " — sleep sospeso finché non finisce.");
    })
    .onEnd([]() {
      Serial.println("\n[OTA] Completato, riavvio...");
      otaInCorso = false;
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      esp_task_wdt_reset(); // un trasferimento può durare più del timeout del watchdog se non lo nutriamo qui dentro
      static unsigned long ultimaStampa = 0;
      if (millis() - ultimaStampa > 2000) {
        Serial.printf("[OTA] %u%%\n", (progress / (total / 100)));
        ultimaStampa = millis();
      }
    })
    .onError([](ota_error_t error) {
      otaInCorso = false;
      Serial.printf("[OTA] Errore [%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("autenticazione fallita");
      else if (error == OTA_BEGIN_ERROR) Serial.println("begin fallito");
      else if (error == OTA_CONNECT_ERROR) Serial.println("connessione fallita");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("ricezione fallita");
      else if (error == OTA_END_ERROR) Serial.println("end fallito");
    });

  ArduinoOTA.begin();
}

// Da chiamare ad ogni ciclo, subito dopo che il WiFi è connesso e PRIMA
// di andare in sleep. Riavvia ArduinoOTA da zero ogni volta (coerente
// col resto del firmware: anche WiFi e MQTT si ri-inizializzano
// completamente ad ogni ciclo, visto che il radio viene spento del
// tutto durante il sonno). Ascolta per una finestra breve; se un
// aggiornamento viene davvero avviato, resta in ascolto SENZA LIMITE DI
// TEMPO finché non finisce (successo o errore), per non rischiare di
// spegnere il WiFi a metà trasferimento.
void ascoltaOTA() {
  if (!OTA_ABILITATO) return;

  setupOTA();

  unsigned long inizio = millis();
  while (true) {
    ArduinoOTA.handle();
    esp_task_wdt_reset();

    if (otaInCorso) {
      continue; // aggiornamento in corso: ignora la finestra di tempo, continua finché non termina
    }
    if (millis() - inizio >= OTA_LISTEN_WINDOW_MS) {
      break;
    }
  }
}

WiFiClientSecure wifiClient;
PubSubClient mqtt(wifiClient);
BMP581 bmp580;
Adafruit_SHT4x sht4;
Adafruit_VEML7700 veml7700 = Adafruit_VEML7700();
Adafruit_MCP9808 mcp9808 = Adafruit_MCP9808();
SparkFun_AS3935 as3935;

String getTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "0000-00-00 00:00:00";
  }
  char timeStringBuff[50];
  strftime(timeStringBuff, sizeof(timeStringBuff), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(timeStringBuff);
}

bool syncTimeViaHttp() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  Serial.print("[TIME-HTTP] Richiesta orario via HTTPS a timeapi.io...");

  if (!http.begin(client, TIME_API_URL)) {
    Serial.println(" Impossibile avviare la richiesta.");
    return false;
  }

  http.setTimeout(8000);

  int httpCode = http.GET();
  if (httpCode != 200) {
    Serial.printf(" Fallita, HTTP code=%d\n", httpCode);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.println(" Errore nel parsing JSON della risposta.");
    return false;
  }

  int year   = doc["year"]   | 0;
  int month  = doc["month"]  | 0;
  int day    = doc["day"]    | 0;
  int hour   = doc["hour"]   | 0;
  int minute = doc["minute"] | 0;
  int second = doc["seconds"]| 0;

  if (year < 2024) {
    Serial.println(" Risposta JSON priva di dati orario validi.");
    return false;
  }

  struct tm t;
  t.tm_year = year - 1900;
  t.tm_mon  = month - 1;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min  = minute;
  t.tm_sec  = second;
  t.tm_isdst = 0;

  time_t epoch = mktime(&t);
  struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
  settimeofday(&tv, NULL);

  Serial.println(" OK! Orario impostato da timeapi.io.");
  return true;
}

void trySyncTime() {
  configTime(3600, 0, NTP_SERVER, "pool.ntp.org", "time.cloudflare.com");

  if (timeSynchronized) return;
  if (WiFi.status() != WL_CONNECTED) return;

  Serial.print("[TIME] Sincronizzazione orario con NTP in corso...");
  struct tm timeinfo;
  int ntpAttempts = 0;

  while (!getLocalTime(&timeinfo) && ntpAttempts < 8) {
    delay(500);
    Serial.print(".");
    ntpAttempts++;
  }

  if (ntpAttempts < 8) {
    Serial.println("\n[TIME] Orologio interno sincronizzato con successo (NTP)!");
    timeSynchronized = true;
    return;
  }

  Serial.println("\n[TIME] Timeout NTP. Provo il fallback HTTP...");
  if (syncTimeViaHttp()) {
    timeSynchronized = true;
  } else {
    Serial.println("[TIME] ATTENZIONE: Fallback HTTP fallito anch'esso. Riproverò più avanti.");
  }
}

bool connectWifiCompleto() {
  Serial.print("[WiFi] Connessione completa in corso...");
  WiFi.mode(WIFI_STA);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    int rssi = WiFi.RSSI();
    Serial.println("\n[WiFi] Connesso! IP: " + WiFi.localIP().toString() +
                    " (RSSI: " + String(rssi) + " dBm)");
    mostraQualitaSegnaleLed(rssi);

    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    trySyncTime();
    return true;
  }

  if (ssidVisibileInAria(WIFI_SSID)) {
    Serial.println("\n[WiFi] Rete rilevata ma connessione fallita (segnale instabile o password errata).");
    uint32_t rosso = rgbLed.Color(200, 0, 0);
    rgbLed.setPixelColor(0, rosso);
    rgbLed.show();
  } else {
    Serial.println("\n[WiFi] Rete NON rilevata nell'aria (router spento o fuori portata).");
    mostraRelaNonTrovata();
  }

  riconnessioniConsecutiveFallite++;
  return false;
}

bool connectMqtt() {
  if (WiFi.status() != WL_CONNECTED) return false;

  wifiClient.setInsecure();
  wifiClient.setTimeout(10000);

  Serial.print("[MQTT] Connessione TLS in corso...");
  if (mqtt.connect(STATION_ID, MQTT_USER, MQTT_PASSWORD)) {
    Serial.println("Connesso!");
    return true;
  }

  Serial.print("Fallita, errore=");
  Serial.println(mqtt.state());

  IPAddress ipAttuale = WiFi.localIP();
  IPAddress gatewayAttuale = WiFi.gatewayIP();
  IPAddress subnetAttuale = WiFi.subnetMask();
  IPAddress dnsEsternoPrimario(8, 8, 8, 8);
  IPAddress dnsEsternoSecondario(1, 1, 1, 1);

  if (WiFi.config(ipAttuale, gatewayAttuale, subnetAttuale, dnsEsternoPrimario, dnsEsternoSecondario)) {
    Serial.print("[MQTT] Riprovo con DNS esterno...");
    delay(200);
    if (mqtt.connect(STATION_ID, MQTT_USER, MQTT_PASSWORD)) {
      Serial.println("Connesso!");
      return true;
    }
    Serial.print("Fallita anche con DNS esterno, errore=");
    Serial.println(mqtt.state());
  }

  return false;
}

bool assicuraConnessioneAttiva() {
  if (WiFi.status() == WL_CONNECTED) {
    if (mqtt.connected()) {
      riconnessioniConsecutiveFallite = 0;
      return true;
    }

    Serial.println("[MQTT] Socket caduto durante il light sleep, riconnessione rapida...");
    if (connectMqtt()) {
      riconnessioniConsecutiveFallite = 0;
      return true;
    }

    riconnessioniConsecutiveFallite++;
  } else {
    Serial.println("[WiFi] Associazione persa durante il light sleep.");
    riconnessioniConsecutiveFallite++;
  }

  if (riconnessioniConsecutiveFallite >= MAX_RICONNESSIONI_CONSECUTIVE) {
    Serial.println("[WiFi] Troppi fallimenti consecutivi, reset completo dello stack WiFi.");
    mqtt.disconnect();
    WiFi.disconnect(true);
    delay(100);
    bool wifiOk = connectWifiCompleto();
    bool mqttOk = wifiOk && connectMqtt();

    if (mqttOk) {
      resetWifiFallitiConsecutivi = 0;
      return true;
    }

    resetWifiFallitiConsecutivi++;
    riconnessioniConsecutiveFallite = 0;

    if (resetWifiFallitiConsecutivi >= MAX_RESET_WIFI_FALLITI_CONSECUTIVI) {
      Serial.println("[SISTEMA] Reset WiFi ripetuti falliti troppe volte: riavvio completo del microcontrollore.");
      Serial.flush();
      delay(200);
      esp_restart();
    }

    return false;
  }

  return false;
}

const unsigned long SD_FLUSH_BUDGET_NORMALE_MS   = 600;
const unsigned long SD_FLUSH_BUDGET_MEDIO_MS     = 3000;
const unsigned long SD_FLUSH_BUDGET_AGGRESSIVO_MS = 8000;
const uint32_t SD_SOGLIA_BACKLOG_MEDIO_BYTES      = 50000;
const uint32_t SD_SOGLIA_BACKLOG_GRANDE_BYTES     = 500000;

const unsigned long SD_FLUSH_DELAY_NORMALE_MS = 50;
const unsigned long SD_FLUSH_DELAY_AGGRESSIVO_MS = 2;
const int SD_FLUSH_MAX_ROWS_PER_CYCLE_NORMALE = 10;

void archiviaBacklogInveceDiCancellare() {
  String ts = getTimestamp();
  ts.replace(' ', '_');
  ts.replace(':', '-');
  String nomeArchivio = "/archivio_" + ts + ".jsonl";

  if (SD.exists(nomeArchivio)) {
    nomeArchivio = "/archivio_" + ts + "_" + String(millis()) + ".jsonl";
  }

  if (SD.rename("/backup.jsonl", nomeArchivio.c_str())) {
    Serial.println("[SD-RECOVERY] Backlog archiviato come: " + nomeArchivio);
  } else {
    Serial.println("[SD-RECOVERY] Rename archivio fallito, elimino il file.");
    SD.remove("/backup.jsonl");
  }
}

void svuotaCodaSD() {
  if (!SD.exists("/backup.jsonl")) {
    sdReadOffset = 0;
    return;
  }

  File file = SD.open("/backup.jsonl", FILE_READ);
  if (!file) {
    Serial.println("[SD-RECOVERY] Errore critico nell'apertura del file di backup.");
    return;
  }

  uint32_t fileSize = file.size();
  if (sdReadOffset > fileSize) sdReadOffset = 0;

  if (sdReadOffset >= fileSize) {
    file.close();
    archiviaBacklogInveceDiCancellare();
    sdReadOffset = 0;
    return;
  }

  uint32_t backlogResiduo = fileSize - sdReadOffset;
  unsigned long flushBudgetMs;
  int maxRighePerCiclo;
  unsigned long flushDelayMs;

  bool inModalitaStabile = (cicloAttualeUs == CICLO_STABILE_US);

  if (inModalitaStabile && backlogResiduo > 0) {
    const unsigned long SD_FLUSH_MARGINE_MS = 1500;
    unsigned long budgetDisponibile = (unsigned long)(cicloAttualeUs / 1000ULL);
    budgetDisponibile = (budgetDisponibile > SD_FLUSH_MARGINE_MS) ? (budgetDisponibile - SD_FLUSH_MARGINE_MS) : SD_FLUSH_BUDGET_AGGRESSIVO_MS;
    flushBudgetMs = (budgetDisponibile > SD_FLUSH_BUDGET_AGGRESSIVO_MS) ? budgetDisponibile : SD_FLUSH_BUDGET_AGGRESSIVO_MS;
    maxRighePerCiclo = INT32_MAX;
    flushDelayMs = SD_FLUSH_DELAY_AGGRESSIVO_MS;
    Serial.printf("[SD-RECOVERY] Modalità stabile con backlog residuo: recupero esteso attivo (budget %lums).\n", flushBudgetMs);
  } else if (backlogResiduo >= SD_SOGLIA_BACKLOG_GRANDE_BYTES) {
    flushBudgetMs = SD_FLUSH_BUDGET_AGGRESSIVO_MS;
    maxRighePerCiclo = INT32_MAX;
    flushDelayMs = SD_FLUSH_DELAY_AGGRESSIVO_MS;
  } else if (backlogResiduo >= SD_SOGLIA_BACKLOG_MEDIO_BYTES) {
    flushBudgetMs = SD_FLUSH_BUDGET_MEDIO_MS;
    maxRighePerCiclo = INT32_MAX;
    flushDelayMs = SD_FLUSH_DELAY_AGGRESSIVO_MS;
  } else {
    flushBudgetMs = SD_FLUSH_BUDGET_NORMALE_MS;
    maxRighePerCiclo = SD_FLUSH_MAX_ROWS_PER_CYCLE_NORMALE;
    flushDelayMs = SD_FLUSH_DELAY_NORMALE_MS;
  }

  Serial.printf("[SD-RECOVERY] Riprendo l'invio da offset %u/%u byte...\n", sdReadOffset, fileSize);

  file.seek(sdReadOffset);

  char topic[64];
  snprintf(topic, sizeof(topic), "station/%s/base", STATION_ID);

  unsigned long flushStart = millis();
  int righeInviate = 0;

  while (file.available()) {
    esp_task_wdt_reset();
    if ((millis() - flushStart) >= flushBudgetMs) {
      break;
    }
    if (righeInviate >= maxRighePerCiclo) {
      break;
    }

    static char rigaBuf[300];
    size_t rigaLen = file.readBytesUntil('\n', rigaBuf, sizeof(rigaBuf) - 1);
    rigaBuf[rigaLen] = '\0';

    while (rigaLen > 0 && (rigaBuf[rigaLen - 1] == '\r' || rigaBuf[rigaLen - 1] == ' ' || rigaBuf[rigaLen - 1] == '\n')) {
      rigaBuf[--rigaLen] = '\0';
    }

    if (rigaLen == 0) {
      sdReadOffset = file.position();
      continue;
    }

    if (mqtt.connected() && mqtt.publish(topic, rigaBuf)) {
      righeInviate++;
      sdReadOffset = file.position();
      delay(flushDelayMs);
      continue;
    }

    Serial.println("[SD-RECOVERY] Invio fallito, mi fermo qui per questo ciclo.");
    break;
  }

  file.close();

  if (sdReadOffset >= fileSize) {
    archiviaBacklogInveceDiCancellare();
    sdReadOffset = 0;
    Serial.printf("[SD-RECOVERY] Lotto inviato: %d righe. Coda SD svuotata (archiviata, non cancellata).\n", righeInviate);
    return;
  }

  Serial.printf("[SD-RECOVERY] Lotto inviato: %d righe. Offset a %u/%u byte.\n",
                righeInviate, sdReadOffset, fileSize);

  if (sdReadOffset >= SD_COMPACT_THRESHOLD_BYTES) {
    Serial.println("[SD-RECOVERY] Compattazione file di backup in corso...");

    File src = SD.open("/backup.jsonl", FILE_READ);
    if (!src) return;
    src.seek(sdReadOffset);

    if (SD.exists("/backup.tmp")) SD.remove("/backup.tmp");
    File dst = SD.open("/backup.tmp", FILE_WRITE);
    if (!dst) { src.close(); return; }

    static uint8_t buf[4096];
    int n;
    while ((n = src.read(buf, sizeof(buf))) > 0) {
      esp_task_wdt_reset();
      dst.write(buf, n);
    }
    src.close();
    dst.close();

    SD.remove("/backup.jsonl");
    SD.rename("/backup.tmp", "/backup.jsonl");
    sdReadOffset = 0;
    Serial.println("[SD-RECOVERY] Compattazione completata.");
  }
}

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

void handleDataCycle(bool mqttReady) {
  bmp5_sensor_data bmpData;
  bmp580.getSensorData(&bmpData);
  float real_temp  = bmpData.temperature;
  float real_press = bmpData.pressure / 100.0F;

  valutaCampionamentoAdattivo(real_temp, real_press);

  sensors_event_t humidity, temp;
  sht4.getEvent(&humidity, &temp);
  float h = humidity.relative_humidity;
  float vemlLux = veml7700.readLux();
  float mcpTemp = mcp9808.readTempC();
  int as3935Intr = as3935.readInterruptReg();

  float wind     = 0.0;
  float wind_dir = 0.0;
  float rain     = 0.0;

  JsonDocument doc;
  doc["station_id"]          = STATION_ID;
  doc["timestamp"]           = getTimestamp();
  doc["temperature"]         = real_temp;
  doc["temperature_mcp9808"] = mcpTemp;
  doc["temperature_sht45"]   = temp.temperature;
  doc["humidity"]            = h;
  doc["pressure"]            = real_press;
  doc["lux"]                 = vemlLux;
  doc["as3935_interrupt"]    = as3935Intr;
  doc["wind_speed"]          = wind;
  doc["wind_direction"]      = wind_dir;
  doc["rain_mm"]             = rain;

  char payload[384];
  serializeJson(doc, payload);

  if (mqttReady && mqtt.connected()) {
    char topic[64];
    snprintf(topic, sizeof(topic), "station/%s/base", STATION_ID);
    if (mqtt.publish(topic, payload)) {
      Serial.println("[MQTT] Inviato: " + String(payload));
      return;
    }
  }

  scriviSuSD(String(payload));
}

void vaiInLightSleepReale(unsigned long cycleStartMillis, bool connessioneRiuscita) {
  uint64_t cicloEffettivoUs = connessioneRiuscita ? cicloAttualeUs : CICLO_VELOCE_US;

  unsigned long elapsedMs = millis() - cycleStartMillis;
  long sleepMs = (long)(cicloEffettivoUs / 1000ULL) - (long)elapsedMs;
  const long SLEEP_MINIMO_MS = 500;
  if (sleepMs < SLEEP_MINIMO_MS) sleepMs = SLEEP_MINIMO_MS;

  Serial.printf("[SLEEP] Ciclo attivo: %lums. Light sleep reale per %ldms. [Modalità: %s%s]\n",
                elapsedMs, sleepMs,
                (cicloAttualeUs == CICLO_VELOCE_US) ? "VELOCE 10s" : "stabile 30s",
                connessioneRiuscita ? "" : " - RETRY RAPIDO per connessione fallita");

  if (mqtt.connected()) mqtt.disconnect();
  WiFi.disconnect(false);
  delay(100);
  esp_wifi_stop();
  delay(50);

  Serial.flush();

  esp_sleep_enable_timer_wakeup((uint64_t)sleepMs * 1000ULL);
  esp_light_sleep_start();

  esp_wifi_start();
  delay(50);
}

void setup() {
  Serial.begin(115200);
  delay(100);

  esp_task_wdt_init(WATCHDOG_TIMEOUT_SEC, true);
  esp_task_wdt_add(NULL);

  Serial.println("\n--- LegmaMiteo: avvio con architettura light sleep + connessione persistente ---");

  rgbLed.begin();
  rgbLed.setBrightness(40);

  configTime(3600, 0, NTP_SERVER, "pool.ntp.org", "time.cloudflare.com");

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);
  delay(200);

  int8_t bmpErr = bmp580.beginI2C(0x46);
  if (bmpErr == BMP5_OK) Serial.println("[BMP580] Inizializzazione I2C riuscita (0x46)!");
  else Serial.printf("[BMP580] ERRORE di inizializzazione. Codice: %d\n", bmpErr);

  if (sht4.begin(&Wire)) { sht4.setPrecision(SHT4X_HIGH_PRECISION); sht4.setHeater(SHT4X_NO_HEATER); Serial.println("[SHT45] Riuscita (0x44)!"); }
  else Serial.println("[SHT45] FALLITA!");

  if (veml7700.begin()) Serial.println("[VEML7700] Riuscita (0x10)!");
  else Serial.println("[VEML7700] FALLITA!");

  if (mcp9808.begin(0x18)) { mcp9808.setResolution(3); Serial.println("[MCP9808] Riuscita (0x18)!"); }
  else Serial.println("[MCP9808] FALLITA!");

  SPI.begin(SPI_CLK, SPI_MISO, SPI_MOSI, chipSelect);
  Serial.print("[HARDWARE] Inizializzazione modulo SD... ");
  if (!SD.begin(chipSelect)) {
    Serial.println("FALLITA! Controlla pin, contatti o formattazione FAT32.");
  } else {
    Serial.println("OK, MicroSD rilevata.");
  }

  if (as3935.beginSPI(AS3935_CS, 1000000, SPI)) {
    as3935.setIndoorOutdoor(INDOOR);
    as3935.setNoiseLevel(2);
    as3935.watchdogThreshold(2);
    as3935.spikeRejection(2);
    Serial.println("[AS3935] Inizializzazione SPI riuscita!");
  } else {
    Serial.println("[AS3935] FALLITA!");
  }

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);
  mqtt.setSocketTimeout(10);
  mqtt.setKeepAlive(15);
}

void loop() {
  esp_task_wdt_reset();

  unsigned long cycleStart = millis();

  bool wifiOk = connectWifiCompleto();

  if (wifiOk) {
    ascoltaOTA(); // finestra di ascolto OTA — vedi commenti nella sezione OTA sopra
  }

  bool mqttOk = wifiOk && connectMqtt();

  if (!wifiOk) {
    riconnessioniConsecutiveFallite++;
  } else if (!mqttOk) {
    Serial.println("[MQTT] Broker non raggiungibile in questo ciclo.");
    mostraMqttNonRaggiungibile();
    riconnessioniConsecutiveFallite++;
  }

  if (riconnessioniConsecutiveFallite >= MAX_RICONNESSIONI_CONSECUTIVE) {
    resetWifiFallitiConsecutivi++;
    riconnessioniConsecutiveFallite = 0;
    if (resetWifiFallitiConsecutivi >= MAX_RESET_WIFI_FALLITI_CONSECUTIVI) {
      Serial.println("[SISTEMA] Troppi cicli falliti consecutivi: riavvio completo del microcontrollore.");
      Serial.flush();
      delay(200);
      esp_restart();
    }
  } else if (mqttOk) {
    resetWifiFallitiConsecutivi = 0;
    riconnessioniConsecutiveFallite = 0;
  }

  if (mqtt.connected()) mqtt.loop();

  handleDataCycle(mqttOk);

  if (mqttOk) svuotaCodaSD();

  vaiInLightSleepReale(cycleStart, mqttOk);
}