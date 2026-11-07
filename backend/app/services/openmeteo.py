# ===========================================
# Services: Open-Meteo — LegmaMiteo Backend
# Helper condiviso per interrogare Open-Meteo (dato attuale e previsione
# giornaliera), usato sia per il bias correction sia per i giorni a bassa
# confidenza della previsione estesa.
# ===========================================

import requests
from datetime import date

# weathercode WMO -> (condition, label, icon), stesse categorie usate da forecast.py
WEATHERCODE_DISPLAY = {
    0:  ("sereno", "Cielo sereno (Open-Meteo)", "☀️"),
    1:  ("sereno_variabile", "Prevalentemente sereno (Open-Meteo)", "🌤️"),
    2:  ("variabile", "Parzialmente nuvoloso (Open-Meteo)", "⛅"),
    3:  ("nuvoloso", "Coperto (Open-Meteo)", "☁️"),
    45: ("nebbia", "Nebbia (Open-Meteo)", "🌫️"),
    48: ("nebbia", "Nebbia con brina (Open-Meteo)", "🌫️"),
    51: ("pioggia", "Pioviggine debole (Open-Meteo)", "🌧️"),
    53: ("pioggia", "Pioviggine moderata (Open-Meteo)", "🌧️"),
    55: ("pioggia", "Pioviggine intensa (Open-Meteo)", "🌧️"),
    56: ("pioggia", "Pioviggine gelata (Open-Meteo)", "🌧️"),
    57: ("pioggia", "Pioviggine gelata intensa (Open-Meteo)", "🌧️"),
    61: ("pioggia", "Pioggia debole (Open-Meteo)", "🌧️"),
    63: ("pioggia", "Pioggia moderata (Open-Meteo)", "🌧️"),
    65: ("pioggia", "Pioggia intensa (Open-Meteo)", "🌧️"),
    66: ("pioggia", "Pioggia gelata (Open-Meteo)", "🌧️"),
    67: ("pioggia", "Pioggia gelata intensa (Open-Meteo)", "🌧️"),
    71: ("pioggia", "Neve debole (Open-Meteo)", "🌨️"),
    73: ("pioggia", "Neve moderata (Open-Meteo)", "🌨️"),
    75: ("pioggia", "Neve intensa (Open-Meteo)", "🌨️"),
    77: ("pioggia", "Granelli di neve (Open-Meteo)", "🌨️"),
    80: ("pioggia", "Rovesci deboli (Open-Meteo)", "🌦️"),
    81: ("pioggia", "Rovesci moderati (Open-Meteo)", "🌦️"),
    82: ("pioggia", "Rovesci violenti (Open-Meteo)", "🌦️"),
    85: ("pioggia", "Rovesci di neve deboli (Open-Meteo)", "🌨️"),
    86: ("pioggia", "Rovesci di neve intensi (Open-Meteo)", "🌨️"),
    95: ("temporale", "Temporale (Open-Meteo)", "⛈️"),
    96: ("temporale", "Temporale con grandine debole (Open-Meteo)", "⛈️"),
    99: ("temporale", "Temporale con grandine forte (Open-Meteo)", "⛈️"),
}


def fetch_current(lat: float, lon: float):
    """Temperatura, umidità e pressione attuali da Open-Meteo. None se fallisce."""
    try:
        url = (
            f"https://api.open-meteo.com/v1/forecast?latitude={lat}&longitude={lon}"
            f"&current=temperature_2m,relative_humidity_2m,surface_pressure"
        )
        resp = requests.get(url, timeout=5)
        if resp.status_code == 200:
            return resp.json().get("current", {})
    except Exception:
        pass
    return None


def fetch_daily_forecast(lat: float, lon: float, target_date: date):
    """
    Weathercode + temp min/max giornalieri Open-Meteo per una data futura vicina
    (entro l'orizzonte di previsione standard, ~16 giorni). Ritorna un dict con
    condition/label/icon/temp_min/temp_max, o None se non disponibile.
    """
    try:
        today = date.today()
        days_ahead = (target_date - today).days
        if days_ahead < 0 or days_ahead > 15:
            return None

        url = (
            f"https://api.open-meteo.com/v1/forecast?latitude={lat}&longitude={lon}"
            f"&daily=weathercode,temperature_2m_max,temperature_2m_min"
            f"&forecast_days={days_ahead + 1}&timezone=auto"
        )
        resp = requests.get(url, timeout=5)
        if resp.status_code != 200:
            return None

        data = resp.json().get("daily", {})
        dates = data.get("time", [])
        target_str = target_date.isoformat()
        if target_str not in dates:
            return None

        idx = dates.index(target_str)
        code = data["weathercode"][idx]
        condition, label, icon = WEATHERCODE_DISPLAY.get(code, ("variabile", "Condizione incerta (Open-Meteo)", "⛅"))

        return {
            "condition": condition,
            "label": label,
            "icon": icon,
            "temp_min": data["temperature_2m_min"][idx],
            "temp_max": data["temperature_2m_max"][idx],
            "weathercode": code,
        }
    except Exception:
        return None