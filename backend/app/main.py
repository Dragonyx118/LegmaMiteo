# ===========================================
# Main — LegmaMiteo Backend
# OpenWeather Station API
# ===========================================

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from app.routers import stations, data
from app.config import API_VERSION, PROJECT_NAME

app = FastAPI(
    title=PROJECT_NAME,
    version=API_VERSION,
    description="""
## LegmaMiteo Weather Station API

API pubblica per accedere ai dati della rete di stazioni meteo LegmaMiteo.

### Dati disponibili
- **BASE** — Temperatura, umidità, pressione, luce, vento, pioggia
- **MOD-AIR** — PM1/PM2.5/PM10, CO2, VOC, AQI
- **MOD-STORM** — Fulmini, pressione delta, temperatura cielo, vibrazioni

### Licenza dati
I dati sono rilasciati sotto **CC BY-NC 4.0**.
Vietato uso militare, sorveglianza e training AI senza permesso esplicito.

### Repository
[github.com/Dragonyx118/LegmaMiteo](https://github.com/Dragonyx118/LegmaMiteo)
    """,
    license_info={
        "name": "Hippocratic License HL3-CL-ECO-LAW-MIL-SV",
        "url": "https://firstdonoharm.dev/version/3/0/cl-eco-law-mil-sv.html"
    }
)

# --- CORS — permette accesso da browser esterni ---
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["GET"],
    allow_headers=["*"],
)

# --- Routers ---
app.include_router(stations.router)
app.include_router(data.router)


@app.get("/", tags=["Info"])
def root():
    return {
        "project": PROJECT_NAME,
        "version": API_VERSION,
        "docs": "/docs",
        "github": "https://github.com/Dragonyx118/LegmaMiteo"
    }


@app.get("/health", tags=["Info"])
def health():
    return {"status": "ok", "version": API_VERSION}