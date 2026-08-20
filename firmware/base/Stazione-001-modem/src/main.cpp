#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
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
#include <Adafruit_NeoPixel.h>
#include "esp_task_wdt.h"

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
const int   DAYLIGHT_OFFSET_SEC = 3600;

const char* TIME_API_URL = "https://timeapi.io/api/time/current/zone?timeZone=Europe/Rome";

const int I2C_SDA = 4;
const int I2C_SCL = 5;

const int SPI_MISO = 16;
const int SPI_CLK  = 17;
const int SPI_MOSI = 15;
const int chipSelect = 18;

// --- Campionamento adattivo ---
// In condizioni stabili si campiona ogni 30s (basso consumo, sufficiente
// per temperatura/pressione che variano lentamente). Se si rileva un
// calo rapido di pressione o temperatura — segnale tipico di un
// downburst/gust front o dell'arrivo di una supercella — si passa
// automaticamente a 10s per non perdere risoluzione temporale proprio
// nei momenti più critici, restando in modalità veloce per un periodo
// minimo dopo l'ultimo trigger (l'evento potrebbe continuare a
// evolvere anche dopo il primo segnale).
const uint64_t CICLO_STABILE_US = 30000000ULL;   // 30s
const uint64_t CICLO_VELOCE_US  = 10000000ULL;   // 10s
const float SOGLIA_CALO_PRESSIONE_HPA_MIN = 1.0f; // hPa/min
const float SOGLIA_CALO_TEMPERATURA_C_MIN = 1.0f; // °C/min
const unsigned long DURATA_MODALITA_VELOCE_MS = 15UL * 60UL * 1000UL; // 15 minuti

uint64_t cicloAttualeUs = CICLO_STABILE_US;
unsigned long modalitaVeloceFinoAMillis = 0; // 0 = modalità stabile attiva

bool primaLetturaAdattiva = true;
float precTemperaturaAdattiva = 0.0f;
float precPressioneAdattivaHpa = 0.0f;
unsigned long precTimestampAdattivoMs = 0;

// Valuta se la lettura corrente indica un evento in rapida evoluzione,
// confrontandola con la precedente e normalizzando per il tempo
// realmente trascorso (che varia: 10s in modalità veloce, 30s in
// stabile). Se supera le soglie, estende/attiva la modalità veloce.
void valutaCampionamentoAdattivo(float temperaturaAttuale, float pressioneAttualeHpa) {
  unsigned long oraMs = millis();

  if (!primaLetturaAdattiva) {
    float deltaMinuti = (float)(oraMs - precTimestampAdattivoMs) / 60000.0f;
    if (deltaMinuti > 0.01f) { // Evita divisioni per intervalli troppo piccoli
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

const uint32_t SD_COMPACT_THRESHOLD_BYTES = 200000;

// Numero massimo di cicli consecutivi in cui il socket MQTT/WiFi risulta
// caduto prima di forzare una disconnessione WiFi completa e ripartire
// da zero (a volte il modulo radio resta in uno stato "zombie" dopo un
// certo numero di risvegli da light sleep: un reset periodico dello
// stack WiFi previene questo).
const int MAX_RICONNESSIONI_CONSECUTIVE = 6;
int riconnessioniConsecutiveFallite = 0;

// Timeout del task watchdog: se loop() non "nutre" il watchdog per più
// di questo tempo (es. blocco per bug imprevisto, exception, deadlock),
// l'ESP32 si riavvia da solo — equivalente automatico del pulsante
// reset fisico. 60s dà ampio margine anche ai cicli più lunghi (es.
// recupero backlog aggressivo, che può arrivare a ~8-9s di lavoro attivo).
const uint32_t WATCHDOG_TIMEOUT_SEC = 60;

// Se il reset completo dello stack WiFi (già tentato dopo
// MAX_RICONNESSIONI_CONSECUTIVE fallimenti) non risolve il problema per
// più round consecutivi, il radio potrebbe essere rimasto in uno stato
// "bloccato" più profondo di quanto un semplice WiFi.disconnect()+begin()
// riesca a sistemare — osservato in campo: serviva il pulsante reset
// fisico (riavvio completo del microcontrollore) per sbloccarlo. Questa
// soglia forza un vero esp_restart() dopo troppi round di reset WiFi
// falliti di fila, automatizzando quello che prima richiedeva
// l'intervento manuale.
const int MAX_RESET_WIFI_FALLITI_CONSECUTIVI = 3;
int resetWifiFallitiConsecutivi = 0;

WiFiClientSecure wifiClient;
PubSubClient mqtt(wifiClient);
Adafruit_BMP085 bmp;

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
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER, "pool.ntp.org", "time.cloudflare.com");

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

// Connessione WiFi completa: usata solo al primo avvio o quando il radio
// va resettato del tutto (fallback). Abilita anche il power-save del
// modem WiFi, che è ciò che rende sostenibile tenerlo associato durante
// il light sleep: il radio si "congela" tra un beacon interval e l'altro
// invece di restare sempre ricevente, ma senza perdere l'associazione.
bool connectWifiCompleto() {
  Serial.print("[WiFi] Connessione completa in corso...");
  WiFi.mode(WIFI_STA);

  // NOTA: qui avevamo forzato un DNS statico esterno (8.8.8.8 / 1.1.1.1)
  // per accorciare il ritardo di risoluzione DNS al boot. Rimosso: sul
  // router di questa rete, le query DNS dirette verso resolver esterni
  // sembrano essere bloccate/filtrate (comune su alcuni router ISP),
  // causando un fallimento DNS permanente invece del solo ritardo
  // iniziale di ~30s che si aveva usando il DNS fornito dal router via
  // DHCP. Si torna quindi al comportamento di default (DNS del router).
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

    // WIFI_PS_MIN_MODEM: power-save minimo del radio (il modem si
    // "congela" tra un beacon e l'altro), compatibile con l'architettura
    // a modem sleep: la CPU resta sempre sveglia (niente light sleep),
    // quindi mantiene l'associazione WiFi in modo affidabile e testato.
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    trySyncTime();
    // NOTA: qui NON azzeriamo riconnessioniConsecutiveFallite (vedi
    // spiegazione estesa nella versione LightSleep). Il WiFi può
    // connettersi correttamente anche quando MQTT/DNS falliscono
    // ripetutamente subito dopo — azzerare qui impedirebbe al contatore
    // di raggiungere mai la soglia di auto-riavvio.
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

  // Secondo tentativo con DNS esterno se quello del router fallisce.
  // Vedi commento esteso nella versione LightSleep per il ragionamento
  // completo: qui l'effetto è meno "auto-resettante" (la connessione
  // persiste tra i cicli), ma essendo applicato solo come fallback su
  // fallimento — non in modo proattivo ad ogni ciclo riuscito — il
  // rischio resta contenuto.
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

// Verifica leggera dello stato della connessione, da chiamare ad ogni
// risveglio dal light sleep. Non fa mai un reset completo del WiFi:
// tenta prima solo di far ripartire MQTT se il WiFi risulta ancora
// associato (caso più comune: l'associazione WiFi sopravvive al light
// sleep, ma il socket TCP/TLS del broker può essere stato droppato da
// un firewall/NAT/Tailscale per inattività).
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

  // Troppi fallimenti di fila: probabile stato "zombie" dello stack
  // WiFi. Si forza un reset completo (disconnect + reconnect da zero)
  // invece di continuare a tentare riconnessioni parziali inutili.
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
    riconnessioniConsecutiveFallite = 0; // Il prossimo round riparte da zero.

    if (resetWifiFallitiConsecutivi >= MAX_RESET_WIFI_FALLITI_CONSECUTIVI) {
      // Anche il reset software dello stack WiFi non ha risolto per
      // troppi round di fila: il radio è probabilmente in uno stato
      // bloccato che solo un riavvio completo del microcontrollore può
      // sbloccare (osservato in campo: serviva il pulsante reset
      // fisico). esp_restart() lo automatizza.
      Serial.println("[SISTEMA] Reset WiFi ripetuti falliti troppe volte: riavvio completo del microcontrollore.");
      Serial.flush();
      delay(200);
      esp_restart();
    }

    return false;
  }

  return false;
}

// --- Budget di flush ADATTIVO in base a quanto backlog resta da inviare ---
// In condizioni normali (backlog piccolo o assente) il flush resta breve
// e a basso consumo. Se il backlog è grande (es. dopo giorni di blackout
// per un evento meteo estremo che ha tirato giù la linea), il budget si
// allarga molto per recuperare aggressivamente, sfruttando il fatto che
// la connessione MQTT è già stabile (niente più costo di handshake ad
// ogni ciclo con l'architettura a connessione persistente). La lettura
// live viene SEMPRE inviata prima di questo flush (vedi loop()), quindi
// allargare questo budget non fa mai perdere un dato live: al massimo
// allunga quel singolo ciclo, il prossimo campione arriverà con qualche
// secondo di ritardo ma nessun dato va perso.
const unsigned long SD_FLUSH_BUDGET_NORMALE_MS   = 600;    // backlog piccolo/assente
const unsigned long SD_FLUSH_BUDGET_MEDIO_MS     = 3000;   // backlog moderato (minuti/poche ore)
const unsigned long SD_FLUSH_BUDGET_AGGRESSIVO_MS = 8000;  // backlog enorme (giorni), lascia
                                                             // comunque margine nel ciclo da 10s
                                                             // per non bloccare mai del tutto
const uint32_t SD_SOGLIA_BACKLOG_MEDIO_BYTES      = 50000;   // ~50KB
const uint32_t SD_SOGLIA_BACKLOG_GRANDE_BYTES     = 500000;  // ~500KB

const unsigned long SD_FLUSH_DELAY_NORMALE_MS = 50;
const unsigned long SD_FLUSH_DELAY_AGGRESSIVO_MS = 8;
const int SD_FLUSH_MAX_ROWS_PER_CYCLE_NORMALE = 10;

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
    SD.remove("/backup.jsonl");
    sdReadOffset = 0;
    return;
  }

  uint32_t backlogResiduo = fileSize - sdReadOffset;
  unsigned long flushBudgetMs;
  int maxRighePerCiclo;
  unsigned long flushDelayMs;

  bool inModalitaStabile = (cicloAttualeUs == CICLO_STABILE_US);

  if (inModalitaStabile && backlogResiduo > 0) {
    // In modalità stabile (30s), qualsiasi backlog residuo — piccolo o
    // grande — sfrutta il tempo che altrimenti passeremmo semplicemente
    // ad aspettare senza far nulla di utile. Non ha senso limitare
    // questo comportamento solo ai backlog enormi: anche pochi KB
    // rimasti, se recuperati a piccoli bocconi (10 righe/ciclo), possono
    // impiegare decine di minuti invece di uno o due cicli.
    const unsigned long SD_FLUSH_MARGINE_MS = 1500; // Tempo lasciato libero per lettura live + overhead del ciclo.
    unsigned long budgetDisponibile = (unsigned long)(cicloAttualeUs / 1000ULL);
    budgetDisponibile = (budgetDisponibile > SD_FLUSH_MARGINE_MS) ? (budgetDisponibile - SD_FLUSH_MARGINE_MS) : SD_FLUSH_BUDGET_AGGRESSIVO_MS;
    flushBudgetMs = (budgetDisponibile > SD_FLUSH_BUDGET_AGGRESSIVO_MS) ? budgetDisponibile : SD_FLUSH_BUDGET_AGGRESSIVO_MS;
    maxRighePerCiclo = INT32_MAX;
    flushDelayMs = SD_FLUSH_DELAY_AGGRESSIVO_MS;
    Serial.printf("[SD-RECOVERY] Modalità stabile con backlog residuo: recupero esteso attivo (budget %lums).\n", flushBudgetMs);
  } else if (backlogResiduo >= SD_SOGLIA_BACKLOG_GRANDE_BYTES) {
    // Modalità veloce con backlog enorme: restiamo più cauti (budget
    // fisso più corto) per non ritardare troppo il prossimo campione,
    // importante durante un evento meteo in corso.
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

    // Buffer fisso invece di String: readStringUntil() alloca un nuovo
    // oggetto String (heap) ad ogni riga letta. Con centinaia di righe
    // per ciclo in modalità di recupero backlog, questo può frammentare
    // l'heap nel tempo. Un buffer statico riutilizzato evita del tutto
    // queste allocazioni/deallocazioni ripetute.
    static char rigaBuf[300];
    size_t rigaLen = file.readBytesUntil('\n', rigaBuf, sizeof(rigaBuf) - 1);
    rigaBuf[rigaLen] = '\0';

    // Trim manuale di eventuali spazi/CR finali (equivalente a .trim()).
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
    SD.remove("/backup.jsonl");
    sdReadOffset = 0;
    Serial.printf("[SD-RECOVERY] Lotto inviato: %d righe. Coda SD svuotata.\n", righeInviate);
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
      esp_task_wdt_reset(); // Con file molto grandi (decine di MB) la
                             // copia può richiedere più del timeout del
                             // watchdog se non lo nutriamo qui dentro.
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
  float real_temp = bmp.readTemperature();
  float real_press = bmp.readPressure() / 100.0F;

  valutaCampionamentoAdattivo(real_temp, real_press);

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
      Serial.println("[MQTT] Inviato: " + String(payload));
      return;
    }
  }

  scriviSuSD(String(payload));
}

// Attesa fino al prossimo ciclo SENZA sospendere la CPU (niente
// esp_light_sleep_start()). Il risparmio energetico arriva solo dal
// power-save del radio WiFi (WIFI_PS_MIN_MODEM, impostato una volta in
// connectWifiCompleto): il radio si "congela" tra un beacon e l'altro,
// ma la CPU resta sveglia e continua a servire lo stack TCP/IP e
// mqtt.loop() regolarmente. Questo evita il problema di sincronizzazione
// radio osservato con il light sleep esplicito (disassociazioni cicliche
// e socket "zombie"), al costo di un consumo medio più alto.
void attesaModemSleep(unsigned long cycleStartMillis, bool connessioneRiuscita) {
  // Stessa logica del file LightSleep: se la connessione è fallita,
  // ritentiamo velocemente indipendentemente dalla modalità meteo.
  uint64_t cicloEffettivoUs = connessioneRiuscita ? cicloAttualeUs : CICLO_VELOCE_US;

  unsigned long elapsedMs = millis() - cycleStartMillis;
  long restanteMs = (long)(cicloEffettivoUs / 1000ULL) - (long)elapsedMs;
  if (restanteMs < 0) restanteMs = 0;

  Serial.printf("[WAIT] Ciclo attivo: %lums. Attesa (modem sleep) per %ldms. [Modalità: %s%s]\n",
                elapsedMs, restanteMs,
                (cicloAttualeUs == CICLO_VELOCE_US) ? "VELOCE 10s" : "stabile 30s",
                connessioneRiuscita ? "" : " - RETRY RAPIDO per connessione fallita");

  unsigned long waitStart = millis();
  while ((long)(millis() - waitStart) < restanteMs) {
    esp_task_wdt_reset();
    if (mqtt.connected()) mqtt.loop();
    delay(20);
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);

  // Task watchdog: se loop() smette di "nutrirlo" per più di
  // WATCHDOG_TIMEOUT_SEC (bug imprevisto, exception, deadlock su I2C/SPI/
  // rete), l'ESP32 si riavvia da solo automaticamente — equivalente
  // software del pulsante reset fisico.
  // API legacy del watchdog (compatibile con core Arduino-ESP32 senza
  // la struct esp_task_wdt_config_t, introdotta solo in versioni più
  // recenti del core/ESP-IDF): timeout in secondi + flag di panic.
  esp_task_wdt_init(WATCHDOG_TIMEOUT_SEC, true);
  esp_task_wdt_add(NULL); // Registra il task corrente (loop principale).

  Serial.println("\n--- LegmaMiteo: avvio con architettura light sleep + connessione persistente ---");

  rgbLed.begin();
  rgbLed.setBrightness(40);

  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER, "pool.ntp.org", "time.cloudflare.com");

  Wire.begin(I2C_SDA, I2C_SCL);
  if (!bmp.begin()) {
    Serial.println("[HARDWARE] BMP180 non rilevato!");
  } else {
    Serial.println("[HARDWARE] BMP180 pronto.");
  }

  SPI.begin(SPI_CLK, SPI_MISO, SPI_MOSI, chipSelect);
  Serial.print("[HARDWARE] Inizializzazione modulo SD... ");
  if (!SD.begin(chipSelect)) {
    Serial.println("FALLITA! Controlla pin, contatti o formattazione FAT32.");
  } else {
    Serial.println("OK, MicroSD rilevata.");
  }

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);
  mqtt.setSocketTimeout(10);
  // Keepalive più corto del default (15s invece di 60s): con connessione
  // persistente conviene rilevare prima un socket morto, così il
  // fallback su SD scatta rapidamente invece di accorgersi del problema
  // solo al timeout lungo di default.
  mqtt.setKeepAlive(15);

  bool wifiOk = connectWifiCompleto();
  bool mqttOk = wifiOk && connectMqtt();

  // Retry serrato SOLO in questa fase di avvio: se il primo tentativo
  // fallisce (tipicamente per DNS non ancora pronto, il router/rete
  // impiega qualche decina di secondi ad "assestarsi" dopo un boot
  // fisico), ritentiamo ogni 3s per un massimo di ~90s invece di
  // aspettare il primo ciclo intero da 10s ripetuto più volte. Questo
  // aggancia la connessione MQTT il prima possibile appena la rete è
  // pronta, invece di sprecare tempo morto tra un tentativo e l'altro.
  // Una volta usciti da setup(), il ritmo normale a 10s per ciclo
  // riprende (vedi loop()), qui serve solo a velocizzare il boot.
  if (wifiOk && !mqttOk) {
    const unsigned long RETRY_AVVIO_INTERVALLO_MS = 3000;
    const unsigned long RETRY_AVVIO_TIMEOUT_MS = 90000;
    unsigned long retryStart = millis();

    Serial.println("[MQTT] Primo tentativo fallito, retry serrato in fase di avvio...");
    while (!mqttOk && (millis() - retryStart) < RETRY_AVVIO_TIMEOUT_MS) {
      esp_task_wdt_reset();
      delay(RETRY_AVVIO_INTERVALLO_MS);
      if (WiFi.status() != WL_CONNECTED) break; // Se anche il WiFi cade,
                                                  // meglio lasciare che
                                                  // sia il ciclo normale
                                                  // (con reset completo)
                                                  // a gestire il caso.
      mqttOk = connectMqtt();
    }
  }

  if (wifiOk && !mqttOk) {
    Serial.println("[MQTT] Broker non raggiungibile nonostante il WiFi attivo.");
    mostraMqttNonRaggiungibile();
  }

  // Connessione stabilita una sola volta qui. Da questo punto in poi
  // setup() non viene più rieseguito: il ciclo vive dentro loop(),
  // alternando lettura/invio dati e light sleep, mantenendo lo stato.
}

void loop() {
  esp_task_wdt_reset(); // "Nutre" il watchdog: conferma che il loop non è bloccato.

  unsigned long cycleStart = millis();

  bool connesso = assicuraConnessioneAttiva();

  if (mqtt.connected()) mqtt.loop();

  handleDataCycle(connesso);

  if (connesso) svuotaCodaSD();

  attesaModemSleep(cycleStart, connesso);
}
