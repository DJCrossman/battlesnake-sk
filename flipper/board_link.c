#include <furi.h>
#include <furi_hal_bt.h>
#include <furi_hal_version.h>
#include <ble/core/ble_defs.h>
#include <bt/bt_service/bt.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <services/battery_service.h>
#include <services/dev_info_service.h>
#include <services/serial_service.h>
#include <storage/storage.h>

#include "board_frame.h"
#include "links.h"

// Flow-control credit announced to the phone. The receive buffer holds twice this, because the
// firmware can re-announce full credit while up to one credit's worth of bytes is still unread.
#define BOARD_LINK_RX_CREDIT 1024
// An unconfirmed indication (phone not subscribed, or link gone) stops blocking TX after this.
#define BOARD_LINK_TX_CONFIRM_TIMEOUT_MS 500

typedef enum {
    BoardLinkEventInput,
    BoardLinkEventBtStatus,
    BoardLinkEventDataReceived,
    BoardLinkEventDataSent,
} BoardLinkEventType;

typedef struct {
    BoardLinkEventType type;
    union {
        InputEvent input;
        BtStatus bt_status;
    };
} BoardLinkEvent;

// The firmware's serial service under our own profile. With the stock ble_profile_serial the
// Bluetooth service would open an RPC session on connect and take every received byte, and the
// stock profile also advertises with the Flipper's own name and address.
typedef struct {
    FuriHalBleProfileBase base;
    BleServiceDevInfo* dev_info;
    BleServiceBattery* battery;
    BleServiceSerial* serial;
} BoardLinkProfile;

typedef struct {
    FuriMessageQueue* queue;
    FuriStreamBuffer* rx;
    volatile bool rx_overflowed; // set on the Bluetooth thread when received bytes did not fit
    BoardFrameParser parser;
    BoardFrame incoming;

    uint8_t tx_pending[32];
    uint8_t tx_pending_len;
    bool tx_in_flight;
    uint32_t tx_sent_tick;

    // Shared with the draw callback, guarded by mutex
    FuriMutex* mutex;
    char adv_name[FURI_HAL_VERSION_DEVICE_NAME_LENGTH];
    bool connected;
    bool has_frame;
    BoardFrame frame;
    uint32_t frame_tick;
} BoardLink;

static void board_link_draw(Canvas* canvas, void* context);
static void board_link_draw_cell(Canvas* canvas, BoardCell cell, uint8_t px, uint8_t py, uint8_t size);
static void board_link_forward_input(InputEvent* input, void* context);
static FuriHalBleProfileBase* board_link_profile_start(FuriHalBleProfileParams params);
static void board_link_profile_stop(FuriHalBleProfileBase* profile);
static void board_link_profile_gap_config(GapConfig* config, FuriHalBleProfileParams params);
static uint16_t board_link_on_serial_event(SerialServiceEvent event, void* context);
static void board_link_forward_bt_status(BtStatus status, void* context);
static void board_link_send_pending(BoardLink* link, BoardLinkProfile* profile);

static const FuriHalBleProfileTemplate board_link_profile = {
    .start = board_link_profile_start,
    .stop = board_link_profile_stop,
    .get_gap_config = board_link_profile_gap_config,
};

// Byte sent to the phone for each button press
static const uint8_t board_link_button_byte[InputKeyMAX] = {
    [InputKeyUp] = 0,
    [InputKeyDown] = 1,
    [InputKeyLeft] = 2,
    [InputKeyRight] = 3,
    [InputKeyOk] = 4,
};

void board_link_run(void) {
    BoardLink* link = malloc(sizeof(BoardLink));
    memset(link, 0, sizeof(BoardLink));
    link->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    link->queue = furi_message_queue_alloc(16, sizeof(BoardLinkEvent));
    link->rx = furi_stream_buffer_alloc(2 * BOARD_LINK_RX_CREDIT, 1);
    // Same name the profile advertises
    const char* flipper_name = furi_hal_version_get_name_ptr();
    snprintf(
        link->adv_name, sizeof(link->adv_name), "Board %s", flipper_name ? flipper_name : "");

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, board_link_draw, link);
    view_port_input_callback_set(view_port, board_link_forward_input, link->queue);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    NotificationApp* notifications = furi_record_open(RECORD_NOTIFICATION);
    notification_message(notifications, &sequence_display_backlight_enforce_on);
    notification_message(notifications, &sequence_blink_start_blue);

    // Same sequence as the keyboard link, with its own bonding-key file
    Bt* bt = furi_record_open(RECORD_BT);
    bt_disconnect(bt);
    furi_delay_ms(200); // Wait for the 2nd core to update NVM storage
    bt_keys_storage_set_storage_path(bt, APP_DATA_PATH(".bt_board.keys"));
    BoardLinkProfile* profile = (BoardLinkProfile*)bt_profile_start(bt, &board_link_profile, NULL);
    furi_check(profile);
    ble_svc_serial_set_callbacks(
        profile->serial, BOARD_LINK_RX_CREDIT, board_link_on_serial_event, link);
    furi_hal_bt_start_advertising();
    bt_set_status_changed_callback(bt, board_link_forward_bt_status, link->queue);

    BoardLinkEvent event;
    bool running = true;
    while(running) {
        // Wakes at least every 50 ms to animate the hold bar and catch dropped wake-ups
        if(furi_message_queue_get(link->queue, &event, 50) == FuriStatusOk) {
            if(event.type == BoardLinkEventBtStatus) {
                const bool connected = event.bt_status == BtStatusConnected;
                if(connected && !link->connected) {
                    notification_message(notifications, &sequence_blink_stop);
                    notification_message(notifications, &sequence_set_only_green_255);
                } else if(!connected && link->connected) {
                    notification_message(notifications, &sequence_reset_rgb);
                    notification_message(notifications, &sequence_blink_start_blue);
                }
                furi_mutex_acquire(link->mutex, FuriWaitForever);
                link->connected = connected;
                if(!connected) link->has_frame = false;
                furi_mutex_release(link->mutex);
                board_frame_parser_reset(&link->parser);
                link->tx_pending_len = 0;
                link->tx_in_flight = false;
            } else if(event.type == BoardLinkEventDataSent) {
                link->tx_in_flight = false;
            } else if(event.type == BoardLinkEventInput && event.input.key == InputKeyBack) {
                running = event.input.type != InputTypeLong;
            } else if(event.type == BoardLinkEventInput && event.input.type == InputTypePress) {
                if(link->tx_pending_len < sizeof(link->tx_pending)) {
                    link->tx_pending[link->tx_pending_len++] =
                        board_link_button_byte[event.input.key];
                }
            }
        }

        board_link_send_pending(link, profile);

        // Read everything the phone sent, then let the firmware re-announce credit. Bytes that
        // land between the last read and the announcement are read on the next pass, and fit
        // because the buffer holds twice the credit.
        bool received;
        do {
            received = false;
            uint8_t chunk[64];
            size_t chunk_len;
            while((chunk_len = furi_stream_buffer_receive(link->rx, chunk, sizeof(chunk), 0)) >
                  0) {
                received = true;
                for(size_t i = 0; i < chunk_len; i++) {
                    if(!board_frame_parser_feed(
                           &link->parser, chunk[i], furi_get_tick(), &link->incoming)) {
                        continue;
                    }
                    furi_mutex_acquire(link->mutex, FuriWaitForever);
                    link->frame = link->incoming;
                    link->has_frame = true;
                    link->frame_tick = furi_get_tick();
                    furi_mutex_release(link->mutex);
                    if(link->incoming.flags & BOARD_FLAG_GAME_STARTED) {
                        notification_message(notifications, &sequence_single_vibro);
                    }
                }
            }
            if(received) ble_svc_serial_notify_buffer_is_empty(profile->serial);
        } while(received);

        // Bytes were lost: drop the frame they belonged to rather than mis-parse it
        if(link->rx_overflowed) {
            link->rx_overflowed = false;
            board_frame_parser_reset(&link->parser);
        }

        view_port_update(view_port);
    }

    // Stop input first: nobody drains the queue during the Bluetooth teardown
    gui_remove_view_port(gui, view_port);

    // Restore Bluetooth exactly as the keyboard link does. Disconnect before clearing the serial
    // callbacks: once no peer is connected nothing can write or confirm, so the firmware cannot
    // call a callback as it is being cleared.
    bt_set_status_changed_callback(bt, NULL, NULL);
    bt_disconnect(bt);
    furi_delay_ms(200); // Wait for the 2nd core to update NVM storage
    ble_svc_serial_set_callbacks(profile->serial, 0, NULL, NULL);
    bt_keys_storage_set_default_path(bt);
    furi_check(bt_profile_restore_default(bt));
    furi_record_close(RECORD_BT);

    notification_message(notifications, &sequence_blink_stop);
    notification_message(notifications, &sequence_reset_rgb);
    notification_message(notifications, &sequence_display_backlight_enforce_auto);
    furi_record_close(RECORD_NOTIFICATION);

    view_port_free(view_port);
    furi_record_close(RECORD_GUI);
    furi_stream_buffer_free(link->rx);
    furi_message_queue_free(link->queue);
    furi_mutex_free(link->mutex);
    free(link);
}

static void board_link_draw(Canvas* canvas, void* context) {
    BoardLink* link = context;
    furi_mutex_acquire(link->mutex, FuriWaitForever);
    const BoardFrame* frame = &link->frame;
    canvas_clear(canvas);

    if(!link->connected) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 11, "Snake Pad: Board");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 26, "Pair me:");
        canvas_draw_str(canvas, 2, 37, link->adv_name);
        canvas_draw_str(canvas, 2, 62, "Hold Back: exit");
    } else if(!link->has_frame || (frame->flags & BOARD_FLAG_WAITING)) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 11, "Snake Pad: Board");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 26, "Connected");
        canvas_draw_str(canvas, 2, 37, "Waiting for a game");
        if(link->has_frame && (frame->flags & BOARD_FLAG_SERVER_LOST)) {
            canvas_draw_str(canvas, 2, 48, "Phone lost the server");
        }
        canvas_draw_str(canvas, 2, 62, "Hold Back: exit");
    } else {
        // Board in a 64 px square on the left, framed, y = 0 at the bottom
        canvas_set_font(canvas, FontSecondary);
        if((frame->flags & BOARD_FLAG_TOO_LARGE) || frame->width == 0 || frame->height == 0) {
            canvas_draw_str(canvas, 2, 28, "Board too");
            canvas_draw_str(canvas, 2, 38, "large to show");
        } else {
            const uint8_t cell =
                62 / (frame->width > frame->height ? frame->width : frame->height);
            const uint8_t x0 = 1 + (62 - frame->width * cell) / 2;
            const uint8_t y0 = 1 + (62 - frame->height * cell) / 2;
            canvas_draw_frame(
                canvas, x0 - 1, y0 - 1, frame->width * cell + 2, frame->height * cell + 2);
            for(uint8_t y = 0; y < frame->height; y++) {
                for(uint8_t x = 0; x < frame->width; x++) {
                    board_link_draw_cell(
                        canvas,
                        board_frame_cell_at(frame, x, y),
                        x0 + x * cell,
                        y0 + (frame->height - 1 - y) * cell,
                        cell);
                }
            }
        }

        char line[24];
        snprintf(line, sizeof(line), "Turn %u", frame->turn);
        canvas_draw_str(canvas, 68, 9, line);
        snprintf(line, sizeof(line), "Health %u", frame->health);
        canvas_draw_str(canvas, 68, 19, line);
        snprintf(line, sizeof(line), "Length %u", frame->length);
        canvas_draw_str(canvas, 68, 29, line);
        static const char* const mode_names[] = {"Assist", "Manual", "Auto", "Mode ?"};
        canvas_draw_str(canvas, 68, 39, mode_names[BOARD_FLAG_MODE(frame->flags)]);

        if(frame->flags & BOARD_FLAG_SERVER_LOST) {
            canvas_draw_str(canvas, 68, 50, "Server lost");
        } else if(frame->flags & BOARD_FLAG_GAME_OVER) {
            canvas_draw_str(canvas, 68, 50, "Game over");
        }

        // Time left to press, counted down locally from the frame's arrival
        const uint32_t hold_ms = frame->hold_10ms * 10;
        const uint32_t elapsed_ms = furi_get_tick() - link->frame_tick;
        if(hold_ms > 0 && !(frame->flags & BOARD_FLAG_GAME_OVER)) {
            canvas_draw_frame(canvas, 68, 56, 58, 7);
            canvas_draw_box(
                canvas, 69, 57, elapsed_ms < hold_ms ? 56 * (hold_ms - elapsed_ms) / hold_ms : 0, 5);
        }
    }

    furi_mutex_release(link->mutex);
}

// How each kind of cell looks at a given cell size. Mine is solid with a hollow-centred head;
// other snakes are outlined (or a sparse pattern at small sizes) with a filled-centre head.
static void board_link_draw_cell(Canvas* canvas, BoardCell cell, uint8_t px, uint8_t py, uint8_t size) {
    const uint8_t mid = size / 2;
    switch(cell) {
    case BoardCellEmpty:
        break;
    case BoardCellFood:
        if(size >= 5) {
            canvas_draw_disc(canvas, px + mid, py + mid, size / 4);
        } else {
            canvas_draw_dot(canvas, px + mid, py + mid);
        }
        break;
    case BoardCellHazard:
        for(uint8_t dy = 0; dy < size; dy += 2) {
            for(uint8_t dx = 0; dx < size; dx += 2) {
                canvas_draw_dot(canvas, px + dx, py + dy);
            }
        }
        break;
    case BoardCellMyBody:
        canvas_draw_box(canvas, px, py, size, size);
        break;
    case BoardCellMyHead:
        canvas_draw_box(canvas, px, py, size, size);
        if(size >= 3) {
            const uint8_t hole = size >= 8 ? size / 4 : 1;
            canvas_set_color(canvas, ColorWhite);
            canvas_draw_box(canvas, px + (size - hole) / 2, py + (size - hole) / 2, hole, hole);
            canvas_set_color(canvas, ColorBlack);
        }
        break;
    case BoardCellOtherBody:
    case BoardCellOtherHead:
        if(size >= 5) {
            canvas_draw_frame(canvas, px, py, size, size);
            if(cell == BoardCellOtherHead) {
                canvas_draw_box(canvas, px + 2, py + 2, size - 4, size - 4);
            }
        } else {
            for(uint8_t dy = 0; dy < size; dy++) {
                for(uint8_t dx = 0; dx < size; dx++) {
                    if((dx + dy) % 2 == 1) canvas_draw_dot(canvas, px + dx, py + dy);
                }
            }
            if(cell == BoardCellOtherHead) canvas_draw_dot(canvas, px, py);
        }
        break;
    }
}

static void board_link_forward_input(InputEvent* input, void* context) {
    BoardLinkEvent event = {.type = BoardLinkEventInput, .input = *input};
    furi_message_queue_put(context, &event, FuriWaitForever);
}

static FuriHalBleProfileBase* board_link_profile_start(FuriHalBleProfileParams params) {
    UNUSED(params);
    BoardLinkProfile* profile = malloc(sizeof(BoardLinkProfile));
    profile->base.config = &board_link_profile;
    profile->dev_info = ble_svc_dev_info_start();
    profile->battery = ble_svc_battery_start(true);
    profile->serial = ble_svc_serial_start();
    return &profile->base;
}

static void board_link_profile_stop(FuriHalBleProfileBase* base) {
    furi_check(base && base->config == &board_link_profile);
    BoardLinkProfile* profile = (BoardLinkProfile*)base;
    ble_svc_battery_stop(profile->battery);
    ble_svc_dev_info_stop(profile->dev_info);
    ble_svc_serial_stop(profile->serial);
    free(profile);
}

// The stock serial profile's GAP settings, with our own name and address so the phone sees a
// separate device from the Flipper it already knows.
static void board_link_profile_gap_config(GapConfig* config, FuriHalBleProfileParams params) {
    UNUSED(params);
    furi_check(config);
    *config = (GapConfig){
        .adv_service =
            {
                .UUID_Type = UUID_TYPE_16,
                .Service_UUID_16 = 0x3080 | furi_hal_version_get_hw_color(),
            },
        .appearance_char = 0x8600,
        .bonding_mode = true,
        .pairing_method = GapPairingPinCodeShow,
        .conn_param =
            {
                .conn_int_min = 0x06, // 7.5 ms
                .conn_int_max = 0x24, // 45 ms
            },
    };
    memcpy(config->mac_address, furi_hal_version_get_ble_mac(), sizeof(config->mac_address));
    config->mac_address[0] ^= 0x44; // mac_xor 0x4244 ("BD"); the keyboard link uses 0x5350
    config->mac_address[1] ^= 0x42;
    const char* flipper_name = furi_hal_version_get_name_ptr();
    snprintf(
        config->adv_name,
        sizeof(config->adv_name),
        "%cBoard %s",
        furi_hal_version_get_ble_local_device_name_ptr()[0], // advertising data type byte
        flipper_name ? flipper_name : "");
}

// Runs on the Bluetooth GAP thread with the serial service locked: hand the bytes to the main
// loop and never block here.
static uint16_t board_link_on_serial_event(SerialServiceEvent event, void* context) {
    BoardLink* link = context;
    if(event.event == SerialServiceEventTypeDataReceived) {
        if(furi_stream_buffer_send(link->rx, event.data.buffer, event.data.size, 0) <
           event.data.size) {
            link->rx_overflowed = true;
        }
        BoardLinkEvent wake = {.type = BoardLinkEventDataReceived};
        furi_message_queue_put(link->queue, &wake, 0);
    } else if(event.event == SerialServiceEventTypeDataSent) {
        BoardLinkEvent sent = {.type = BoardLinkEventDataSent};
        furi_message_queue_put(link->queue, &sent, 0);
    }
    // SerialServiceEventTypesBleResetRequest (RPC status written to 0): there is no RPC here
    return furi_stream_buffer_spaces_available(link->rx);
}

static void board_link_forward_bt_status(BtStatus status, void* context) {
    BoardLinkEvent event = {.type = BoardLinkEventBtStatus, .bt_status = status};
    furi_message_queue_put(context, &event, FuriWaitForever);
}

// Button bytes go out as one indication at a time; presses made while one is unconfirmed are
// batched into the next.
static void board_link_send_pending(BoardLink* link, BoardLinkProfile* profile) {
    if(link->tx_in_flight &&
       furi_get_tick() - link->tx_sent_tick > BOARD_LINK_TX_CONFIRM_TIMEOUT_MS) {
        link->tx_in_flight = false;
    }
    if(link->tx_in_flight || link->tx_pending_len == 0) return;

    // Without a connection there is no one to send to; the bytes are dropped either way
    if(link->connected &&
       ble_svc_serial_update_tx(profile->serial, link->tx_pending, link->tx_pending_len)) {
        link->tx_in_flight = true;
        link->tx_sent_tick = furi_get_tick();
    }
    link->tx_pending_len = 0;
}
