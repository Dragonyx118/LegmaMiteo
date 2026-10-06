# ===========================================
# Router: Data — LegmaMiteo Backend
# Lettura dati storici e in tempo reale
# ===========================================

from fastapi import APIRouter, HTTPException, Query
from app.config import INFLUX_URL, INFLUX_TOKEN, INFLUX_ORG, INFLUX_BUCKET
from influxdb_client import InfluxDBClient
from datetime import datetime
from app.config import STATION_ALTITUDE_M
from app.services.forecast import compute_forecast
from app.services.official_alerts import get_official_alerts_summary

router = APIRouter(prefix="/data", tags=["Data"])

def get_influx_client():
    return InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)


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

        result = compute_forecast(
            pressure_station=pressure_vals_10[-1],
            temp_now=temp_vals_10[-1],
            altitude_m=STATION_ALTITUDE_M,
            month=datetime.now().month,
            delta_3h=delta_3h,
            delta_24h_same_hour=delta_24h,
            acceleration=acceleration,
            temp_range_24h=temp_range_24h,
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