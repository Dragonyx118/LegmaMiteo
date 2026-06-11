# ===========================================
# Schemas — LegmaMiteo Backend
# Validazione dati in entrata e uscita
# ===========================================

from pydantic import BaseModel, Field
from typing import Optional
from datetime import datetime


# --- Stazione ---
class StationInfo(BaseModel):
    station_id: str
    name: Optional[str] = None
    latitude: Optional[float] = None
    longitude: Optional[float] = None
    altitude: Optional[float] = None
    modules: Optional[list[str]] = []


# --- Dati BASE ---
class BaseData(BaseModel):
    station_id: str
    temperature: Optional[float] = None
    humidity: Optional[float] = None
    pressure: Optional[float] = None
    lux: Optional[float] = None
    wind_speed: Optional[float] = None
    wind_direction: Optional[float] = None
    rain_mm: Optional[float] = None


# --- MOD-AIR ---
class AirData(BaseModel):
    station_id: str
    pm1: Optional[float] = None
    pm25: Optional[float] = None
    pm10: Optional[float] = None
    co2: Optional[float] = None
    voc: Optional[float] = None
    aqi: Optional[int] = None


# --- MOD-STORM ---
class StormData(BaseModel):
    station_id: str
    lightning_distance: Optional[float] = None
    pressure_hpa: Optional[float] = None
    sky_temp: Optional[float] = None
    vibration: Optional[float] = None


# --- Risposta generica ---
class APIResponse(BaseModel):
    success: bool
    message: str
    data: Optional[dict] = None