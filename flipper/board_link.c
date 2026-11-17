#include <furi.h>
#include <furi_hal_bt.h>
#include <furi_hal_version.h>
#include <ble/core/ble_defs.h>
#include <ble/core/ble_std.h>
#include <bt/bt_service/bt.h>
#include <furi_ble/event_dispatcher.h>
#include <furi_ble/gatt.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>

#include "board_frame.h"
#include "links.h"

// Flow-control credit announced to the phone. The receive buffer holds twice this, because the
// credit can be re-announced while up to one credit's worth of bytes is still unread.
#define BOARD_LINK_RX_CREDIT 1024
// An unconfirmed indication (phone not subscribed, or link gone) stops blocking TX after this.
#define BOARD_LINK_TX_CONFIRM_TIMEOUT_MS 500
// Longest value either way. The Flipper never starts an MTU exchange, so a value has to fit the
// default ATT MTU of 23 bytes less its 3-byte header; the page writes frames in 20-byte chunks.
#define BOARD_LINK_VALUE_LEN_MAX 20

// Vendor event codes from ST's STM32_WPAN ble/core/auto/ble_vs_codes.h, which the SDK does not ship
#define ACI_GATT_ATTRIBUTE_MODIFIED_VSEVT_CODE  0x0C01U
#define ACI_GATT_SERVER_CONFIRMATION_VSEVT_CODE 0x0C17U

// The packet the BLE event dispatcher hands to a service handler, from ST's STM32_WPAN
// interface/patterns/ble_thread/tl/tl.h, which the SDK does not ship
typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t data[1];
} hci_uart_pckt;

typedef struct __attribute__((packed)) {
    uint8_t evt;
    uint8_t plen;
    uint8_t data[1];
} hci_event_pckt;

typedef struct __attribute__((packed)) {
    uint16_t ecode;
    uint8_t data[1];
} evt_blecore_aci;

typedef enum {
    BoardLinkEventInput,
    BoardLinkEventLinkChanged,
    BoardLinkEventDataReceived,
    BoardLinkEventDataSent,
} BoardLinkEventType;

typedef struct {
    BoardLinkEventType type;
    InputEvent input;
} BoardLinkEvent;

typedef enum {
    BoardLinkCharRx, // page → Flipper: frame bytes
    BoardLinkCharTx, // Flipper → page: button bytes
    BoardLinkCharFlow, // Flipper → page: flow-control credit
    BoardLinkCharCount,
} BoardLinkChar;

// UUIDs are stored little-endian: the service is 4b514e43-fc33-4934-b8e7-4a26f71991ee and the
// characteristics 324ba1da-cf74-46e4-8654-b49ad6b8f401 (RX), …f402 (TX) and …f403 (flow).
static const Service_UUID_t board_link_service_uuid = {
    .Service_UUID_128 = {
        0xee, 0x91, 0x19, 0xf7, 0x26, 0x4a, 0xe7, 0xb8,
        0x34, 0x49, 0x33, 0xfc, 0x43, 0x4e, 0x51, 0x4b,
    }};

typedef struct {
    FuriMessageQueue* queue;
    FuriStreamBuffer* rx;
    volatile bool rx_overflowed; // set on the Bluetooth thread when received bytes did not fit
    // Set on the Bluetooth thread: true from the peer's first write or subscribe until it drops
    volatile bool peer_connected;
    // Bytes the phone may still write before the credit is announced again; changed on both
    // threads, inside critical sections
    uint16_t credit_left;
    BoardFrameParser parser;
    BoardFrame incoming;

    uint8_t tx_pending[BOARD_LINK_VALUE_LEN_MAX];
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

// Our own service under our own profile. Its characteristics need no encryption, so the phone
// connects without pairing (the firmware's serial service only works over an authenticated link,
// which takes a PIN), and the profile advertises with its own name and address.
typedef struct {
    FuriHalBleProfileBase base;
    BoardLink* link;
    uint16_t svc_handle;
    BleGattCharacteristicInstance chars[BoardLinkCharCount];
    GapSvcEventHandler* event_handler;
} BoardLinkProfile;

static void board_link_draw(Canvas* canvas, void* context);
static void board_link_draw_cell(Canvas* canvas, BoardCell cell, uint8_t px, uint8_t py, uint8_t size);
static void board_link_forward_input(InputEvent* input, void* context);
static FuriHalBleProfileBase* board_link_profile_start(FuriHalBleProfileParams params);
static void board_link_profile_stop(FuriHalBleProfileBase* profile);
static void board_link_profile_gap_config(GapConfig* config, FuriHalBleProfileParams params);
static bool board_link_tx_value(const void* context, const uint8_t** data, uint16_t* data_len);
static BleEventAckStatus board_link_on_ble_event(void* event, void* context);
static void board_link_send_pending(BoardLink* link, BoardLinkProfile* profile);

static const FuriHalBleProfileTemplate board_link_profile = {
    .start = board_link_profile_start,
    .stop = board_link_profile_stop,
    .get_gap_config = board_link_profile_gap_config,
};

// No security permissions anywhere: a failed read or write would make the phone start pairing.
static const BleGattCharacteristicParams board_link_chars[BoardLinkCharCount] = {
    [BoardLinkCharRx] =
        {.name = "Board RX",
         .data_prop_type = FlipperGattCharacteristicDataFixed,
         .data.fixed.length = BOARD_LINK_VALUE_LEN_MAX,
         .uuid.Char_UUID_128 =
             {0x01, 0xf4, 0xb8, 0xd6, 0x9a, 0xb4, 0x54, 0x86,
              0xe4, 0x46, 0x74, 0xcf, 0xda, 0xa1, 0x4b, 0x32},
         .uuid_type = UUID_TYPE_128,
         .char_properties = CHAR_PROP_WRITE_WITHOUT_RESP | CHAR_PROP_WRITE,
         .security_permissions = ATTR_PERMISSION_NONE,
         .gatt_evt_mask = GATT_NOTIFY_ATTRIBUTE_WRITE,
         .is_variable = CHAR_VALUE_LEN_VARIABLE},
    [BoardLinkCharTx] =
        {.name = "Board TX",
         .data_prop_type = FlipperGattCharacteristicDataCallback,
         .data.callback.fn = board_link_tx_value,
         .uuid.Char_UUID_128 =
             {0x02, 0xf4, 0xb8, 0xd6, 0x9a, 0xb4, 0x54, 0x86,
              0xe4, 0x46, 0x74, 0xcf, 0xda, 0xa1, 0x4b, 0x32},
         .uuid_type = UUID_TYPE_128,
         .char_properties = CHAR_PROP_INDICATE,
         .security_permissions = ATTR_PERMISSION_NONE,
         .gatt_evt_mask = GATT_NOTIFY_ATTRIBUTE_WRITE,
         .is_variable = CHAR_VALUE_LEN_VARIABLE},
    // Big-endian uint32, always the full credit: the phone reads it, then waits for it to be
    // notified again each time it has spent the credit.
    [BoardLinkCharFlow] =
        {.name = "Board flow control",
         .data_prop_type = FlipperGattCharacteristicDataFixed,
         .data.fixed.length = sizeof(uint32_t),
         .uuid.Char_UUID_128 =
             {0x03, 0xf4, 0xb8, 0xd6, 0x9a, 0xb4, 0x54, 0x86,
              0xe4, 0x46, 0x74, 0xcf, 0xda, 0xa1, 0x4b, 0x32},
         .uuid_type = UUID_TYPE_128,
         .char_properties = CHAR_PROP_READ | CHAR_PROP_NOTIFY,
         .security_permissions = ATTR_PERMISSION_NONE,
         .gatt_evt_mask = GATT_NOTIFY_ATTRIBUTE_WRITE,
         .is_variable = CHAR_VALUE_LEN_CONSTANT},
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
    link->credit_left = BOARD_LINK_RX_CREDIT;
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
    BoardLinkProfile* profile = (BoardLinkProfile*)bt_profile_start(bt, &board_link_profile, link);
    furi_check(profile);
    furi_hal_bt_start_advertising();

    BoardLinkEvent event;
    bool running = true;
    while(running) {
        // Wakes at least every 50 ms to animate the hold bar and catch dropped wake-ups
        if(furi_message_queue_get(link->queue, &event, 50) == FuriStatusOk) {
            if(event.type == BoardLinkEventDataSent) {
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

        // Without pairing the firmware never reports a connection, so the Bluetooth thread tracks
        // the peer itself; read it on every pass, so a dropped wake-up only delays the change
        const bool connected = link->peer_connected;
        if(connected != link->connected) {
            if(connected) {
                notification_message(notifications, &sequence_blink_stop);
                notification_message(notifications, &sequence_set_only_green_255);
            } else {
                notification_message(notifications, &sequence_reset_rgb);
                notification_message(notifications, &sequence_blink_start_blue);
                // The next peer starts from the full credit it reads
                FURI_CRITICAL_ENTER();
                link->credit_left = BOARD_LINK_RX_CREDIT;
                FURI_CRITICAL_EXIT();
            }
            furi_mutex_acquire(link->mutex, FuriWaitForever);
            link->connected = connected;
            if(!connected) link->has_frame = false;
            furi_mutex_release(link->mutex);
            board_frame_parser_reset(&link->parser);
            link->tx_pending_len = 0;
            link->tx_in_flight = false;
        }

        board_link_send_pending(link, profile);

        // Read everything the phone sent, then re-announce the credit if it is spent. Bytes that
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
            if(received) {
                FURI_CRITICAL_ENTER();
                const bool spent = link->credit_left == 0;
                if(spent) link->credit_left = BOARD_LINK_RX_CREDIT;
                FURI_CRITICAL_EXIT();
                if(spent) {
                    uint32_t credit = REVERSE_BYTES_U32(BOARD_LINK_RX_CREDIT);
                    ble_gatt_characteristic_update(
                        profile->svc_handle, &profile->chars[BoardLinkCharFlow], &credit);
                }
            }
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

    // Restore Bluetooth exactly as the keyboard link does. Stopping our profile unregisters its
    // event handler, so nothing on the Bluetooth thread touches the link once this returns.
    bt_disconnect(bt);
    furi_delay_ms(200); // Wait for the 2nd core to update NVM storage
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
        canvas_draw_str(canvas, 2, 26, "Connect the pad to:");
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

// Only our service: the firmware's device-information and battery services need an authenticated
// link to read, and a read that fails for that reason makes the phone start pairing.
static FuriHalBleProfileBase* board_link_profile_start(FuriHalBleProfileParams params) {
    BoardLinkProfile* profile = malloc(sizeof(BoardLinkProfile));
    profile->base.config = &board_link_profile;
    profile->link = params;
    // Service declaration, then RX (declaration, value), TX and flow (declaration, value, CCCD)
    furi_check(ble_gatt_service_add(
        UUID_TYPE_128,
        &board_link_service_uuid,
        PRIMARY_SERVICE,
        1 + 2 + 3 + 3,
        &profile->svc_handle));
    for(size_t i = 0; i < BoardLinkCharCount; i++) {
        ble_gatt_characteristic_init(profile->svc_handle, &board_link_chars[i], &profile->chars[i]);
    }
    uint32_t credit = REVERSE_BYTES_U32(BOARD_LINK_RX_CREDIT);
    ble_gatt_characteristic_update(
        profile->svc_handle, &profile->chars[BoardLinkCharFlow], &credit);
    // Registered once the handles it compares against exist
    profile->event_handler =
        ble_event_dispatcher_register_svc_handler(board_link_on_ble_event, profile);
    return &profile->base;
}

static void board_link_profile_stop(FuriHalBleProfileBase* base) {
    furi_check(base && base->config == &board_link_profile);
    BoardLinkProfile* profile = (BoardLinkProfile*)base;
    ble_event_dispatcher_unregister_svc_handler(profile->event_handler);
    for(size_t i = 0; i < BoardLinkCharCount; i++) {
        ble_gatt_characteristic_delete(profile->svc_handle, &profile->chars[i]);
    }
    ble_gatt_service_delete(profile->svc_handle);
    free(profile);
}

// The stock serial profile's GAP settings, without pairing or bonding, and with our own name and
// address so the phone sees a separate device from the Flipper it already knows.
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
        .bonding_mode = false,
        .pairing_method = GapPairingNone,
        .conn_param =
            {
                .conn_int_min = 0x06, // 7.5 ms
                .conn_int_max = 0x24, // 45 ms
            },
    };
    memcpy(config->mac_address, furi_hal_version_get_ble_mac(), sizeof(config->mac_address));
    // mac_xor 0x424E ("BN"): the Board link that paired used 0x4244, so a phone still bonded to
    // it sees a new device; the keyboard link uses 0x5350
    config->mac_address[0] ^= 0x4E;
    config->mac_address[1] ^= 0x42;
    const char* flipper_name = furi_hal_version_get_name_ptr();
    snprintf(
        config->adv_name,
        sizeof(config->adv_name),
        "%cBoard %s",
        furi_hal_version_get_ble_local_device_name_ptr()[0], // advertising data type byte
        flipper_name ? flipper_name : "");
}

// The TX value: its largest size when the characteristic is created (data is NULL then), and the
// pending button bytes when it is updated with the link as context.
static bool board_link_tx_value(const void* context, const uint8_t** data, uint16_t* data_len) {
    if(data) {
        const BoardLink* link = context;
        *data = link->tx_pending;
        *data_len = link->tx_pending_len;
    } else {
        *data_len = BOARD_LINK_VALUE_LEN_MAX;
    }
    return false; // the bytes stay ours
}

// Runs on the Bluetooth thread for every Bluetooth event: hand what concerns the link to the main
// loop and never block here.
static BleEventAckStatus board_link_on_ble_event(void* event, void* context) {
    BoardLinkProfile* profile = context;
    BoardLink* link = profile->link;
    hci_event_pckt* event_pckt = (hci_event_pckt*)((hci_uart_pckt*)event)->data;
    if(event_pckt->evt == HCI_DISCONNECTION_COMPLETE_EVT_CODE) {
        link->peer_connected = false;
        BoardLinkEvent wake = {.type = BoardLinkEventLinkChanged};
        furi_message_queue_put(link->queue, &wake, 0);
        return BleEventNotAck; // GAP handles it too, and advertises again
    }
    if(event_pckt->evt != HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE) return BleEventNotAck;

    evt_blecore_aci* blecore_evt = (evt_blecore_aci*)event_pckt->data;
    if(blecore_evt->ecode == ACI_GATT_SERVER_CONFIRMATION_VSEVT_CODE) {
        // Only our TX indicates, so this confirms the button bytes
        BoardLinkEvent sent = {.type = BoardLinkEventDataSent};
        furi_message_queue_put(link->queue, &sent, 0);
        return BleEventAckFlowEnable;
    }
    if(blecore_evt->ecode != ACI_GATT_ATTRIBUTE_MODIFIED_VSEVT_CODE) return BleEventNotAck;

    aci_gatt_attribute_modified_event_rp0* modified =
        (aci_gatt_attribute_modified_event_rp0*)blecore_evt->data;
    BoardLinkEvent wake = {.type = BoardLinkEventLinkChanged};
    if(modified->Attr_Handle == profile->chars[BoardLinkCharRx].handle + 1) {
        FURI_CRITICAL_ENTER();
        link->credit_left -= MIN(link->credit_left, modified->Attr_Data_Length);
        FURI_CRITICAL_EXIT();
        if(furi_stream_buffer_send(link->rx, modified->Attr_Data, modified->Attr_Data_Length, 0) <
           modified->Attr_Data_Length) {
            link->rx_overflowed = true;
        }
        wake.type = BoardLinkEventDataReceived;
    } else if(
        modified->Attr_Handle != profile->chars[BoardLinkCharTx].handle + 2 &&
        modified->Attr_Handle != profile->chars[BoardLinkCharFlow].handle + 2) {
        return BleEventNotAck;
    }
    // A frame write or a subscribe (a CCCD write) means a peer is using the link
    link->peer_connected = true;
    furi_message_queue_put(link->queue, &wake, 0);
    return BleEventAckFlowEnable;
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
    // ble_gatt_characteristic_update returns true when the update failed
    if(link->connected &&
       !ble_gatt_characteristic_update(
           profile->svc_handle, &profile->chars[BoardLinkCharTx], link)) {
        link->tx_in_flight = true;
        link->tx_sent_tick = furi_get_tick();
    }
    link->tx_pending_len = 0;
}
