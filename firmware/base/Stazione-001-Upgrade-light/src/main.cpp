#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SparkFun_BMP581_Arduino_Library.h>
#include <Adafruit_SHT4x.h>
#include <Adafruit_VEML7700.h>
#include <Adafruit_MCP9808.h>
#include <SparkFun_AS3935.h>

// Pin I2C condivisi
const int I2C_SDA = 6;
const int I2C_SCL = 5;

// Pin SPI condivisi (AS3935 sullo stesso bus della SD)
const int SPI_SCK = 17;
const int SPI_MISO = 16;
const int SPI_MOSI = 15;
const int AS3935_CS = 9;
const int AS3935_IRQ = 7;

// Oggetti sensori
BMP581 bmp580;
Adafruit_SHT4x sht4;
Adafruit_VEML7700 veml7700 = Adafruit_VEML7700();
Adafruit_MCP9808 mcp9808 = Adafruit_MCP9808();
SparkFun_AS3935 as3935(AS3935_CS, AS3935_IRQ);

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n--- LegmaMiteo: BMP580 + SHT45 + VEML7700 + MCP9808 Test I2C ---");

  // Inizializzazione bus I2C
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000); // 100 kHz per la massima stabilità
  delay(200);

  // --- 1. Inizializzazione BMP580 (Indirizzo 0x46) ---
  int8_t bmpErr = bmp580.beginI2C(0x46);
  if (bmpErr == BMP5_OK) {
    Serial.println("[BMP580] Inizializzazione I2C riuscita (0x46)!");
  } else {
    Serial.printf("[BMP580] ERRORE di inizializzazione. Codice: %d\n", bmpErr);
  }

  // --- 2. Inizializzazione SHT45 (Indirizzo nativo 0x44) ---
  Serial.print("[SHT45] Inizializzazione... ");
  if (sht4.begin(&Wire)) {
    Serial.println("Riuscita (0x44)!");
    sht4.setPrecision(SHT4X_HIGH_PRECISION);
    sht4.setHeater(SHT4X_NO_HEATER);
  } else {
    Serial.println("FALLITA!");
  }

  // --- 3. Inizializzazione VEML7700 (Indirizzo nativo 0x10) ---
  Serial.print("[VEML7700] Inizializzazione... ");
  if (veml7700.begin()) {
    Serial.println("Riuscita (0x10)!");
  } else {
    Serial.println("FALLITA!");
  }

  // --- 4. Inizializzazione MCP9808 (Indirizzo 0x18 con A0/A1/A2 a GND) ---
  Serial.print("[MCP9808] Inizializzazione... ");
  if (mcp9808.begin(0x18)) {
    mcp9808.setResolution(3); // massima risoluzione, 0.0625°C
    Serial.println("Riuscita (0x18)!");
  } else {
    Serial.println("FALLITA!");
  }

  // --- 5. Inizializzazione AS3935 su SPI condiviso ---
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, AS3935_CS);
  pinMode(AS3935_CS, OUTPUT);
  digitalWrite(AS3935_CS, HIGH);
  delay(100);
  if (as3935.beginSPI()) {
    as3935.setIndoorOutdoor(INDOOR);
    as3935.setNoiseLevel(2);
    as3935.setWatchdogThreshold(2);
    as3935.setSpikeRejection(2);
    Serial.println("[AS3935] Inizializzazione SPI riuscita!");
  } else {
    Serial.println("[AS3935] FALLITA!");
  }

  Serial.println("--- Setup completato. Avvio letture ---\n");
}

void loop() {
  Serial.println("----------------------------------------");

  // --- Lettura BMP580 (Pressione e Temperatura) ---
  bmp5_sensor_data bmpData;
  int8_t readErr = bmp580.getSensorData(&bmpData);

  if (readErr == BMP5_OK) {
    Serial.printf("[BMP580] Pressione: %.2f hPa | Temp: %.2f °C\n",
                  bmpData.pressure / 100.0f, bmpData.temperature);
  } else {
    Serial.printf("[BMP580] Errore lettura dati. Codice: %d\n", readErr);
  }

  // --- Lettura SHT45 (Umidità e Temperatura) ---
  sensors_event_t humidity, temp;
  if (sht4.getEvent(&humidity, &temp)) {
    Serial.printf("[SHT45]  Umidità:  %.2f %%  | Temp: %.2f °C\n",
                  humidity.relative_humidity, temp.temperature);
  } else {
    Serial.println("[SHT45]  Errore lettura dati.");
  }

  // --- Lettura VEML7700 (Luminosità) ---
  Serial.printf("[VEML7700] Lux: %.2f\n", veml7700.readLux());

  // --- Lettura MCP9808 (Temperatura) ---
  Serial.printf("[MCP9808] Temp: %.2f °C\n", mcp9808.readTempC());

  // --- Lettura AS3935 (interrupt fulmini) ---
  int intr = as3935.readInterruptReg();
  if (intr == 1) Serial.println("[AS3935] Rilevato disturbo/noise");
  else if (intr == 4) Serial.println("[AS3935] Disturber rilevato");
  else if (intr == 8) Serial.println("[AS3935] FULMINE rilevato!");

  delay(2000);
}
