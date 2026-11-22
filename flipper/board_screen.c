#include "board_screen.h"

#include <furi.h>
#include <notification/notification_messages.h>

typedef enum {
    BoardScreenAnimationNone,
    BoardScreenAnimationStart,
    BoardScreenAnimationWon,
    BoardScreenAnimationLost,
} BoardScreenAnimation;

// A loss: the board shakes, then our snake falls off cell by cell until the head goes, then the
// banner. A cell takes 400 ms to fall off the screen.
#define BOARD_SCREEN_SHAKE_MS       400
#define BOARD_SCREEN_HEAD_FALLS_MS  1200
#define BOARD_SCREEN_LOST_BANNER_MS 1600

const char* const board_screen_mode_names[] = {
    [BoardModeUnknown] = "",
    [BoardModeAssist] = "Assist",
    [BoardModeManual] = "Manual",
    [BoardModeAuto] = "Auto",
};

// One buzz with a two-note blip
static const NotificationSequence board_screen_sequence_start = {
    &message_vibro_on,
    &message_note_e5,
    &message_delay_50,
    &message_note_c6,
    &message_delay_50,
    &message_vibro_off,
    &message_sound_off,
    NULL,
};

// Rising to the octave with the LED blinking green. Without a reset at the end the LED stays as
// the app keeps it while connected, so the sound has to be stopped here.
static const NotificationSequence board_screen_sequence_won = {
    &message_red_0,
    &message_blue_0,
    // C5, E5 and G5 for 100 ms each, the LED off, on, off
    &message_green_0,
    &message_note_c5,
    &message_delay_100,
    &message_green_255,
    &message_note_e5,
    &message_delay_100,
    &message_green_0,
    &message_note_g5,
    &message_delay_100,
    // C6 for 250 ms, the LED on
    &message_green_255,
    &message_note_c6,
    &message_delay_250,
    &message_sound_off,
    // Two more blinks, ending on solid green
    &message_green_0,
    &message_delay_100,
    &message_green_255,
    &message_delay_100,
    &message_green_0,
    &message_delay_100,
    &message_green_255,
    &message_do_not_reset,
    NULL,
};

// Falling slowly with the LED red, then back to green
static const NotificationSequence board_screen_sequence_lost = {
    &message_green_0,
    &message_blue_0,
    &message_red_255,
    // G4 for 300 ms and E4 for 300 ms, buzzing for the first 400 ms
    &message_vibro_on,
    &message_note_g4,
    &message_delay_250,
    &message_delay_50,
    &message_note_e4,
    &message_delay_100,
    &message_vibro_off,
    &message_delay_100,
    &message_delay_100,
    // C4 for 750 ms
    &message_note_c4,
    &message_delay_500,
    &message_delay_250,
    &message_sound_off,
    &message_red_0,
    &message_green_255,
    &message_do_not_reset,
    NULL,
};

// How long each animation runs from the frame's arrival, and what the frame plays as it arrives
static const struct {
    uint16_t length_ms;
    const NotificationSequence* sequence;
} board_screen_animations[] = {
    [BoardScreenAnimationNone] = {0, NULL},
    [BoardScreenAnimationStart] = {800, &board_screen_sequence_start},
    [BoardScreenAnimationWon] = {3000, &board_screen_sequence_won},
    [BoardScreenAnimationLost] = {2500, &board_screen_sequence_lost},
};

// A win's fireworks: where each burst goes off around the banner, and when
static const struct {
    uint8_t x;
    uint8_t y;
    uint16_t at_ms;
} board_screen_bursts[] = {
    {18, 12, 0},
    {108, 10, 300},
    {64, 8, 600},
    {20, 52, 900},
    {106, 54, 1200},
    {64, 58, 1500},
    {12, 32, 1800},
    {116, 32, 2100},
};

// The eight directions a burst's dots fly, in sixteenths of its radius
static const struct {
    int8_t dx;
    int8_t dy;
} board_screen_burst_directions[] = {
    {16, 0},
    {11, 11},
    {0, 16},
    {-11, 11},
    {-16, 0},
    {-11, -11},
    {0, -16},
    {11, -11},
};

// One turn of a sine, 3 px high, for the bouncing letters
static const int8_t board_screen_wave[16] = {0, 1, 2, 3, 3, 3, 2, 1, 0, -1, -2, -3, -3, -3, -2, -1};

static BoardScreenAnimation board_screen_running_animation(
    const BoardScreen* screen,
    uint32_t elapsed_ms,
    uint32_t started_ms);
static BoardScreenAnimation board_screen_animation(uint8_t flags);
static void
    board_screen_draw_cell(Canvas* canvas, BoardCell cell, int32_t px, int32_t py, uint8_t size);
static void board_screen_draw_rings(
    Canvas* canvas,
    const BoardFrame* frame,
    int32_t x0,
    int32_t y0,
    uint8_t cell,
    uint32_t started_ms);
static void board_screen_draw_falling_snake(
    Canvas* canvas,
    const BoardFrame* previous,
    const BoardFrame* shown,
    int32_t x0,
    int32_t y0,
    uint8_t cell,
    uint32_t elapsed_ms);
static void board_screen_draw_fireworks(Canvas* canvas, uint32_t elapsed_ms);
static void
    board_screen_draw_banner(Canvas* canvas, const char* text, bool bouncing, uint32_t elapsed_ms);

void board_screen_draw(Canvas* canvas, const BoardScreen* screen) {
    const BoardFrame* frame = screen->shown;
    const uint32_t now = furi_get_tick();
    const uint32_t elapsed_ms = now - screen->shown_tick;
    const uint32_t started_ms = now - screen->started_tick;
    const BoardScreenAnimation animation =
        board_screen_running_animation(screen, elapsed_ms, started_ms);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);

    if((frame->flags & BOARD_FLAG_TOO_LARGE) || frame->width == 0 || frame->height == 0) {
        canvas_draw_str(canvas, 2, 28, "Board too");
        canvas_draw_str(canvas, 2, 38, "large to show");
    } else {
        // Board in a 64 px square on the left, framed, y = 0 at the bottom. A loss shakes it.
        const bool shaking = animation == BoardScreenAnimationLost &&
                             elapsed_ms < BOARD_SCREEN_SHAKE_MS;
        const uint8_t cell = 62 / (frame->width > frame->height ? frame->width : frame->height);
        const int32_t x0 =
            1 + (62 - frame->width * cell) / 2 + (shaking ? (elapsed_ms / 33 % 2 ? 2 : -2) : 0);
        const int32_t y0 =
            1 + (62 - frame->height * cell) / 2 + (shaking ? (elapsed_ms / 66 % 2 ? 1 : -1) : 0);
        canvas_draw_frame(
            canvas, x0 - 1, y0 - 1, frame->width * cell + 2, frame->height * cell + 2);
        for(uint8_t y = 0; y < frame->height; y++) {
            for(uint8_t x = 0; x < frame->width; x++) {
                board_screen_draw_cell(
                    canvas,
                    board_frame_cell_at(frame, x, y),
                    x0 + x * cell,
                    y0 + (frame->height - 1 - y) * cell,
                    cell);
            }
        }
        if(animation == BoardScreenAnimationStart) {
            board_screen_draw_rings(canvas, frame, x0, y0, cell, started_ms);
        } else if(animation == BoardScreenAnimationLost) {
            board_screen_draw_falling_snake(
                canvas, screen->previous, frame, x0, y0, cell, elapsed_ms);
        }
    }

    char line[24];
    snprintf(line, sizeof(line), "Turn %u", frame->turn);
    canvas_draw_str(canvas, 68, 9, line);
    snprintf(line, sizeof(line), "Health %u", frame->health);
    canvas_draw_str(canvas, 68, 19, line);
    snprintf(line, sizeof(line), "Length %u", frame->length);
    canvas_draw_str(canvas, 68, 29, line);
    canvas_draw_str(canvas, 68, 39, board_screen_mode_names[screen->mode]);

    if(frame->flags & BOARD_FLAG_GAME_OVER) {
        const char* outcome = "Game over";
        if(frame->flags & BOARD_FLAG_WON) outcome = "You won";
        if(frame->flags & BOARD_FLAG_LOST) outcome = "You lost";
        canvas_draw_str(canvas, 68, 50, outcome);
    } else if(animation == BoardScreenAnimationStart && started_ms / 100 % 2 == 0) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 68, 50, "GO!");
        canvas_set_font(canvas, FontSecondary);
    }

    // Time left to press, counted down from the frame's arrival. The bar's full width is the
    // hold the server sent, so the time the frame took to get here is already gone from it.
    if(frame->hold_10ms > 0) {
        const uint32_t left_ms = elapsed_ms < screen->hold_ms ? screen->hold_ms - elapsed_ms : 0;
        canvas_draw_frame(canvas, 68, 56, 58, 7);
        canvas_draw_box(canvas, 69, 57, 56 * left_ms / (frame->hold_10ms * 10), 5);
    }

    if(animation == BoardScreenAnimationWon) {
        board_screen_draw_fireworks(canvas, elapsed_ms);
        board_screen_draw_banner(canvas, "YOU WON!", true, elapsed_ms);
    } else if(animation == BoardScreenAnimationLost && elapsed_ms >= BOARD_SCREEN_LOST_BANNER_MS) {
        board_screen_draw_banner(canvas, "GAME OVER", false, elapsed_ms);
    }
}

bool board_screen_is_moving(const BoardScreen* screen) {
    if(!screen->has_frame) return false;
    const uint32_t now = furi_get_tick();
    const uint32_t elapsed_ms = now - screen->shown_tick;
    return board_screen_running_animation(screen, elapsed_ms, now - screen->started_tick) !=
               BoardScreenAnimationNone ||
           elapsed_ms < screen->hold_ms;
}

const NotificationSequence* board_screen_arrival_sequence(const BoardFrame* frame) {
    return board_screen_animations[board_screen_animation(frame->flags)].sequence;
}

// A game over takes over at once and plays from its frame's arrival. Until then a start plays from
// the start frame's arrival, over the turn-0 frames right behind it.
static BoardScreenAnimation board_screen_running_animation(
    const BoardScreen* screen,
    uint32_t elapsed_ms,
    uint32_t started_ms) {
    if(screen->shown->flags & BOARD_FLAG_GAME_OVER) {
        const BoardScreenAnimation animation = board_screen_animation(screen->shown->flags);
        return elapsed_ms < board_screen_animations[animation].length_ms ?
                   animation :
                   BoardScreenAnimationNone;
    }
    return started_ms < board_screen_animations[BoardScreenAnimationStart].length_ms ?
               BoardScreenAnimationStart :
               BoardScreenAnimationNone;
}

// A game that just started ripples, a won one sets off fireworks and a lost one drops our snake
static BoardScreenAnimation board_screen_animation(uint8_t flags) {
    if(flags & BOARD_FLAG_GAME_STARTED) return BoardScreenAnimationStart;
    if(!(flags & BOARD_FLAG_GAME_OVER)) return BoardScreenAnimationNone;
    if(flags & BOARD_FLAG_WON) return BoardScreenAnimationWon;
    if(flags & BOARD_FLAG_LOST) return BoardScreenAnimationLost;
    return BoardScreenAnimationNone;
}

// How each kind of cell looks at a given cell size. Mine is solid with a hollow-centred head;
// other snakes are outlined (or a sparse pattern at small sizes) with a filled-centre head.
static void
    board_screen_draw_cell(Canvas* canvas, BoardCell cell, int32_t px, int32_t py, uint8_t size) {
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

// A start: three rings ripple out 150 ms apart from our head in the frame shown now, so they
// follow it, inverted so they show over any cell
static void board_screen_draw_rings(
    Canvas* canvas,
    const BoardFrame* frame,
    int32_t x0,
    int32_t y0,
    uint8_t cell,
    uint32_t started_ms) {
    canvas_set_color(canvas, ColorXOR);
    for(uint8_t y = 0; y < frame->height; y++) {
        for(uint8_t x = 0; x < frame->width; x++) {
            if(board_frame_cell_at(frame, x, y) != BoardCellMyHead) continue;
            for(uint32_t ring = 0; ring < 3; ring++) {
                if(started_ms < ring * 150 || started_ms - ring * 150 >= 500) continue;
                canvas_draw_circle(
                    canvas,
                    x0 + x * cell + cell / 2,
                    y0 + (frame->height - 1 - y) * cell + cell / 2,
                    2 + (started_ms - ring * 150) * 36 / 500);
            }
        }
    }
    canvas_set_color(canvas, ColorBlack);
}

// A loss: our snake, from the frame before, falls off the bottom with gravity one cell after
// another, lowest first and the head last. Body cells leave evenly between the end of the shake
// and the head. A frame before of another size (the empty one after a reconnect) drops nothing.
static void board_screen_draw_falling_snake(
    Canvas* canvas,
    const BoardFrame* previous,
    const BoardFrame* shown,
    int32_t x0,
    int32_t y0,
    uint8_t cell,
    uint32_t elapsed_ms) {
    if(previous->width != shown->width || previous->height != shown->height) return;
    uint16_t body_cells = 0;
    for(uint8_t y = 0; y < previous->height; y++) {
        for(uint8_t x = 0; x < previous->width; x++) {
            if(board_frame_cell_at(previous, x, y) == BoardCellMyBody) body_cells++;
        }
    }

    uint16_t body_cells_before = 0;
    for(uint8_t y = 0; y < previous->height; y++) {
        for(uint8_t x = 0; x < previous->width; x++) {
            const BoardCell code = board_frame_cell_at(previous, x, y);
            if(code != BoardCellMyBody && code != BoardCellMyHead) continue;
            const uint32_t falls_at_ms =
                code == BoardCellMyHead ?
                    BOARD_SCREEN_HEAD_FALLS_MS :
                    BOARD_SCREEN_SHAKE_MS +
                        body_cells_before++ *
                            (BOARD_SCREEN_HEAD_FALLS_MS - BOARD_SCREEN_SHAKE_MS) / body_cells;
            const uint32_t falling_ms = elapsed_ms > falls_at_ms ? elapsed_ms - falls_at_ms : 0;
            // 64 px in the first 400 ms
            const int32_t py =
                y0 + (previous->height - 1 - y) * cell + (int32_t)(falling_ms * falling_ms / 2500);
            if(py < 64) board_screen_draw_cell(canvas, code, x0 + x * cell, py, cell);
        }
    }
}

// A win: each burst's eight dots spread out, slowing, and fade: big dots, then small ones, then
// every other one. Inverted, so they show over the board and the text.
static void board_screen_draw_fireworks(Canvas* canvas, uint32_t elapsed_ms) {
    canvas_set_color(canvas, ColorXOR);
    for(size_t b = 0; b < COUNT_OF(board_screen_bursts); b++) {
        if(elapsed_ms < board_screen_bursts[b].at_ms) continue;
        const uint32_t age_ms = elapsed_ms - board_screen_bursts[b].at_ms;
        if(age_ms >= 800) continue;
        const int32_t radius = 14 * age_ms * (1600 - age_ms) / (800 * 800);
        for(size_t d = 0; d < COUNT_OF(board_screen_burst_directions); d++) {
            if(age_ms >= 550 && d % 2) continue;
            const int32_t x =
                board_screen_bursts[b].x + board_screen_burst_directions[d].dx * radius / 16;
            const int32_t y =
                board_screen_bursts[b].y + board_screen_burst_directions[d].dy * radius / 16;
            if(age_ms < 300) {
                canvas_draw_box(canvas, x, y, 2, 2);
            } else {
                canvas_draw_dot(canvas, x, y);
            }
        }
    }
    canvas_set_color(canvas, ColorBlack);
}

// A boxed banner across the middle, its letters bouncing on a wave when asked
static void
    board_screen_draw_banner(Canvas* canvas, const char* text, bool bouncing, uint32_t elapsed_ms) {
    canvas_set_font(canvas, FontPrimary);
    const int32_t width = canvas_string_width(canvas, text) + 14;
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_rbox(canvas, (128 - width) / 2, 21, width, 22, 3);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_rframe(canvas, (128 - width) / 2, 21, width, 22, 3);
    int32_t x = (128 - width) / 2 + 7;
    for(size_t i = 0; text[i]; i++) {
        const int32_t lift = bouncing ? board_screen_wave[(elapsed_ms / 40 + i * 2) % 16] : 0;
        canvas_draw_glyph(canvas, x, 36 - lift, (uint8_t)text[i]);
        x += canvas_glyph_width(canvas, (uint8_t)text[i]);
    }
}
