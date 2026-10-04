// Copy this file to config.h and fill in your real values.
// config.h is listed in .gitignore, so it is never uploaded to GitHub.
#pragma once

#define WIFI_SSID    "YOUR_WIFI_NAME"
#define WIFI_PASS    "YOUR_WIFI_PASSWORD"

// The Web app URL from Apps Script: Deploy > Manage deployments (must end in /exec, NOT /dev)
#define SCRIPT_URL   "https://script.google.com/macros/s/PASTE_YOUR_ID_HERE/exec"

// Same value as SECRET_KEY in Code.gs. Leave "" if the script's key is empty.
#define SECRET_KEY   ""

// Shown in the Source column of the Log
#define DEVICE_NAME  "Main Gate RFID-01"