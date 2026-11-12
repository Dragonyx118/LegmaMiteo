# ===========================================
# Router: Data — LegmaMiteo Backend (Con Open-Meteo & Self-Correction)
# ===========================================

from fastapi import APIRouter, HTTPException, Query
from app.config import INFLUX_URL, INFLUX_TOKEN, INFLUX_ORG, INFLUX_BUCKET, STATION_ALTITUDE_M
from influxdb_client import InfluxDBClient
from datetime import datetime, timedelta, timezone
import requests
import json
import os
from app.services.forecast import compute_forecast
from app.services.official_alerts import get_official_alerts_summary
from app.models.stations_registry import get_station_meta
from app.services.astronomy import get_sun_times, is_daylight
from app.services.influx_queries import query_recent_values, query_day_mean, query_latest_value

router = APIRouter(prefix="/data", tags=["Data"])


def compute_feels_like(temp_c, humidity, wind_speed_ms):
    """Temperatura percepita — stessa formula usata nel pannello Grafana e nel bot Telegram.
    Heat Index (Rothfusz) sopra 27°C, Wind Chill (NWS/EC) sotto 10°C con vento >= 4.8 km/h,
    altrimenti la temperatura grezza. wind_speed_ms in m/s, convertito in km/h per il wind chill."""
    if temp_c is None or humidity is None:
        return None

    wind_kmh = (wind_speed_ms * 3.6) if wind_speed_ms is not None else 0.0

    if temp_c >= 27.0:
        t, h = temp_c, humidity
        return (
            -8.78469475556
            + 1.61139411 * t
            + 2.33854883889 * h
            - 0.14611605 * t * h
            - 0.012308094 * t * t
            - 0.0164248277778 * h * h
            + 0.002211732 * t * t * h
            + 0.00072546 * t * h * h
            - 0.000003582 * t * t * h * h
        )
    elif temp_c <= 10.0 and wind_kmh >= 4.8:
        v = wind_kmh ** 0.16
        return 13.12 + 0.6215 * temp_c - 11.37 * v + 0.3965 * temp_c * v
    else:
        return temp_c

def get_influx_client():
    return InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)

# --- BIAS CORRECTION ---
# L'aggiornamento del bias (confronto con Open-Meteo) ora gira nello scheduler
# giornaliero (scripts/forecast_tracker.py: update_bias_from_openmeteo), non più
# ad ogni chiamata di questo endpoint — prima restava fermo per giorni se l'app
# non apriva la schermata dettagli. Qui lo leggiamo soltanto, in sola lettura.
from scripts.forecast_tracker import load_bias_memory
from app.services import openmeteo


@router.get("/{station_id}/latest")
def get_latest(station_id: str, module: str = "base"):
    """Ultimo dato ricevuto da una stazione per un modulo specifico."""
    client = get_influx_client()
    query_api = client.query_api()

    query = f'''
    from(bucket: "{INFLUX_BUCKET}")
      |> range(start: -30d)
      |> filter(fn: (r) => r._measurement == "weather_station")
      |> filter(fn: (r) => r.topic == "station/{station_id}/{module}")
      |> last()
    '''

    try:
        result = query_api.query(query)
        data = {}
        for table in result:
            for record in table.records:
                data[record.get_field()] = record.get_value()

        if not data:
            raise HTTPException(status_code=404, detail="No recent data found")

        # --- Temperatura percepita (solo se ci sono temperatura e umidità, es. modulo "base") ---
        if "temperature" in data and "humidity" in data:
            data["feels_like"] = compute_feels_like(
                data.get("temperature"), data.get("humidity"), data.get("wind_speed")
            )

        # --- Alba/tramonto astronomici + controllo coerenza col sensore lux ---
        meta = get_station_meta(station_id) or {}
        lat = meta.get("latitude")
        lon = meta.get("longitude")

        sun_info = get_sun_times(lat, lon)

        sensor_warning = None
        if "lux" in data:
            daylight = is_daylight(lat, lon)
            if daylight and data["lux"] < 50:
                sensor_warning = "⚠️ Lux basso in pieno giorno — controllare sensore VEML7700"
            elif not daylight and data["lux"] > 500:
                sensor_warning = "⚠️ Lux alto di notte — controllare sensore VEML7700"

        return {
            "success": True,
            "station_id": station_id,
            "module": module,
            "data": data,
            "sun": sun_info,
            "sensor_warning": sensor_warning,
        }
    except HTTPException:
        raise
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))
    finally:
        client.close()


@router.get("/{station_id}/history")
def get_history(
    station_id: str,
    module: str = "base",
    field: str = "temperature",
    range_hours: int = Query(default=24, ge=1, le=720)
):
    """Storico di un campo specifico per una stazione."""
    client = get_influx_client()
    query_api = client.query_api()

    query = f'''
    from(bucket: "{INFLUX_BUCKET}")
      |> range(start: -{range_hours}h)
      |> filter(fn: (r) => r._measurement == "weather_station")
      |> filter(fn: (r) => r.topic == "station/{station_id}/{module}")
      |> filter(fn: (r) => r._field == "{field}")
      |> aggregateWindow(every: 5m, fn: mean, createEmpty: false)
    '''

    try:
        result = query_api.query(query)
        points = []
        for table in result:
            for record in table.records:
                points.append({
                    "time": record.get_time().isoformat(),
                    "value": record.get_value()
                })

        return {
            "success": True,
            "station_id": station_id,
            "module": module,
            "field": field,
            "range_hours": range_hours,
            "points": points
        }
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))
    finally:
        client.close()


@router.get("/{station_id}/alerts")
def get_alerts(station_id: str):
    """Controlla condizioni critiche per una stazione."""
    client = get_influx_client()
    query_api = client.query_api()

    alerts = []

    checks = {
        "pm25": ("mod-air", 55.0, "PM2.5 sopra soglia WHO"),
        "co2": ("mod-air", 1000.0, "CO2 elevata"),
        "lightning_distance": ("mod-storm", 10.0, "Fulmine a meno di 10km"),
    }

    try:
        for field, (module, threshold, message) in checks.items():
            query = f'''
            from(bucket: "{INFLUX_BUCKET}")
              |> range(start: -15m)
              |> filter(fn: (r) => r._measurement == "weather_station")
              |> filter(fn: (r) => r.topic == "station/{station_id}/{module}")
              |> filter(fn: (r) => r._field == "{field}")
              |> last()
            '''
            result = query_api.query(query)
            for table in result:
                for record in table.records:
                    value = record.get_value()
                    if value is not None and value >= threshold:
                        alerts.append({
                            "field": field,
                            "value": value,
                            "threshold": threshold,
                            "message": message,
                            "severity": "critical" if value >= threshold * 1.5 else "warning"
                        })

        return {"success": True, "station_id": station_id, "alerts": alerts}
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))
    finally:
        client.close()
    
@router.get("/{station_id}/extended-forecast")
def get_extended_forecast(station_id: str, module: str = "base"):
    """
    Previsione estesa a 5 giorni: usa il calcolo fisico reale di compute_extended_forecast
    (pressione + trend + umidità + fulmini), corretto nel tempo confrontandosi con Open-Meteo.
    Ogni giorno è una proiezione per persistenza smorzata, non più una rotazione di icone finte.
    """
    client = get_influx_client()

    try:
        pressure_vals_10 = _query_recent_values(client, station_id, module, "pressure", 10)
        temp_vals_10 = _query_recent_values(client, station_id, module, "temperature", 10)
        humidity_vals_10 = _query_recent_values(client, station_id, module, "humidity", 10)

        if not pressure_vals_10 or not temp_vals_10:
            return {"success": False, "station_id": station_id, "forecast": None}

        current_pressure = pressure_vals_10[-1]
        current_temp = temp_vals_10[-1]
        current_humidity = humidity_vals_10[-1] if humidity_vals_10 else None

        # NEW — vento (per temperatura percepita), lux (per copertura nuvolosa) e
        # fulmini (per override temporale), stesso pattern usato in /forecast
        current_wind_speed = _query_latest_value(client, station_id, module, "wind_speed", 10)
        current_lux = _query_latest_value(client, station_id, module, "lux", 10)
        lightning_distance_km = _query_latest_value(client, station_id, module, "lightning_distance_km", 30)
        lightning_strikes_last_hour = _query_latest_value(client, station_id, module, "lightning_strikes_last_hour", 60)

        # NEW — trend di pressione (mancava: senza questo il forecast risultava sempre "stabile")
        pressure_3h = _query_recent_values(client, station_id, module, "pressure", 180)
        delta_3h = (pressure_3h[-1] - pressure_3h[0]) if len(pressure_3h) >= 2 else None

        meta = get_station_meta(station_id) or {}
        altitude = meta.get("altitude", STATION_ALTITUDE_M)
        lat = meta.get("latitude", 45.45)  # Default Crema circa
        lon = meta.get("longitude", 9.68)
        now = datetime.now()

        # --- Bias letto dallo scheduler giornaliero (temp + pressione + umidità) ---
        bias_memory = load_bias_memory()
        corrected_current_temp = current_temp + bias_memory.get("temp_bias", 0.0)
        corrected_current_pressure = current_pressure + bias_memory.get("pressure_bias", 0.0)
        corrected_current_humidity = (
            current_humidity + bias_memory.get("humidity_bias", 0.0) if current_humidity is not None else None
        )

        # NEW — temperatura percepita sul dato corretto
        feels_like = compute_feels_like(corrected_current_temp, corrected_current_humidity, current_wind_speed)

        # Calcola il forecast reale (oggi + proiezione a 5 giorni) con tutti i dati disponibili
        base_forecast = compute_forecast(
            station_id=station_id,
            pressure_station=corrected_current_pressure,
            temp_now=corrected_current_temp,
            altitude_m=altitude,
            month=now.month,
            delta_3h=delta_3h,
            humidity=corrected_current_humidity,
            lux=current_lux,
            dt_utc=datetime.now(timezone.utc),
            latitude_deg=lat,
            longitude_deg=lon,
            lightning_distance_km=lightning_distance_km,
            lightning_strikes_last_hour=lightning_strikes_last_hour,
        )
        base_forecast["feels_like"] = round(feels_like, 1) if feels_like is not None else None

        days_it = ["Lunedì", "Martedì", "Mercoledì", "Giovedì", "Venerdì", "Sabato", "Domenica"]

        # Giorno 0 ("Oggi") usa direttamente il risultato di compute_forecast
        today_range = base_forecast["temp_range_estimate"]
        forecast_days = [{
            "dayName": "Oggi",
            "icon": base_forecast["icon"],
            "condition": base_forecast["label"],
            "tempRange": f"{int(round(corrected_current_temp - today_range))}° / {int(round(corrected_current_temp + today_range * 1.1))}°",
            "confidence": 1.0,
            "hourlyList": _build_hourly_preview(corrected_current_temp, base_forecast["icon"], base_forecast["condition"]),
        }]

        # Giorni 1-5 dalla proiezione fisica reale (extended_forecast), non più finta.
        # Dal giorno 3 in poi lo smorzamento locale ha ormai poco segnale (vedi
        # forecast.py), quindi si usa direttamente Open-Meteo: condizione reale e
        # temp min/max reali, non più un'euristica locale sulla pressione proiettata.
        for day_data in base_forecast["extended_forecast"]:
            offset = day_data["day_offset"]
            target_date = now + timedelta(days=offset)
            day_name = "Domani" if offset == 1 else days_it[target_date.weekday()]

            om_daily = openmeteo.fetch_daily_forecast(lat, lon, target_date.date()) if offset >= 3 else None

            if om_daily:
                icon = om_daily["icon"]
                condition_label = om_daily["label"]
                temp_min, temp_max = om_daily["temp_min"], om_daily["temp_max"]
                confidence = 0.6  # previsione NWP reale, più affidabile della nostra persistenza smorzata
                hourly_icon, hourly_condition = icon, om_daily["condition"]
                effective_day_temp = (temp_min + temp_max) / 2
            else:
                # Fallback: stima locale (giorni 1-2, o se Open-Meteo non risponde)
                pressure_delta_from_today = day_data["pressure_slp_estimate"] - base_forecast["pressure_slp"]
                effective_day_temp = corrected_current_temp + pressure_delta_from_today * 0.3
                day_range = day_data["temp_range_estimate"]
                temp_min, temp_max = effective_day_temp - day_range, effective_day_temp + day_range * 1.1
                icon, condition_label = day_data["icon"], day_data["label"]
                confidence = day_data["confidence"]
                hourly_icon, hourly_condition = icon, day_data["condition"]

            forecast_days.append({
                "dayName": day_name,
                "icon": icon,
                "condition": condition_label,
                "tempRange": f"{int(round(temp_min))}° / {int(round(temp_max))}°",
                "confidence": confidence,
                "source": "open-meteo" if om_daily else "local",
                "hourlyList": _build_hourly_preview(effective_day_temp, hourly_icon, hourly_condition),
            })

        return {
            "success": True,
            "station_id": station_id,
            "current_summary": base_forecast,
            "forecast_days": forecast_days,
            "bias_applied": bias_memory  # Utile per debuggare l'apprendimento (temp/pressione/umidità)
        }
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))
    finally:
        client.close()


def _build_hourly_preview(day_temp: float, icon: str, condition: str):
    """
    Curva oraria semplificata attorno alla temperatura stimata del giorno (picco
    verso le 14, minimo notturno), con l'icona/condizione del giorno applicata a
    tutte le ore diurne e la luna di notte. Non è un modello orario reale — serve
    solo a dare una preview coerente nell'app, non più icone casuali per ora.
    """
    hourly_list = []
    is_rain_like = condition in ("pioggia", "temporale", "temporale_in_corso", "temporale_avvicinamento")
    for hour in range(0, 24):
        temp_variation = 4.0 * ((hour - 14) / 12.0) if hour <= 14 else 4.0 * ((22 - hour) / 10.0)
        h_temp = round(day_temp + temp_variation, 1)

        h_icon = icon
        h_cond = condition
        if hour < 6 or hour > 21:
            h_icon = "🌙" if icon in ("☀️", "🌤️") else icon
            h_cond = "sereno" if icon in ("☀️", "🌤️") else condition

        hourly_list.append({
            "time": f"{hour:02d}:00",
            "icon": h_icon,
            "condition": h_cond,
            "temp": f"{int(round(h_temp))}°",
            "rain": "0.5 mm" if is_rain_like else "assente",
        })
    return hourly_list


def _query_recent_values(client, station_id: str, module: str, field: str, minutes: int):
    """Ritorna lista di valori (crescente nel tempo) per un campo, aggregati ogni 5 min."""
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


def _query_latest_value(client, station_id: str, module: str, field: str, minutes: int):
    """Ultimo valore di un campo negli ultimi N minuti (None se assente — usato per lightning,
    che non è presente in ogni ciclo come pressure/temperature)."""
    query_api = client.query_api()
    query = f'''
    from(bucket: "{INFLUX_BUCKET}")
      |> range(start: -{minutes}m)
      |> filter(fn: (r) => r._measurement == "weather_station")
      |> filter(fn: (r) => r.topic == "station/{station_id}/{module}")
      |> filter(fn: (r) => r._field == "{field}")
      |> last()
    '''
    result = query_api.query(query)
    for table in result:
        for record in table.records:
            return record.get_value()
    return None


def _query_single_mean(client, station_id: str, module: str, field: str, start: str, stop: str):
    """Media di un campo in una finestra temporale specifica (per il confronto 24h)."""
    query_api = client.query_api()
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


@router.get("/{station_id}/forecast")
def get_forecast(station_id: str, module: str = "base"):
    """Previsione barometrica a breve termine basata su trend di pressione."""
    client = get_influx_client()

    try:
        pressure_vals_10 = _query_recent_values(client, station_id, module, "pressure", 10)
        temp_vals_10 = _query_recent_values(client, station_id, module, "temperature", 10)

        if not pressure_vals_10 or not temp_vals_10:
            return {"success": False, "station_id": station_id, "forecast": None}

        pressure_1h = _query_recent_values(client, station_id, module, "pressure", 60)
        pressure_3h = _query_recent_values(client, station_id, module, "pressure", 180)
        pressure_6h = _query_recent_values(client, station_id, module, "pressure", 360)

        delta_1h = (pressure_1h[-1] - pressure_1h[0]) if len(pressure_1h) >= 2 else None
        delta_3h = (pressure_3h[-1] - pressure_3h[0]) if len(pressure_3h) >= 2 else None
        delta_6h = (pressure_6h[-1] - pressure_6h[0]) if len(pressure_6h) >= 2 else None

        acceleration = None
        if delta_1h is not None and delta_3h is not None:
            delta_prior_2h = delta_3h - delta_1h
            acceleration = delta_1h - (delta_prior_2h / 2)

        pressure_24h_ago = _query_single_mean(client, station_id, module, "pressure", "-25h", "-23h")
        delta_24h = (pressure_vals_10[-1] - pressure_24h_ago) if pressure_24h_ago is not None else None

        temp_vals_24h = _query_recent_values(client, station_id, module, "temperature", 24 * 60)
        temp_range_24h = (max(temp_vals_24h) - min(temp_vals_24h)) if temp_vals_24h else None

        # NEW — dati AS3935 (lightning), ultimo valore noto nella finestra recente.
        # Nessun aggregateWindow qui: a differenza di pressure/temp, il fulmine
        # non è continuo, quindi ha senso solo "l'ultimo rilevato", non una media.
        lightning_distance_km = _query_latest_value(client, station_id, module, "lightning_distance_km", 30)
        lightning_strikes_last_hour = _query_latest_value(client, station_id, module, "lightning_strikes_last_hour", 60)

        # NEW — coordinate/altitudine da registry invece che costanti fisse,
        # con fallback a STATION_ALTITUDE_M se la stazione non è ancora nel registry
        meta = get_station_meta(station_id) or {}
        altitude = meta.get("altitude", STATION_ALTITUDE_M)

        result = compute_forecast(
            station_id=station_id,
            pressure_station=pressure_vals_10[-1],
            temp_now=temp_vals_10[-1],
            altitude_m=altitude,
            month=datetime.now().month,
            delta_3h=delta_3h,
            delta_24h_same_hour=delta_24h,
            acceleration=acceleration,
            temp_range_24h=temp_range_24h,
            lightning_distance_km=lightning_distance_km,
            lightning_strikes_last_hour=lightning_strikes_last_hour,
        )
        result["delta_1h"] = delta_1h
        result["delta_6h"] = delta_6h

        return {"success": True, "station_id": station_id, "forecast": result}
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))
    finally:
        client.close()

@router.get("/{station_id}/official-alerts")
def get_official_alerts(station_id: str, region: str = "Lombardia"):
    """Allerte ufficiali attive (METEOALARM + Protezione Civile) per la regione della stazione."""
    try:
        alerts = get_official_alerts_summary(region)
        return {"success": True, "station_id": station_id, "alerts": alerts}
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))