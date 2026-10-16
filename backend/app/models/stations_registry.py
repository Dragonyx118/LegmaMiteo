# ===========================================
# Stations Registry — LegmaMiteo Backend
# Metadati statici (coordinate, nome, altitudine) per stazione
# ===========================================

import json
from pathlib import Path

_REGISTRY_PATH = Path(__file__).parent / "stations_registry.json"


def load_registry() -> dict:
    with open(_REGISTRY_PATH, encoding="utf-8") as f:
        return json.load(f)


def get_station_meta(station_id: str) -> dict | None:
    return load_registry().get(station_id)