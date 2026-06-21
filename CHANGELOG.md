# Changelog

Tutte le modifiche rilevanti al progetto sono documentate in questo file.

Il formato segue [Keep a Changelog](https://keepachangelog.com/it/1.0.0/),
e il progetto adotta (informalmente) il [Semantic Versioning](https://semver.org/lang/it/).

## [Unreleased]

### Security
- Rimosse credenziali hardcoded da `server/docker-compose.yml`,
  `server/telegraf/telegraf.conf` e dal firmware (`main.cpp`).
  Ora si usano variabili d'ambiente (`server/.env`, non versionato) e
  file `secrets.h` locali (non versionati) generati da un template
  `.example` incluso nel repo.
- Rimossi dal tracking Git i file `server/mosquitto/config/passwd` e
  `passwd.backup.*`, che contenevano hash di password reali.

### Added
- Colonna "Status" nella tabella dei moduli nel README, per distinguere
  moduli effettivamente deployati da quelli pianificati o ancora concettuali.
- Istruzioni nel README per l'installazione tramite il pacchetto server
  standalone (vedi sezione Release).

### Fixed
- Corretto un link errato nel README (`Dragonyx` → `Dragonyx118`).

## [1.0.0] - 2026-06-20

### Added
- Prima release pubblica: pacchetto server standalone (`LegmaMiteo-server.tar.gz`
  / `.zip`) installabile su Raspberry Pi, Linux o Windows tramite script
  automatico (`install.sh` / `install.ps1`).
- Lo script di installazione genera credenziali casuali per InfluxDB,
  Grafana e MQTT ad ogni setup, evitando password fisse condivise.
- Rilevamento automatico di installazioni precedenti nella stessa cartella,
  con pulizia guidata per evitare conflitti di dati/porte.
