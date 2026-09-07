import os
import time
import requests
from datetime import datetime
from influxdb_client import InfluxDBClient

# --- Configurazione da variabili d'ambiente ---
BOT_TOKEN = os.environ["TELEGRAM_BOT_TOKEN"]
CHAT_ID = os.environ["TELEGRAM_CHAT_ID"]
INFLUX_URL = os.environ["INFLUX_URL"]
INFLUX_TOKEN = os.environ["INFLUX_TOKEN"]
INFLUX_ORG = os.environ.get("INFLUX_ORG", "legmamiteo")
BUCKET = os.environ.get("INFLUX_BUCKET", "stations")
STATION_ID = os.environ.get("STATION_ID", "station-001")

# --- Soglie di allerta (regolabili via env, con default sensati) ---
PRESSURE_DROP_THRESHOLD = float(os.environ.get("PRESSURE_DROP_THRESHOLD", 1.0))   # hPa in 30 min
TEMP_DROP_THRESHOLD = float(os.environ.get("TEMP_DROP_THRESHOLD", 3.0))           # °C in 30 min
ALERT_COOLDOWN_S = int(os.environ.get("ALERT_COOLDOWN_S", 3600))                  # 1 ora
REPORT_INTERVAL_S = int(os.environ.get("REPORT_INTERVAL_S", 1800))                # 30 min
CHECK_INTERVAL_S = int(os.environ.get("CHECK_INTERVAL_S", 120))                   # ogni 2 min

client = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
query_api = client.query_api()

last_alert_time = 0


def send_telegram(text: str):
    url = f"https://api.telegram.org/bot{BOT_TOKEN}/sendMessage"
    try:
        resp = requests.post(
            url,
            json={"chat_id": CHAT_ID, "text": text, "parse_mode": "HTML"},
            timeout=10,
        )
        if resp.status_code != 200:
            print(f"[ERRORE] Telegram ha risposto {resp.status_code}: {resp.text}")
    except Exception as e:
        print(f"[ERRORE] Invio Telegram fallito: {e}")


def get_recent_data(minutes: int):
    """Ritorna liste ordinate (crescente nel tempo) di valori pressione e temperatura."""
    query = f'''
    from(bucket: "{BUCKET}")
      |> range(start: -{minutes}m)
      |> filter(fn: (r) => r._measurement == "weather_station")
      |> filter(fn: (r) => r._field == "pressure" or r._field == "temperature")
      |> filter(fn: (r) => r.station_id == "{STATION_ID}")
      |> aggregateWindow(every: 5m, fn: mean, createEmpty: false)
      |> sort(columns: ["_time"])
    '''
    tables = query_api.query(query)
    pressure_vals, temp_vals = [], []
    for table in tables:
        for record in table.records:
            field = record.get_field()
            value = record.get_value()
            if field == "pressure":
                pressure_vals.append(value)
            elif field == "temperature":
                temp_vals.append(value)
    return pressure_vals, temp_vals


def check_emergency():
    global last_alert_time

    now = time.time()
    if now - last_alert_time < ALERT_COOLDOWN_S:
        return  # ancora in cooldown, non controllare nemmeno

    pressure_vals, temp_vals = get_recent_data(30)

    if len(pressure_vals) < 2:
        return  # dati insufficienti per calcolare un trend

    pressure_delta = pressure_vals[-1] - pressure_vals[0]
    temp_delta = (temp_vals[-1] - temp_vals[0]) if len(temp_vals) >= 2 else 0

    if pressure_delta <= -PRESSURE_DROP_THRESHOLD:
        send_telegram(
            f"⚠️ <b>ALLERTA TEMPORALE - {STATION_ID}</b>\n"
            f"Pressione in calo: {pressure_delta:.2f} hPa in ~30 min\n"
            f"Attuale: {pressure_vals[-1]:.1f} hPa\n"
            f"Possibile peggioramento in arrivo."
        )
        last_alert_time = now
        return

    if temp_delta <= -TEMP_DROP_THRESHOLD:
        send_telegram(
            f"⚠️ <b>ALLERTA - {STATION_ID}</b>\n"
            f"Calo termico brusco: {temp_delta:.1f}°C in ~30 min\n"
            f"Possibile outflow da temporale nelle vicinanze."
        )
        last_alert_time = now


def send_periodic_report():
    pressure_vals, temp_vals = get_recent_data(10)

    if not pressure_vals or not temp_vals:
        send_telegram(
            f"⚠️ <b>{STATION_ID}</b>: nessun dato ricevuto negli ultimi 10 minuti."
        )
        return

    send_telegram(
        f"📊 <b>Bollettino {STATION_ID}</b>\n"
        f"🕐 {datetime.now().strftime('%H:%M')}\n"
        f"🌡️ Temp: {temp_vals[-1]:.1f}°C\n"
        f"📈 Pressione: {pressure_vals[-1]:.1f} hPa"
    )


def main():
    print(f"[AVVIO] Bot Telegram LegmaMiteo — stazione {STATION_ID}")
    send_telegram(f"🤖 Bot LegmaMiteo avviato — monitoraggio {STATION_ID} attivo.")

    last_report = 0

    while True:
        try:
            check_emergency()

            if time.time() - last_report >= REPORT_INTERVAL_S:
                send_periodic_report()
                last_report = time.time()

        except Exception as e:
            print(f"[ERRORE] Loop principale: {e}")

        time.sleep(CHECK_INTERVAL_S)


if __name__ == "__main__":
    main()