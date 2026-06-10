# Contributing to LegmaMiteo

First off — grazie! 🙏 LegmaMiteo is a community project and every contribution makes the network better and more useful for everyone.

---

## Table of contents

- [Code of Conduct](#code-of-conduct)
- [Ways to contribute](#ways-to-contribute)
- [Reporting bugs](#reporting-bugs)
- [Suggesting features](#suggesting-features)
- [Contributing code](#contributing-code)
- [Contributing hardware designs](#contributing-hardware-designs)
- [Contributing a station to the network](#contributing-a-station-to-the-network)
- [Style guidelines](#style-guidelines)

---

## Code of Conduct

By participating in this project you agree to:

- Be respectful and constructive in all interactions
- Welcome contributors of any background, experience level, or nationality
- Keep discussions focused on the project and its mission: open, ethical, accessible weather monitoring
- Not use this project or its community for any purpose prohibited by the [Data Policy](DATA_POLICY.md) or the [Hippocratic License](LICENSE)

---

## Ways to contribute

You don't have to write code to contribute. Here are all the ways you can help:

| Type | Examples |
|------|---------|
| 🐛 Bug reports | Firmware crashes, incorrect sensor readings, server config issues |
| 💡 Feature requests | New sensor modules, protocol improvements, dashboard panels |
| 🔧 Code | Firmware, server stack, tooling, tests |
| 🔌 Hardware | PCB designs, enclosure files, sensor module schematics |
| 📖 Documentation | README improvements, wiring guides, tutorials |
| 🌍 Station deployment | Build and connect a station to the network |
| 🌐 Translation | Translate docs to other languages |

---

## Reporting bugs

Before opening a bug report, please check if a similar issue already exists.

When filing a bug, include:

1. **What you did** — exact steps to reproduce
2. **What you expected** — what should have happened
3. **What happened** — the actual behavior, including any error messages or logs
4. **Environment** — hardware (ESP32 version, sensors), firmware version, OS, Docker version if relevant

Open an issue with the label `bug`.

---

## Suggesting features

Open an issue with the label `enhancement` and describe:

- What problem does this feature solve?
- How would it work from the user's perspective?
- Are there any alternatives you've considered?

---

## Contributing code

### 1. Fork and clone

```bash
git clone https://github.com/Dragonyx118/LegmaMiteo.git
cd LegmaMiteo
```

### 2. Create a branch

Use a descriptive branch name:

```bash
git checkout -b feature/wind-direction-sensor
git checkout -b fix/bme280-calibration
```

### 3. Make your changes

- Keep commits small and focused
- Write clear commit messages: `fix: correct humidity offset in BME280 driver`
- If you're adding a feature, add or update documentation

### 4. Test your changes

For firmware: flash to an ESP32 and verify sensor readings are correct.  
For server: run `docker compose up -d` and verify the full pipeline works (MQTT → Telegraf → InfluxDB → Grafana).

### 5. Open a pull request

- Describe what you changed and why
- Reference any related issues with `Closes #123`
- If it's a hardware or firmware change, include photos or serial output if possible

---

## Contributing hardware designs

Hardware files live in `/hardware/`. Please follow these conventions:

- **Schematics**: KiCad preferred (`.kicad_sch`, `.kicad_pcb`), EasyEDA JSON accepted
- **3D models**: STL files for 3D-printable parts, with print settings noted in a README
- **BOM**: include a `BOM.csv` with component names, values, footprints, and suggested suppliers
- **Enclosures**: note the IP rating and whether it's been field-tested

All hardware contributions must be compatible with the modular SP13 IP68 connector system used by the base station.

---

## Contributing a station to the network

If you build a LegmaMiteo station and want to connect it to the shared network:

1. Read the [Data Policy](DATA_POLICY.md) and make sure you agree
2. Open an issue with the label `new-station` and include:
   - Your approximate location (city/region is enough, no exact coordinates needed)
   - Which sensors/modules you have installed
   - Whether your station will be permanently deployed or used for testing
3. You'll receive a `station_id` to use in your MQTT topic

---

## Style guidelines

### Firmware (C++ / Arduino)

- Use 2-space indentation
- Name variables and functions in `snake_case`
- Comment non-obvious logic
- Keep sensor drivers in separate files under `firmware/src/sensors/`

### Server / configuration files

- YAML: 2-space indentation
- Flux queries: one filter per line
- Keep secrets out of config files — use environment variables

### Documentation

- Write in English (Italian is fine for comments in issues/PRs)
- Use simple, clear language — not everyone is a native speaker
- Prefer examples over abstract descriptions

---

## License

By contributing to LegmaMiteo, you agree that your contributions will be licensed under the same [Hippocratic License HL3-CL-ECO-LAW-MIL-SV](LICENSE) that covers the rest of the project.

If you're not comfortable with that license, please open an issue to discuss before contributing.
