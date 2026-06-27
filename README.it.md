# 🌤️ LegmaMiteo — Rete di Stazioni Meteo Open Source

[![Hippocratic License HL3-CL-ECO-LAW-MIL-SV](https://img.shields.io/static/v1?label=Hippocratic%20License&message=HL3-CL-ECO-LAW-MIL-SV&labelColor=5e2751&color=bc8c3d&Style=flat)](https://firstdonoharm.dev/version/3/0/cl-eco-law-mil-sv.html)
![Hardware](https://img.shields.io/badge/ESP32--S3-Espressif-E7352C?style=flat&logo=espressif&logoColor=white)
![Connectivity](https://img.shields.io/badge/WiFi_%7C_LoRa-Connected-0088CC?Style=flat&logo=wifi&logoColor=white)
![Power](https://img.shields.io/badge/Solar_Powered-Autonomous-FFB300?Style=flat&logo=solus&logoColor=white)
![Cost](https://img.shields.io/badge/Cost-under_€200-2ECC71?Style=flat&logo=monero&logoColor=white)
![Platform](https://img.shields.io/badge/Docker-Containerized-2496ED?Style=flat&logo=docker&logoColor=white)
![DB](https://img.shields.io/badge/InfluxDB-Time--Series-22ADF6?Style=flat&logo=influxdb&logoColor=white)
![Dashboard](https://img.shields.io/badge/Grafana-Dashboard-F46800?Style=flat&logo=grafana&logoColor=white)
![Protocol](https://img.shields.io/badge/MQTT-Protocol-660066?Style=flat&logo=eclipse-mosquitto&logoColor=white)
![GitHub](https://img.shields.io/badge/GitHub-Dragonyx-181717?Style=flat&logo=github&logoColor=white)
![Status](https://img.shields.io/badge/Status-WIP-E74C3C?Style=flat&logo=git&logoColor=white)

Una rete open-source, modulare e scalabile di stazioni meteo per il
monitoraggio climatico in tempo reale. Pensata per essere economica e
replicabile — ogni unità costa meno di €200 e può essere installata
ovunque.

🇬🇧 **[Read in English](README.md)**

---

## 🌍 Perché questo progetto

Il cambiamento climatico sta aumentando frequenza e intensità degli
eventi meteo convettivi severi — supercelle, grandinate, tornado —
nel Nord Italia. I modelli di previsione nazionali (ARPA, ECMWF, ICON)
lavorano a risoluzioni spaziali e temporali troppo grossolane per fare
**nowcasting** di questi fenomeni: localizzati e a sviluppo rapido, spesso
sono già passati nel momento in cui la griglia del modello si aggiorna.

LegmaMiteo nasce per colmare questo vuoto con una rete densa e a basso
costo di micro-stazioni meteorologiche, pensata per il nowcasting — la
previsione a brevissimo termine e ad alta risoluzione locale — con un
primo focus sulla Lombardia. Più stazioni installate significano un
"ground truth" più fine e più aggiornato di quanto qualsiasi modello
nazionale possa offrire, e un dataset aperto che cresce insieme alla
community che lo costruisce. Le reti di stazioni meteo esistenti sono
rade, costose e spesso a dati chiusi: LegmaMiteo vuole democratizzare
il monitoraggio degli eventi meteo severi, permettendo a chiunque di
costruire, installare e contribuire dati a una rete aperta e globale.

---

## ✨ Caratteristiche

- **Design modulare** — unità base + moduli di espansione intercambiabili
- **Doppia connettività** — WiFi (principale) + mesh LoRa (zone remote)
- **Alimentazione solare** — completamente autonoma, nessuna rete elettrica richiesta
- **Self-hosted** — i tuoi dati restano sul tuo server
- **Dati aperti** — tutte le misurazioni accessibili pubblicamente via REST API
- **Sotto i €200 a unità** — economica e replicabile ovunque

---

## 🧩 Moduli

| Modulo | Sensori | Caso d'uso | Stato |
|--------|---------|------------|-------|
| **BASE** | BME280, VEML7700, sensore pioggia, anemometro, pluviometro | Ovunque | ✅ Deployato (station-001) |
| **MOD-AIR** | PMS5003, SCD40, ENS160+AHT21 | Urbano / Industriale | 🚧 Dashboard pronta, firmware in lavorazione |
| **MOD-STORM** | AS3935, BMP580, GY-91, MLX90614BAA, TSL2591 | Monitoraggio temporali | 🚧 Dashboard pronta, firmware in lavorazione |
| **MOD-HYDRO** | Livello a ultrasuoni, sensore di flusso, turbidità | Fiumi / Zone a rischio alluvione | 📋 Pianificato |
| **MOD-SOIL** | Umidità capacitiva x3, DS18B20 | Agricoltura / Forestale | 📋 Pianificato |
| **MOD-SNOW** | VL53L1X, cella di carico, DS18B20, OV2640 | Montagna / Alpino | 📋 Pianificato |
| **MOD-FIRE** | IR fiamma, MQ-7, MQ-2 | Mediterraneo / Forestale | 📋 Pianificato |
| **MOD-NOISE** | Microfono MEMS SPH0645 | Urbano / Industriale | 💡 Concept |
| **MOD-RAD** | UV ML8511, tubo Geiger | Alta quota | 💡 Concept |

---

## 🏗️ Architettura

```
[Stazione ESP32-S3]
      │
      ├── WiFi ──────────────────────▶ [Broker MQTT]
      │                                      │
      └── LoRa ──▶ [Gateway LoRa RPi] ───────┘
                                             │
                                       [Telegraf]
                                             │
                                       [InfluxDB]
                                             │
                                       [Grafana]
                                             │
                                       [REST API]
```

### Connettore moduli — SP13 IP68 8 pin

Tutti i moduli di espansione si collegano all'unità base tramite un
connettore stagno standardizzato SP13 IP68 a 8 pin, con questo pinout:

| Pin | Segnale |
|-----|---------|
| 1 | VCC 3.3V |
| 2 | VCC 5V |
| 3 | GND |
| 4 | I2C SDA |
| 5 | I2C SCL |
| 6 | UART TX |
| 7 | UART RX |
| 8 | GPIO / Interrupt |

---

## 🛠️ Stack hardware

- **MCU**: ESP32-S3
- **Connettività**: WiFi 802.11 b/g/n + LoRa 868MHz (CDEBYTE E22-900M22S)
- **Alimentazione**: pannello solare 10W + batteria LiFePO4 + controller MPPT
- **Custodia**: scatola IP66 + capannina Stevenson stampata in 3D
- **Connettore moduli**: SP13 IP68 8 pin (JST-XH 8 pin interno)

---

## 💻 Stack server

- **Broker MQTT**: Mosquitto (con autenticazione password)
- **Database time-series**: InfluxDB 2.x
- **Visualizzazione**: Grafana
- **Bridge**: Telegraf
- **REST API**: FastAPI (Python)
- **Tutto containerizzato**: Docker Compose

---

## 🌐 REST API

L'API REST pubblica espone dati in tempo reale e storici da tutte le stazioni.

**URL base**: `http://localhost:8000` (oppure il tuo URL del tunnel Cloudflare)

| Endpoint | Descrizione |
|----------|-------------|
| `GET /` | Info API |
| `GET /health` | Controllo stato |
| `GET /stations/` | Lista delle stazioni attive |
| `GET /stations/{id}` | Metadati di una stazione |
| `GET /data/{id}/latest?module=base` | Ultima lettura di una stazione |
| `GET /data/{id}/history?field=temperature&range_hours=24` | Dati storici |
| `GET /data/{id}/alerts` | Condizioni di allerta attive |

**Esempio — ultimo dato di station-001:**
```bash
curl http://localhost:8000/data/station-001/latest
```
```json
{
  "success": true,
  "station_id": "station-001",
  "module": "base",
  "data": {
    "temperature": 29.7,
    "humidity": 61.8,
    "pressure": 1017.6,
    "lux": 63261.0,
    "wind_speed": 29.2,
    "wind_direction": 51.0,
    "rain_mm": 1.2
  }
}
```

Documentazione interattiva completa su `/docs` (Swagger UI).

I dati sono rilasciati sotto licenza **CC BY-NC 4.0**. È vietato l'uso per training di modelli AI/ML senza permesso scritto esplicito.

---

## 🔔 Sistema di allerta

Allerte basate su Grafana con notifiche Telegram. Regole attive:

| Allerta | Condizione | Gravità |
|---------|-----------|---------|
| 🌡️ Temperatura alta | temperatura > 35°C | Attenzione |
| 😷 PM2.5 critico | pm25 > 55 µg/m³ (soglia OMS) | Critico |
| 🏭 CO2 alta | co2 > 1000 ppm | Attenzione |
| ⚡ Fulmine vicino | distanza fulmine < 10 km | Critico (immediato) |
| 🌩️ Calo pressione rapido | calo pressione > 3 hPa / 30 min | Attenzione (temporale in arrivo) |

---

## 🚀 Come iniziare

### Opzione 1 — Pacchetto server standalone (consigliata)

Il modo più rapido per avviare un server su **Raspberry Pi, PC Linux o
Windows** — senza configurazione manuale di Docker. Scarica l'ultima
release, che include un installer che genera credenziali sicure e
casuali automaticamente:

👉 **[Ultima release](https://github.com/Dragonyx118/LegmaMiteo/releases/latest)**

```bash
# Linux / Raspberry Pi
tar -xzf LegmaMiteo-server.tar.gz
cd LegmaMiteo-server
chmod +x install.sh
./install.sh
```

```powershell
# Windows (PowerShell, con Docker Desktop in esecuzione)
Expand-Archive LegmaMiteo-server.zip
cd LegmaMiteo-server
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
.\install.ps1
```

L'installer stampa le credenziali admin generate e gli URL alla fine —
salvale in un posto sicuro.

### Opzione 2 — Clonare il repo completo (per sviluppo)

```bash
git clone https://github.com/Dragonyx118/LegmaMiteo
cd LegmaMiteo/server
docker compose up -d
```

Poi apri:
- Grafana: http://localhost:3000
- InfluxDB: http://localhost:8086

⚠️ Questo percorso **non** genera credenziali automaticamente. Imposta
tu `INFLUXDB_ADMIN_PASSWORD`, `GRAFANA_ADMIN_PASSWORD` e `INFLUX_TOKEN`
in un file `server/.env` prima di lanciare `docker compose up` — non
committare mai credenziali reali nel repo. Vedi
`server/telegraf/telegraf.conf.example` e
`firmware/base/*/src/secrets.h.example` per i file da copiare e
compilare localmente.

### Firmware

Documentazione in arrivo.

### Hardware

Schemi e file PCB in arrivo.

---

## 📡 Politica sui dati

Tutti i dati raccolti da questa rete sono rilasciati sotto licenza
**Creative Commons Attribution-NonCommercial 4.0**.

Sono **espressamente vietati** i seguenti usi:
- Applicazioni militari o di difesa
- Sistemi d'arma o targeting
- Sorveglianza massiva o profilazione individuale
- Training di modelli AI/ML senza permesso scritto esplicito
- Qualsiasi uso volto a causare danno a persone o comunità

Vedi [DATA_POLICY.md](DATA_POLICY.md) per i dettagli completi.

---

## 🤝 Contribuire

I contributi sono benvenuti! Leggi
[CONTRIBUTING.md](CONTRIBUTING.md) prima di aprire una pull request.

Contribuendo accetti di rispettare le linee guida etiche del progetto.

Se fai parte di una community meteo italiana (Meteonetwork, Centro Meteo
Lombardo, o gruppi locali di osservatori) e vuoi installare una stazione
o integrare i dati, apri una issue o contatta l'autore — la rete cresce
con ogni stazione installata.

---

## 📄 Licenza

Questo progetto è rilasciato sotto **Hippocratic License HL3-CL-ECO-LAW-MIL-SV** — vedi [LICENSE](LICENSE) per i dettagli.

Moduli inclusi:
- **CL** — Copyleft
- **ECO** — Ecocidio
- **LAW** — Forze dell'ordine
- **MIL** — Attività militari
- **SV** — Sorveglianza massiva

---

## 👤 Autore

[Dragonyx](https://github.com/Dragonyx) — costruito con ❤️ per il pianeta.