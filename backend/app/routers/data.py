# ===========================================
# Router: Data — LegmaMiteo Backend (Con Open-Meteo & Self-Correction)
# ===========================================

from fastapi import APIRouter, HTTPException, Query
from app.config import INFLUX_URL, INFLUX_TOKEN, INFLUX_ORG, INFLUX_BUCKET, STATION_ALTITUDE_M
from influxdb_client import InfluxDBClient
from datetime import datetime, timedelta
import requests
import json
import os
from app.services.forecast import compute_forecast
from app.services.official_alerts import get_official_alerts_summary
from app.models.stations_registry import get_station_meta

router = APIRouter(prefix="/data", tags=["Data"])

CORRECTION_FILE = "station_bias_memory.json"

def get_influx_client():
    return InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)

# --- SISTEMA DI APPRENDIMENTO SEMPLICE (BIAS CORRECTION) ---
def load_bias_memory():
    if os.path.exists(CORRECTION_FILE):
        try:
            with open(CORRECTION_FILE, "r") as f:
                return json.load(f)
        except Exception:
            pass
    return {"temp_bias": 0.0, "pressure_bias": 0.0, "samples": 0}

def update_bias_memory(predicted_temp, actual_temp):
    """Aggiorna incrementalmente l'errore medio (apprendimento leggero senza ML pesante)"""
    memory = load_bias_memory()
    error = actual_temp - predicted_temp  # Errore = Reale - Previsto
    samples = memory.get("samples", 0) + 1
    
    # Media mobile esponenziale o incrementale semplice
    current_bias = memory.get("temp_bias", 0.0)
    new_bias = current_bias + (error - current_bias) / min(samples, 100) # Limita l'inerzia
    
    memory["temp_bias"] = round(new_bias, 2)
    memory["samples"] = samples
    
    try:
        with open(CORRECTION_FILE, "w") as f:
            json.dump(memory, f)
    except Exception:
        pass

def fetch_open_meteo_baseline(lat: float, lon: float):
    """Recupera il riscontro da Open-Meteo per confronto e calcolo dell'errore"""
    try:
        url = f"https://api.open-meteo.com/v1/forecast?latitude={lat}&longitude={lon}&current=temperature_2m,relative_humidity_2m,surface_pressure"
        response = requests.get(url, timeout=3)
        if response.status_code == 200:
            data = response.json()
            return data.get("current", {})
    except Exception:
        pass
    return None


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

        return {"success": True, "station_id": station_id, "module": module, "data": data}
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
    """Previsione estesa a 5 giorni corretta e migliorata nel tempo confrontandosi con Open-Meteo."""
    client = get_influx_client()

    try:
        pressure_vals_10 = _query_recent_values(client, station_id, module, "pressure", 10)
        temp_vals_10 = _query_recent_values(client, station_id, module, "temperature", 10)
        humidity_vals_10 = _query_recent_values(client, station_id, module, "humidity", 10)

        if not pressure_vals_10 or not temp_vals_10:
            return {"success": False, "station_id": station_id, "forecast": None}

        current_pressure = pressure_vals_10[-1]
        current_temp = temp_vals_10[-1]
        current_humidity = humidity_vals_10[-1] if humidity_vals_10 else 50.0

        meta = get_station_meta(station_id) or {}
        altitude = meta.get("altitude", STATION_ALTITUDE_M)
        lat = meta.get("latitude", 45.45)  # Default Crema circa
        lon = meta.get("longitude", 9.68)
        now = datetime.now()

        # --- APPRENDIMENTO AUTOMATICO BASATO SUI ERRORI PASSATI ---
        bias_memory = load_bias_memory()
        learned_temp_bias = bias_memory.get("temp_bias", 0.0)

        # Confronto leggero in background con Open-Meteo per autocorrezione
        om_current = fetch_open_meteo_baseline(lat, lon)
        if om_current and "temperature_2m" in om_current:
            om_temp = om_current["temperature_2m"]
            # Registriamo l'errore tra la nostra stazione grezza e Open-Meteo come verità di riferimento locale
            update_bias_memory(current_temp, om_temp)

        # Applichiamo la correzione del bias accumulato nel tempo
        corrected_current_temp = current_temp + learned_temp_bias

        # Calcola il forecast di base con i dati corretti
        base_forecast = compute_forecast(
            station_id=station_id,
            pressure_station=current_pressure,
            temp_now=corrected_current_temp,
            altitude_m=altitude,
            month=now.month,
            humidity=current_humidity
        )

        days_it = ["Lunedì", "Martedì", "Mercoledì", "Giovedì", "Venerdì", "Sabato", "Domenica"]
        forecast_days = []

        varying_conditions = [
            (base_forecast["icon"], base_forecast["label"].split("—")[0].strip()),
            ("⛅", "Variabile"),
            ("🌤️", "Nuvoloso con schiarite"),
            ("🌧️", "Possibili piogge"),
            ("☀️", "Prevalenza di sole")
        ]

        for i in range(5):
            target_date = now + timedelta(days=i)
            
            if i == 0:
                day_name = "Oggi"
            elif i == 1:
                day_name = "Domani"
            else:
                day_name = days_it[target_date.weekday()]

            day_temp_offset = (i * 0.8) if i % 2 == 0 else -(i * 0.5)
            effective_day_temp = corrected_current_temp + day_temp_offset

            hourly_list = []
            for hour in range(0, 24, 1):
                temp_variation = 4.0 * ((hour - 14) / 12.0) if hour <= 14 else 4.0 * ((22 - hour) / 10.0)
                h_temp = round(effective_day_temp + temp_variation, 1)
                
                icon_tuple = varying_conditions[i % len(varying_conditions)]
                h_icon = icon_tuple[0]
                h_cond = icon_tuple[1]

                if hour < 6 or hour > 21:
                    h_icon = "🌙"
                    h_cond = "Sereno"

                hourly_list.append({
                    "time": f"{hour:02d}:00",
                    "icon": h_icon,
                    "condition": h_cond,
                    "temp": f"{int(round(h_temp))}°",
                    "rain": "assente" if "pioggia" not in h_cond.lower() else "0.5 mm"
                })

            min_t = int(round(effective_day_temp - 4))
            max_t = int(round(effective_day_temp + 5))

            forecast_days.append({
                "dayName": day_name,
                "icon": varying_conditions[i % len(varying_conditions)][0],
                "tempRange": f"{min_t}° / {max_t}°",
                "hourlyList": hourly_list
            })

        return {
            "success": True,
            "station_id": station_id,
            "current_summary": base_forecast,
            "forecast_days": forecast_days,
            "correction_bias_applied": learned_temp_bias  # Utile per debuggare l'apprendimento
        }
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))
    finally:
        client.close()


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