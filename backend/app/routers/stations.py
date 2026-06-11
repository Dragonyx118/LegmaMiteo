# ===========================================
# Router: Stations — LegmaMiteo Backend
# Gestione registrazione e info stazioni
# ===========================================

from fastapi import APIRouter, HTTPException
from app.models.schemas import StationInfo, APIResponse
from app.config import INFLUX_URL, INFLUX_TOKEN, INFLUX_ORG, INFLUX_BUCKET
from influxdb_client import InfluxDBClient
from influxdb_client.client.write_api import SYNCHRONOUS

router = APIRouter(prefix="/stations", tags=["Stations"])

def get_influx_client():
    return InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)


@router.get("/")
def list_stations():
    """Lista tutte le stazioni che hanno mandato dati."""
    client = get_influx_client()
    query_api = client.query_api()

    query = f'''
    from(bucket: "{INFLUX_BUCKET}")
      |> range(start: -30d)
      |> filter(fn: (r) => r._measurement == "weather_station")
      |> keep(columns: ["topic"])
      |> distinct(column: "topic")
    '''

    try:
        result = query_api.query(query)
        stations = []
        for table in result:
            for record in table.records:
                topic = record.values.get("topic", "")
                parts = topic.split("/")
                if len(parts) >= 2:
                    stations.append({"station_id": parts[1], "topic": topic})
        return {"success": True, "stations": stations}
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))
    finally:
        client.close()


@router.get("/{station_id}")
def get_station(station_id: str):
    """Info e ultimo dato ricevuto da una stazione specifica."""
    client = get_influx_client()
    query_api = client.query_api()

    query = f'''
    from(bucket: "{INFLUX_BUCKET}")
      |> range(start: -1h)
      |> filter(fn: (r) => r._measurement == "weather_station")
      |> filter(fn: (r) => r.topic == "station/{station_id}/base")
      |> last()
    '''

    try:
        result = query_api.query(query)
        data = {}
        for table in result:
            for record in table.records:
                data[record.get_field()] = record.get_value()

        if not data:
            raise HTTPException(status_code=404, detail=f"Station {station_id} not found or no recent data")

        return {"success": True, "station_id": station_id, "last_data": data}
    except HTTPException:
        raise
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))
    finally:
        client.close()