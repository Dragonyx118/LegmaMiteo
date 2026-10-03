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
STATION_ALTITUDE_M = float(os.getenv("STATION_ALTITUDE_M", 84))  # Campagnola Cremasca (CR)

API_VERSION = "1.0.0"
PROJECT_NAME = "LegmaMiteo Weather API"