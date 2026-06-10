# Data Policy — LegmaMiteo

**Version:** 1.0  
**Last updated:** 2026-06-10  
**Project:** [LegmaMiteo](https://github.com/Dragonyx118/LegmaMiteo)

---

## 1. What data is collected

Each LegmaMiteo station collects and transmits the following environmental data:

- Temperature (°C)
- Relative humidity (%)
- Atmospheric pressure (hPa)
- Ambient light / lux (lx)
- Wind speed (m/s)
- Rainfall (mm)
- Optional module data: PM2.5/PM10, CO₂, UV index, lightning, soil moisture, snow depth

All data is strictly **environmental and meteorological**. No personal data, no location tracking of individuals, no audio or video.

Each station transmits a `station_id` (e.g. `station/test-001/base`) which identifies the device, not any person.

---

## 2. Who can use the data

Data collected and published by LegmaMiteo stations is available for:

- ✅ **Personal use** — anyone can use the data for personal projects, curiosity, or learning
- ✅ **Scientific research** — universities, independent researchers, citizen science initiatives
- ✅ **Meteorological institutions** — national and international weather agencies (ARPA, WMO, ECMWF, etc.)
- ✅ **Environmental monitoring** — non-profit organizations and public interest projects
- ✅ **Education** — schools, courses, open educational resources

---

## 3. Prohibited uses

The following uses are **explicitly prohibited**:

- ❌ **Military applications** — any use by armed forces, defense contractors, or military intelligence agencies
- ❌ **Surveillance** — using station location or data patterns to track, monitor, or profile individuals or communities
- ❌ **Environmental harm** — using data to facilitate or optimize activities that cause environmental damage
- ❌ **Training proprietary AI models** — data may not be used to train closed-source or commercial AI/ML models without explicit written permission from the project maintainer
- ❌ **Any use that violates human rights** — as defined by the Hippocratic License HL3-CL-ECO-LAW-MIL-SV under which this project is released

These restrictions apply to both raw data and any derivative datasets.

---

## 4. Data accuracy and liability

LegmaMiteo stations are community-built devices. Data is provided **as-is**, without any guarantee of accuracy, continuity, or availability.

- Readings may contain errors due to sensor calibration, environmental interference, or hardware faults
- The project maintainer assumes no responsibility for decisions made based on this data
- For critical applications (aviation, civil protection, etc.) always rely on certified official sources

---

## 5. Station operators

Anyone who builds and deploys a LegmaMiteo station (a "station operator") agrees to:

- Operate the station in compliance with local laws and regulations
- Not use their station to collect data in contexts where doing so would violate privacy or property rights
- Not deliberately inject false or misleading data into the network

---

## 6. Open data and attribution

Data from public LegmaMiteo stations is published as **open data** under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/), subject to the prohibited uses listed in section 3.

When using or publishing data from LegmaMiteo stations, attribution is appreciated:

> *Data source: LegmaMiteo open weather network — https://github.com/Dragonyx118/LegmaMiteo*

---

## 7. Changes to this policy

This policy may be updated over time. Changes will be tracked in the repository's commit history. Continued use of the data after a policy update implies acceptance of the new terms.

---

## 8. Contact

For questions or permissions not covered by this policy, open an issue on the [GitHub repository](https://github.com/Dragonyx118/LegmaMiteo/issues).
