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
    root_path="/api",  # <- Questa è l'unica cosa che serve a FastAPI per capire il proxy
    description="""
## LegmaMiteo Weather Station API

API pubblica per accedere ai dati della rete di stazioni meteo LegmaMiteo.
    """,
    license_info={
        "name": "Hippocratic License HL3-CL-ECO-LAW-MIL-SV",
        "url": "https://firstdonoharm.dev/version/3/0/cl-eco-law-mil-sv.html"
    }
)

# --- CORS ---
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
        "docs": "/api/docs (AGGIORNATO)",  # <- Se non vedi questa scritta online, il codice non si è aggiornato!
        "github": "https://github.com/Dragonyx118/LegmaMiteo"
    }


@app.get("/health", tags=["Info"])
def health():
    return {"status": "ok", "version": API_VERSION}