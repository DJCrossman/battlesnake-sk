#pragma once

// The WiFi settings file, apps_data/snake_pad/wifi.json on the SD card (wifi.example.json): the
// server, the pad token and the networks to try, in order. Plain C with no Furi calls, so it also
// builds and runs on the host (wifi_settings_test.c).
//
// Keys may come in any order, and any others are ignored. Values are JSON strings, decoded with
// every escape, and raw UTF-8 passes through. None may hold a line break, as the board reads one
// line per value.

#include <stddef.h>
#include <stdint.h>

// A longer file is refused rather than read in part
#define WIFI_SETTINGS_FILE_MAX 4096

typedef enum {
    WifiSettingsFound,
    WifiSettingsPastLast, // the index is past the last network; the first is at 0
    WifiSettingsIncomplete, // no server, token or network, or a network without its name or password
    WifiSettingsInvalid, // not JSON, too long, or a value that is not a string or cannot be sent
} WifiSettingsResult;

// What the board is sent for one network, in the order it reads them, each ending in a NUL
typedef struct {
    const char* hotspot;
    const char* password;
    const char* server;
    const char* token;
} WifiSettings;

/** Finds the settings for the network at `index`, counting from 0 in the order the file lists
 * them. Every network is checked whichever is asked for, so a mistake shows on the first try.
 * Only WifiSettingsFound changes `json`: the four values are decoded in place, and `settings`
 * points into it. */
WifiSettingsResult
    wifi_settings_for_network(char* json, size_t len, uint32_t index, WifiSettings* settings);
