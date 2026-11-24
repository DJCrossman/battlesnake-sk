// Host-side test of the WiFi settings file reader. Not part of the app.
// cc -std=c11 -Wall -Wextra -Werror -o /tmp/wifi_settings_test wifi_settings.c wifi_settings_test.c

#include "wifi_settings.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char two_networks[] = "{\n"
                                   "  \"server\": \"wss://example.cloudfront.net\",\n"
                                   "  \"token\": \"pad-token\",\n"
                                   "  \"networks\": [\n"
                                   "    { \"name\": \"Home\", \"password\": \"home password\" },\n"
                                   "    { \"name\": \"Work\", \"password\": \"work password\" }\n"
                                   "  ]\n"
                                   "}\n";

static int failures = 0;
// The reader decodes in place, so each check reads from a copy. One byte past the cap fits.
static char json[WIFI_SETTINGS_FILE_MAX + 2];

static void check(bool ok, const char* what);
static WifiSettingsResult read_network(const char* text, uint32_t index, WifiSettings* settings);
static bool is_found(
    WifiSettingsResult result,
    const WifiSettings* settings,
    const char* hotspot,
    const char* password,
    const char* server,
    const char* token);

int main(void) {
    WifiSettings settings;

    // Several networks, in the order listed, and past the last
    check(
        is_found(
            read_network(two_networks, 0, &settings),
            &settings,
            "Home",
            "home password",
            "wss://example.cloudfront.net",
            "pad-token"),
        "two networks: the first at 0");
    check(
        is_found(
            read_network(two_networks, 1, &settings),
            &settings,
            "Work",
            "work password",
            "wss://example.cloudfront.net",
            "pad-token"),
        "two networks: the second at 1");
    check(
        read_network(two_networks, 2, &settings) == WifiSettingsPastLast,
        "two networks: 2 is past the last");
    check(
        read_network(two_networks, 1000, &settings) == WifiSettingsPastLast,
        "two networks: 1000 is past the last");

    // The app asks again for 0 from the same bytes, which past the last must leave as they were
    const size_t two_len = strlen(two_networks);
    memcpy(json, two_networks, two_len);
    check(
        wifi_settings_for_network(json, two_len, 2, &settings) == WifiSettingsPastLast &&
            memcmp(json, two_networks, two_len) == 0,
        "past the last: the bytes are left as they were");
    check(
        is_found(
            wifi_settings_for_network(json, two_len, 0, &settings),
            &settings,
            "Home",
            "home password",
            "wss://example.cloudfront.net",
            "pad-token"),
        "past the last: then 0 from the same bytes finds the first");

    check(
        is_found(
            read_network(
                "{\"networks\": [{\"password\": \"p\", \"band\": \"2.4\", \"server\": \"x\", "
                "\"name\": \"n\"}], \"comment\": {\"why\": [\"a\", 1, true, null, {\"deep\": {}}]}, "
                "\"name\": \"not a network\", \"token\": \"t\", \"version\": 2, \"server\": \"s\"}",
                0,
                &settings),
            &settings,
            "n",
            "p",
            "s",
            "t"),
        "keys in any order, unknown keys and values of any kind ignored");
    check(
        is_found(
            read_network(
                "{\"server\": \"old\", \"token\": \"t\", \"server\": \"new\", "
                "\"networks\": [{\"name\": \"n\", \"password\": \"p\"}]}",
                0,
                &settings),
            &settings,
            "n",
            "p",
            "new",
            "t"),
        "a key given twice: the last counts");

    check(
        is_found(
            read_network(
                "{\"server\": \"s\", \"token\": \"t\", \"networks\": [{\"name\": \"n\", "
                "\"password\": \"q\\\" b\\\\ s\\/ b\\b f\\f r\\r t\\t A\\u0041 e\\u00E9 "
                "euro\\u20ac smile\\uD83D\\uDE00\"}]}",
                0,
                &settings),
            &settings,
            "n",
            "q\" b\\ s/ b\b f\f r\r t\t AA e\xC3\xA9 euro\xE2\x82\xAC smile\xF0\x9F\x98\x80",
            "s",
            "t"),
        "every escape, a surrogate pair included, decoded to UTF-8");
    check(
        is_found(
            read_network(
                "{\"server\": \"s\", \"token\": \"t\", \"networks\": [{\"name\": \"David\\u2019s "
                "iPhone\", \"password\": \"p\"}]}",
                0,
                &settings),
            &settings,
            "David\xE2\x80\x99s iPhone",
            "p",
            "s",
            "t"),
        "an escaped curly apostrophe");
    check(
        is_found(
            read_network(
                "{\"server\": \"s\", \"token\": \"t\", \"networks\": [{\"name\": \"David\xE2\x80\x99s "
                "iPhone\", \"password\": \"p\xC3\xA4ssw\xC3\xB6rd \xF0\x9F\x98\x80\"}]}",
                0,
                &settings),
            &settings,
            "David\xE2\x80\x99s iPhone",
            "p\xC3\xA4ssw\xC3\xB6rd \xF0\x9F\x98\x80",
            "s",
            "t"),
        "raw UTF-8 passes through");
    check(
        is_found(
            read_network(
                "{\"server\": \"s\", \"token\": \"t\", \"networks\": [{\"name\": \"open\", "
                "\"password\": \"\"}]}",
                0,
                &settings),
            &settings,
            "open",
            "",
            "s",
            "t"),
        "an empty password, for an open network");

    // Something missing
    check(
        read_network(
            "{\"token\": \"t\", \"networks\": [{\"name\": \"n\", \"password\": \"p\"}]}",
            0,
            &settings) == WifiSettingsIncomplete,
        "no server: incomplete");
    check(
        read_network(
            "{\"server\": \"s\", \"networks\": [{\"name\": \"n\", \"password\": \"p\"}]}",
            0,
            &settings) == WifiSettingsIncomplete,
        "no token: incomplete");
    check(
        read_network("{\"server\": \"s\", \"token\": \"t\"}", 0, &settings) ==
            WifiSettingsIncomplete,
        "no networks: incomplete");
    check(
        read_network("{\"server\": \"s\", \"token\": \"t\", \"networks\": []}", 0, &settings) ==
            WifiSettingsIncomplete,
        "an empty networks list: incomplete");
    check(
        read_network(
            "{\"server\": \"s\", \"token\": \"t\", \"networks\": [{\"name\": \"n\", "
            "\"password\": \"p\"}, {\"name\": \"m\"}]}",
            0,
            &settings) == WifiSettingsIncomplete,
        "the second network without its password, asked for the first: incomplete");
    check(
        read_network(
            "{\"server\": \"s\", \"token\": \"t\", \"networks\": [{\"password\": \"p\"}]}",
            0,
            &settings) == WifiSettingsIncomplete,
        "a network without its name: incomplete");

    // A value that is not a string, or cannot be sent
    check(
        read_network(
            "{\"server\": \"s\", \"token\": \"t\", \"networks\": [{\"name\": \"n\", "
            "\"password\": 12345678}]}",
            0,
            &settings) == WifiSettingsInvalid,
        "a password not in quotes: invalid");
    check(
        read_network(
            "{\"server\": null, \"token\": \"t\", \"networks\": [{\"name\": \"n\", "
            "\"password\": \"p\"}]}",
            0,
            &settings) == WifiSettingsInvalid,
        "a null server: invalid");
    check(
        read_network(
            "{\"server\": \"s\", \"token\": [\"t\"], \"networks\": [{\"name\": \"n\", "
            "\"password\": \"p\"}]}",
            0,
            &settings) == WifiSettingsInvalid,
        "a token in a list: invalid");
    check(
        read_network(
            "{\"server\": \"s\", \"token\": \"t\", \"networks\": {\"name\": \"n\", "
            "\"password\": \"p\"}}",
            0,
            &settings) == WifiSettingsInvalid,
        "networks not a list: invalid");
    check(
        read_network(
            "{\"server\": \"s\", \"token\": \"t\", \"networks\": [\"Home\"]}", 0, &settings) ==
            WifiSettingsInvalid,
        "a network that is only a name: invalid");
    check(
        read_network(
            "{\"server\": \"s\", \"token\": \"t\", \"networks\": [{\"name\": \"n\", "
            "\"password\": \"a\\nb\"}]}",
            0,
            &settings) == WifiSettingsInvalid,
        "an escaped line break: invalid");
    check(
        read_network(
            "{\"server\": \"s\", \"token\": \"t\\u000A\", \"networks\": [{\"name\": \"n\", "
            "\"password\": \"p\"}]}",
            0,
            &settings) == WifiSettingsInvalid,
        "a line break as \\u000A: invalid");
    check(
        read_network(
            "{\"server\": \"s\", \"token\": \"t\", \"networks\": [{\"name\": \"n\", "
            "\"password\": \"a\nb\"}]}",
            0,
            &settings) == WifiSettingsInvalid,
        "a raw line break in a value: invalid");
    check(
        read_network(
            "{\"server\": \"s\", \"token\": \"t\", \"networks\": [{\"name\": \"n\", "
            "\"password\": \"p\"}, {\"name\": \"m\", \"password\": \"a\\nb\"}]}",
            0,
            &settings) == WifiSettingsInvalid,
        "a line break in the second network, asked for the first: invalid");
    check(
        read_network(
            "{\"server\": \"s\", \"token\": \"t\", \"networks\": [{\"name\": \"n\", "
            "\"password\": \"a\\u0000b\"}]}",
            0,
            &settings) == WifiSettingsInvalid,
        "a NUL: invalid");
    check(
        read_network(
            "{\"server\": \"s\", \"token\": \"t\", \"networks\": [{\"name\": \"n\\uD83D\", "
            "\"password\": \"p\"}]}",
            0,
            &settings) == WifiSettingsInvalid,
        "a high surrogate on its own: invalid");
    check(
        read_network(
            "{\"server\": \"s\", \"token\": \"t\", \"networks\": [{\"name\": \"n\\uDE00\", "
            "\"password\": \"p\"}]}",
            0,
            &settings) == WifiSettingsInvalid,
        "a low surrogate on its own: invalid");

    // Not JSON
    static const char* const malformed[] = {
        "",
        "  \n",
        "{\"server\": \"s\", \"token\": \"t\", \"networks\": [{\"name\": \"n\", \"password\": \"p\"}]",
        "{\"server\": \"s\" \"token\": \"t\", \"networks\": [{\"name\": \"n\", \"password\": \"p\"}]}",
        "{\"server\" \"s\", \"token\": \"t\", \"networks\": [{\"name\": \"n\", \"password\": \"p\"}]}",
        "{\"server\": \"s\", \"token\": \"t\", \"networks\": [{\"name\": \"n\" \"password\": \"p\"}]}",
        "{server: \"s\", \"token\": \"t\", \"networks\": [{\"name\": \"n\", \"password\": \"p\"}]}",
        "{'server': 's', 'token': 't', 'networks': [{'name': 'n', 'password': 'p'}]}",
        "{\xE2\x80\x9Cserver\xE2\x80\x9D: \"s\", \"token\": \"t\", \"networks\": [{\"name\": \"n\", "
        "\"password\": \"p\"}]}",
        "{\"server\": \"s\", \"token\": \"t\", \"networks\": [{\"name\": \"n\", \"password\": "
        "\"p\"}]} x",
        "{\"server\": \"s\", \"token\": \"t\", \"networks\": [{\"name\": \"n\", \"password\": "
        "\"p\"}]} {}",
        "[{\"server\": \"s\", \"token\": \"t\", \"networks\": [{\"name\": \"n\", \"password\": "
        "\"p\"}]}]",
        "{\"server\": \"s\", \"token\": \"t\", \"networks\": [{\"name\": \"n\", \"password\": "
        "\"\\x41\"}]}",
        "{\"server\": \"s\", \"token\": \"t\", \"networks\": [{\"name\": \"n\", \"password\": "
        "\"\\u41\"}]}",
        "{\"server\": \"s\", \"token\": \"t\", \"networks\": [{\"name\": \"n\", \"password\": "
        "\"p\"}}",
    };
    bool all_invalid = true;
    for(size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        const bool invalid = read_network(malformed[i], 0, &settings) == WifiSettingsInvalid;
        if(!invalid) printf("      not refused: %s\n", malformed[i]);
        all_invalid = all_invalid && invalid;
    }
    check(
        all_invalid,
        "malformed (empty, unclosed, missing comma or colon, bare or single-quoted or curly-quoted "
        "key, text after, a list, bad escapes, mismatched brackets): invalid");

    // The file at the cap: an unknown key padded to make it exactly WIFI_SETTINGS_FILE_MAX bytes
    static const char padded_start[] =
        "{\"server\": \"s\", \"token\": \"t\", \"networks\": [{\"name\": \"n\", \"password\": "
        "\"p\"}], \"padding\": \"";
    static const char padded_end[] = "\"}";
    char padded[WIFI_SETTINGS_FILE_MAX + 2];
    const size_t padding = WIFI_SETTINGS_FILE_MAX - strlen(padded_start) - strlen(padded_end);
    memcpy(padded, padded_start, strlen(padded_start));
    memset(padded + strlen(padded_start), 'x', padding);
    strcpy(padded + strlen(padded_start) + padding, padded_end);
    check(
        strlen(padded) == WIFI_SETTINGS_FILE_MAX &&
            is_found(read_network(padded, 0, &settings), &settings, "n", "p", "s", "t"),
        "a file at the size cap: read");
    memset(padded + strlen(padded_start), 'x', padding + 1);
    strcpy(padded + strlen(padded_start) + padding + 1, padded_end);
    check(
        strlen(padded) == WIFI_SETTINGS_FILE_MAX + 1 &&
            read_network(padded, 0, &settings) == WifiSettingsInvalid,
        "one byte over the size cap: invalid");

    // The token budget: 24 networks fit and 25 do not
    char many[2048] = "{\"server\": \"s\", \"token\": \"t\", \"networks\": [";
    for(int i = 0; i < 24; i++) {
        snprintf(
            many + strlen(many),
            sizeof(many) - strlen(many),
            "%s{\"name\": \"n%d\", \"password\": \"p%d\"}",
            i ? ", " : "",
            i,
            i);
    }
    strcat(many, "]}");
    check(
        is_found(read_network(many, 23, &settings), &settings, "n23", "p23", "s", "t"),
        "24 networks: the last is found");
    many[strlen(many) - 2] = '\0';
    strcat(many, ", {\"name\": \"n24\", \"password\": \"p24\"}]}");
    check(read_network(many, 0, &settings) == WifiSettingsInvalid, "25 networks: invalid");

    printf(failures ? "FAILED: %d check(s)\n" : "All checks passed\n", failures);
    return failures ? 1 : 0;
}

static void check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if(!ok) failures++;
}

// Reads the text as the app reads the file, from a copy the reader may change
static WifiSettingsResult read_network(const char* text, uint32_t index, WifiSettings* settings) {
    const size_t len = strlen(text);
    memcpy(json, text, len);
    return wifi_settings_for_network(json, len, index, settings);
}

static bool is_found(
    WifiSettingsResult result,
    const WifiSettings* settings,
    const char* hotspot,
    const char* password,
    const char* server,
    const char* token) {
    return result == WifiSettingsFound && strcmp(settings->hotspot, hotspot) == 0 &&
           strcmp(settings->password, password) == 0 && strcmp(settings->server, server) == 0 &&
           strcmp(settings->token, token) == 0;
}
