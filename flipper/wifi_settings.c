#include "wifi_settings.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// jsmn.h is https://github.com/zserge/jsmn at v1.1.0, commit
// fdcef3ebf886fa210d14956d3c068a653e76a24e, unmodified. Static, as only this file uses it; strict,
// so a bare word is not taken for a value; with parent links, so each token says what holds it.
#define JSMN_STATIC
#define JSMN_STRICT
#define JSMN_PARENT_LINKS
#include "jsmn.h"

// The file's object, the server, the token and the networks list take 7 tokens and each network
// 5, so 24 networks fit. 2.5 KB, on the heap as the app's stack is 4 KB.
#define WIFI_SETTINGS_TOKENS_MAX 128

static bool wifi_settings_key_is(const char* json, const jsmntok_t* key, const char* name);
static int wifi_settings_board_value(char* json, const jsmntok_t* value, bool in_place);

WifiSettingsResult
    wifi_settings_for_network(char* json, size_t len, uint32_t index, WifiSettings* settings) {
    if(len > WIFI_SETTINGS_FILE_MAX) return WifiSettingsInvalid;
    jsmntok_t* tokens = malloc(WIFI_SETTINGS_TOKENS_MAX * sizeof(jsmntok_t));
    jsmn_parser parser;
    jsmn_init(&parser);
    const int count = jsmn_parse(&parser, json, len, tokens, WIFI_SETTINGS_TOKENS_MAX);

    // jsmn only splits the text up. Here every key must hold one value, nothing else may hold one
    // and nothing may follow the file's object, which leaves stray commas as the only mistake let
    // through. The file's own keys are noted on the way, each value right after its key; a key
    // given twice keeps the last.
    bool invalid = count < 1 || tokens[0].type != JSMN_OBJECT;
    int server = -1;
    int token = -1;
    int networks = -1;
    for(int i = 1; i < count && !invalid; i++) {
        const jsmntok_t* t = &tokens[i];
        if(t->parent < 0) {
            invalid = true;
        } else if(tokens[t->parent].type == JSMN_OBJECT) {
            invalid = t->type != JSMN_STRING || t->size != 1;
        } else {
            invalid = (t->type == JSMN_STRING || t->type == JSMN_PRIMITIVE) && t->size != 0;
        }
        if(invalid || t->parent != 0) continue;
        if(wifi_settings_key_is(json, t, "server")) server = i + 1;
        if(wifi_settings_key_is(json, t, "token")) token = i + 1;
        if(wifi_settings_key_is(json, t, "networks")) networks = i + 1;
    }
    invalid = invalid ||
              (server >= 0 && wifi_settings_board_value(json, &tokens[server], false) < 0) ||
              (token >= 0 && wifi_settings_board_value(json, &tokens[token], false) < 0) ||
              (networks >= 0 && tokens[networks].type != JSMN_ARRAY);
    bool incomplete = server < 0 || token < 0;

    uint32_t networks_seen = 0;
    int name = -1;
    int password = -1;
    for(int i = networks + 1; networks >= 0 && i < count && !invalid; i++) {
        if(tokens[i].parent != networks) continue;
        int its_name = -1;
        int its_password = -1;
        for(int k = i + 1; k < count; k++) {
            if(tokens[k].parent != i) continue;
            if(wifi_settings_key_is(json, &tokens[k], "name")) its_name = k + 1;
            if(wifi_settings_key_is(json, &tokens[k], "password")) its_password = k + 1;
        }
        invalid =
            tokens[i].type != JSMN_OBJECT ||
            (its_name >= 0 && wifi_settings_board_value(json, &tokens[its_name], false) < 0) ||
            (its_password >= 0 &&
             wifi_settings_board_value(json, &tokens[its_password], false) < 0);
        incomplete = incomplete || its_name < 0 || its_password < 0;
        if(networks_seen++ == index) {
            name = its_name;
            password = its_password;
        }
    }
    incomplete = incomplete || networks_seen == 0;

    WifiSettingsResult result = WifiSettingsFound;
    if(invalid) {
        result = WifiSettingsInvalid;
    } else if(incomplete) {
        result = WifiSettingsIncomplete;
    } else if(index >= networks_seen) {
        result = WifiSettingsPastLast;
    } else {
        wifi_settings_board_value(json, &tokens[name], true);
        wifi_settings_board_value(json, &tokens[password], true);
        wifi_settings_board_value(json, &tokens[server], true);
        wifi_settings_board_value(json, &tokens[token], true);
        *settings = (WifiSettings){
            .hotspot = json + tokens[name].start,
            .password = json + tokens[password].start,
            .server = json + tokens[server].start,
            .token = json + tokens[token].start,
        };
    }
    free(tokens);
    return result;
}

// Compares the key as written, so "name" is not "name"
static bool wifi_settings_key_is(const char* json, const jsmntok_t* key, const char* name) {
    const size_t len = strlen(name);
    return (size_t)(key->end - key->start) == len && memcmp(json + key->start, name, len) == 0;
}

// How long the string at value is once decoded, which is how the board is sent it, or -1 when it
// is not a string or cannot be sent whole: a line break would end the board's line early, and a
// NUL the C string. In place, it is also decoded where it starts and a NUL put after it; no value
// grows as it is decoded, so the NUL lands on the closing quote at the latest. jsmn has already
// checked each escape, and that \u has four hex digits after it.
static int wifi_settings_board_value(char* json, const jsmntok_t* value, bool in_place) {
    if(value->type != JSMN_STRING) return -1;
    char* out = json + value->start;
    int len = 0;
    for(int i = value->start; i < value->end; i++) {
        // Raw text, UTF-8 included, passes through. JSON allows no raw control character in a
        // string, a line break included.
        if((unsigned char)json[i] < 0x20) return -1;
        if(json[i] != '\\') {
            if(in_place) out[len] = json[i];
            len++;
            continue;
        }

        uint32_t code = 0;
        switch(json[++i]) {
        case 'b':
            code = '\b';
            break;
        case 'f':
            code = '\f';
            break;
        case 'n':
            code = '\n';
            break;
        case 'r':
            code = '\r';
            break;
        case 't':
            code = '\t';
            break;
        case 'u':
            for(int k = 0; k < 4; k++) {
                const char hex = json[++i];
                code = code << 4 | (uint32_t)(hex <= '9' ? hex - '0' : (hex | 0x20) - 'a' + 10);
            }
            // Past U+FFFF a character comes as two: a high surrogate, then a low one
            if(code >= 0xD800 && code < 0xDC00 && i + 6 < value->end && json[i + 1] == '\\' &&
               json[i + 2] == 'u') {
                uint32_t low = 0;
                for(int k = 3; k < 7; k++) {
                    const char hex = json[i + k];
                    low = low << 4 | (uint32_t)(hex <= '9' ? hex - '0' : (hex | 0x20) - 'a' + 10);
                }
                if(low >= 0xDC00 && low < 0xE000) {
                    code = 0x10000 + ((code - 0xD800) << 10 | (low - 0xDC00));
                    i += 6;
                }
            }
            // A surrogate on its own is not a character, so UTF-8 has no bytes for it
            if(code >= 0xD800 && code < 0xE000) return -1;
            break;
        default:
            // " \ / stand for themselves
            code = (unsigned char)json[i];
            break;
        }
        if(code == '\n' || code == 0) return -1;

        // As UTF-8, like the raw text around it
        const int bytes = code < 0x80 ? 1 : code < 0x800 ? 2 : code < 0x10000 ? 3 : 4;
        if(in_place) {
            for(int k = bytes - 1; k > 0; k--, code >>= 6) {
                out[len + k] = (char)(0x80 | (code & 0x3F));
            }
            // The lead byte: 110xxxxx, 1110xxxx or 11110xxx
            out[len] = (char)(bytes == 1 ? code : ((0xFF00 >> bytes) & 0xFF) | code);
        }
        len += bytes;
    }
    if(in_place) out[len] = '\0';
    return len;
}
