#pragma once
// ---------------------------------------------------------------
// Startwerte – normalerweise muss hier NICHTS eingetragen werden.
//
// WLAN, IP der Violet, Benutzer und Passwort werden direkt am Display
// eingestellt (Kopfzeile 2 Sekunden gedrueckt halten) und dauerhaft
// gespeichert. Findet das Display beim Start kein WLAN, oeffnet es die
// WLAN-Einstellungen von selbst.
//
// Wer beim Entwickeln eigene Startwerte fest einbauen moechte, legt sie in
// src/config_local.h ab. Diese Datei wird von Git ignoriert, damit keine
// Passwoerter im Repository landen.
// ---------------------------------------------------------------

#if __has_include("config_local.h")
#include "config_local.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID        ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS        ""
#endif

// IP-Adresse der Violet (ohne http://)
#ifndef VIOLET_HOST
#define VIOLET_HOST      ""
#endif

// Fuer Schaltbefehle verlangt die Violet Benutzer und Passwort
#ifndef VIOLET_USER
#define VIOLET_USER      ""
#endif
#ifndef VIOLET_PASS
#define VIOLET_PASS      ""
#endif

// Abfrageintervall in Millisekunden
#ifndef POLL_INTERVAL_MS
#define POLL_INTERVAL_MS 60000
#endif
