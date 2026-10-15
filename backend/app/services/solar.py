# ===========================================
# Services: Solar / Clear-sky lux estimate
# Nessun I/O: riceve data/ora/coordinate già risolte dal chiamante
# ===========================================

import math
from datetime import datetime, timezone


def solar_elevation_deg(dt_utc: datetime, latitude_deg: float, longitude_deg: float) -> float:
    """
    Calcola l'elevazione solare (gradi sopra l'orizzonte) per una data/ora UTC
    e una posizione geografica, con le formule astronomiche standard
    (approssimazione sufficiente per uso meteo, non per telescopi).
    """
    day_of_year = dt_utc.timetuple().tm_yday

    # Declinazione solare (gradi)
    declination = 23.45 * math.sin(math.radians(360 / 365 * (day_of_year - 81)))

    # Equazione del tempo (minuti), correzione per orbita ellittica e asse inclinato
    b = math.radians(360 / 365 * (day_of_year - 81))
    eq_time = 9.87 * math.sin(2 * b) - 7.53 * math.cos(b) - 1.5 * math.sin(b)

    # Ora solare locale (minuti dalla mezzanotte UTC, corretta per longitudine + eq. del tempo)
    utc_minutes = dt_utc.hour * 60 + dt_utc.minute + dt_utc.second / 60
    solar_time_minutes = utc_minutes + 4 * longitude_deg + eq_time

    # Angolo orario (gradi): 0° a mezzogiorno solare, 15°/ora
    hour_angle = (solar_time_minutes / 4) - 180

    lat_rad = math.radians(latitude_deg)
    decl_rad = math.radians(declination)
    hour_rad = math.radians(hour_angle)

    elevation_rad = math.asin(
        math.sin(lat_rad) * math.sin(decl_rad)
        + math.cos(lat_rad) * math.cos(decl_rad) * math.cos(hour_rad)
    )
    return math.degrees(elevation_rad)


# Lux massimo teorico a cielo sereno con sole allo zenit (valore empirico comune)
CLEAR_SKY_LUX_AT_ZENITH = 120_000.0

# Sotto questa elevazione il contributo diretto è trascurabile/nullo (crepuscolo/notte)
MIN_USABLE_ELEVATION_DEG = 2.0


def clear_sky_lux_estimate(
    dt_utc: datetime,
    latitude_deg: float,
    longitude_deg: float,
    month: int,
    cloud_transmittance_override: float | None = None,
) -> tuple[float, bool]:
    """
    Stima il lux atteso in condizioni di cielo sereno per la data/ora/posizione
    date. Usato come riferimento per interpretare il dato VEML7700 reale e
    dedurre la copertura nuvolosa (vedi _cloud_cover_from_lux nel forecast).

    Ritorna (lux_atteso, is_daytime).
    """
    elevation = solar_elevation_deg(dt_utc, latitude_deg, longitude_deg)

    if elevation <= MIN_USABLE_ELEVATION_DEG:
        return 0.0, False

    # Il lux scala approssimativamente col seno dell'elevazione solare
    # (legge del coseno di Lambert applicata alla superficie orizzontale)
    base_lux = CLEAR_SKY_LUX_AT_ZENITH * math.sin(math.radians(elevation))

    # Correzione stagionale leggera: in inverno l'atmosfera è spesso più tersa
    # (meno foschia/umidità), in estate più torbida (haze, afa) — fattore empirico
    seasonal_factor = {
        12: 0.95, 1: 0.95, 2: 0.97,
        3: 1.0, 4: 1.0, 5: 1.0,
        6: 0.92, 7: 0.90, 8: 0.92,
        9: 0.97, 10: 0.97, 11: 0.95,
    }.get(month, 1.0)

    lux = base_lux * seasonal_factor

    if cloud_transmittance_override is not None:
        lux *= cloud_transmittance_override

    return round(lux, 0), True