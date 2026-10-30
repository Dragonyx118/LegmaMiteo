#include <Arduino.h>
#include <Wire.h>
#include <SparkFun_BMP581_Arduino_Library.h>
#include <Adafruit_SHT4x.h>

// Pin I2C condivisi
const int I2C_SDA = 6;
const int I2C_SCL = 5;

// Oggetti sensori
BMP581 bmp580;
Adafruit_SHT4x sht4;

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n--- LegmaMiteo: BMP580 + SHT45 Test I2C ---");

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

  delay(2000);
}