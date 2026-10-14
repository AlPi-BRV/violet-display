# Violet Pool-Display

Eigenständiges Touch-Display für die **Violet-Poolsteuerung** von PoolDigital –
auf einem günstigen ESP32-S3-Display, ohne ioBroker, Home Assistant oder Server.
Das Display holt sich die Werte per WLAN direkt von der Violet.

🇬🇧 [English summary below](#english)

> **Kein offizielles Produkt von PoolDigital.** Bei Fragen zum Display bitte hier
> im Repository ein Issue anlegen, nicht bei PoolDigital. Nutzung auf eigene Gefahr –
> das Display kann die Filterpumpe und die Rückspülung schalten.

## Funktionen

- **Übersicht mit vier großen Kacheln:** Wassertemperatur (mit Luft und Absorber),
  Wasserwerte (pH, Redox, Chlor – Kachel grün/gelb/rot), Filterpumpe, Heizungen
- **Bedienung der Filterpumpe:** EIN / AUS / AUTO, Drehzahl ECO / Normal / Boost
- **Rückspülung starten** – mit Sicherheitsabfrage; steht die Rückspülung in der
  Violet nicht auf AUTO, bietet das Display an, sie auf AUTO zu stellen
- **Einrichtung komplett am Display:** WLAN suchen und wählen, IP der Violet,
  Benutzer und Passwort – mit Bildschirmtastatur
- **Update-Hinweis:** prüft einmal am Tag, ob es hier eine neue Version gibt
- Dimmt nach 5 Minuten ohne Berührung; die erste Berührung weckt nur auf

## Hardware

| | |
|---|---|
| Display | **Elecrow CrowPanel ESP32 5.0"**, SKU **DIS07050H**, Board-Revision **V3.0** |
| Chip | ESP32-S3-WROOM-1-N4R8 (4 MB Flash, 8 MB PSRAM) |
| Auflösung | 800 × 480, kapazitiver Touch |

Getestet ist nur diese Variante. Andere CrowPanel-Größen oder ältere Revisionen
haben eine andere Pin-Belegung.

## Installation

### Einfach: im Browser flashen

1. Display per USB-C anschließen.
2. **[Web-Installer öffnen](https://alpi-brv.github.io/violet-display/)** in Chrome oder Edge.
3. *Installieren* klicken, Anschluss wählen – fertig.

### Selbst bauen (PlatformIO)

1. Repository klonen und den Ordner in VS Code mit der PlatformIO-Erweiterung öffnen.
2. Unten in der Statusleiste auf *Upload* (Pfeil) klicken.

Für eigene Startwerte beim Entwickeln eine Datei `src/config_local.h` anlegen –
sie wird von Git ignoriert:

```cpp
#define WIFI_SSID   "MeinWLAN"
#define WIFI_PASS   "geheim"
#define VIOLET_HOST "192.168.1.50"
#define VIOLET_USER "admin"
#define VIOLET_PASS "geheim"
```

## Einrichtung

Beim ersten Start öffnet das Display die Einstellungen von selbst:

1. Reiter **WLAN**: *Suchen*, Netz antippen, WLAN-Passwort eingeben.
2. Reiter **Violet**: IP-Adresse der Violet, Benutzer und Passwort.
3. **Speichern** – das Display verbindet sich und meldet, ob die Violet erreichbar ist.

Später: **Kopfzeile 2 Sekunden gedrückt halten** öffnet die Einstellungen wieder.

Benutzer und Passwort sind nur für Schaltbefehle nötig; das Lesen der Werte
funktioniert bei der Violet ohne Anmeldung.

## Bedienung

- Kachel **Filterpumpe** antippen → Bedienseite
- **AUS** und **Rückspülung** fragen vorher nach
- Manuell EIN/AUS gilt, bis **AUTO** gedrückt wird
- Die Bedienseite schließt sich nach 60 Sekunden ohne Berührung

## Sicherheit

Die Zugangsdaten werden unverschlüsselt im Flash des ESP32 gespeichert. Wer das
Display per USB an einen Rechner anschließt, kann sie auslesen. Für ein Display
im Technikraum ist das in der Regel unkritisch.

## Verwandtes Projekt

[ioBroker.violet](https://github.com/AlPi-BRV/ioBroker.violet) – Adapter für die
Violet-Poolsteuerung in ioBroker.

## Lizenz

MIT – siehe [LICENSE](LICENSE).
Verwendete Bibliotheken: [LovyanGFX](https://github.com/lovyan03/LovyanGFX),
[ArduinoJson](https://github.com/bblanchon/ArduinoJson).

---

## English

Standalone touch display for the **Violet pool controller** by PoolDigital, running
on an Elecrow CrowPanel ESP32-S3 5" (DIS07050H, V3.0). Reads values directly from the
Violet over Wi-Fi – no ioBroker or server required.

- Overview: water temperature, water values (pH, ORP, chlorine), filter pump, heaters
- Control: filter pump ON / OFF / AUTO with speed, manual backwash (with confirmation)
- Setup entirely on the device: Wi-Fi scan, Violet IP, user and password
  (long-press the header for 2 seconds)
- Daily check for new releases
- Flash in the browser: **[web installer](https://alpi-brv.github.io/violet-display/)**

The user interface is German. Not an official PoolDigital product; use at your own risk.
