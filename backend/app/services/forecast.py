# ===========================================
# Services: Forecast — LegmaMiteo Backend
# Logica pura di previsione barometrica (nessun I/O)
# ===========================================


def sea_level_pressure(station_pressure_hpa: float, altitude_m: float, temp_c: float) -> float:
    """Corregge la pressione stazione al livello del mare (formula barometrica standard)."""
    return station_pressure_hpa * (1 - (0.0065 * altitude_m) / (temp_c + 0.0065 * altitude_m + 273.15)) ** -5.257


SEASONAL_THRESHOLDS = {
    "inverno":   {"alta": 1025, "media": 1015, "bassa": 1000},
    "primavera": {"alta": 1023, "media": 1014, "bassa": 1000},
    "estate":    {"alta": 1020, "media": 1012, "bassa": 1003},
    "autunno":   {"alta": 1023, "media": 1014, "bassa": 1000},
}


def get_season(month: int) -> str:
    if month in (12, 1, 2):
        return "inverno"
    elif month in (3, 4, 5):
        return "primavera"
    elif month in (6, 7, 8):
        return "estate"
    else:
        return "autunno"


def compute_forecast(
    pressure_station: float,
    temp_now: float,
    altitude_m: float,
    month: int,
    delta_3h: float | None = None,
    delta_24h_same_hour: float | None = None,
    acceleration: float | None = None,
    temp_range_24h: float | None = None,
) -> dict:
    """
    Calcola la previsione barometrica a breve termine.
    Nessun I/O: riceve i dati già aggregati dal chiamante.
    """
    pressure_slp = sea_level_pressure(pressure_station, altitude_m, temp_now)
    season = get_season(month)
    t = SEASONAL_THRESHOLDS[season]

    d3 = delta_3h if delta_3h is not None else 0.0

    if d3 >= 1.6:
        trend, trend_icon = "in rapida salita", "⬆️"
    elif d3 >= 0.5:
        trend, trend_icon = "in salita", "↗️"
    elif d3 <= -1.6:
        trend, trend_icon = "in rapido calo", "⬇️"
    elif d3 <= -0.5:
        trend, trend_icon = "in calo", "↘️"
    else:
        trend, trend_icon = "stabile", "➡️"

    if pressure_slp >= t["alta"]:
        if temp_range_24h is not None and temp_range_24h < 4 and season in ("inverno", "autunno") and d3 >= -0.5:
            condition, label, icon = "nebbia", "Alta pressione stabile — possibile nebbia/foschia in pianura", "🌫️"
        elif d3 >= 0.5:
            condition, label, icon = "sereno", "Bel tempo, cielo sereno in consolidamento", "☀️"
        elif d3 <= -0.5:
            condition, label, icon = "sereno_variabile", "Bel tempo ma in graduale peggioramento", "🌤️"
        else:
            condition, label, icon = "sereno", "Bel tempo stabile", "☀️"
    elif pressure_slp >= t["media"]:
        if d3 >= 0.5:
            condition, label, icon = "variabile", "Tempo in miglioramento, variabile", "⛅"
        elif d3 <= -0.5:
            condition, label, icon = "variabile", "Tempo variabile, possibile peggioramento", "🌥️"
        else:
            condition, label, icon = "variabile", "Tempo variabile stabile", "⛅"
    elif pressure_slp >= t["bassa"]:
        if d3 >= 0.5:
            condition, label, icon = "instabile", "Instabile ma in miglioramento", "🌥️"
        elif d3 <= -1.0:
            condition, label, icon = "pioggia", "Instabile, possibili rovesci in arrivo", "🌧️"
        else:
            condition, label, icon = "nuvoloso", "Nuvoloso, tempo incerto", "☁️"
    else:
        if d3 <= -0.5:
            condition, label, icon = "temporale", "Perturbato, condizioni in peggioramento", "⛈️"
        else:
            condition, label, icon = "pioggia", "Perturbato, piogge probabili", "🌧️"

    worsening = acceleration is not None and acceleration <= -0.5
    improving = acceleration is not None and acceleration >= 0.5

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
        "worsening": worsening,
        "improving": improving,
    }