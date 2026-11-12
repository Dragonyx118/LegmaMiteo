# ===========================================
# Services: Influx Queries — LegmaMiteo Backend
# Helper condivisi, prima duplicati identici tra app/routers/data.py e
# scripts/forecast_tracker.py (e ora anche usati dal bridge MQTT).
# ===========================================

from datetime import date, timedelta
from app.config import INFLUX_BUCKET


def query_recent_values(client, station_id: str, module: str, field: str, minutes: int):
    """Ritorna lista di valori (crescente nel tempo), aggregati ogni 5 min."""
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


def query_latest_value(client, station_id: str, module: str, field: str, minutes: int):
    """Ultimo valore di un campo negli ultimi N minuti (None se assente — usato
    per fulmini e altri campi non continui)."""
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


def query_single_mean(client, station_id: str, module: str, field: str, start: str, stop: str):
    """Media di un campo in una finestra temporale specifica."""
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


def query_day_mean(client, station_id: str, module: str, field: str, day: date):
    """Media di un campo per un giorno specifico (usato in verify_due_forecasts)."""
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
