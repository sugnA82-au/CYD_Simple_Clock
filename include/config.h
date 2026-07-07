#pragma once

// ---- Edit these before flashing ----
#define WIFI_SSID     "xxxx"
#define WIFI_PASSWORD "xxxx"

// POSIX timezone string (handles DST automatically).
// Europe/London:   "GMT0BST,M3.5.0/1,M10.5.0"
// US Eastern:      "EST5EDT,M3.2.0,M11.1.0"
// US Pacific:      "PST8PDT,M3.2.0,M11.1.0"
// Australia East:  "AEST-10AEDT,M10.1.0,M4.1.0/3"
// Full list: https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
#define TZ_INFO "AEST-10AEDT,M10.1.0,M4.1.0/3"

#define NTP_SERVER1 "time.google.com"
#define NTP_SYNC_INTERVAL_MIN 15