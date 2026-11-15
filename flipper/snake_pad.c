#include <furi.h>
#include <furi_hal_bt.h>
#include <furi_hal_usb_hid.h>
#include <furi_hal_version.h>
#include <bt/bt_service/bt.h>
#include <extra_profiles/hid_profile.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>

typedef enum {
    SnakePadEventInput,
    SnakePadEventBtStatus,
} SnakePadEventType;

typedef struct {
    SnakePadEventType type;
    union {
        InputEvent input;
        BtStatus bt_status;
    };
} SnakePadEvent;

// iOS 18.4+ Safari can drop keydown/keyup from keyboard+mouse HID devices, but letters typed
// into a focused text field still arrive, so the page also accepts WASD.
typedef enum {
    SnakePadKeySetArrows,
    SnakePadKeySetWasd,
    SnakePadKeySetCount,
} SnakePadKeySet;

typedef struct {
    FuriMutex* mutex;
    char adv_name[FURI_HAL_VERSION_DEVICE_NAME_LENGTH];
    bool connected;
    SnakePadKeySet key_set;
    uint8_t held; // one bit per InputKey
    uint32_t presses_sent;
} SnakePadState;

static void snake_pad_draw(Canvas* canvas, void* context);
static void snake_pad_forward_input(InputEvent* input, void* context);
static void snake_pad_forward_bt_status(BtStatus status, void* context);

int32_t snake_pad_app(void* p) {
    UNUSED(p);

    // Same name the HID profile advertises: "<prefix> <flipper name>"
    SnakePadState state = {.mutex = furi_mutex_alloc(FuriMutexTypeNormal)};
    const char* flipper_name = furi_hal_version_get_name_ptr();
    snprintf(
        state.adv_name, sizeof(state.adv_name), "Snake %s", flipper_name ? flipper_name : "");

    FuriMessageQueue* queue = furi_message_queue_alloc(16, sizeof(SnakePadEvent));

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, snake_pad_draw, &state);
    view_port_input_callback_set(view_port, snake_pad_forward_input, queue);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    NotificationApp* notifications = furi_record_open(RECORD_NOTIFICATION);
    notification_message(notifications, &sequence_display_backlight_enforce_on);
    notification_message(notifications, &sequence_blink_start_blue);

    // Switch to the HID keyboard profile the way the stock Bluetooth Remote app does,
    // with its own bonding-key file so the normal Flipper pairing stays untouched.
    Bt* bt = furi_record_open(RECORD_BT);
    bt_disconnect(bt);
    furi_delay_ms(200); // Wait for the 2nd core to update NVM storage
    bt_keys_storage_set_storage_path(bt, APP_DATA_PATH(".bt_hid.keys"));
    FuriHalBleProfileBase* hid = bt_profile_start(
        bt,
        ble_profile_hid,
        &(BleProfileHidParams){.device_name_prefix = "Snake", .mac_xor = 0x5350});
    furi_check(hid);
    furi_hal_bt_start_advertising();
    bt_set_status_changed_callback(bt, snake_pad_forward_bt_status, queue);

    static const uint16_t hid_key_for_button[SnakePadKeySetCount][InputKeyMAX] = {
        [SnakePadKeySetArrows] =
            {
                [InputKeyUp] = HID_KEYBOARD_UP_ARROW,
                [InputKeyDown] = HID_KEYBOARD_DOWN_ARROW,
                [InputKeyLeft] = HID_KEYBOARD_LEFT_ARROW,
                [InputKeyRight] = HID_KEYBOARD_RIGHT_ARROW,
                [InputKeyOk] = HID_KEYBOARD_SPACEBAR,
            },
        [SnakePadKeySetWasd] =
            {
                [InputKeyUp] = HID_KEYBOARD_W,
                [InputKeyDown] = HID_KEYBOARD_S,
                [InputKeyLeft] = HID_KEYBOARD_A,
                [InputKeyRight] = HID_KEYBOARD_D,
                [InputKeyOk] = HID_KEYBOARD_SPACEBAR,
            },
    };

    SnakePadEvent event;
    bool running = true;
    while(running) {
        furi_check(furi_message_queue_get(queue, &event, FuriWaitForever) == FuriStatusOk);
        furi_mutex_acquire(state.mutex, FuriWaitForever);

        if(event.type == SnakePadEventBtStatus) {
            const bool connected = event.bt_status == BtStatusConnected;
            if(connected && !state.connected) {
                notification_message(notifications, &sequence_blink_stop);
                notification_message(notifications, &sequence_set_only_green_255);
            } else if(!connected && state.connected) {
                notification_message(notifications, &sequence_reset_rgb);
                notification_message(notifications, &sequence_blink_start_blue);
            }
            if(!connected) {
                ble_profile_hid_kb_release_all(hid);
                state.held = 0;
            }
            state.connected = connected;
        } else if(event.input.key == InputKeyBack) {
            if(event.input.type == InputTypeShort) {
                ble_profile_hid_kb_release_all(hid);
                state.held = 0;
                state.key_set = (state.key_set + 1) % SnakePadKeySetCount;
            }
            running = event.input.type != InputTypeLong;
        } else if(event.input.type == InputTypePress) {
            state.held |= 1 << event.input.key;
            if(ble_profile_hid_kb_press(
                   hid, hid_key_for_button[state.key_set][event.input.key])) {
                state.presses_sent++;
            }
        } else if(event.input.type == InputTypeRelease) {
            state.held &= ~(1 << event.input.key);
            ble_profile_hid_kb_release(hid, hid_key_for_button[state.key_set][event.input.key]);
        }

        furi_mutex_release(state.mutex);
        view_port_update(view_port);
    }

    // Restore Bluetooth exactly as the stock app does on exit
    ble_profile_hid_kb_release_all(hid);
    bt_set_status_changed_callback(bt, NULL, NULL);
    bt_disconnect(bt);
    furi_delay_ms(200); // Wait for the 2nd core to update NVM storage
    bt_keys_storage_set_default_path(bt);
    furi_check(bt_profile_restore_default(bt));
    furi_record_close(RECORD_BT);

    notification_message(notifications, &sequence_blink_stop);
    notification_message(notifications, &sequence_reset_rgb);
    notification_message(notifications, &sequence_display_backlight_enforce_auto);
    furi_record_close(RECORD_NOTIFICATION);

    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(queue);
    furi_mutex_free(state.mutex);

    return 0;
}

static void snake_pad_draw(Canvas* canvas, void* context) {
    SnakePadState* state = context;
    furi_mutex_acquire(state->mutex, FuriWaitForever);

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "Snake Pad");

    canvas_set_font(canvas, FontSecondary);
    if(state->connected) {
        canvas_draw_str(canvas, 2, 22, "Connected");
    } else {
        canvas_draw_str(canvas, 2, 22, "Pair me:");
        canvas_draw_str(canvas, 2, 31, state->adv_name);
    }

    canvas_draw_str(
        canvas, 2, 40, state->key_set == SnakePadKeySetWasd ? "Keys: WASD" : "Keys: Arrows");

    char sent[24];
    snprintf(sent, sizeof(sent), "Sent: %lu", state->presses_sent);
    canvas_draw_str(canvas, 2, 49, sent);

    static const struct {
        InputKey key;
        uint8_t x;
        uint8_t y;
    } dpad[] = {
        {InputKeyUp, 100, 11},
        {InputKeyLeft, 87, 24},
        {InputKeyOk, 100, 24},
        {InputKeyRight, 113, 24},
        {InputKeyDown, 100, 37},
    };
    for(size_t i = 0; i < COUNT_OF(dpad); i++) {
        if(state->held & (1 << dpad[i].key)) {
            canvas_draw_rbox(canvas, dpad[i].x, dpad[i].y, 11, 11, 2);
        } else {
            canvas_draw_rframe(canvas, dpad[i].x, dpad[i].y, 11, 11, 2);
        }
    }

    canvas_draw_line(canvas, 0, 52, 127, 52);
    canvas_draw_str(canvas, 2, 62, "OK:mode  Back:keys  Hold:exit");

    furi_mutex_release(state->mutex);
}

static void snake_pad_forward_input(InputEvent* input, void* context) {
    SnakePadEvent event = {.type = SnakePadEventInput, .input = *input};
    furi_message_queue_put(context, &event, FuriWaitForever);
}

static void snake_pad_forward_bt_status(BtStatus status, void* context) {
    SnakePadEvent event = {.type = SnakePadEventBtStatus, .bt_status = status};
    furi_message_queue_put(context, &event, FuriWaitForever);
}
