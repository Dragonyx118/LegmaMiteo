import os
import time
import requests
from datetime import datetime, time as dtime
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
      |> filter(fn: (r) => r.topic == "station/{STATION_ID}/base")
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


def get_pressure_delta(hours: int = 3):
    """Ritorna il delta di pressione nelle ultime N ore (None se dati insufficienti)."""
    pressure_vals, _ = get_recent_data(hours * 60)
    if len(pressure_vals) < 2:
        return None
    return pressure_vals[-1] - pressure_vals[0]


def get_lux_condition(minutes: int = 15):
    """Legge il valore lux più recente e lo classifica. Solo di giorno (7:00-20:00)."""
    now_time = datetime.now().time()
    if not (dtime(7, 0) <= now_time <= dtime(20, 0)):
        return None  # di notte il lux non è indicativo del cielo

    query = f'''
    from(bucket: "{BUCKET}")
      |> range(start: -{minutes}m)
      |> filter(fn: (r) => r._measurement == "weather_station")
      |> filter(fn: (r) => r._field == "lux")
      |> filter(fn: (r) => r.topic == "station/{STATION_ID}/base")
      |> sort(columns: ["_time"], desc: true)
      |> limit(n: 1)
    '''
    tables = query_api.query(query)
    for table in tables:
        for record in table.records:
            lux = record.get_value()
            # Soglie indicative — da tarare sul sensore reale una volta raccolti dati
            if lux < 50:
                return "🌙 Molto scuro (cielo coperto o crepuscolo)"
            elif lux < 1000:
                return "☁️ Cielo nuvoloso"
            elif lux < 10000:
                return "⛅ Parzialmente nuvoloso"
            elif lux < 30000:
                return "🌤️ Soleggiato velato"
            else:
                return "☀️ Pieno sole"
    return None


def get_forecast_text(pressure_now: float, pressure_delta_3h) -> str:
    """
    Previsione semplificata stile barometro analogico (pressione assoluta + trend).
    pressure_delta_3h può essere None se non ci sono abbastanza dati storici.
    """
    delta = pressure_delta_3h if pressure_delta_3h is not None else 0.0

    # Classificazione del trend
    if delta >= 1.6:
        trend, trend_icon = "in rapida salita", "⬆️"
    elif delta >= 0.5:
        trend, trend_icon = "in salita", "↗️"
    elif delta <= -1.6:
        trend, trend_icon = "in rapido calo", "⬇️"
    elif delta <= -0.5:
        trend, trend_icon = "in calo", "↘️"
    else:
        trend, trend_icon = "stabile", "➡️"

    # Classificazione livello assoluto + previsione testuale
    if pressure_now >= 1022:
        if delta >= 0.5:
            forecast = "☀️ Bel tempo, cielo sereno in consolidamento"
        elif delta <= -0.5:
            forecast = "🌤️ Bel tempo ma in graduale peggioramento"
        else:
            forecast = "☀️ Bel tempo stabile"
    elif pressure_now >= 1013:
        if delta >= 0.5:
            forecast = "⛅ Tempo in miglioramento, variabile"
        elif delta <= -0.5:
            forecast = "🌥️ Tempo variabile, possibile peggioramento"
        else:
            forecast = "⛅ Tempo variabile stabile"
    elif pressure_now >= 1000:
        if delta >= 0.5:
            forecast = "🌥️ Instabile ma in miglioramento"
        elif delta <= -1.0:
            forecast = "🌧️ Instabile, possibili rovesci in arrivo"
        else:
            forecast = "☁️ Nuvoloso, tempo incerto"
    else:
        if delta <= -0.5:
            forecast = "⛈️ Perturbato, condizioni in peggioramento"
        else:
            forecast = "🌧️ Perturbato, piogge probabili"

    delta_text = f"{delta:+.1f} hPa/3h" if pressure_delta_3h is not None else "dati insufficienti"
    return f"{forecast}\nTrend: {trend_icon} {trend} ({delta_text})"


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

    pressure_delta_3h = get_pressure_delta(3)
    forecast_text = get_forecast_text(pressure_vals[-1], pressure_delta_3h)
    lux_condition = get_lux_condition()

    message = (
        f"📊 <b>Bollettino {STATION_ID}</b>\n"
        f"🕐 {datetime.now().strftime('%H:%M')}\n"
        f"🌡️ Temp: {temp_vals[-1]:.1f}°C\n"
        f"📈 Pressione: {pressure_vals[-1]:.1f} hPa\n\n"
        f"<b>Previsione:</b>\n{forecast_text}"
    )

    if lux_condition:
        message += f"\n\n<b>Cielo attuale:</b> {lux_condition}"

    send_telegram(message)


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