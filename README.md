# 🌤️ LegmaMiteo — OpenWeather Station

![License](https://img.shields.io/badge/license-Hippocratic_3.0-brightgreen)
![Hardware](https://img.shields.io/badge/hardware-ESP32--S3-blue)
![Connectivity](https://img.shields.io/badge/connectivity-WiFi_%7C_LoRa-orange)
![Power](https://img.shields.io/badge/power-Solar-yellow)
![Cost](https://img.shields.io/badge/cost-under_€200-green)
![Platform](https://img.shields.io/badge/platform-Docker-2496ED)
![DB](https://img.shields.io/badge/database-InfluxDB-22ADF6)
![Dashboard](https://img.shields.io/badge/dashboard-Grafana-F46800)
![Protocol](https://img.shields.io/badge/protocol-MQTT-660066)
![Status](https://img.shields.io/badge/status-WIP-red)

An open-source, modular, and scalable weather station network designed for
real-time climate monitoring. Built with affordability and replicability in
mind — each unit costs under €200 and can be deployed anywhere.

---

## 🌍 Why This Project

Climate change is accelerating the frequency and intensity of extreme weather
events. Existing weather station networks are sparse, expensive, and often
closed. LegmaMiteo aims to democratize weather monitoring by enabling
anyone to build, deploy, and contribute data to a global open network.

---

## ✨ Features

- **Modular design** — base unit + swappable expansion modules
- **Dual connectivity** — WiFi (primary) + LoRa mesh (remote areas)
- **Solar powered** — fully autonomous, no grid required
- **Self-hosted** — your data stays on your server
- **Open data** — all measurements publicly accessible via REST API
- **Under €200 per unit** — affordable and replicable worldwide

---

## 🧩 Modules

| Module | Sensors | Use Case |
|--------|---------|----------|
| **BASE** | BME280, VEML7700, Rain detector, Anemometer, Rain gauge | Everywhere |
| **MOD-AIR** | PMS5003, SCD40, ENS160+AHT21 | Urban / Industrial |
| **MOD-STORM** | AS3935, BMP580, GY-91, MLX90614BAA, TSL2591 | Storm monitoring |
| **MOD-HYDRO** | Ultrasonic level, flow sensor, turbidity | Rivers / Flood zones |
| **MOD-SOIL** | Capacitive moisture x3, DS18B20 | Agriculture / Forest |
| **MOD-SNOW** | VL53L1X, load cell, DS18B20, OV2640 | Mountain / Alpine |
| **MOD-FIRE** | Flame IR, MQ-7, MQ-2 | Mediterranean / Forest |
| **MOD-NOISE** | MEMS microphone SPH0645 | Urban / Industrial |
| **MOD-RAD** | ML8511 UV, Geiger tube | High altitude |

---

## 🏗️ Architecture

```
[ESP32-S3 Station]
      │
      ├── WiFi ──────────────────────▶ [MQTT Broker]
      │                                      │
      └── LoRa ──▶ [RPi LoRa Gateway] ───────┘
                                             │
                                       [Telegraf]
                                             │
                                       [InfluxDB]
                                             │
                                       [Grafana]
                                             │
                                       [REST API]
```

### Module connector — SP13 IP68 8-pin

All expansion modules connect to the base unit via a standardized
waterproof SP13 IP68 8-pin connector with the following pinout:

| Pin | Signal |
|-----|--------|
| 1 | VCC 3.3V |
| 2 | VCC 5V |
| 3 | GND |
| 4 | I2C SDA |
| 5 | I2C SCL |
| 6 | UART TX |
| 7 | UART RX |
| 8 | GPIO / Interrupt |

---

## 🛠️ Hardware Stack

- **MCU**: ESP32-S3
- **Connectivity**: WiFi 802.11 b/g/n + LoRa 868MHz (CDEBYTE E22-900M22S)
- **Power**: 10W solar panel + LiFePO4 battery + MPPT controller
- **Enclosure**: IP66 box + 3D printed Stevenson screen
- **Module connector**: SP13 IP68 8-pin (JST-XH 8-pin internal)

---

## 💻 Server Stack

- **MQTT Broker**: Mosquitto
- **Time-series DB**: InfluxDB 2.x
- **Visualization**: Grafana
- **Bridge**: Telegraf
- **All containerized**: Docker Compose

---

## 🚀 Getting Started

### Server (your PC or VPS)

```bash
git clone https://github.com/Dragonyx/LegmaMiteo
cd LegmaMiteo/server
docker compose up -d
```

Then open:
- Grafana: http://localhost:3000
- InfluxDB: http://localhost:8086

Default credentials: `admin` / `openweather123`
(change these before any public deployment)

### Firmware

Documentation coming soon.

### Hardware

Schematics and PCB files coming soon.

---

## 📡 Data Policy

All data collected by this network is released under
**Creative Commons Attribution-NonCommercial 4.0**.

The following uses are **strictly prohibited**:
- Military or defense applications
- Weapons systems or targeting
- Mass surveillance or individual profiling
- AI/ML training without explicit written permission
- Any use intended to cause harm to people or communities

See [DATA_POLICY.md](DATA_POLICY.md) for full details.

---

## 🤝 Contributing

Contributions are welcome! Please read
[CONTRIBUTING.md](CONTRIBUTING.md) before submitting pull requests.

By contributing you agree to uphold the ethical guidelines of this project.

---

## 📄 License

This project is licensed under the
**Hippocratic License 3.0** — see [LICENSE](LICENSE) for details.

---

## 👤 Author

[Dragonyx](https://github.com/Dragonyx) — built with ❤️ for the planet.
