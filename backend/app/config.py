# ===========================================
# Config — LegmaMiteo Backend
# OpenWeather Station
# ===========================================

from dotenv import load_dotenv
import os

load_dotenv("../.env")

INFLUX_URL = os.getenv("INFLUX_URL", "http://legmamiteo-influxdb:8086")
INFLUX_TOKEN = os.getenv("INFLUX_TOKEN")
INFLUX_ORG = os.getenv("INFLUX_ORG", "legmamiteo")
INFLUX_BUCKET = os.getenv("INFLUX_BUCKET", "stations")

API_VERSION = "1.0.0"
PROJECT_NAME = "LegmaMiteo Weather API"