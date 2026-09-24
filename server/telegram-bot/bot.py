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
STATION_ALTITUDE_M = float(os.environ.get("STATION_ALTITUDE_M", 84))  # Campagnola Cremasca (CR)

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


def get_pressure_delta(hours: float):
    """Ritorna il delta di pressione nelle ultime N ore (None se dati insufficienti)."""
    pressure_vals, _ = get_recent_data(int(hours * 60))
    if len(pressure_vals) < 2:
        return None
    return pressure_vals[-1] - pressure_vals[0]


def get_multi_window_trend():
    """Ritorna i delta di pressione su più finestre temporali: 1h, 3h, 6h."""
    return {
        "1h": get_pressure_delta(1),
        "3h": get_pressure_delta(3),
        "6h": get_pressure_delta(6),
    }


def get_pressure_acceleration():
    """
    Confronta il trend dell'ultima ora con quello delle 2 ore precedenti,
    per capire se il calo/salita si sta intensificando o attenuando.
    Valore negativo forte = il calo sta accelerando (peggioramento rapido).
    """
    delta_1h = get_pressure_delta(1)
    delta_3h = get_pressure_delta(3)
    if delta_1h is None or delta_3h is None:
        return None
    delta_prior_2h = delta_3h - delta_1h
    return delta_1h - (delta_prior_2h / 2)


def get_pressure_delta_24h_same_hour():
    """
    Confronta la pressione attuale con quella di 24h fa alla stessa ora circa,
    per eliminare l'effetto del ciclo semidiurno naturale di pressione.
    """
    query = f'''
    from(bucket: "{BUCKET}")
      |> range(start: -25h, stop: -23h)
      |> filter(fn: (r) => r._measurement == "weather_station")
      |> filter(fn: (r) => r._field == "pressure")
      |> filter(fn: (r) => r.topic == "station/{STATION_ID}/base")
      |> mean()
    '''
    tables = query_api.query(query)
    pressure_24h_ago = None
    for table in tables:
        for record in table.records:
            pressure_24h_ago = record.get_value()

    if pressure_24h_ago is None:
        return None

    pressure_vals, _ = get_recent_data(10)
    if not pressure_vals:
        return None

    return pressure_vals[-1] - pressure_24h_ago


def get_temp_range_24h():
    """Escursione termica nelle ultime 24h — bassa escursione suggerisce cielo coperto/nebbia."""
    _, temp_vals = get_recent_data(24 * 60)
    if not temp_vals:
        return None
    return max(temp_vals) - min(temp_vals)


def sea_level_pressure(station_pressure_hpa: float, altitude_m: float, temp_c: float) -> float:
    """Corregge la pressione stazione al livello del mare (formula barometrica standard)."""
    return station_pressure_hpa * (1 - (0.0065 * altitude_m) / (temp_c + 0.0065 * altitude_m + 273.15)) ** -5.257


def get_season():
    month = datetime.now().month
    if month in (12, 1, 2):
        return "inverno"
    elif month in (3, 4, 5):
        return "primavera"
    elif month in (6, 7, 8):
        return "estate"
    else:
        return "autunno"


SEASONAL_THRESHOLDS = {
    "inverno":   {"alta": 1025, "media": 1015, "bassa": 1000},
    "primavera": {"alta": 1023, "media": 1014, "bassa": 1000},
    "estate":    {"alta": 1020, "media": 1012, "bassa": 1003},
    "autunno":   {"alta": 1023, "media": 1014, "bassa": 1000},
}


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


def get_forecast_text(pressure_station: float, temp_now: float) -> str:
    """
    Previsione basata su pressione corretta al livello del mare, soglie stagionali,
    trend multi-finestra, accelerazione e confronto 24h a parità di ora.
    """
    pressure_slp = sea_level_pressure(pressure_station, STATION_ALTITUDE_M, temp_now)
    season = get_season()
    t = SEASONAL_THRESHOLDS[season]

    trends = get_multi_window_trend()
    delta_3h = trends["3h"] if trends["3h"] is not None else 0.0
    delta_24h_same_hour = get_pressure_delta_24h_same_hour()
    acceleration = get_pressure_acceleration()
    temp_range = get_temp_range_24h()

    # --- Trend principale (3h) ---
    if delta_3h >= 1.6:
        trend, trend_icon = "in rapida salita", "⬆️"
    elif delta_3h >= 0.5:
        trend, trend_icon = "in salita", "↗️"
    elif delta_3h <= -1.6:
        trend, trend_icon = "in rapido calo", "⬇️"
    elif delta_3h <= -0.5:
        trend, trend_icon = "in calo", "↘️"
    else:
        trend, trend_icon = "stabile", "➡️"

    # --- Previsione base su soglie stagionali ---
    if pressure_slp >= t["alta"]:
        if temp_range is not None and temp_range < 4 and season in ("inverno", "autunno") and delta_3h >= -0.5:
            forecast = "🌫️ Alta pressione stabile — possibile nebbia/foschia in pianura"
        elif delta_3h >= 0.5:
            forecast = "☀️ Bel tempo, cielo sereno in consolidamento"
        elif delta_3h <= -0.5:
            forecast = "🌤️ Bel tempo ma in graduale peggioramento"
        else:
            forecast = "☀️ Bel tempo stabile"
    elif pressure_slp >= t["media"]:
        if delta_3h >= 0.5:
            forecast = "⛅ Tempo in miglioramento, variabile"
        elif delta_3h <= -0.5:
            forecast = "🌥️ Tempo variabile, possibile peggioramento"
        else:
            forecast = "⛅ Tempo variabile stabile"
    elif pressure_slp >= t["bassa"]:
        if delta_3h >= 0.5:
            forecast = "🌥️ Instabile ma in miglioramento"
        elif delta_3h <= -1.0:
            forecast = "🌧️ Instabile, possibili rovesci in arrivo"
        else:
            forecast = "☁️ Nuvoloso, tempo incerto"
    else:
        if delta_3h <= -0.5:
            forecast = "⛈️ Perturbato, condizioni in peggioramento"
        else:
            forecast = "🌧️ Perturbato, piogge probabili"

    # --- Nota di accelerazione, se significativa ---
    accel_note = ""
    if acceleration is not None and acceleration <= -0.5:
        accel_note = "\n⚠️ Il calo di pressione si sta intensificando"
    elif acceleration is not None and acceleration >= 0.5:
        accel_note = "\n✅ Il trend di miglioramento si sta rafforzando"

    # --- Riepilogo trend multi-finestra ---
    def fmt(v):
        return f"{v:+.1f}" if v is not None else "n/d"

    trend_summary = (
        f"1h: {fmt(trends['1h'])} · 3h: {fmt(trends['3h'])} · "
        f"6h: {fmt(trends['6h'])} · 24h: {fmt(delta_24h_same_hour)} hPa"
    )

    return (
        f"{forecast}\n"
        f"Trend: {trend_icon} {trend}{accel_note}\n"
        f"<i>{trend_summary}</i>\n"
        f"<i>Pressione slm: {pressure_slp:.1f} hPa (stagione: {season})</i>"
    )


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
    acceleration = get_pressure_acceleration()

    # Allerta standard sulla soglia assoluta di calo in 30 min
    triggered = pressure_delta <= -PRESSURE_DROP_THRESHOLD

    # Allerta anticipata se il calo sta accelerando fortemente, anche sotto soglia
    early_warning = (
        not triggered
        and acceleration is not None
        and acceleration <= -0.8
        and pressure_delta <= -0.5
    )

    if triggered:
        send_telegram(
            f"⚠️ <b>ALLERTA TEMPORALE - {STATION_ID}</b>\n"
            f"Pressione in calo: {pressure_delta:.2f} hPa in ~30 min\n"
            f"Attuale: {pressure_vals[-1]:.1f} hPa\n"
            f"Possibile peggioramento in arrivo."
        )
        last_alert_time = now
        return

    if early_warning:
        send_telegram(
            f"🟡 <b>PREALLERTA - {STATION_ID}</b>\n"
            f"Il calo di pressione si sta intensificando\n"
            f"Attuale: {pressure_vals[-1]:.1f} hPa ({pressure_delta:+.2f} hPa/30min)\n"
            f"Monitorare l'evoluzione nelle prossime ore."
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

    forecast_text = get_forecast_text(pressure_vals[-1], temp_vals[-1])
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