# ===========================================
# Services: Forecast — LegmaMiteo Backend
# Logica pura di previsione barometrica (nessun I/O di rete/DB,
# ma importa il calcolo solare che è anch'esso puro)
# ===========================================

from datetime import datetime, timezone
from .solar import clear_sky_lux_estimate

STATION_LAT = 45.3667  # coordinate reali della stazione
STATION_LON = 9.6833


def sea_level_pressure(station_pressure_hpa: float, altitude_m: float, temp_c: float) -> float:
    """Corregge la pressione stazione al livello del mare (formula barometrica standard)."""
    return station_pressure_hpa * (1 - (0.0065 * altitude_m) / (temp_c + 0.0065 * altitude_m + 273.15)) ** -5.257


SEASONAL_THRESHOLDS = {
    "inverno":   {"alta": 1025, "media": 1015, "bassa": 1000},
    "primavera": {"alta": 1023, "media": 1014, "bassa": 1000},
    "estate":    {"alta": 1020, "media": 1012, "bassa": 1003},
    "autunno":   {"alta": 1023, "media": 1014, "bassa": 1000},
}

# Umidità relativa (%)
HUMIDITY_FOG_THRESHOLD = 90.0
HUMIDITY_RAIN_BIAS_THRESHOLD = 75.0
HUMIDITY_DRY_THRESHOLD = 40.0

# Fulmini (AS3935: stima distanza in km, 1-40km, "disturber" già scartati a monte)
LIGHTNING_NEARBY_KM = 10.0
LIGHTNING_APPROACHING_KM = 20.0

# Luce (VEML7700, lux) — rapporto osservato/atteso
LUX_CLEAR_RATIO = 0.7
LUX_OVERCAST_RATIO = 0.25


def get_season(month: int) -> str:
    if month in (12, 1, 2):
        return "inverno"
    elif month in (3, 4, 5):
        return "primavera"
    elif month in (6, 7, 8):
        return "estate"
    else:
        return "autunno"


def _trend_from_delta(d3: float) -> tuple[str, str]:
    if d3 >= 1.6:
        return "in rapida salita", "⬆️"
    elif d3 >= 0.5:
        return "in salita", "↗️"
    elif d3 <= -1.6:
        return "in rapido calo", "⬇️"
    elif d3 <= -0.5:
        return "in calo", "↘️"
    return "stabile", "➡️"


def _cloud_cover_from_lux(lux: float | None, expected_lux: float | None, is_daytime: bool) -> str | None:
    """Stima qualitativa copertura nuvolosa da luminosità reale vs attesa. None se dato mancante/notte."""
    if not is_daytime or lux is None or expected_lux is None or expected_lux <= 0:
        return None
    ratio = lux / expected_lux
    if ratio >= LUX_CLEAR_RATIO:
        return "sereno"
    elif ratio >= LUX_OVERCAST_RATIO:
        return "parziale"
    return "coperto"


def _classify_condition(
    pressure_slp: float,
    t: dict,
    d3: float,
    humidity: float | None,
    temp_range_24h: float | None,
    season: str,
    cloud_cover: str | None,
) -> tuple[str, str, str]:
    """Classifica condizione/etichetta/icona usando pressione, trend, umidità e (se diurno) luce."""
    humid_known = humidity is not None

    if pressure_slp >= t["alta"]:
        fog_by_temp = temp_range_24h is not None and temp_range_24h < 4
        fog_by_humidity = humid_known and humidity >= HUMIDITY_FOG_THRESHOLD
        if (fog_by_temp or fog_by_humidity) and season in ("inverno", "autunno") and d3 >= -0.5:
            return "nebbia", "Alta pressione stabile — possibile nebbia/foschia in pianura", "🌫️"
        if cloud_cover == "coperto":
            return "variabile", "Pressione alta ma cielo coperto (strato basso/nebbia alta)", "🌥️"
        if d3 >= 0.5:
            return "sereno", "Bel tempo, cielo sereno in consolidamento", "☀️"
        if d3 <= -0.5:
            return "sereno_variabile", "Bel tempo ma in graduale peggioramento", "🌤️"
        return "sereno", "Bel tempo stabile", "☀️"

    elif pressure_slp >= t["media"]:
        humid_rain_bias = humid_known and humidity >= HUMIDITY_RAIN_BIAS_THRESHOLD
        humid_dry_bias = humid_known and humidity <= HUMIDITY_DRY_THRESHOLD
        if cloud_cover == "sereno" and not humid_rain_bias:
            return "sereno_variabile", "Cielo prevalentemente sereno, pressione media", "🌤️"
        if d3 >= 0.5:
            if humid_dry_bias:
                return "sereno_variabile", "Tempo in miglioramento, aria secca", "🌤️"
            return "variabile", "Tempo in miglioramento, variabile", "⛅"
        if d3 <= -0.5 or humid_rain_bias or cloud_cover == "coperto":
            return "variabile", "Tempo variabile, possibile peggioramento", "🌥️"
        return "variabile", "Tempo variabile stabile", "⛅"

    elif pressure_slp >= t["bassa"]:
        humid_rain_bias = humid_known and humidity >= HUMIDITY_RAIN_BIAS_THRESHOLD
        if d3 >= 0.5 and not humid_rain_bias and cloud_cover != "coperto":
            return "instabile", "Instabile ma in miglioramento", "🌥️"
        if d3 <= -1.0 or (humid_rain_bias and d3 <= 0) or cloud_cover == "coperto":
            return "pioggia", "Instabile, possibili rovesci in arrivo", "🌧️"
        return "nuvoloso", "Nuvoloso, tempo incerto", "☁️"

    else:
        if d3 <= -0.5:
            return "temporale", "Perturbato, condizioni in peggioramento", "⛈️"
        return "pioggia", "Perturbato, piogge probabili", "🌧️"


def _apply_lightning_override(
    condition: str, label: str, icon: str,
    lightning_distance_km: float | None,
    lightning_strikes_last_hour: int | None,
) -> tuple[str, str, str, bool]:
    """Il fulmine è un dato diretto: se l'AS3935 rileva scariche vicine, scavalca
    la classificazione basata su pressione/umidità/luce. Ritorna anche `lightning_alert`."""
    if lightning_distance_km is None:
        return condition, label, icon, False

    if lightning_distance_km <= LIGHTNING_NEARBY_KM:
        strikes_txt = f" ({lightning_strikes_last_hour} scariche nell'ultima ora)" if lightning_strikes_last_hour else ""
        return (
            "temporale_in_corso",
            f"⚠️ Attività elettrica in corso entro {lightning_distance_km:.0f} km{strikes_txt}",
            "🌩️",
            True,
        )
    elif lightning_distance_km <= LIGHTNING_APPROACHING_KM:
        return (
            "temporale_avvicinamento",
            f"Temporale in avvicinamento, fulmini rilevati a ~{lightning_distance_km:.0f} km",
            "⛈️",
            True,
        )
    return condition, label, icon, False


def compute_forecast(
    pressure_station: float,
    temp_now: float,
    altitude_m: float,
    month: int,
    delta_3h: float | None = None,
    delta_24h_same_hour: float | None = None,
    acceleration: float | None = None,
    temp_range_24h: float | None = None,
    humidity: float | None = None,
    lux: float | None = None,                         # VEML7700, lettura grezza
    dt_utc: datetime | None = None,                    # timestamp della lettura, per calcolo solare
    latitude_deg: float = STATION_LAT,
    longitude_deg: float = STATION_LON,
    lightning_distance_km: float | None = None,        # AS3935
    lightning_strikes_last_hour: int | None = None,    # AS3935
) -> dict:
    """
    Calcola la previsione barometrica a breve termine (oggi) più un'estrapolazione
    a 5 giorni. Nessun I/O di rete/DB: riceve i dati già aggregati dal chiamante.
    Il calcolo del lux atteso a cielo sereno è fatto qui internamente (via solar.py),
    quindi il chiamante passa solo `lux` grezzo e `dt_utc`, non più valori precalcolati.
    """
    pressure_slp = sea_level_pressure(pressure_station, altitude_m, temp_now)
    season = get_season(month)
    t = SEASONAL_THRESHOLDS[season]

    d3 = delta_3h if delta_3h is not None else 0.0
    trend, trend_icon = _trend_from_delta(d3)

    expected_lux, is_daytime = (None, True)
    if lux is not None and dt_utc is not None:
        expected_lux, is_daytime = clear_sky_lux_estimate(
            dt_utc=dt_utc, latitude_deg=latitude_deg, longitude_deg=longitude_deg, month=month
        )

    cloud_cover = _cloud_cover_from_lux(lux, expected_lux, is_daytime)

    condition, label, icon = _classify_condition(
        pressure_slp, t, d3, humidity, temp_range_24h, season, cloud_cover
    )

    condition, label, icon, lightning_alert = _apply_lightning_override(
        condition, label, icon, lightning_distance_km, lightning_strikes_last_hour
    )

    worsening = acceleration is not None and acceleration <= -0.5
    improving = acceleration is not None and acceleration >= 0.5

    extended = compute_extended_forecast(
        pressure_slp=pressure_slp,
        d3=d3,
        acceleration=acceleration,
        humidity=humidity,
        season=season,
        month=month,
        recent_lightning_activity=lightning_alert,
    )

    return {
        "condition": condition,
        "label": label,
        "icon": icon,
        "pressure_slp": round(pressure_slp, 1),
        "season": season,
        "trend": trend,
        "trend_icon": trend_icon,
        "delta_3h": delta_3h,
        "delta_24h": delta_24h_same_hour,
        "humidity": humidity,
        "lux": lux,
        "expected_lux": expected_lux,
        "cloud_cover": cloud_cover,
        "lightning_distance_km": lightning_distance_km,
        "lightning_strikes_last_hour": lightning_strikes_last_hour,
        "lightning_alert": lightning_alert,
        "worsening": worsening,
        "improving": improving,
        "extended_forecast": extended,
    }


# ===========================================
# Previsione estesa (5 giorni, incertezza crescente)
# ===========================================

DAILY_DAMPING_FACTOR = 0.55
CONFIDENCE_DECAY = [1.0, 0.75, 0.6, 0.45, 0.35, 0.3]


def compute_extended_forecast(
    pressure_slp: float,
    d3: float,
    acceleration: float | None,
    humidity: float | None,
    season: str,
    month: int,
    days: int = 5,
    recent_lightning_activity: bool = False,
) -> list[dict]:
    """
    Estrapola una previsione giorno-per-giorno fino a `days` giorni, per
    persistenza smorzata della pressione + decadimento del trend/accelerazione.
    Non è un modello fisico: indica una tendenza, non un valore esatto.
    """
    t = SEASONAL_THRESHOLDS[season]
    accel = acceleration if acceleration is not None else 0.0

    results = []
    projected_slp = pressure_slp
    projected_d3 = d3
    projected_humidity = humidity

    for day in range(1, days + 1):
        damping = DAILY_DAMPING_FACTOR ** day

        daily_drift = (d3 * 2.0 + accel * 4.0) * damping
        projected_slp = projected_slp + daily_drift
        projected_d3 = d3 * damping

        if projected_humidity is not None:
            seasonal_humidity_mean = 70.0 if season in ("autunno", "inverno") else 55.0
            projected_humidity = projected_humidity + (seasonal_humidity_mean - projected_humidity) * 0.15

        condition, label, icon = _classify_condition(
            projected_slp, t, projected_d3, projected_humidity, None, season, cloud_cover=None
        )

        if recent_lightning_activity and day == 1 and condition not in ("temporale", "pioggia"):
            condition, label, icon = "variabile", "Possibile instabilità residua dopo attività elettrica recente", "🌥️"

        confidence = CONFIDENCE_DECAY[min(day, len(CONFIDENCE_DECAY) - 1)]

        results.append({
            "day_offset": day,
            "condition": condition,
            "label": label,
            "icon": icon,
            "pressure_slp_estimate": round(projected_slp, 1),
            "confidence": confidence,
        })

    return results