# ===========================================
# Script: write_sun_times.py — LegmaMiteo Backend
# Scrive alba/tramonto/crepuscoli del giorno su InfluxDB.
# Da lanciare 1 volta al giorno (es. ogni notte a mezzanotte) via cron.
# ===========================================

from datetime import datetime, UTC
from influxdb_client import InfluxDBClient, Point
from influxdb_client.client.write_api import SYNCHRONOUS

from app.config import INFLUX_URL, INFLUX_TOKEN, INFLUX_ORG, INFLUX_BUCKET
from app.models.stations_registry import load_registry
from app.services.astronomy import get_sun_times


def write_sun_times_for_all_stations():
    client = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
    write_api = client.write_api(write_options=SYNCHRONOUS)

    registry = load_registry()

    for station_id, meta in registry.items():
        lat = meta.get("latitude")
        lon = meta.get("longitude")
        if lat is None or lon is None:
            continue

        sun_info = get_sun_times(lat, lon)
        if "error" in sun_info:
            continue

        # Un singolo punto al giorno, misurazione separata da "weather_station"
        # per non mischiarla con i dati dei sensori
        point = (
            Point("sun_times")
            .tag("station_id", station_id)
            .field("sunrise_minutes", _to_minutes(sun_info["sunrise"]))
            .field("sunset_minutes", _to_minutes(sun_info["sunset"]))
            .field("day_length_minutes", sun_info["day_length_minutes"])
            .time(datetime.now(UTC))
        )
        write_api.write(bucket=INFLUX_BUCKET, record=point)
        print(f"[{station_id}] sunrise={sun_info['sunrise']} sunset={sun_info['sunset']}")

    client.close()


def _to_minutes(iso_time: str) -> int:
    """Converte un orario ISO (es. '2026-11-01T07:12:00+01:00') in minuti dalla
    mezzanotte locale, cosi' Grafana puo' disegnarlo come numero su un grafico
    nello stesso pannello dove mostri il lux (che e' anch'esso un numero semplice)."""
    dt = datetime.fromisoformat(iso_time)
    return dt.hour * 60 + dt.minute


if __name__ == "__main__":
    write_sun_times_for_all_stations()