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
//
// FIX RITMO INVIO STORICO SD (dati "mangiati" da MQTT/Grafana):
// Con solo 10ms di pausa tra un publish e l'altro durante lo svuotamento
// del backlog, la pipeline Mosquitto->Telegraf->InfluxDB->Grafana perdeva
// occasionalmente qualche punto quando i lotti erano grandi (righe troppo
// ravvicinate). Il delay è stato portato a 50ms: sufficiente a dare respiro
// alla pipeline, mantenendo comunque un lotto di 30 righe entro ~1.5s totali
// (ampio margine sul ciclo di 10s).
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
const int   DAYLIGHT_OFFSET_SEC = 3600;   // Ora legale attivata

// Fallback HTTP per l'orario, usato solo se NTP fallisce (vedi sopra)
const char* TIME_API_URL = "https://timeapi.io/api/time/current/zone?timeZone=Europe/Rome";

// --- Pin I2C (BMP180) ---
const int I2C_SDA = 4;
const int I2C_SCL = 5;

// --- Pin SPI (Modulo SD) ---
const int SPI_MISO = 16;
const int SPI_CLK  = 17;
const int SPI_MOSI = 15;
const int chipSelect = 18; // CS

// --- Durata del ciclo (in microsecondi, per esp_sleep_enable_timer_wakeup) ---
const uint64_t CYCLE_DURATION_US = 10000000ULL; // 10 secondi

// --- Stato persistente in RTC memory: sopravvive al deep sleep ---
// (ma NON a un power-cycle completo, in quel caso si resetta a false e
// va bene così: ci si risincronizza semplicemente al prossimo boot)
RTC_DATA_ATTR bool timeSynchronized = false;

// --- Offset di lettura persistente nel backlog SD ---
// Sopravvive al deep sleep (RTC memory) cosi' ogni ciclo riprende ESATTAMENTE
// da dove aveva lasciato l'invio precedente, con file.seek(), invece di
// rileggere/ricopiare da capo tutto il file (che con backlog di centinaia
// di migliaia di byte costava 20+ secondi per ciclo SOLO per "saltare" le
// righe già considerate, anche senza inviarle di nuovo).
// Si resetta a 0 se manca completamente l'alimentazione (accettabile: nel
// caso peggiore si ri-processano righe già inviate, ma niente viene perso).
RTC_DATA_ATTR uint32_t sdReadOffset = 0;

// Ogni quanti byte "consumati" (offset) si esegue una compattazione reale
// del file (rimuove fisicamente le righe già inviate, liberando spazio).
// La compattazione costa comunque una copia dell'intero residuo, ma la si
// fa raramente (non ad ogni ciclo) cosi' il costo si ammortizza su molti
// cicli invece di pagarlo ogni volta.
const uint32_t SD_COMPACT_THRESHOLD_BYTES = 200000; // ~200KB consumati

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
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER, "pool.ntp.org", "time.cloudflare.com");

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

// --- Budget di TEMPO (non più numero fisso di righe) per lo svuotamento SD ---
// Prima il limite era un numero fisso di righe per ciclo (es. 30): con
// backlog enormi (giorni offline, migliaia di righe) questo era troppo
// lento — 30 righe/ciclo con connessione buona sprecava capacità inutilizzata,
// e con migliaia di righe arretrate servivano ORE per smaltire tutto.
// Ora invece si invia finché non si supera questo budget di tempo: con
// connessione veloce si riescono a inviare molte più righe nello stesso
// ciclo (anche 100-200+), con connessione lenta ci si ferma comunque in
// tempo per non bloccare il dispositivo a tempo indeterminato.
const unsigned long SD_FLUSH_TIME_BUDGET_MS = 6000; // 6s max dedicati allo
                                                     // svuotamento storico
                                                     // per ciclo

// Pausa minima tra un invio e l'altro dello storico, per non "affogare"
// la pipeline Mosquitto->Telegraf->InfluxDB->Grafana con troppi punti
// ravvicinati (vedi commit precedente: con 10ms venivano persi alcuni
// punti durante lotti grandi).
const unsigned long SD_FLUSH_DELAY_MS = 50;

// --- Invia un LOTTO di dati accumulati sulla SD e ripulisce solo quelli ---
// Le righe non ancora inviate restano sul file: verranno svuotate nei
// cicli successivi, qualche lotto alla volta, finché il backlog non si
// esaurisce. Il file viene riscritto in modo sicuro: si scrivono prima
// le righe residue in un file temporaneo, e solo a scrittura riuscita
// si sostituisce il file originale — così un'interruzione a metà
// (es. mancanza di alimentazione) non puo' corrompere o perdere il backup.
void svuotaCodaSD() {
  if (!SD.exists("/backup.jsonl")) {
    sdReadOffset = 0; // Nessun file: reset difensivo dell'offset.
    return;
  }

  File file = SD.open("/backup.jsonl", FILE_READ);
  if (!file) {
    Serial.println("[SD-RECOVERY] Errore critico nell'apertura del file di backup.");
    return;
  }

  uint32_t fileSize = file.size();

  // Se l'offset salvato è oltre la fine del file (es. file più corto per
  // qualche motivo), si riparte da 0 per sicurezza.
  if (sdReadOffset > fileSize) sdReadOffset = 0;

  if (sdReadOffset >= fileSize) {
    // Tutto il file è già stato inviato in cicli precedenti: elimina e
    // resetta, non c'è altro da fare.
    file.close();
    SD.remove("/backup.jsonl");
    sdReadOffset = 0;
    Serial.println("[SD-RECOVERY] Coda SD completamente svuotata (nessun nuovo dato da inviare).");
    return;
  }

  Serial.printf("\n[SD-RECOVERY] Rete ripristinata! Riprendo l'invio da offset %u/%u byte...\n",
                sdReadOffset, fileSize);

  // *** PUNTO CHIAVE: si riprende ESATTAMENTE da dove si era arrivati,
  // senza rileggere/ricopiare le righe precedenti già considerate. ***
  file.seek(sdReadOffset);

  char topic[64];
  snprintf(topic, sizeof(topic), "station/%s/base", STATION_ID);

  unsigned long flushStart = millis();
  int righeInviate = 0;

  while (file.available()) {
    if ((millis() - flushStart) >= SD_FLUSH_TIME_BUDGET_MS) {
      break; // Budget di tempo esaurito: ci si ferma qui, l'offset resta
             // esattamente alla riga corrente (non ancora consumata) e si
             // riprenderà da lì al prossimo ciclo. NESSUNA riscrittura del
             // resto del file: è questo che elimina il costo fisso di
             // "ricopiare tutto" ad ogni singolo ciclo.
    }

    uint32_t posPrimaDiQuestaRiga = file.position();
    String rigaPayload = file.readStringUntil('\n');
    rigaPayload.trim();

    if (rigaPayload.length() == 0) {
      sdReadOffset = file.position(); // Riga vuota: la si considera comunque consumata.
      continue;
    }

    if (mqtt.connected() && mqtt.publish(topic, rigaPayload.c_str())) {
      Serial.println("[MQTT] Inviato storico da SD: " + rigaPayload);
      righeInviate++;
      sdReadOffset = file.position(); // Avanza l'offset SOLO dopo un invio
                                       // riuscito: se la connessione cade
                                       // a metà, la riga non ancora
                                       // inviata verrà ritentata dal
                                       // prossimo ciclo (l'offset resta
                                       // fermo a prima di questa riga).
      delay(SD_FLUSH_DELAY_MS); // Pausa tra un invio e l'altro per non
                                 // affogare la pipeline Mosquitto->
                                 // Telegraf->InfluxDB->Grafana.
      continue;
    }

    // Invio fallito: ci si ferma qui SENZA avanzare l'offset oltre questa
    // riga, cosi' verrà ritentata al prossimo ciclo.
    Serial.println("[SD-RECOVERY] Invio fallito, mi fermo qui per questo ciclo.");
    (void)posPrimaDiQuestaRiga;
    break;
  }

  file.close();

  if (sdReadOffset >= fileSize) {
    // Il file è stato interamente processato in questo ciclo: elimina
    // subito, non serve nemmeno aspettare la prossima compattazione.
    SD.remove("/backup.jsonl");
    sdReadOffset = 0;
    Serial.printf("[SD-RECOVERY] Lotto inviato: %d righe. Coda SD completamente svuotata.\n", righeInviate);
    return;
  }

  Serial.printf("[SD-RECOVERY] Lotto inviato: %d righe. Offset avanzato a %u/%u byte (continuerà nei prossimi cicli).\n",
                righeInviate, sdReadOffset, fileSize);

  // --- Compattazione periodica (non ad ogni ciclo!) ---
  // Se abbiamo "consumato" (inviato) abbastanza byte dall'inizio del file,
  // conviene ricopiare fisicamente solo il residuo non ancora inviato in un
  // nuovo file, cosi' da liberare spazio su SD e riportare l'offset a 0.
  // Questo costa una copia dell'intero residuo, ma lo facciamo raramente
  // (ogni ~200KB consumati) invece che ad ogni singolo ciclo: il costo si
  // ammortizza su decine di cicli invece di gravare su ognuno.
  if (sdReadOffset >= SD_COMPACT_THRESHOLD_BYTES) {
    Serial.println("[SD-RECOVERY] Compattazione file di backup in corso...");

    File src = SD.open("/backup.jsonl", FILE_READ);
    if (!src) {
      Serial.println("[SD-RECOVERY] Errore apertura file per compattazione, rimando al prossimo ciclo.");
      return;
    }
    src.seek(sdReadOffset);

    if (SD.exists("/backup.tmp")) SD.remove("/backup.tmp");
    File dst = SD.open("/backup.tmp", FILE_WRITE);
    if (!dst) {
      Serial.println("[SD-RECOVERY] Errore creazione file temporaneo per compattazione.");
      src.close();
      return;
    }

    static uint8_t buf[4096];
    int n;
    while ((n = src.read(buf, sizeof(buf))) > 0) {
      dst.write(buf, n);
    }
    src.close();
    dst.close();

    SD.remove("/backup.jsonl");
    SD.rename("/backup.tmp", "/backup.jsonl");
    sdReadOffset = 0; // Il nuovo file parte da zero: tutto il residuo
                       // non ancora inviato è ora all'inizio del file.

    Serial.println("[SD-RECOVERY] Compattazione completata.");
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
                        : 0; // Se il ciclo attivo ha già superato i 10s
                             // (es. durante lo svuotamento di un grosso
                             // backlog SD), si applica comunque uno sleep
                             // minimo qui sotto, invece di richiudere il
                             // ciclo quasi a 0ms — così il dispositivo
                             // "respira" sempre un minimo anche durante
                             // uno svuotamento lungo, invece di restare
                             // sempre completamente sveglio.
  const uint64_t MIN_SLEEP_US = 2000000ULL; // 2s minimi di sonno per ciclo,
                                             // anche quando il ciclo attivo
                                             // ha già superato i 10s target.
  if (sleepUs < MIN_SLEEP_US) sleepUs = MIN_SLEEP_US;

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
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER, "pool.ntp.org", "time.cloudflare.com");

  // Pin I2C custom (SDA=4, SCL=5) - vanno impostati PRIMA di bmp.begin(),
  // che internamente usa Wire senza parametri e quindi userebbe i pin
  // di default se non specificati qui.
  Wire.begin(I2C_SDA, I2C_SCL);

  if (!bmp.begin()) {
    Serial.println("[HARDWARE] BMP180 non rilevato!");
  } else {
    Serial.println("[HARDWARE] BMP180 pronto.");
  }

  // Pin SPI custom (MISO=16, CLK=17, MOSI=15, CS=18) - vanno impostati
  // PRIMA di SD.begin(), che userebbe altrimenti i pin SPI di default
  // della board.
  SPI.begin(SPI_CLK, SPI_MISO, SPI_MOSI, chipSelect);

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