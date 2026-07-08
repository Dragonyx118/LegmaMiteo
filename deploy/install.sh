#!/usr/bin/env bash
#
# LegmaMiteo — installer server (Raspberry Pi, Raspberry Pi OS 64-bit)
#
# Uso:
#   curl -fsSL https://github.com/Dragonyx118/LegmaMiteo/releases/latest/download/install.sh -o install.sh
#   sudo bash install.sh
#
# Cosa fa:
#   1. Installa Docker + Tailscale
#   2. Clona/aggiorna la repo in /opt/legmamiteo
#   3. Genera server/.env con secrets casuali (InfluxDB, Grafana)
#   4. Genera server/mosquitto/config/passwd (utente telegraf-reader + stazioni)
#   5. Genera server/telegraf/telegraf.conf dal file .example
#   6. Espone MQTT (1883) e Grafana (3000) via Tailscale Funnel con TLS
#   7. Avvia `docker compose up -d`

set -euo pipefail

REPO_URL="https://github.com/Dragonyx118/LegmaMiteo.git"
INSTALL_DIR="/opt/legmamiteo"
SERVER_DIR="${INSTALL_DIR}/server"
ENV_FILE="${SERVER_DIR}/.env"
MOSQUITTO_CONFIG_DIR="${SERVER_DIR}/mosquitto/config"
PASSWD_FILE="${MOSQUITTO_CONFIG_DIR}/passwd"
TELEGRAF_EXAMPLE="${SERVER_DIR}/telegraf/telegraf.conf.example"
TELEGRAF_CONF="${SERVER_DIR}/telegraf/telegraf.conf"

log()  { echo -e "\033[1;32m[legmamiteo]\033[0m $1"; }
warn() { echo -e "\033[1;33m[legmamiteo]\033[0m $1"; }
err()  { echo -e "\033[1;31m[legmamiteo]\033[0m $1" >&2; }

require_root() {
  if [[ $EUID -ne 0 ]]; then
    err "Esegui questo script con sudo: sudo bash install.sh"
    exit 1
  fi
}

check_arch() {
  ARCH="$(uname -m)"
  if [[ "$ARCH" != "aarch64" ]]; then
    err "Rilevata architettura ${ARCH}. Serve Raspberry Pi OS a 64-bit (aarch64)."
    exit 1
  fi
}

install_docker() {
  if command -v docker &>/dev/null; then
    log "Docker già installato, salto."
  else
    log "Installo Docker..."
    curl -fsSL https://get.docker.com | sh
    usermod -aG docker "${SUDO_USER:-$USER}"
    systemctl enable --now docker
  fi
}

install_tailscale() {
  if ! command -v tailscale &>/dev/null; then
    log "Installo Tailscale..."
    curl -fsSL https://tailscale.com/install.sh | sh
  fi

  log "Avvio Tailscale (autentica dal link se richiesto)..."
  tailscale up --accept-routes || true
}

clone_repo() {
  if [[ -d "$INSTALL_DIR/.git" ]]; then
    log "Repo già presente, eseguo pull..."
    git -C "$INSTALL_DIR" pull origin main
  else
    log "Clono il repository in ${INSTALL_DIR}..."
    git clone "$REPO_URL" "$INSTALL_DIR"
  fi
}

generate_env() {
  if [[ -f "$ENV_FILE" ]]; then
    log "server/.env già presente, non lo sovrascrivo."
    return
  fi

  log "Genero server/.env..."
  INFLUX_TOKEN=$(openssl rand -hex 32)
  INFLUXDB_ADMIN_PASSWORD=$(openssl rand -base64 18 | tr -d '=+/')
  GRAFANA_ADMIN_PASSWORD=$(openssl rand -base64 18 | tr -d '=+/')

  cat > "$ENV_FILE" <<EOF
# Generato da install.sh — NON committare
INFLUX_TOKEN=${INFLUX_TOKEN}
INFLUXDB_ADMIN_PASSWORD=${INFLUXDB_ADMIN_PASSWORD}
GRAFANA_ADMIN_PASSWORD=${GRAFANA_ADMIN_PASSWORD}
INFLUX_URL=http://legmamiteo-influxdb:8086
INFLUX_ORG=legmamiteo
INFLUX_BUCKET=stations
EOF
  chmod 600 "$ENV_FILE"

  log "Credenziali generate — salvale subito, non verranno ristampate:"
  echo "  InfluxDB admin password: ${INFLUXDB_ADMIN_PASSWORD}"
  echo "  Grafana admin password:  ${GRAFANA_ADMIN_PASSWORD}"
}

generate_mosquitto_passwd() {
  mkdir -p "$MOSQUITTO_CONFIG_DIR"

  if [[ -f "$PASSWD_FILE" ]]; then
    log "mosquitto/config/passwd già presente, salto generazione utenti."
  else
    log "Creo mosquitto/config/passwd..."
    TELEGRAF_MQTT_PASSWORD=$(openssl rand -base64 18 | tr -d '=+/')

    docker run --rm -v "${MOSQUITTO_CONFIG_DIR}:/mosquitto/config" \
      eclipse-mosquitto:2 \
      mosquitto_passwd -b -c /mosquitto/config/passwd telegraf-reader "${TELEGRAF_MQTT_PASSWORD}"

    log "Utente MQTT 'telegraf-reader' creato. Password:"
    echo "  ${TELEGRAF_MQTT_PASSWORD}"

    # Genera telegraf.conf sostituendo il placeholder
    sed "s/__TELEGRAF_MQTT_PASSWORD__/${TELEGRAF_MQTT_PASSWORD}/" \
      "$TELEGRAF_EXAMPLE" > "$TELEGRAF_CONF"
    log "server/telegraf/telegraf.conf generato da telegraf.conf.example."
  fi

  # Richiesto: il file deve essere leggibile dall'utente mosquitto nel container
  chmod 644 "$PASSWD_FILE"

  read -rp "Aggiungere una stazione ora? Nome utente (invio per saltare): " STATION_USER
  if [[ -n "${STATION_USER:-}" ]]; then
    STATION_PASSWORD=$(openssl rand -base64 18 | tr -d '=+/')
    docker run --rm -v "${MOSQUITTO_CONFIG_DIR}:/mosquitto/config" \
      eclipse-mosquitto:2 \
      mosquitto_passwd -b "/mosquitto/config/passwd" "$STATION_USER" "$STATION_PASSWORD"
    chmod 644 "$PASSWD_FILE"
    log "Stazione '${STATION_USER}' creata. Password (da inserire in secrets.h sull'ESP32):"
    echo "  ${STATION_PASSWORD}"
  fi
}

setup_funnel() {
  log "Espongo MQTT (1883) via Tailscale Funnel TCP con TLS..."
  tailscale funnel --bg --tcp=8883 tcp://localhost:1883 || \
    warn "Funnel TCP non abilitato automaticamente. Potrebbe servire abilitare Funnel dal pannello admin di Tailscale (Access Controls) per questo tailnet, poi ripetere:  tailscale funnel --bg --tcp=8883 tcp://localhost:1883"

  log "Espongo Grafana via Tailscale Funnel HTTPS..."
  tailscale funnel --bg 443 tcp://localhost:3000 || \
    warn "Funnel per Grafana non abilitato automaticamente, controlla manualmente."
}

start_stack() {
  log "Avvio lo stack Docker Compose..."
  cd "$SERVER_DIR"
  docker compose pull
  docker compose up -d
}

print_summary() {
  local ts_name
  ts_name=$(tailscale status --json 2>/dev/null | grep -o '"DNSName":"[^"]*"' | head -1 | cut -d'"' -f4 || echo "N/D")

  echo
  log "Installazione completata."
  echo "  Hostname Tailscale: ${ts_name}"
  echo "  MQTT (via Funnel):  ${ts_name%.}:8883  (TLS terminato da Tailscale)"
  echo "  Grafana:            https://${ts_name%.}"
  echo
  warn "Aggiorna API_BASE nel frontend (LegmaMiteo-Web) con l'URL Funnel del backend FastAPI (porta 8000)."
  warn "Aggiorna secrets.h su ogni stazione ESP32 con host/porta/utente/password MQTT generati sopra."
  warn "Cambia comunque le password di default se in futuro decidi di non usare questo script su una macchina già esposta."
}

main() {
  require_root
  check_arch
  install_docker
  clone_repo
  generate_env
  generate_mosquitto_passwd
  install_tailscale
  setup_funnel
  start_stack
  print_summary
}

main "$@"
