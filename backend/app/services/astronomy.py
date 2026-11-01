# ===========================================
# Service: Astronomy — LegmaMiteo Backend
# Calcolo alba/tramonto/crepuscolo da coordinate stazione
# Richiede: pip install astral
# ===========================================

from datetime import date, datetime
from zoneinfo import ZoneInfo
from astral import LocationInfo
from astral.sun import sun


# Fallback usato solo se una stazione non ha lat/lon nel registry
# (valori approssimativi per Campagnola Cremasca, CR — verifica/correggi
# con le coordinate esatte già presenti nel tuo stations_registry)
DEFAULT_LATITUDE = 45.326
DEFAULT_LONGITUDE = 9.699
DEFAULT_TIMEZONE = "Europe/Rome"


def get_sun_times(latitude: float | None, longitude: float | None,
                   when: date | None = None, tz_name: str = DEFAULT_TIMEZONE) -> dict:
    """
    Calcola alba, tramonto e crepuscoli astronomici per una coppia di coordinate.

    Args:
        latitude, longitude: coordinate della stazione. Se None, usa il fallback.
        when: giorno per cui calcolare (default: oggi).
        tz_name: timezone IANA per la conversione degli orari (default Europe/Rome).

    Returns:
        dict con orari ISO 8601 locali, oppure dict con "error" se fallisce.
    """
    lat = latitude if latitude is not None else DEFAULT_LATITUDE
    lon = longitude if longitude is not None else DEFAULT_LONGITUDE
    target_day = when or date.today()
    tz = ZoneInfo(tz_name)

    location = LocationInfo(latitude=lat, longitude=lon, timezone=tz_name)

    try:
        s = sun(location.observer, date=target_day, tzinfo=tz)
    except Exception as e:
        # Puo' succedere a latitudini estreme (sole di mezzanotte / notte polare) — non
        # un caso realistico per LegmaMiteo, ma gestito per sicurezza
        return {"error": f"Impossibile calcolare alba/tramonto: {e}"}

    return {
        "dawn": s["dawn"].isoformat(),          # alba civile (inizio luce)
        "sunrise": s["sunrise"].isoformat(),    # sorgere del sole
        "noon": s["noon"].isoformat(),          # mezzogiorno solare
        "sunset": s["sunset"].isoformat(),      # tramonto
        "dusk": s["dusk"].isoformat(),          # crepuscolo civile (fine luce)
        "day_length_minutes": round((s["sunset"] - s["sunrise"]).total_seconds() / 60),
    }


def is_daylight(latitude: float | None, longitude: float | None,
                 tz_name: str = DEFAULT_TIMEZONE) -> bool:
    """
    True se in questo momento, per quelle coordinate, il sole e' sopra l'orizzonte
    (tra sunrise e sunset). Utile per confrontare col lux reale e rilevare
    anomalie del sensore VEML7700 (es. ostruito, sporco, orientato male).
    """
    tz = ZoneInfo(tz_name)
    now = datetime.now(tz)
    times = get_sun_times(latitude, longitude, when=now.date(), tz_name=tz_name)

    if "error" in times:
        return True  # fallback prudente: non segnalare anomalie se il calcolo fallisce

    sunrise = datetime.fromisoformat(times["sunrise"])
    sunset = datetime.fromisoformat(times["sunset"])
    return sunrise <= now <= sunset