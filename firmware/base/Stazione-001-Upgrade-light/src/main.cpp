// =====================================================================
// LegmaMiteo — Sketch di TEST v2 (configurazione finale)
// =====================================================================
// BMP580 e AS3935 si sono rivelati poco affidabili su I2C condiviso
// (transazioni multi-byte che fallivano o restituivano dati fissi) e
// sono quindi entrambi passati a SPI. Il bus I2C principale ora ha
// solo 3 dispositivi (SHT45, MCP9808, VEML7700), molto piu' stabile.
//
// Pin:
//   I2C principale (SHT45, MCP9808, VEML7700): SDA=GPIO4, SCL=GPIO5
//   SPI condiviso SD + BMP580: SCK=GPIO17, MISO=GPIO16, MOSI=GPIO15
//               SD_CS=GPIO18, BMP580_CS=GPIO9 (stesso bus fisico,
//               CS diversi - SPI supporta piu' dispositivi cosi')
//   SPI AS3935 (bus FSPI separato): SCK=GPIO12, MISO=GPIO13,
//               MOSI=GPIO11, CS=GPIO10
//   AS3935 IRQ: GPIO6
//   AS3935 SI:  GND (fissa la modalità SPI)
//
// Librerie richieste: Adafruit SHT4x, Adafruit MCP9808, Adafruit
// VEML7700, SparkFun BMP581 Arduino Library, Adafruit Unified Sensor.
// Nessuna libreria esterna per l'AS3935 (registro diretto via SPI).
// =====================================================================

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>

#include <Adafruit_SHT4x.h>
#include <Adafruit_MCP9808.h>
#include <SparkFun_BMP581_Arduino_Library.h>
#include <Adafruit_VEML7700.h>

// --- Pin I2C (sensori ambientali) ---
const int I2C_SDA = 4;
const int I2C_SCL = 5;

// --- BMP580 ora in SPI, sullo stesso bus fisico della SD (SCK=17,
// MISO=16, MOSI=15) ma con CS dedicato - SPI supporta piu' dispositivi
// sullo stesso bus, ognuno selezionato dal proprio CS.
const int BMP580_CS = 9;

// --- Pin SPI (SD card) ---
const int SD_SPI_MISO = 16;
const int SD_SPI_CLK  = 17;
const int SD_SPI_MOSI = 15;
const int SD_CS        = 18;

// --- Pin SPI (AS3935, bus FSPI separato) ---
const int AS3935_SCK  = 12;
const int AS3935_MISO = 13;
const int AS3935_MOSI = 11;
const int AS3935_CS   = 10;
const int AS3935_IRQ_PIN = 6;

// --- Oggetti sensori ---
Adafruit_SHT4x sht45 = Adafruit_SHT4x();
Adafruit_MCP9808 mcp9808 = Adafruit_MCP9808();
BMP581 bmp580;
Adafruit_VEML7700 veml7700 = Adafruit_VEML7700();
SPIClass as3935Spi(FSPI);

// =====================================================================
// AS3935 — funzioni a registro diretto via SPI
// =====================================================================
uint8_t as3935ReadReg(uint8_t reg) {
  uint8_t comando = 0x40 | (reg & 0x3F); // bit7=1 -> lettura

  digitalWrite(AS3935_CS, LOW);
  delayMicroseconds(5);
  as3935Spi.transfer(comando);
  uint8_t valore = as3935Spi.transfer(0x00);
  digitalWrite(AS3935_CS, HIGH);

  return valore;
}

void as3935WriteReg(uint8_t reg, uint8_t mask, uint8_t data) {
  uint8_t regVal = as3935ReadReg(reg);
  regVal &= ~mask;
  regVal |= (data & mask);

  uint8_t comando = 0x00 | (reg & 0x3F); // bit7=0 -> scrittura

  digitalWrite(AS3935_CS, LOW);
  delayMicroseconds(5);
  as3935Spi.transfer(comando);
  as3935Spi.transfer(regVal);
  digitalWrite(AS3935_CS, HIGH);
}

void as3935PowerUp() {
  // Reg 0x00, bit0 = PWD (0 = power up)
  as3935WriteReg(0x00, 0x01, 0x00);
  delay(3);

  // Calibrazione oscillatore RC interno (comando diretto, non è un
  // registro con mask, va scritto per intero).
  digitalWrite(AS3935_CS, LOW);
  delayMicroseconds(5);
  as3935Spi.transfer(0x3D & 0x3F); // scrittura reg 0x3D
  as3935Spi.transfer(0x96);        // comando CALIB_RCO
  digitalWrite(AS3935_CS, HIGH);
  delay(2);

  as3935WriteReg(0x08, 0x20, 0x20); // DISP_TRCO on (diagnostico)
  delay(2);
  as3935WriteReg(0x08, 0x20, 0x00); // DISP_TRCO off
}

float as3935MisuraFrequenzaIrqHz(int campioni = 20) {
  unsigned long sommaPeriodoUs = 0;
  int validi = 0;

  for (int i = 0; i < campioni; i++) {
    unsigned long alto = pulseIn(AS3935_IRQ_PIN, HIGH, 50000UL);
    unsigned long basso = pulseIn(AS3935_IRQ_PIN, LOW, 50000UL);
    if (alto == 0 || basso == 0) continue;
    sommaPeriodoUs += (alto + basso);
    validi++;
  }

  if (validi == 0) return 0.0f;

  float periodoMedioUs = (float)sommaPeriodoUs / validi;
  return 1000000.0f / periodoMedioUs;
}

void as3935TuningAntenna() {
  Serial.println("\n=== AS3935: tuning antenna LC (via SPI) ===");

  const float FREQ_TARGET_HZ = 31250.0f; // 500kHz / 16

  as3935WriteReg(0x08, 0x80, 0x80); // DISP_LCO on -> frequenza antenna sul pin IRQ

  float migliorDelta = 1e9f;
  uint8_t migliorTunCap = 0;
  float migliorFreq = 0;

  for (uint8_t tunCap = 0; tunCap <= 15; tunCap++) {
    as3935WriteReg(0x08, 0x0F, tunCap);
    delay(3);

    float freq = as3935MisuraFrequenzaIrqHz();
    float delta = fabs(freq - FREQ_TARGET_HZ);

    Serial.printf("  TUN_CAP=%2d  ->  %8.1f Hz  (target %.0f Hz, delta %.1f Hz)\n",
                  tunCap, freq, FREQ_TARGET_HZ, delta);

    if (freq > 0 && delta < migliorDelta) {
      migliorDelta = delta;
      migliorTunCap = tunCap;
      migliorFreq = freq;
    }
  }

  as3935WriteReg(0x08, 0x0F, migliorTunCap);
  as3935WriteReg(0x08, 0x80, 0x00); // DISP_LCO off

  float erroreRelativoPct = (migliorFreq - FREQ_TARGET_HZ) / FREQ_TARGET_HZ * 100.0f;

  Serial.printf("\n  -> Migliore: TUN_CAP=%d, %.1f Hz (errore %.2f%%)\n",
                migliorTunCap, migliorFreq, erroreRelativoPct);

  if (migliorFreq > 0 && fabs(erroreRelativoPct) <= 3.5f) {
    Serial.println("  OK: entro la tolleranza datasheet (+-3.5%).");
  } else {
    Serial.println("  ATTENZIONE: fuori tolleranza (+-3.5%) o nessuna");
    Serial.println("  frequenza misurabile. Ricontrolla IRQ/antenna.");
  }

  Serial.println("=== Tuning completato ===\n");
}

// =====================================================================
// SETUP
// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n--- LegmaMiteo: TEST v2 (AS3935 via SPI) ---\n");

  pinMode(AS3935_IRQ_PIN, INPUT);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000); // 100kHz esplicito (invece del default, piu'
                          // alto su alcune config ESP32) - un bus piu'
                          // lento tollera meglio capacita' parassita e
                          // contatti non perfetti, utile per isolare se
                          // i fallimenti su BMP580/VEML7700 sono un
                          // problema di integrita' del segnale



  delay(100); // piccola pausa per lasciare che il bus I2C si stabilizzi
              // dopo Wire.begin(), evita fallimenti transitori di begin()
              // sui primi dispositivi interrogati

  // --- SHT45 --- (con un retry: il primo tentativo appena dopo
  // Wire.begin() a volte fallisce per transitori del bus, anche se il
  // sensore è fisicamente a posto)
  Serial.print("[SHT45] Inizializzazione... ");
  bool sht45Ok = sht45.begin();
  if (!sht45Ok) {
    delay(50);
    sht45Ok = sht45.begin();
  }
  if (sht45Ok) {
    sht45.setPrecision(SHT4X_HIGH_PRECISION);
    Serial.println("OK.");
  } else {
    Serial.println("FALLITA!");
  }

  // --- MCP9808 ---
  Serial.print("[MCP9808] Inizializzazione... ");
  if (mcp9808.begin(0x18)) {
    mcp9808.setResolution(3);
    Serial.println("OK.");
  } else {
    Serial.println("FALLITA!");
  }

  // --- BMP580 (ora via SPI, non piu' I2C) ---
  // Usa il bus SPI globale (stesso della SD: SCK=17,MISO=16,MOSI=15),
  // con il proprio CS dedicato. SPI.begin() per questo bus viene
  // chiamato piu' sotto insieme all'inizializzazione della SD - deve
  // essere gia' avvenuto prima di chiamare beginSPI() qui, quindi
  // spostiamo l'inizializzazione del BMP580 DOPO quella della SD.
  pinMode(BMP580_CS, OUTPUT);
  digitalWrite(BMP580_CS, HIGH);

  // --- VEML7700 --- (tornato sul bus principale, ora che il BMP580 e'
  // passato a SPI il bus I2C ha un dispositivo in meno e dovrebbe
  // restare stabile senza bisogno di isolarlo)
  Serial.print("[VEML7700] Inizializzazione... ");
  bool veml7700Ok = veml7700.begin();
  if (!veml7700Ok) {
    delay(50);
    veml7700Ok = veml7700.begin();
  }
  if (veml7700Ok) {
    Serial.println("OK.");
  } else {
    Serial.println("FALLITA!");
  }

  // --- SD card ---
  Serial.print("[SD] Inizializzazione... ");
  SPI.begin(SD_SPI_CLK, SD_SPI_MISO, SD_SPI_MOSI, SD_CS);
  if (SD.begin(SD_CS)) {
    Serial.println("OK.");
    File f = SD.open("/test_legmamiteo.txt", FILE_WRITE);
    if (f) {
      f.println("Test scrittura OK - LegmaMiteo v2");
      f.close();
      Serial.println("[SD] Scrittura di test riuscita.");
    } else {
      Serial.println("[SD] ERRORE: impossibile aprire il file in scrittura.");
    }
  } else {
    Serial.println("FALLITA!");
  }

  // --- BMP580 via SPI (bus condiviso con la SD, CS dedicato) ---
  // SPI.begin() e' gia' stato chiamato sopra per la SD, quindi il bus
  // fisico e' gia' attivo: qui serve solo beginSPI() con il CS giusto.
  Serial.print("[BMP580] Inizializzazione (SPI, CS=9)... ");
  int8_t bmp580Err = bmp580.beginSPI(BMP580_CS, 2000000);
  if (bmp580Err == BMP5_OK) {
    Serial.println("OK.");
  } else {
    Serial.printf("FALLITA! Codice errore: %d\n", bmp580Err);
  }

  // --- AS3935 (SPI) ---
  pinMode(AS3935_CS, OUTPUT);
  digitalWrite(AS3935_CS, HIGH);
  as3935Spi.begin(AS3935_SCK, AS3935_MISO, AS3935_MOSI, AS3935_CS);
  as3935Spi.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE1));

  Serial.print("\n[AS3935] Power-up e calibrazione oscillatore interno... ");
  as3935PowerUp();
  Serial.println("fatto.");
  as3935TuningAntenna();

  Serial.println("--- Setup completato. Letture periodiche in corso. ---\n");
}

// =====================================================================
// LOOP
// =====================================================================
void loop() {
  Serial.println("----------------------------------------");

  sensors_event_t humidity, temp;
  sht45.getEvent(&humidity, &temp);
  Serial.printf("[SHT45]    Temp: %.2f C   Umidita: %.2f %%\n", temp.temperature, humidity.relative_humidity);

  Serial.printf("[MCP9808]  Temp: %.2f C\n", mcp9808.readTempC());

  bmp5_sensor_data bmpData;
  int8_t bmp580ReadErr = bmp580.getSensorData(&bmpData);
  if (bmp580ReadErr == BMP5_OK) {
    Serial.printf("[BMP580]   Pressione: %.2f hPa   Temp: %.2f C\n",
                  bmpData.pressure / 100.0f, bmpData.temperature);
  } else {
    Serial.printf("[BMP580]   Errore lettura. Codice: %d\n", bmp580ReadErr);
  }

  Serial.printf("[VEML7700] Lux: %.2f\n", veml7700.readLux());

  // AS3935: lettura a polling del pin IRQ (nel firmware finale andrà
  // gestita con attachInterrupt, qui va bene così per il test).
  if (digitalRead(AS3935_IRQ_PIN) == HIGH) {
    delay(2); // datasheet: attendere 2ms prima di leggere il registro interrupt
    uint8_t intReg = as3935ReadReg(0x03) & 0x0F;
    if (intReg == 0x08) {
      Serial.println("[AS3935]   *** FULMINE RILEVATO ***");
      uint8_t distanzaKm = as3935ReadReg(0x07) & 0x3F;
      Serial.printf("[AS3935]   Distanza stimata: %d km\n", distanzaKm);
    } else if (intReg == 0x01) {
      Serial.println("[AS3935]   Disturbo rilevato (noise level troppo alto).");
    } else if (intReg == 0x04) {
      Serial.println("[AS3935]   Disturbo rilevato (interferenza/stirring).");
    }
  }

  delay(3000);
}
