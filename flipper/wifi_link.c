#include <furi.h>
#include <furi_hal_serial.h>
#include <furi_hal_serial_control.h>
#include <expansion/expansion.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>

#include "board_screen.h"
#include "wifi_settings.h"

// What the Flipper sends the WiFi Devboard over the header UART
typedef enum {
    WifiLinkCommandHeartbeat = 5,
    WifiLinkCommandStop = 6,
    WifiLinkCommandSettings = 0x10,
} WifiLinkCommand;

// What the board sends back. Every byte in 0xA0..0xA7 is one of these. The server's mode bytes
// and board frames come through it unchanged (board_frame.h), and anything else is the ESP32's
// boot text.
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
    WifiLinkEventBoardBytes, // the board sent bytes, which wait in WifiLinkRx
    WifiLinkEventTick,
    WifiLinkEventGameOverDone, // a game over has been on the screen for 15 s
} WifiLinkEventType;

typedef struct {
    WifiLinkEventType type;
    InputEvent input;
} WifiLinkEvent;

// Filled by the UART interrupt: every byte the board sends, and one wake-up at a time
typedef struct {
    FuriMessageQueue* queue;
    FuriStreamBuffer* bytes;
    volatile bool woken; // a wake-up is in the queue, so more bytes need none
} WifiLinkRx;

// The press bytes the server reads, the same ones the pad page sends
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
    FuriString* hotspot; // the one the board was last sent, so the screen can say which
    bool pinged;
    uint32_t ping_ms;
    uint32_t presses_sent;
    BoardScreen screen;
} WifiLinkState;

static void wifi_link_draw(Canvas* canvas, void* context);
static void wifi_link_forward_input(InputEvent* input, void* context);
static void wifi_link_forward_board_bytes(
    FuriHalSerialHandle* serial,
    FuriHalSerialRxEvent event,
    void* context);
static void wifi_link_forward_tick(void* context);
static void wifi_link_forward_game_over_done(void* context);
static void wifi_link_set_board(
    WifiLinkState* state,
    NotificationApp* notifications,
    WifiLinkBoardByte board);
static const char* wifi_link_answer_settings_request(
    FuriHalSerialHandle* serial,
    uint32_t* hotspot_index,
    FuriString* hotspot);

// A WiFi Devboard on the GPIO header holds the connection to the game server, and the Flipper
// sends it the settings from the SD card and the button bytes. Needs a hotspot. Runs until the
// player holds Back.
int32_t snake_pad_app(void* p) {
    UNUSED(p);
    // Frames and the parser live on the heap, as the stack is 4 KB. Zeroed, the parser waits for
    // a frame start and a frame is an empty board.
    WifiLinkState state = {
        .mutex = furi_mutex_alloc(FuriMutexTypeNormal),
        .hotspot = furi_string_alloc(),
        .screen =
            {.shown = calloc(1, sizeof(BoardFrame)), .previous = calloc(1, sizeof(BoardFrame))},
    };
    FuriMessageQueue* queue = furi_message_queue_alloc(16, sizeof(WifiLinkEvent));
    BoardFrameParser* parser = calloc(1, sizeof(BoardFrameParser));
    // The parser decodes into the spare frame, which then takes the shown one's place
    BoardFrame* spare = calloc(1, sizeof(BoardFrame));
    WifiLinkRx rx = {.queue = queue, .bytes = furi_stream_buffer_alloc(1024, 1)};

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
    furi_hal_serial_async_rx_start(serial, wifi_link_forward_board_bytes, &rx, false);

    // A board still set up by an earlier run starts over and asks for the settings again
    furi_hal_serial_tx(serial, (const uint8_t[]){WifiLinkCommandStop}, 1);

    // First heartbeat now, then every 2 s
    furi_message_queue_put(queue, &(WifiLinkEvent){.type = WifiLinkEventTick}, 0);
    FuriTimer* heartbeat = furi_timer_alloc(wifi_link_forward_tick, FuriTimerTypePeriodic, queue);
    furi_timer_start(heartbeat, furi_ms_to_ticks(2000));
    // A game over stays up for 15 s from its frame's arrival, then the status screen returns
    FuriTimer* game_over =
        furi_timer_alloc(wifi_link_forward_game_over_done, FuriTimerTypeOnce, queue);

    uint32_t unanswered_heartbeats = 0;
    uint32_t heartbeat_sent_at = 0;
    uint32_t hotspot_index = 0;
    WifiLinkEvent event;
    bool running = true;
    bool moving = false;
    while(running) {
        // While the board screen moves, it is redrawn about 30 times a second
        const FuriStatus got =
            furi_message_queue_get(queue, &event, moving ? furi_ms_to_ticks(33) : FuriWaitForever);
        furi_check(got == FuriStatusOk || got == FuriStatusErrorTimeout);
        furi_mutex_acquire(state.mutex, FuriWaitForever);

        if(got == FuriStatusErrorTimeout) {
            // Only the redraw
        } else if(event.type == WifiLinkEventTick) {
            if(unanswered_heartbeats >= 3) {
                wifi_link_set_board(&state, notifications, WifiLinkBoardSilent);
            }
            heartbeat_sent_at = furi_get_tick();
            unanswered_heartbeats++;
            furi_hal_serial_tx(serial, (const uint8_t[]){WifiLinkCommandHeartbeat}, 1);
        } else if(event.type == WifiLinkEventBoardBytes) {
            rx.woken = false;
        } else if(event.type == WifiLinkEventGameOverDone) {
            // Not if a newer frame has come: more turns after all, or a game over whose 15 s
            // started while this waited in the queue
            if((state.screen.shown->flags & BOARD_FLAG_GAME_OVER) &&
               furi_get_tick() - state.screen.shown_tick >= furi_ms_to_ticks(15000)) {
                state.screen.has_frame = false;
            }
        } else if(event.input.key == InputKeyBack) {
            running = event.input.type != InputTypeLong;
        } else if(event.input.type == InputTypePress) {
            furi_hal_serial_tx(serial, &wifi_link_button_byte[event.input.key], 1);
            if(state.board == WifiLinkBoardConnected) state.presses_sent++;
        }

        // On every pass, as the wake-up for these bytes may not have fit in the queue
        uint8_t byte;
        while(furi_stream_buffer_receive(rx.bytes, &byte, 1, 0) == 1) {
            if(byte >= WifiLinkBoardWaiting && byte <= WifiLinkBoardHeartbeatAnswered) {
                if(byte == WifiLinkBoardHeartbeatAnswered) {
                    state.ping_ms = furi_get_tick() - heartbeat_sent_at;
                    state.pinged = true;
                } else {
                    unanswered_heartbeats = 0;
                    // The next hotspot in the file gets its turn. The board answers every
                    // heartbeat with its state, so only the first "not found" of a try counts.
                    // Stop makes the board start over and ask for the settings again.
                    if(byte == WifiLinkBoardHotspotNotFound &&
                       state.board != WifiLinkBoardHotspotNotFound) {
                        hotspot_index++;
                        furi_hal_serial_tx(serial, (const uint8_t[]){WifiLinkCommandStop}, 1);
                    }
                    wifi_link_set_board(&state, notifications, byte);
                    if(state.board == WifiLinkBoardWaiting) {
                        state.settings_problem = wifi_link_answer_settings_request(
                            serial, &hotspot_index, state.hotspot);
                    }
                }
            } else if(byte >= BOARD_MODE_BYTE_ASSIST && byte <= BOARD_MODE_BYTE_AUTO) {
                // In BoardMode's order
                state.screen.mode = BoardModeAssist + (byte - BOARD_MODE_BYTE_ASSIST);
            } else if(board_frame_parser_feed(parser, byte, spare)) {
                // The frame shown so far becomes the previous one, for the loss animation
                BoardFrame* const oldest = state.screen.previous;
                state.screen.previous = state.screen.shown;
                state.screen.shown = spare;
                spare = oldest;
                state.screen.has_frame = true;
                state.screen.shown_tick = furi_get_tick();
                // The start plays on over the turn-0 frames right behind it
                if(state.screen.shown->flags & BOARD_FLAG_GAME_STARTED) {
                    state.screen.started_tick = state.screen.shown_tick;
                }
                // Each game over restarts the 15 s. After any other frame they run out unheeded.
                if(state.screen.shown->flags & BOARD_FLAG_GAME_OVER) {
                    furi_timer_start(game_over, furi_ms_to_ticks(15000));
                }
                // Half the ping is about how long the frame took to get here
                const uint32_t hold_ms = state.screen.shown->hold_10ms * 10;
                const uint32_t late_ms = state.pinged ? state.ping_ms / 2 : 0;
                state.screen.hold_ms = hold_ms > late_ms ? hold_ms - late_ms : 0;
                const NotificationSequence* sequence =
                    board_screen_arrival_sequence(state.screen.shown);
                if(sequence) notification_message(notifications, sequence);
            }
        }

        moving = state.board == WifiLinkBoardConnected && board_screen_is_moving(&state.screen);
        furi_mutex_release(state.mutex);
        view_port_update(view_port);
    }

    // Stop input first: nobody drains the queue during the teardown, and a full queue would
    // block the GUI thread inside the input callback.
    gui_remove_view_port(gui, view_port);
    furi_timer_stop(heartbeat);
    furi_timer_free(heartbeat);
    furi_timer_stop(game_over);
    furi_timer_free(game_over);

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
    // A win or loss tune lives in the app, so the app waits for the last one to finish
    notification_message_block(notifications, &sequence_empty);
    furi_record_close(RECORD_NOTIFICATION);

    view_port_free(view_port);
    furi_record_close(RECORD_GUI);
    // Receiving stopped above, so the interrupt no longer writes to these
    furi_stream_buffer_free(rx.bytes);
    furi_message_queue_free(queue);
    free(spare);
    free(parser);
    free(state.screen.previous);
    free(state.screen.shown);
    furi_string_free(state.hotspot);
    furi_mutex_free(state.mutex);
    return 0;
}

static void wifi_link_draw(Canvas* canvas, void* context) {
    WifiLinkState* state = context;
    furi_mutex_acquire(state->mutex, FuriWaitForever);
    if(state->board == WifiLinkBoardConnected && state->screen.has_frame) {
        board_screen_draw(canvas, &state->screen);
        furi_mutex_release(state->mutex);
        return;
    }

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "Battlesnake");

    canvas_set_font(canvas, FontSecondary);
    const char* board = "Board not answering";
    const char* detail = "";
    char ping[24];
    switch(state->board) {
    case WifiLinkBoardWaiting:
        board = state->settings_problem ? state->settings_problem : "Board ready";
        // The full path is wider than the screen; apps_data is where every app keeps its files
        if(state->settings_problem) detail = "snake_pad/wifi.json";
        break;
    case WifiLinkBoardJoining:
        board = "Joining hotspot";
        detail = furi_string_get_cstr(state->hotspot);
        break;
    case WifiLinkBoardHotspotNotFound:
        board = "Hotspot not found";
        detail = furi_string_get_cstr(state->hotspot);
        break;
    case WifiLinkBoardHotspotRefused:
        board = "Hotspot refused password";
        detail = furi_string_get_cstr(state->hotspot);
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

    if(state->screen.mode != BoardModeUnknown) {
        char mode[24];
        snprintf(mode, sizeof(mode), "Mode: %s", board_screen_mode_names[state->screen.mode]);
        canvas_draw_str(canvas, 2, 49, mode);
    }

    canvas_draw_line(canvas, 0, 52, 127, 52);
    canvas_draw_str(canvas, 2, 62, "OK:mode   Hold Back:exit");

    furi_mutex_release(state->mutex);
}

static void wifi_link_forward_input(InputEvent* input, void* context) {
    WifiLinkEvent event = {.type = WifiLinkEventInput, .input = *input};
    furi_message_queue_put(context, &event, FuriWaitForever);
}

// Runs in the UART interrupt, so nothing here may wait. The main loop reads every waiting byte on
// each pass, so one queued wake-up at a time is enough: a frame or the ESP32's boot text at reset
// cannot flood the queue, and one that did not fit is tried again with the next byte.
static void wifi_link_forward_board_bytes(
    FuriHalSerialHandle* serial,
    FuriHalSerialRxEvent event,
    void* context) {
    WifiLinkRx* rx = context;
    if(!(event & FuriHalSerialRxEventData)) return;
    while(furi_hal_serial_async_rx_available(serial)) {
        const uint8_t byte = furi_hal_serial_async_rx(serial);
        furi_stream_buffer_send(rx->bytes, &byte, 1, 0);
    }
    if(!rx->woken) {
        WifiLinkEvent board_event = {.type = WifiLinkEventBoardBytes};
        rx->woken = furi_message_queue_put(rx->queue, &board_event, 0) == FuriStatusOk;
    }
}

// Runs on the timer thread, which must not block on a full queue
static void wifi_link_forward_tick(void* context) {
    furi_message_queue_put(context, &(WifiLinkEvent){.type = WifiLinkEventTick}, 0);
}

// Also on the timer thread
static void wifi_link_forward_game_over_done(void* context) {
    furi_message_queue_put(context, &(WifiLinkEvent){.type = WifiLinkEventGameOverDone}, 0);
}

// The board's state, from its byte or its silence. The LED follows it onto and off the server,
// and each connection starts with no ping, board or mode until the server sends them.
static void wifi_link_set_board(
    WifiLinkState* state,
    NotificationApp* notifications,
    WifiLinkBoardByte board) {
    const bool was_connected = state->board == WifiLinkBoardConnected;
    state->board = board;
    const bool connected = state->board == WifiLinkBoardConnected;
    if(connected && !was_connected) {
        notification_message(notifications, &sequence_blink_stop);
        notification_message(notifications, &sequence_set_only_green_255);
        // A new connection shows no ping until the server answers it
        state->pinged = false;
    } else if(!connected && was_connected) {
        notification_message(notifications, &sequence_reset_rgb);
        notification_message(notifications, &sequence_blink_start_blue);
        // Forgotten as the board leaves rather than on connecting, because the server's mode byte
        // can come through just before the board says it is connected. The shown frame becomes
        // the next one's previous, so it is emptied rather than left from this connection.
        state->screen.has_frame = false;
        state->screen.mode = BoardModeUnknown;
        memset(state->screen.shown, 0, sizeof(BoardFrame));
    }
}

// Reads wifi.json (wifi_settings.h) from the SD card each time the board asks, so the hotspot
// passwords and pad token never live in the app. The board is sent the network at
// *hotspot_index, or the first one once the index has gone past the last, and its name is left
// in hotspot. Returns what is wrong with the file, or NULL once sent.
static const char* wifi_link_answer_settings_request(
    FuriHalSerialHandle* serial,
    uint32_t* hotspot_index,
    FuriString* hotspot) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    // On the heap, as the stack is 4 KB. One byte past the cap is read, so that a longer file is
    // refused rather than read in part.
    char* json = malloc(WIFI_SETTINGS_FILE_MAX + 1);
    FuriString* settings = furi_string_alloc();
    furi_string_push_back(settings, WifiLinkCommandSettings);
    const char* problem = NULL;

    if(!storage_file_open(file, APP_DATA_PATH("wifi.json"), FSAM_READ, FSOM_OPEN_EXISTING)) {
        problem = "No settings file";
    } else {
        const size_t len = storage_file_read(file, json, WIFI_SETTINGS_FILE_MAX + 1);
        WifiSettings values;
        WifiSettingsResult result = wifi_settings_for_network(json, len, *hotspot_index, &values);
        if(result == WifiSettingsPastLast) {
            *hotspot_index = 0;
            result = wifi_settings_for_network(json, len, 0, &values);
        }
        if(result == WifiSettingsFound) {
            furi_string_set_str(hotspot, values.hotspot);
            // In the order the board reads them
            furi_string_cat_printf(
                settings,
                "%s\n%s\n%s\n%s\n",
                values.hotspot,
                values.password,
                values.server,
                values.token);
        } else {
            problem = result == WifiSettingsInvalid ? "Settings file invalid" :
                                                      "Settings file incomplete";
        }
    }

    if(!problem) {
        furi_hal_serial_tx(
            serial, (const uint8_t*)furi_string_get_cstr(settings), furi_string_size(settings));
    }

    furi_string_free(settings);
    free(json);
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return problem;
}
