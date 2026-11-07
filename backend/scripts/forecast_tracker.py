# ===========================================
# Script: forecast_tracker.py — LegmaMiteo Backend
# Logga le previsioni (oggi + 5 giorni) e, quando il giorno target è passato,
# le verifica contro il dato reale della stazione e Open-Meteo.
#
# Pensato per girare 1 volta al giorno (via scheduler), stesso pattern di
# write_sun_times.py:
#   - log_forecast_snapshot()   -> registra la previsione corrente come "in sospeso"
#   - verify_due_forecasts()    -> chiude le previsioni il cui target_date è passato
# ===========================================

import os
import json
import requests
from datetime import datetime, timedelta, date

from influxdb_client import InfluxDBClient

from app.config import INFLUX_URL, INFLUX_TOKEN, INFLUX_ORG, INFLUX_BUCKET, STATION_ALTITUDE_M
from app.models.stations_registry import load_registry
from app.services.forecast import compute_forecast, sea_level_pressure

PENDING_FILE = "data/forecast_predictions.json"
ACCURACY_FILE = "data/forecast_accuracy.json"

# Mappa i weathercode Open-Meteo (WMO) alle stesse categorie usate da forecast.py,
# per poter confrontare "mele con mele" la condizione prevista vs quella osservata
WEATHER_CODE_BUCKET = {
    0: "sereno", 1: "sereno_variabile", 2: "variabile", 3: "nuvoloso",
    45: "nebbia", 48: "nebbia",
    51: "pioggia", 53: "pioggia", 55: "pioggia", 56: "pioggia", 57: "pioggia",
    61: "pioggia", 63: "pioggia", 65: "pioggia", 66: "pioggia", 67: "pioggia",
    71: "pioggia", 73: "pioggia", 75: "pioggia", 77: "pioggia",
    80: "pioggia", 81: "pioggia", 82: "pioggia",
    85: "pioggia", 86: "pioggia",
    95: "temporale", 96: "temporale", 99: "temporale",
}

# Le nostre condition sono più granulari di quelle Open-Meteo: per il confronto
# le raggruppiamo in bucket larghi, altrimenti "sereno_variabile" vs "sereno"
# risulterebbe sempre un "errore" anche quando la previsione era sostanzialmente giusta
CONDITION_BUCKET = {
    "sereno": "sereno", "sereno_variabile": "sereno",
    "variabile": "variabile", "nuvoloso": "variabile", "instabile": "variabile",
    "nebbia": "nebbia",
    "pioggia": "pioggia",
    "temporale": "temporale", "temporale_in_corso": "temporale", "temporale_avvicinamento": "temporale",
}


def _load_json(path, default):
    if os.path.exists(path):
        try:
            with open(path, "r") as f:
                return json.load(f)
        except Exception:
            pass
    return default


def _save_json(path, data):
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            json.dump(data, f, indent=2, default=str)
    except Exception as e:
        print(f"[forecast_tracker] Errore salvataggio {path}: {e}")


def _get_influx_client():
    return InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)


def _query_recent_values(client, station_id, module, field, minutes):
    query_api = client.query_api()
    query = f'''
    from(bucket: "{INFLUX_BUCKET}")
      |> range(start: -{minutes}m)
      |> filter(fn: (r) => r._measurement == "weather_station")
      |> filter(fn: (r) => r.topic == "station/{station_id}/{module}")
      |> filter(fn: (r) => r._field == "{field}")
      |> aggregateWindow(every: 5m, fn: mean, createEmpty: false)
      |> sort(columns: ["_time"])
    '''
    result = query_api.query(query)
    values = []
    for table in result:
        for record in table.records:
            values.append(record.get_value())
    return values


def _query_day_mean(client, station_id, module, field, day: date):
    """Media di un campo per un giorno specifico (usato in verifica, non in log)."""
    query_api = client.query_api()
    start = day.isoformat()
    stop = (day + timedelta(days=1)).isoformat()
    query = f'''
    from(bucket: "{INFLUX_BUCKET}")
      |> range(start: {start}, stop: {stop})
      |> filter(fn: (r) => r._measurement == "weather_station")
      |> filter(fn: (r) => r.topic == "station/{station_id}/{module}")
      |> filter(fn: (r) => r._field == "{field}")
      |> mean()
    '''
    result = query_api.query(query)
    for table in result:
        for record in table.records:
            return record.get_value()
    return None


def _fetch_openmeteo_daily_weathercode(lat, lon, target_date: date):
    """Weathercode giornaliero Open-Meteo per una data (passata o odierna/futura vicina)."""
    today = date.today()
    days_ago = (today - target_date).days
    try:
        if days_ago > 0:
            url = (
                f"https://api.open-meteo.com/v1/forecast?latitude={lat}&longitude={lon}"
                f"&daily=weathercode&past_days={min(days_ago + 1, 92)}&timezone=auto"
            )
        else:
            url = (
                f"https://api.open-meteo.com/v1/forecast?latitude={lat}&longitude={lon}"
                f"&daily=weathercode&forecast_days=1&timezone=auto"
            )
        resp = requests.get(url, timeout=10)
        if resp.status_code != 200:
            return None
        data = resp.json()
        dates = data.get("daily", {}).get("time", [])
        codes = data.get("daily", {}).get("weathercode", [])
        target_str = target_date.isoformat()
        if target_str in dates:
            idx = dates.index(target_str)
            return codes[idx]
    except Exception as e:
        print(f"[forecast_tracker] Errore Open-Meteo weathercode: {e}")
    return None


# =====================================================================
# 1) LOG — registra la previsione corrente come "in sospeso"
# =====================================================================

def log_forecast_snapshot():
    """
    Calcola la previsione attuale (oggi + 5 giorni proiettati) per ogni stazione
    nel registry e la registra come "in sospeso" per ciascuna data target non
    ancora loggata oggi — idempotente: non duplica se già fatto nella giornata.
    """
    registry = load_registry()
    pending = _load_json(PENDING_FILE, [])
    today = date.today()

    already_logged_today = {
        (p["station_id"], p["target_date"], p.get("logged_on"))
        for p in pending
    }

    for station_id, meta in registry.items():
        lat = meta.get("latitude")
        lon = meta.get("longitude")
        altitude = meta.get("altitude", STATION_ALTITUDE_M)
        if lat is None or lon is None:
            continue

        client = _get_influx_client()
        try:
            pressure_vals = _query_recent_values(client, station_id, "base", "pressure", 10)
            temp_vals = _query_recent_values(client, station_id, "base", "temperature", 10)
            humidity_vals = _query_recent_values(client, station_id, "base", "humidity", 10)
            pressure_3h = _query_recent_values(client, station_id, "base", "pressure", 180)

            if not pressure_vals or not temp_vals:
                continue

            delta_3h = (pressure_3h[-1] - pressure_3h[0]) if len(pressure_3h) >= 2 else None
            humidity_now = humidity_vals[-1] if humidity_vals else None

            forecast = compute_forecast(
                station_id=station_id,
                pressure_station=pressure_vals[-1],
                temp_now=temp_vals[-1],
                altitude_m=altitude,
                month=datetime.now().month,
                delta_3h=delta_3h,
                humidity=humidity_now,
            )
        finally:
            client.close()

        # Giorno 0: la previsione "di oggi" si verifica a fine giornata (domani la logghiamo chiusa)
        all_days = [{
            "day_offset": 0,
            "condition": forecast["condition"],
            "pressure_slp_estimate": forecast["pressure_slp"],
            "confidence": 1.0,
        }] + forecast["extended_forecast"]

        for day_data in all_days:
            target_date = today + timedelta(days=day_data["day_offset"])
            key = (station_id, target_date.isoformat(), today.isoformat())
            if key in already_logged_today:
                continue  # già loggata oggi per questo target, non duplicare

            pending.append({
                "station_id": station_id,
                "logged_on": today.isoformat(),
                "target_date": target_date.isoformat(),
                "day_offset_at_log_time": day_data["day_offset"],
                "predicted_condition": day_data["condition"],
                "predicted_condition_bucket": CONDITION_BUCKET.get(day_data["condition"], day_data["condition"]),
                "predicted_pressure_slp": day_data["pressure_slp_estimate"],
                "predicted_confidence": day_data["confidence"],
                "latitude": lat,
                "longitude": lon,
            })

    _save_json(PENDING_FILE, pending)
    print(f"[forecast_tracker] Log completato — {len(pending)} previsioni in sospeso")


# =====================================================================
# 2) VERIFY — chiude le previsioni il cui target_date è ormai passato
# =====================================================================

def verify_due_forecasts():
    """
    Per ogni previsione in sospeso il cui target_date è un giorno ormai concluso,
    confronta la previsione con: (a) la pressione slm reale media di quel giorno
    dalla stazione, (b) il weathercode Open-Meteo di quel giorno. Sposta il
    risultato nel log di accuratezza e lo rimuove dalle previsioni in sospeso.
    """
    pending = _load_json(PENDING_FILE, [])
    accuracy_log = _load_json(ACCURACY_FILE, [])
    today = date.today()

    still_pending = []
    resolved_count = 0

    for entry in pending:
        target_date = date.fromisoformat(entry["target_date"])

        if target_date >= today:
            still_pending.append(entry)  # giorno non ancora concluso, aspetta
            continue

        station_id = entry["station_id"]
        client = _get_influx_client()
        try:
            actual_pressure_station = _query_day_mean(client, station_id, "base", "pressure", target_date)
            actual_temp = _query_day_mean(client, station_id, "base", "temperature", target_date)
        finally:
            client.close()

        actual_pressure_slp = None
        pressure_error = None
        if actual_pressure_station is not None and actual_temp is not None:
            meta_altitude = STATION_ALTITUDE_M  # approssimazione: non rilegge il registry qui
            actual_pressure_slp = sea_level_pressure(actual_pressure_station, meta_altitude, actual_temp)
            pressure_error = round(abs(actual_pressure_slp - entry["predicted_pressure_slp"]), 2)

        weathercode = _fetch_openmeteo_daily_weathercode(entry["latitude"], entry["longitude"], target_date)
        actual_condition_bucket = WEATHER_CODE_BUCKET.get(weathercode) if weathercode is not None else None
        condition_match = (
            actual_condition_bucket is not None
            and actual_condition_bucket == entry["predicted_condition_bucket"]
        )

        accuracy_log.append({
            **entry,
            "actual_pressure_slp": round(actual_pressure_slp, 1) if actual_pressure_slp is not None else None,
            "pressure_error_hpa": pressure_error,
            "actual_weathercode": weathercode,
            "actual_condition_bucket": actual_condition_bucket,
            "condition_match": condition_match,
            "resolved_on": today.isoformat(),
        })
        resolved_count += 1

    _save_json(PENDING_FILE, still_pending)
    _save_json(ACCURACY_FILE, accuracy_log)
    print(f"[forecast_tracker] Verifica completata — {resolved_count} previsioni chiuse, {len(still_pending)} ancora in sospeso")


def print_accuracy_summary():
    """Riepilogo rapido da riga di comando: accuratezza media per day_offset."""
    accuracy_log = _load_json(ACCURACY_FILE, [])
    if not accuracy_log:
        print("Nessun dato di accuratezza ancora disponibile.")
        return

    by_offset = {}
    for entry in accuracy_log:
        offset = entry["day_offset_at_log_time"]
        by_offset.setdefault(offset, []).append(entry)

    for offset in sorted(by_offset.keys()):
        entries = by_offset[offset]
        pressure_errors = [e["pressure_error_hpa"] for e in entries if e["pressure_error_hpa"] is not None]
        condition_matches = [e["condition_match"] for e in entries if e["actual_condition_bucket"] is not None]

        avg_pressure_error = round(sum(pressure_errors) / len(pressure_errors), 2) if pressure_errors else None
        condition_accuracy = round(100 * sum(condition_matches) / len(condition_matches), 1) if condition_matches else None

        print(
            f"Giorno +{offset}: {len(entries)} previsioni verificate — "
            f"errore pressione medio: {avg_pressure_error} hPa — "
            f"condizione corretta: {condition_accuracy}%"
        )


if __name__ == "__main__":
    log_forecast_snapshot()
    verify_due_forecasts()
    print_accuracy_summary()