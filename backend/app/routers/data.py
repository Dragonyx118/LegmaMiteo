# ===========================================
# Router: Data — LegmaMiteo Backend
# Lettura dati storici e in tempo reale
# ===========================================

from fastapi import APIRouter, HTTPException, Query
from app.config import INFLUX_URL, INFLUX_TOKEN, INFLUX_ORG, INFLUX_BUCKET
from influxdb_client import InfluxDBClient

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
      |> range(start: -1h)
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