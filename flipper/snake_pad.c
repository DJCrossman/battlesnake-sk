#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>

#include "links.h"

typedef enum {
    SnakePadLinkBoard,
    SnakePadLinkKeyboard,
    SnakePadLinkCount,
} SnakePadLink;

static void snake_pad_draw_chooser(Canvas* canvas, void* context);
static void snake_pad_forward_input(InputEvent* input, void* context);

int32_t snake_pad_app(void* p) {
    UNUSED(p);

    // Link chooser: Up/Down to pick, OK to start, Back to leave the app
    SnakePadLink selected = SnakePadLinkBoard;
    FuriMessageQueue* queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, snake_pad_draw_chooser, &selected);
    view_port_input_callback_set(view_port, snake_pad_forward_input, queue);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    InputEvent input;
    bool chosen = false;
    bool leaving = false;
    while(!chosen && !leaving) {
        furi_check(furi_message_queue_get(queue, &input, FuriWaitForever) == FuriStatusOk);
        if(input.type != InputTypeShort && input.type != InputTypeRepeat) continue;
        if(input.key == InputKeyUp || input.key == InputKeyDown) {
            selected = (selected + 1) % SnakePadLinkCount;
            view_port_update(view_port);
        } else {
            chosen = input.key == InputKeyOk;
            leaving = input.key == InputKeyBack;
        }
    }

    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(queue);

    if(chosen && selected == SnakePadLinkBoard) {
        board_link_run();
    } else if(chosen) {
        keyboard_link_run();
    }
    return 0;
}

static void snake_pad_draw_chooser(Canvas* canvas, void* context) {
    const SnakePadLink* selected = context;
    static const char* const names[SnakePadLinkCount] = {"Board", "Keyboard"};
    static const char* const needs[SnakePadLinkCount] = {"Chrome on Android", "any phone"};

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "Snake Pad");
    for(SnakePadLink link = 0; link < SnakePadLinkCount; link++) {
        const uint8_t top = 14 + link * 25;
        if(link == *selected) canvas_draw_rframe(canvas, 0, top, 128, 23, 3);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 5, top + 10, names[link]);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 5, top + 19, needs[link]);
    }
}

static void snake_pad_forward_input(InputEvent* input, void* context) {
    furi_message_queue_put(context, input, FuriWaitForever);
}
