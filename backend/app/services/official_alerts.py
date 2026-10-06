# ===========================================
# Services: Official Alerts — LegmaMiteo Backend
# METEOALARM + Protezione Civile (DPC)
# ===========================================

import re
import requests
from datetime import datetime
import xml.etree.ElementTree as ET

METEOALARM_URL = "https://feeds.meteoalarm.org/feeds/meteoalarm-legacy-atom-italy"
DPC_REPO_API = "https://api.github.com/repos/pcm-dpc/DPC-Bollettini-Criticita-Idrogeologica-Idraulica/git/trees/master?recursive=1"
DPC_RAW_BASE = "https://raw.githubusercontent.com/pcm-dpc/DPC-Bollettini-Criticita-Idrogeologica-Idraulica/master"

SEVERITY_ORDER = {"gialla": 1, "arancione": 2, "rossa": 3}


def get_meteoalarm_lombardia(region: str = "Lombardia"):
    try:
        resp = requests.get(METEOALARM_URL, timeout=10)
        print(f"[ALERTS] METEOALARM HTTP status: {resp.status_code}", flush=True)
        if resp.status_code != 200:
            return []
    except Exception as e:
        print(f"[ALERTS] Errore fetch METEOALARM: {e}", flush=True)
        return []

    try:
        ns = {"atom": "http://www.w3.org/2005/Atom"}
        root = ET.fromstring(resp.content)
        alerts = []
        total_entries = 0

        for entry in root.findall("atom:entry", ns):
            total_entries += 1
            title_el = entry.find("atom:title", ns)
            title = title_el.text if title_el is not None else ""

            if region not in title:
                continue

            color = None
            if "Red" in title:
                color = "rossa"
            elif "Orange" in title:
                color = "arancione"
            elif "Yellow" in title:
                color = "gialla"

            if not color:
                continue

            summary_el = entry.find("atom:summary", ns)
            summary = summary_el.text if summary_el is not None else ""
            zone_match = re.search(r"intensi\s+([A-ZÀÈÌÒÙ\s]+?)(?:\s*\(DISCLAIMER|\.|\n)", summary)
            zone = zone_match.group(1).strip() if zone_match else None

            alerts.append({"title": title.split(" - ")[0], "color": color, "zone": zone})

        print(f"[ALERTS] METEOALARM: {total_entries} entry totali nel feed, {len(alerts)} allerte per '{region}'", flush=True)
        return alerts
    except Exception as e:
        print(f"[ALERTS] Errore parsing METEOALARM: {e}", flush=True)
        return []


def get_dpc_latest_bulletin():
    today_prefix = datetime.now().strftime("%Y%m%d")

    try:
        resp = requests.get(DPC_REPO_API, timeout=15, headers={"Accept": "application/vnd.github+json"})
        print(f"[ALERTS] DPC GitHub API status: {resp.status_code}", flush=True)
        if resp.status_code != 200:
            return None
        tree = resp.json().get("tree", [])
    except Exception as e:
        print(f"[ALERTS] Errore fetch albero DPC: {e}", flush=True)
        return None

    candidates = [
        item["path"] for item in tree
        if item["path"].startswith(f"files/{today_prefix}_") and item["path"].endswith(".json")
        and item["path"].count("/") == 1
    ]

    print(f"[ALERTS] DPC: {len(candidates)} bollettini candidati per oggi ({today_prefix})", flush=True)

    if not candidates:
        return None

    latest_file = sorted(candidates)[-1]
    raw_url = f"{DPC_RAW_BASE}/{latest_file}"

    try:
        resp = requests.get(raw_url, timeout=10)
        print(f"[ALERTS] DPC bollettino {latest_file} status: {resp.status_code}", flush=True)
        if resp.status_code == 200:
            return resp.json()
    except Exception as e:
        print(f"[ALERTS] Errore download bollettino DPC: {e}", flush=True)

    return None


def get_civil_protection_alert(region: str = "Lombardia"):
    data = get_dpc_latest_bulletin()
    if data is None:
        print("[ALERTS] DPC: nessun bollettino disponibile per oggi", flush=True)
        return None, None

    html = data.get("today", {}).get("html_descrition", "")

    if region not in html:
        print(f"[ALERTS] DPC: '{region}' non citata nel bollettino odierno", flush=True)
        return "verde", None

    matches = re.findall(
        rf"ALLERTA (GIALLA|ARANCIONE|ROSSA):</b><br\s*/><b>{region}</b>:\s*([^<]+)",
        html
    )

    if not matches:
        print(f"[ALERTS] DPC: '{region}' citata ma nessun livello di allerta estratto", flush=True)
        return "verde", None

    livello, zone = matches[0]
    print(f"[ALERTS] DPC: allerta {livello} per {region}, zone: {zone}", flush=True)
    return livello.lower(), zone.strip()


def get_official_alerts_summary(region: str = "Lombardia") -> list[dict]:
    """
    Ritorna una lista unificata di allerte ufficiali, pronta per il consumo
    da parte di un client (app/web): ogni voce ha source, color, title, zone.
    """
    result = []

    meteoalarm = get_meteoalarm_lombardia(region)
    for a in meteoalarm:
        result.append({
            "source": "meteoalarm",
            "color": a["color"],
            "title": a["title"],
            "zone": a["zone"],
        })

    dpc_level, dpc_zones = get_civil_protection_alert(region)
    if dpc_level and dpc_level != "verde":
        result.append({
            "source": "protezione_civile",
            "color": dpc_level,
            "title": "Allerta Protezione Civile",
            "zone": dpc_zones,
        })

    print(f"[ALERTS] Totale allerte restituite: {len(result)}", flush=True)
    return result