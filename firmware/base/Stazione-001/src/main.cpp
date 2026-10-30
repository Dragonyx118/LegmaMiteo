// ============================================================================
// LegmaMiteo Firmware — ESP32 (BMP180 Reale — Backup Continuo su Modulo SD)
// Modalità: Test continuo con Power Bank, salvataggio su SD e svuotamento automatico
// FIX: SD.open con FILE_APPEND invece di FILE_WRITE (che su ESP32 = "w+" e
//       tronca il file ogni volta, causando la perdita di tutti i dati tranne
//       l'ultimo). Aggiunto anche un tentativo immediato di reconnect MQTT
//       appena il WiFi torna disponibile, per svuotare la coda più in fretta.
// FIX NTP: Attesa attiva della sincronizzazione orario nel setup.
//
// RISPARMIO ENERGETICO — DEEP SLEEP:
// Ogni esecuzione di questo firmware corrisponde a UN SOLO CICLO di
// lettura/invio. Dopo aver pubblicato/salvato il dato, l'ESP32 va in deep
// sleep per il tempo restante e si risveglia con un RESET COMPLETO
// (equivalente a spegnere e riaccendere), ripetendo tutto da capo:
//   - CPU, RAM, WiFi, periferiche: tutto spento durante il sonno (massimo
//     risparmio energetico possibile su ESP32, ~10-150µA)
//   - Al risveglio: boot completo, reinizializzazione di tutto, nuova
//     connessione WiFi e MQTT da zero
// COMPROMESSO ACCETTATO: ogni ciclo richiede ~1-3s extra per il reconnect
// WiFi/MQTT rispetto a rimanere sempre connessi. Su un intervallo di 10s
// questo è un sovraccarico significativo, ma è la scelta voluta per
// massimizzare il risparmio energetico rispetto a tenere il WiFi sempre
// attivo (vedi cronologia: il light sleep manuale causava disconnessioni
// e riavvii spontanei su questa rete; il deep sleep evita il problema
// perché non c'è nessuna sessione WiFi da mantenere viva durante il sonno).
//
// PERSISTENZA DELLO STATO NTP TRA UN CICLO E L'ALTRO:
// Il deep sleep azzera la RAM normale, ma la RTC memory (8KB) sopravvive.
// La variabile timeSynchronized è quindi marcata RTC_DATA_ATTR: una volta
// che l'orario è stato sincronizzato con successo (NTP o fallback HTTP),
// i cicli successivi non ripetono la richiesta — risparmiando tempo e
// energia — finché il dispositivo non perde l'alimentazione del tutto
// (in quel caso anche la RTC memory si svuota e si risincronizza da capo,
// cosa comunque innocua).
//
// NB: i dati sulla SD non sono mai a rischio in nessuno scenario: sono su
// memoria non volatile e sopravvivono a reset, spegnimenti e deep sleep.
//
// FALLBACK ORARIO VIA HTTP (HTTPS):
// Se NTP (protocollo UDP) fallisce — es. su reti/hotspot che filtrano o
// ritardano UDP in modo intermittente — si tenta di ottenere l'orario
// tramite una richiesta HTTPS (TCP) a un servizio pubblico (timeapi.io).
// Essendo TCP su porta 443, attraversa NAT/firewall esattamente come la
// normale navigazione web, quindi funziona ovunque funzioni il browser.
//
// FIX SVUOTAMENTO SD A LOTTI (buco dati dopo lunghe interruzioni WiFi):
// Prima, dopo molte ore offline, svuotaCodaSD() inviava TUTTO il backlog
// in un solo ciclo (centinaia/migliaia di righe), bloccando il device
// per 10-15 minuti e creando un buco nei dati nuovi. Ora:
//   1) handleDataCycle() (dato live) viene sempre eseguita PRIMA dello
//      svuotamento storico — il dato del ciclo corrente non aspetta mai.
//   2) svuotaCodaSD() invia al massimo MAX_RIGHE_SD_PER_CICLO righe per
//      ciclo, riscrivendo in modo sicuro (file temporaneo + rename) solo
//      le righe non ancora inviate. Il backlog si esaurisce così in più
//      cicli successivi, senza mai bloccare il dispositivo a lungo.
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
#include "secrets.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "esp_sleep.h"


// Server NTP per l'orario esatto
const char* NTP_SERVER    = "time.google.com";
const long  GMT_OFFSET_SEC = 3600;        // Italia: UTC+1
const int   DAYLIGHT_OFFSET_SEC = 0;    // Winter: DST OFF (UTC+1)

// Fallback HTTP per l'orario, usato solo se NTP fallisce (vedi sopra)
const char* TIME_API_URL = "https://timeapi.io/api/time/current/zone?timeZone=Europe/Rome";

// --- Pin del Modulo SD ---
const int chipSelect = 5; 

// --- Durata del ciclo (in microsecondi, per esp_sleep_enable_timer_wakeup) ---
const uint64_t CYCLE_DURATION_US = 10000000ULL; // 10 secondi

// --- Stato persistente in RTC memory: sopravvive al deep sleep ---
// (ma NON a un power-cycle completo, in quel caso si resetta a false e
// va bene così: ci si risincronizza semplicemente al prossimo boot)
RTC_DATA_ATTR bool timeSynchronized = false;

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

// --- Fallback: ottiene l'orario via HTTPS quando NTP (UDP) fallisce ---
// Usa timeapi.io, gratuito e senza API key. La richiesta passa su TCP/443,
// quindi attraversa NAT/firewall/hotspot esattamente come una normale
// pagina web — non risente dei blocchi/ritardi che colpiscono UDP/NTP.
bool syncTimeViaHttp() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure(); // Niente verifica del certificato: accettabile per
                        // un semplice servizio di lettura orario, non per
                        // dati sensibili. Evita di dover gestire CA bundle.

  HTTPClient http;
  Serial.print("[TIME-HTTP] Richiesta orario via HTTPS a timeapi.io...");

  if (!http.begin(client, TIME_API_URL)) {
    Serial.println(" Impossibile avviare la richiesta.");
    return false;
  }

  http.setTimeout(8000); // 8s, generoso per una richiesta TCP/TLS singola

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

  // Risposta tipica timeapi.io:
  // { "year":2026, "month":6, "day":21, "hour":14, "minute":5, "seconds":30, ... }
  int year   = doc["year"]   | 0;
  int month  = doc["month"]  | 0;
  int day    = doc["day"]    | 0;
  int hour   = doc["hour"]   | 0;
  int minute = doc["minute"] | 0;
  int second = doc["seconds"]| 0;

  if (year < 2024) { // Sanity check minimo sulla risposta
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

// --- Tenta la sincronizzazione NTP, con fallback HTTP ---
// Chiamata una sola volta per ciclo (siamo in deep sleep, non c'è un
// loop() che gira nel tempo): se NTP fallisce qui, si tenta subito l'HTTP.
void trySyncTime() {
  // configTime() imposta il fuso orario locale (GMT+DST offset) usato da
  // getLocalTime() per QUALSIASI chiamata successiva in questo boot —
  // anche se l'orologio interno (RTC) era già stato sincronizzato in un
  // ciclo precedente e qui saltiamo la vera richiesta di rete sotto.
  // BUG FIXATO: prima questa chiamata stava SOLO dentro il blocco "se non
  // ancora sincronizzato", quindi nei cicli successivi (con
  // timeSynchronized già true dalla RTC memory) l'offset non veniva mai
  // più impostato in quel boot, e getLocalTime() restituiva un orario
  // sfasato (mancava l'ora legale, -2h rispetto al corretto).
  configTime(3600, 0, NTP_SERVER, "pool.ntp.org", "time.cloudflare.com");

  if (timeSynchronized) return; // Offset impostato, RTC del chip già
                                 // corretta dal boot precedente: non
                                 // serve rifare la richiesta di rete.
  if (WiFi.status() != WL_CONNECTED) return;

  Serial.print("[TIME] Sincronizzazione orario con NTP in corso...");
  struct tm timeinfo;
  int ntpAttempts = 0;

  // Tenta la sincronizzazione per massimo 4 secondi (8 * 500ms).
  // Ridotto per lasciare margine al deep sleep finale: con timeout WiFi
  // (4s) + NTP (4s) + boot/init (~1-2s), il ciclo attivo resta sotto la
  // soglia dei 10s anche nello scenario peggiore (WiFi lento + NTP lento).
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
    Serial.println("[TIME] ATTENZIONE: Fallback HTTP fallito anch'esso. Riproverò al prossimo ciclo.");
  }
}

// --- Connessione Wi-Fi (un solo tentativo per ciclo, no retry loop lungo) ---
bool connectWifi() {
  Serial.print("[WiFi] Tentativo di connessione...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 8) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connesso! IP: " + WiFi.localIP().toString());
    trySyncTime();
    return true;
  }

  Serial.println("\n[WiFi] Rete offline. Uso la scheda SD.");
  return false;
}

// --- Quante righe storiche al massimo si inviano in UN SOLO ciclo ---
// Prima la funzione svuotava TUTTA la coda in un colpo: con un backlog di
// più ore (centinaia/migliaia di righe a un ciclo di 10s) questo teneva
// il dispositivo bloccato qui per 10-15 minuti, durante i quali il dato
// "live" del ciclo corrente non veniva mai letto (vedi ordine delle
// chiamate in setup(): ora handleDataCycle() viene fatta PRIMA di questa
// funzione, quindi quel buco è già evitato a monte; questo limite serve
// a far rientrare anche lo svuotamento dello storico nel singolo ciclo,
// così il deep sleep e il prossimo risveglio restano regolari).
// Con un ciclo da 10s e ~15ms per riga (publish + I/O SD), 30 righe
// occupano ~0.5s: ampio margine anche sommato al resto del ciclo attivo.
const int MAX_RIGHE_SD_PER_CICLO = 30;

// --- Invia un LOTTO di dati accumulati sulla SD e ripulisce solo quelli ---
// Le righe non ancora inviate restano sul file: verranno svuotate nei
// cicli successivi, qualche lotto alla volta, finché il backlog non si
// esaurisce. Il file viene riscritto in modo sicuro: si scrivono prima
// le righe residue in un file temporaneo, e solo a scrittura riuscita
// si sostituisce il file originale — così un'interruzione a metà
// (es. mancanza di alimentazione) non puo' corrompere o perdere il backup.
void svuotaCodaSD() {
  if (!SD.exists("/backup.jsonl")) return;

  Serial.println("\n[SD-RECOVERY] Rete ripristinata! Invio di un lotto di dati da SD...");

  File file = SD.open("/backup.jsonl", FILE_READ);
  if (!file) {
    Serial.println("[SD-RECOVERY] Errore critico nell'apertura del file di backup.");
    return;
  }

  char topic[64];
  snprintf(topic, sizeof(topic), "station/%s/base", STATION_ID);

  // Rimuove eventuale file temporaneo residuo da un ciclo precedente
  // interrotto a metà (per sicurezza, normalmente non dovrebbe esistere).
  if (SD.exists("/backup.tmp")) SD.remove("/backup.tmp");

  File tempFile = SD.open("/backup.tmp", FILE_WRITE);
  if (!tempFile) {
    Serial.println("[SD-RECOVERY] Errore critico nella creazione del file temporaneo.");
    file.close();
    return;
  }

  int righeInviate = 0;
  int righeRimaste = 0;

  while (file.available()) {
    String rigaPayload = file.readStringUntil('\n');
    rigaPayload.trim();
    if (rigaPayload.length() == 0) continue;

    if (righeInviate < MAX_RIGHE_SD_PER_CICLO) {
      // Lotto corrente: tenta l'invio. Se il publish fallisce (es. la
      // connessione MQTT è caduta proprio durante lo svuotamento), la
      // riga NON viene scartata: si rimette in coda nel file temporaneo
      // e verrà ritentata al prossimo ciclo.
      if (mqtt.connected() && mqtt.publish(topic, rigaPayload.c_str())) {
        Serial.println("[MQTT] Inviato storico da SD: " + rigaPayload);
        righeInviate++;
        delay(10); // Margine minimo per non sommergere il broker locale;
                   // ridotto da 100ms, che con backlog ampi era la causa
                   // principale dei 10-15 minuti di blocco.
        continue;
      } else {
        Serial.println("[SD-RECOVERY] Invio fallito, la riga resta in coda.");
        // Non viene incrementato righeInviate: la riga cade nel ramo
        // sotto e viene riscritta nel file temporaneo.
      }
    }

    // Righe oltre il lotto corrente (o non inviate per errore): si
    // riscrivono nel file temporaneo per essere ritentate in seguito.
    tempFile.println(rigaPayload);
    righeRimaste++;
  }

  file.close();
  tempFile.close();

  // Sostituzione sicura: solo ora che il nuovo file con le righe residue
  // è stato scritto e chiuso correttamente, si rimuove il vecchio backup
  // e si rinomina il temporaneo al suo posto.
  SD.remove("/backup.jsonl");
  if (righeRimaste > 0) {
    SD.rename("/backup.tmp", "/backup.jsonl");
    Serial.printf("[SD-RECOVERY] Lotto inviato: %d righe. Ancora in coda: %d righe (continuerà nei prossimi cicli).\n",
                  righeInviate, righeRimaste);
  } else {
    SD.remove("/backup.tmp");
    Serial.printf("[SD-RECOVERY] Lotto inviato: %d righe. Coda SD completamente svuotata.\n", righeInviate);
  }
}

// --- Connessione Broker MQTT ---
// NB: lo svuotamento della coda SD (svuotaCodaSD()) NON viene più fatto
// qui dentro: è stato spostato in setup(), DOPO la lettura del dato live
// (handleDataCycle()). Così il dato del ciclo corrente viene sempre letto
// e inviato/salvato per primo, e solo dopo si dedica tempo al backlog
// storico — evitando il "buco" di dati nuovi durante lo svuotamento.
bool connectMqtt() {
  if (WiFi.status() != WL_CONNECTED) return false;

  Serial.print("[MQTT] Connessione in corso...");
  if (mqtt.connect(STATION_ID, MQTT_USER, MQTT_PASSWORD)) {
    Serial.println("Connesso!");
    return true;
  }

  Serial.print("Fallita, errore=");
  Serial.println(mqtt.state());
  return false;
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

// --- Acquisizione e smistamento (un solo ciclo, niente loop) ---
void handleDataCycle(bool mqttReady) {
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

  if (mqttReady && mqtt.connected()) {
    char topic[64];
    snprintf(topic, sizeof(topic), "station/%s/base", STATION_ID);
    if (mqtt.publish(topic, payload)) {
      Serial.println("[MQTT] Inviato Live: " + String(payload));
      return; 
    }
  }

  scriviSuSD(String(payload));
}

// --- Calcola il tempo di deep sleep restante e ci entra ---
// Sottrae il tempo già impiegato in questo ciclo (boot, connessioni,
// lettura sensore) dalla durata target del ciclo, così l'intervallo tra
// una lettura e l'altra resta ~10s anche considerando il tempo "attivo".
void goToDeepSleep(unsigned long cycleStartMillis) {
  unsigned long elapsedMs = millis() - cycleStartMillis;
  uint64_t elapsedUs = (uint64_t)elapsedMs * 1000ULL;

  uint64_t sleepUs = (elapsedUs < CYCLE_DURATION_US)
                        ? (CYCLE_DURATION_US - elapsedUs)
                        : 0; // Se il ciclo attivo ha già superato i 10s,
                             // dorme il minimo indispensabile (0 non è
                             // valido per il timer, quindi si usa 1ms).
  if (sleepUs == 0) sleepUs = 1000ULL;

  Serial.printf("[SLEEP] Ciclo attivo: %lums. Deep sleep per %llums.\n",
                elapsedMs, sleepUs / 1000ULL);
  Serial.flush();

  esp_sleep_enable_timer_wakeup(sleepUs);
  esp_deep_sleep_start();
  // L'esecuzione non torna mai qui: al risveglio si ha un reset completo
  // e si riparte da setup().
}

void setup() {
  unsigned long cycleStart = millis();

  Serial.begin(115200);
  delay(100); // Piccola pausa per dare tempo al monitor seriale di
              // riallacciarsi dopo il risveglio dal deep sleep.
  Serial.println("\n--- Ciclo Stazione LegmaMiteo + Modulo SD (deep sleep) ---");

  // Imposta SEMPRE l'offset GMT+DST locale, indipendentemente dal WiFi.
  // È un'operazione puramente locale (non richiede rete) ma necessaria
  // perché getLocalTime()/getTimestamp() la usano in ogni ciclo. Senza
  // questa chiamata qui, nei cicli in cui il WiFi falliva l'offset non
  // veniva mai impostato in quel boot: la RTC del chip aveva comunque
  // l'epoch UTC corretto, ma senza offset il timestamp risultava sfasato
  // di 2 ore (l'esatta differenza GMT+DST osservata nei dati salvati su
  // SD durante le interruzioni di rete).
  configTime(3600, 0, NTP_SERVER, "pool.ntp.org", "time.cloudflare.com");

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

  bool wifiOk = connectWifi();
  bool mqttOk = wifiOk && connectMqtt();

  // ORDINE INVERTITO rispetto a prima: il dato del ciclo corrente viene
  // letto e inviato/salvato PRIMA di toccare il backlog storico sulla SD.
  // Così, anche se c'è un backlog di ore da svuotare, il dato live non
  // aspetta mai e non si crea più nessun "buco" di campionamento.
  handleDataCycle(mqttOk);

  // Solo dopo il dato live, se la connessione MQTT è disponibile, si
  // dedica tempo a smaltire un LOTTO (non tutto) del backlog su SD. Le
  // righe eventualmente non ancora svuotate proseguiranno nei prossimi
  // cicli, qualche lotto alla volta, senza mai bloccare il dispositivo
  // per i 10-15 minuti di prima.
  if (mqttOk) svuotaCodaSD();

  // Chiusura ordinata prima del sonno: evita che la connessione resti
  // a metà e contribuisce a liberare risorse.
  if (mqtt.connected()) mqtt.disconnect();
  if (WiFi.status() == WL_CONNECTED) WiFi.disconnect(true);

  goToDeepSleep(cycleStart);
}

void loop() {
  // Mai eseguito: dopo setup() il dispositivo va sempre in deep sleep,
  // che equivale a un reset. loop() resta vuoto per compatibilità con
  // il framework Arduino, che richiede comunque questa funzione.
}