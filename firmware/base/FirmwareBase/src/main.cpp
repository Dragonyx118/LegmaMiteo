#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  delay(2000); // tempo per aprire il monitor seriale

  Serial.println("\n\n=== VERIFICA BOARD ESP32-S3 N16R8 ===");

  // --- Controllo PSRAM ---
  if (psramFound()) {
    Serial.printf("✅ PSRAM trovata: %d bytes (%.2f MB)\n",
                  ESP.getPsramSize(), ESP.getPsramSize() / (1024.0 * 1024.0));
  } else {
    Serial.println("❌ PSRAM NON trovata! Controlla board_build.arduino.memory_type = qio_opi nel platformio.ini");
  }

  // --- Controllo Flash ---
  uint32_t flashSize = ESP.getFlashChipSize();
  Serial.printf("Flash size rilevata: %d bytes (%.2f MB)\n",
                flashSize, flashSize / (1024.0 * 1024.0));
  if (flashSize < 15 * 1024 * 1024) {
    Serial.println("⚠️  Flash size sembra inferiore a 16MB - controlla board_build.flash_size nel platformio.ini");
  } else {
    Serial.println("✅ Flash size corretta (16MB)");
  }

  // --- Info chip generali ---
  Serial.printf("Chip model: %s\n", ESP.getChipModel());
  Serial.printf("Chip revision: %d\n", ESP.getChipRevision());
  Serial.printf("CPU freq: %d MHz\n", ESP.getCpuFreqMhz());
  Serial.printf("SDK version: %s\n", ESP.getSdkVersion());

  Serial.println("\n=== TEST ANTENNA WIFI (scan ogni 5s) ===");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
}

void loop() {
  Serial.println("\n--- Nuova scansione ---");
  int n = WiFi.scanNetworks();

  if (n == 0) {
    Serial.println("❌ Nessuna rete trovata! Controlla antenna/connettore IPEX.");
  } else {
    Serial.printf("Trovate %d reti:\n", n);
    for (int i = 0; i < n; i++) {
      int rssi = WiFi.RSSI(i);
      String qualita;
      if (rssi > -50) qualita = "OTTIMO";
      else if (rssi > -60) qualita = "BUONO";
      else if (rssi > -70) qualita = "DISCRETO";
      else if (rssi > -80) qualita = "DEBOLE";
      else qualita = "MOLTO DEBOLE";

      Serial.printf("%2d) %-30s RSSI: %4d dBm  Ch:%2d  [%s]\n",
                    i + 1,
                    WiFi.SSID(i).c_str(),
                    rssi,
                    WiFi.channel(i),
                    qualita.c_str());
    }
  }
  WiFi.scanDelete();
  delay(5000);
}