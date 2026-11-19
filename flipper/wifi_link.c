#include <furi.h>
#include <furi_hal_serial.h>
#include <furi_hal_serial_control.h>
#include <expansion/expansion.h>
#include <flipper_format/flipper_format.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>

#include "links.h"

// What the Flipper sends the WiFi Devboard over the header UART
typedef enum {
    WifiLinkCommandHeartbeat = 5,
    WifiLinkCommandStop = 6,
    WifiLinkCommandSettings = 0x10,
} WifiLinkCommand;

// What the board sends back. Every byte in 0xA0..0xA7 is one of these; anything else is the
// ESP32's boot text.
typedef enum {
    WifiLinkBoardSilent = 0, // never sent: the board missed three heartbeats in a row
    WifiLinkBoardWaiting = 0xA0,
    WifiLinkBoardJoining = 0xA1,
    WifiLinkBoardHotspotNotFound = 0xA2,
    WifiLinkBoardHotspotRefused = 0xA3,
    WifiLinkBoardReachingServer = 0xA4,
    WifiLinkBoardServerRefused = 0xA5,
    WifiLinkBoardConnected = 0xA6,
    WifiLinkBoardHeartbeatAnswered = 0xA7, // not a state: the server answered the heartbeat
} WifiLinkBoardByte;

typedef enum {
    WifiLinkEventInput,
    WifiLinkEventBoardByte,
    WifiLinkEventTick,
} WifiLinkEventType;

typedef struct {
    WifiLinkEventType type;
    union {
        InputEvent input;
        uint8_t board_byte;
    };
} WifiLinkEvent;

// Same table as the Board link, so the server reads the same bytes from either
static const uint8_t wifi_link_button_byte[InputKeyMAX] = {
    [InputKeyUp] = 0,
    [InputKeyDown] = 1,
    [InputKeyLeft] = 2,
    [InputKeyRight] = 3,
    [InputKeyOk] = 4,
};

typedef struct {
    FuriMutex* mutex;
    WifiLinkBoardByte board;
    const char* settings_problem;
    bool pinged;
    uint32_t ping_ms;
    uint32_t presses_sent;
} WifiLinkState;

static void wifi_link_draw(Canvas* canvas, void* context);
static void wifi_link_forward_input(InputEvent* input, void* context);
static void wifi_link_forward_board_byte(
    FuriHalSerialHandle* serial,
    FuriHalSerialRxEvent event,
    void* context);
static void wifi_link_forward_tick(void* context);
static const char* wifi_link_answer_settings_request(FuriHalSerialHandle* serial);

void wifi_link_run(void) {
    WifiLinkState state = {.mutex = furi_mutex_alloc(FuriMutexTypeNormal)};
    FuriMessageQueue* queue = furi_message_queue_alloc(16, sizeof(WifiLinkEvent));

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, wifi_link_draw, &state);
    view_port_input_callback_set(view_port, wifi_link_forward_input, queue);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    NotificationApp* notifications = furi_record_open(RECORD_NOTIFICATION);
    notification_message(notifications, &sequence_display_backlight_enforce_on);
    notification_message(notifications, &sequence_blink_start_blue);

    // The expansion service listens on the header UART for modules, so it lets go first
    Expansion* expansion = furi_record_open(RECORD_EXPANSION);
    expansion_disable(expansion);
    FuriHalSerialHandle* serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    furi_check(serial);
    furi_hal_serial_init(serial, 115200);
    furi_hal_serial_async_rx_start(serial, wifi_link_forward_board_byte, queue, false);

    // A board still set up by an earlier run starts over and asks for the settings again
    furi_hal_serial_tx(serial, (const uint8_t[]){WifiLinkCommandStop}, 1);

    // First heartbeat now, then every 2 s
    furi_message_queue_put(queue, &(WifiLinkEvent){.type = WifiLinkEventTick}, 0);
    FuriTimer* heartbeat = furi_timer_alloc(wifi_link_forward_tick, FuriTimerTypePeriodic, queue);
    furi_timer_start(heartbeat, furi_ms_to_ticks(2000));

    uint32_t unanswered_heartbeats = 0;
    uint32_t heartbeat_sent_at = 0;
    WifiLinkEvent event;
    bool running = true;
    while(running) {
        furi_check(furi_message_queue_get(queue, &event, FuriWaitForever) == FuriStatusOk);
        furi_mutex_acquire(state.mutex, FuriWaitForever);
        const bool was_connected = state.board == WifiLinkBoardConnected;

        if(event.type == WifiLinkEventTick) {
            if(unanswered_heartbeats >= 3) state.board = WifiLinkBoardSilent;
            heartbeat_sent_at = furi_get_tick();
            unanswered_heartbeats++;
            furi_hal_serial_tx(serial, (const uint8_t[]){WifiLinkCommandHeartbeat}, 1);
        } else if(event.type == WifiLinkEventBoardByte) {
            if(event.board_byte == WifiLinkBoardHeartbeatAnswered) {
                state.ping_ms = furi_get_tick() - heartbeat_sent_at;
                state.pinged = true;
            } else {
                unanswered_heartbeats = 0;
                state.board = event.board_byte;
                if(state.board == WifiLinkBoardWaiting) {
                    state.settings_problem = wifi_link_answer_settings_request(serial);
                }
            }
        } else if(event.input.key == InputKeyBack) {
            running = event.input.type != InputTypeLong;
        } else if(event.input.type == InputTypePress) {
            furi_hal_serial_tx(serial, &wifi_link_button_byte[event.input.key], 1);
            if(state.board == WifiLinkBoardConnected) state.presses_sent++;
        }

        const bool connected = state.board == WifiLinkBoardConnected;
        if(connected && !was_connected) {
            notification_message(notifications, &sequence_blink_stop);
            notification_message(notifications, &sequence_set_only_green_255);
            // A new connection shows no ping until the server answers it
            state.pinged = false;
        } else if(!connected && was_connected) {
            notification_message(notifications, &sequence_reset_rgb);
            notification_message(notifications, &sequence_blink_start_blue);
        }

        furi_mutex_release(state.mutex);
        view_port_update(view_port);
    }

    // Stop input first: nobody drains the queue during the teardown, and a full queue would
    // block the GUI thread inside the input callback.
    gui_remove_view_port(gui, view_port);
    furi_timer_stop(heartbeat);
    furi_timer_free(heartbeat);

    // Tell the board to drop WiFi before the port closes, so it waits for the next run
    furi_hal_serial_tx(serial, (const uint8_t[]){WifiLinkCommandStop}, 1);
    furi_hal_serial_tx_wait_complete(serial);
    furi_hal_serial_async_rx_stop(serial);
    furi_hal_serial_deinit(serial);
    furi_hal_serial_control_release(serial);
    expansion_enable(expansion);
    furi_record_close(RECORD_EXPANSION);

    notification_message(notifications, &sequence_blink_stop);
    notification_message(notifications, &sequence_reset_rgb);
    notification_message(notifications, &sequence_display_backlight_enforce_auto);
    furi_record_close(RECORD_NOTIFICATION);

    view_port_free(view_port);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(queue);
    furi_mutex_free(state.mutex);
}

static void wifi_link_draw(Canvas* canvas, void* context) {
    WifiLinkState* state = context;
    furi_mutex_acquire(state->mutex, FuriWaitForever);

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "Snake Pad");

    canvas_set_font(canvas, FontSecondary);
    const char* board = "Board not answering";
    const char* detail = "";
    char ping[24];
    switch(state->board) {
    case WifiLinkBoardWaiting:
        board = state->settings_problem ? state->settings_problem : "Board ready";
        // The full path is wider than the screen; apps_data is where every app keeps its files
        if(state->settings_problem) detail = "snake_pad/wifi.txt";
        break;
    case WifiLinkBoardJoining:
        board = "Joining hotspot";
        break;
    case WifiLinkBoardHotspotNotFound:
        board = "Hotspot not found";
        // The ESP32-S2 has no 5 GHz radio, so a 5 GHz-only hotspot never shows up
        detail = "Needs 2.4 GHz";
        break;
    case WifiLinkBoardHotspotRefused:
        board = "Hotspot refused password";
        break;
    case WifiLinkBoardReachingServer:
        board = "Reaching server";
        break;
    case WifiLinkBoardServerRefused:
        board = "Server refused token";
        break;
    case WifiLinkBoardConnected:
        board = "Connected";
        if(state->pinged) {
            snprintf(ping, sizeof(ping), "Ping: %lu ms", state->ping_ms);
            detail = ping;
        }
        break;
    default:
        break;
    }
    canvas_draw_str(canvas, 2, 22, board);
    canvas_draw_str(canvas, 2, 31, detail);

    char sent[24];
    snprintf(sent, sizeof(sent), "Sent: %lu", state->presses_sent);
    canvas_draw_str(canvas, 2, 40, sent);

    canvas_draw_line(canvas, 0, 52, 127, 52);
    canvas_draw_str(canvas, 2, 62, "OK:mode   Hold Back:exit");

    furi_mutex_release(state->mutex);
}

static void wifi_link_forward_input(InputEvent* input, void* context) {
    WifiLinkEvent event = {.type = WifiLinkEventInput, .input = *input};
    furi_message_queue_put(context, &event, FuriWaitForever);
}

// Runs in the UART interrupt, so nothing here may wait
static void wifi_link_forward_board_byte(
    FuriHalSerialHandle* serial,
    FuriHalSerialRxEvent event,
    void* context) {
    if(!(event & FuriHalSerialRxEventData)) return;
    const uint8_t byte = furi_hal_serial_async_rx(serial);
    // Drop the ESP32's boot text here, or its ~100 bytes at reset would flood the queue
    if(byte < WifiLinkBoardWaiting || byte > WifiLinkBoardHeartbeatAnswered) return;
    WifiLinkEvent board_event = {.type = WifiLinkEventBoardByte, .board_byte = byte};
    furi_message_queue_put(context, &board_event, 0);
}

// Runs on the timer thread, which must not block on a full queue
static void wifi_link_forward_tick(void* context) {
    furi_message_queue_put(context, &(WifiLinkEvent){.type = WifiLinkEventTick}, 0);
}

// Reads the settings from the SD card each time the board asks, so the hotspot password and
// pad token never live in the app. Returns what is wrong with the file, or NULL once sent.
static const char* wifi_link_answer_settings_request(FuriHalSerialHandle* serial) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* file = flipper_format_file_alloc(storage);
    FuriString* settings = furi_string_alloc();
    furi_string_push_back(settings, WifiLinkCommandSettings);
    FuriString* value = furi_string_alloc();
    const char* problem = NULL;

    if(!flipper_format_file_open_existing(file, APP_DATA_PATH("wifi.txt"))) {
        problem = "No settings file";
    } else {
        // In the order the board reads them. Rewind before each, so the file may list them in
        // any order. flipper_format already drops the \r of a CRLF file and keeps spaces inside
        // a value, which a password may have.
        static const char* const keys[] = {"Hotspot", "Password", "Server", "Token"};
        for(size_t i = 0; i < COUNT_OF(keys) && !problem; i++) {
            if(flipper_format_rewind(file) && flipper_format_read_string(file, keys[i], value)) {
                furi_string_cat(settings, value);
                furi_string_push_back(settings, '\n');
            } else {
                problem = "Settings file incomplete";
            }
        }
    }

    if(!problem) {
        furi_hal_serial_tx(
            serial, (const uint8_t*)furi_string_get_cstr(settings), furi_string_size(settings));
    }

    furi_string_free(value);
    furi_string_free(settings);
    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);
    return problem;
}
