# ===========================================
# Main — LegmaMiteo Backend
# OpenWeather Station API
# ===========================================

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from app.routers import stations, data
from app.config import API_VERSION, PROJECT_NAME
from contextlib import asynccontextmanager
from apscheduler.schedulers.background import BackgroundScheduler
from scripts.write_sun_times import write_sun_times_for_all_stations
from scripts.forecast_tracker import log_forecast_snapshot, verify_due_forecasts
# NB: NON "from backend.scripts..." — dentro il container /app corrisponde già
# a backend/, quindi non esiste un package "backend" da un livello più su.


scheduler = BackgroundScheduler(timezone="Europe/Rome")


@asynccontextmanager
async def lifespan(app: FastAPI):
    # --- STARTUP ---
    # Calcolo immediato all'avvio del server, cosi' i dati di alba/tramonto
    # sono gia' disponibili senza aspettare la prossima esecuzione pianificata
    try:
        write_sun_times_for_all_stations()
    except Exception as e:
        print(f"[sun_times] Errore al calcolo iniziale: {e}")

    # Pianifica l'esecuzione automatica ogni giorno alle 00:05
    scheduler.add_job(
        write_sun_times_for_all_stations,
        "cron",
        hour=0,
        minute=5,
        id="write_sun_times_daily",
        replace_existing=True,
    )

    # Logga la previsione corrente (oggi + 5 giorni) come "in sospeso" ogni
    # giorno alle 00:10 — dopo i dati solari, prima della verifica
    scheduler.add_job(
        log_forecast_snapshot,
        "cron",
        hour=0,
        minute=10,
        id="log_forecast_daily",
        replace_existing=True,
    )

    # Verifica le previsioni il cui giorno target è ormai concluso, ogni
    # giorno alle 00:15 — confronta con dato reale stazione + Open-Meteo
    scheduler.add_job(
        verify_due_forecasts,
        "cron",
        hour=0,
        minute=15,
        id="verify_forecast_daily",
        replace_existing=True,
    )

    scheduler.start()

    yield  # <-- il server gira qui

    # --- SHUTDOWN ---
    scheduler.shutdown()


app = FastAPI(
    title=PROJECT_NAME,
    version=API_VERSION,
    root_path="/api",
    description="""
## LegmaMiteo Weather Station API

API pubblica per accedere ai dati della rete di stazioni meteo LegmaMiteo.
    """,
    license_info={
        "name": "Hippocratic License HL3-CL-ECO-LAW-MIL-SV",
        "url": "https://firstdonoharm.dev/version/3/0/cl-eco-law-mil-sv.html"
    },
    lifespan=lifespan
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
        "docs": "/api/docs (AGGIORNATO)",
        "github": "https://github.com/Dragonyx118/LegmaMiteo"
    }


@app.get("/health", tags=["Info"])
def health():
    return {"status": "ok", "version": API_VERSION}