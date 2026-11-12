# ===========================================
# Services: MQTT Bridge — LegmaMiteo Backend
# Client MQTT persistente (thread separato via loop_start, non blocca
# uvicorn/FastAPI) verso lo stesso broker a cui si collegano gli ESP32
# (esposto via Tailscale Funnel). Due responsabilità:
#   1) pubblica le decisioni di automazione per SegmaSirra (tetto/luci)
#   2) tiene traccia dello stato OTA (attivo/disattivo) di stazione e serra
#
# Richiede: pip install paho-mqtt
# ===========================================

import json
import ssl
import paho.mqtt.client as mqtt

from app.config import MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_PASSWORD

AUTOMATION_TOPIC = "legmamiteo/automation/serra"
OTA_STATUS_FILTER_STATION = "station/+/ota/stato"
OTA_STATUS_FILTER_SERRA = "serra/+/ota/stato"

# Ultimo stato OTA noto per dispositivo (device_id -> "attivo"/"disattivo"),
# aggiornato dai messaggi retained in arrivo. In memoria: sufficiente per un
# semplice stato on/off, nessuna persistenza necessaria per ora.
ota_status: dict[str, str] = {}

_client: "mqtt.Client | None" = None


def _on_connect(client, userdata, flags, rc, properties=None):
    if rc == 0:
        print("[MQTT-BRIDGE] Connesso al broker")
        client.subscribe(OTA_STATUS_FILTER_STATION, qos=1)
        client.subscribe(OTA_STATUS_FILTER_SERRA, qos=1)
    else:
        print(f"[MQTT-BRIDGE] Connessione fallita, rc={rc}")


def _on_disconnect(client, userdata, rc, properties=None):
    print(f"[MQTT-BRIDGE] Disconnesso (rc={rc}) — paho riconnette da solo se loop_start è attivo")


def _on_message(client, userdata, msg):
    # topic atteso: "station/<id>/ota/stato" oppure "serra/<id>/ota/stato"
    parts = msg.topic.split("/")
    if len(parts) >= 2:
        device_id = parts[1]
        stato = msg.payload.decode(errors="ignore")
        ota_status[device_id] = stato
        print(f"[MQTT-BRIDGE] OTA {device_id}: {stato}")


def start():
    """Da chiamare una volta, all'avvio dell'app (lifespan startup)."""
    global _client
    if _client is not None:
        return

    _client = mqtt.Client(client_id="legmamiteo-backend", protocol=mqtt.MQTTv311)
    if MQTT_USER:
        _client.username_pw_set(MQTT_USER, MQTT_PASSWORD)

    # Stesso pattern TLS degli ESP32 (setInsecure()): il tunnel Tailscale è
    # già il livello di sicurezza di trasporto reale, qui non verifichiamo
    # il certificato del broker.
    _client.tls_set(cert_reqs=ssl.CERT_NONE)
    _client.tls_insecure_set(True)

    _client.on_connect = _on_connect
    _client.on_disconnect = _on_disconnect
    _client.on_message = _on_message

    _client.connect(MQTT_HOST, MQTT_PORT, keepalive=30)
    _client.loop_start()


def stop():
    """Da chiamare allo shutdown dell'app (lifespan shutdown)."""
    global _client
    if _client is not None:
        _client.loop_stop()
        _client.disconnect()
        _client = None


def publish_serra_automation(condition: str, roof_should_close: bool,
                              lights_should_be_off: bool, lightning_alert: bool):
    """Pubblica (retained) la decisione di automazione per SegmaSirra."""
    if _client is None:
        print("[MQTT-BRIDGE] Client non avviato, publish saltato")
        return
    payload = {
        "roof_should_close": roof_should_close,
        "lights_should_be_off": lights_should_be_off,
        "lightning_alert": lightning_alert,
        "condition": condition,
    }
    _client.publish(AUTOMATION_TOPIC, json.dumps(payload), qos=1, retain=True)