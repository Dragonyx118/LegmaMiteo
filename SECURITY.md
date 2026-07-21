# Security Policy for LegmaMiteo

We take the security of **LegmaMiteo** very seriously. Since the project involves both IoT/firmware devices (ESP32, WiFi, LoRa) and self-hosted data infrastructure (MQTT, InfluxDB, Grafana), protecting communications, credentials, and infrastructure is a top priority.

If you believe you have discovered a security vulnerability in the firmware code or infrastructure configuration, please report it to us responsibly before making it public.

---

## 🛡️ Supported Versions

Use the table below to check which versions or branches of the project currently receive security updates.

| Version / Branch | Supported |
| ---------------- | --------- |
| `main` (latest)  | Yes       |
| Official releases (latest) | Yes |
| Secondary branches / Deprecated releases | No |

---

## ✉️ Reporting a Vulnerability

**Please DO NOT open a public GitHub Issue to report a security vulnerability.**

Instead, please use one of these private channels:

1. **Private Reporting via GitHub (Recommended):**
   - Go to the **Security** tab of this repository.
   - Select **Advisories** from the left menu and click **Report a vulnerability**.
   - Fill out the form with the details of the issue.

2. **Direct Email:**
   - Send an email to **[INSERT_YOUR_EMAIL@DOMAIN.COM]** with the subject line: `[SECURITY] Vulnerability in LegmaMiteo`.

### Information to Include in Your Report

To help us understand and resolve the issue quickly, please include:
- **Description of the issue:** A clear explanation of what the vulnerability is.
- **Affected component:** E.g., ESP32 Firmware (WiFi/LoRa), MQTT communication, InfluxDB/Grafana authentication, etc.
- **Steps to reproduce (Proof of Concept):** Detailed step-by-step instructions to replicate the vulnerability.
- **Potential impact:** What an attacker could achieve by exploiting this issue (e.g., weather data interception, sensor spoofing, Denial of Service, credential leak).

---

## ⏱️ Response Times and Handling Process

- **Acknowledgment:** We will acknowledge receipt of your vulnerability report within **48 hours**.
- **Assessment and Fix:** We will evaluate the vulnerability and work on a patch within **7 business days** (or a timeline agreed upon based on severity).
- **Responsible Disclosure:** Once the fix is released (firmware or configuration update), we will publish a *Security Advisory* and credit the reporter (if desired).

---

## 🔒 Security Best Practices for Deployment (IoT & Cloud)

If you are setting up or deploying the **LegmaMiteo** system, we recommend following these security guidelines:

- **Credentials & ESP32 Firmware:**
  - Never commit Wi-Fi credentials, API tokens, or private keys into the source code. Use local configuration files excluded from Git (e.g., via `.gitignore`).
  - Use encrypted communications (TLS/MQTTS) where supported.
- **MQTT Broker / InfluxDB / Grafana:**
  - Always change default passwords for containers or services.
  - Do not expose nodes or the MQTT broker directly to the Internet without a Reverse Proxy (e.g., Nginx/Traefik) secured with HTTPS/SSL, or without a VPN.
  - Regularly apply security updates to your Docker/Linux infrastructure.

---

Thank you for helping keep **LegmaMiteo** secure and reliable for the entire community!
