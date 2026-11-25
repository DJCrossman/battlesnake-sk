#pragma once

// The live board, drawn on the GUI thread from what the app last received. A win or loss plays
// from its frame's arrival, so the next frame ends it at once. A start plays from the start frame
// over the frames right behind it (turn 0 follows within milliseconds) until a game over.

#include <gui/canvas.h>
#include <notification/notification.h>

#include "board_frame.h"

typedef enum {
    BoardModeUnknown, // no mode byte since connecting
    BoardModeAssist,
    BoardModeManual,
    BoardModeAuto,
} BoardMode;

// The app fills this under its mutex
typedef struct {
    // A frame arrived since connecting and is not a game over past its 15 s. Shown and previous
    // are always allocated.
    bool has_frame;
    BoardFrame* shown;
    // The frame before it, for the loss animation: at the end the engine has already removed our
    // snake from the board
    BoardFrame* previous;
    uint32_t shown_tick; // when the shown frame arrived
    // When the last start frame arrived. Zero reads as long ago, as the app opens long after boot.
    uint32_t started_tick;
    uint32_t hold_ms; // time left to press when it arrived: its hold less half the last ping
    BoardMode mode;
} BoardScreen;

// "" while unknown
extern const char* const board_screen_mode_names[];

/** Draws the whole screen. Only while screen->has_frame. */
void board_screen_draw(Canvas* canvas, const BoardScreen* screen);

/** Whether an animation or the hold bar is running, so the screen needs redrawing. */
bool board_screen_is_moving(const BoardScreen* screen);

/** What a frame plays as it arrives: a buzz for a start, a tune for a win or a loss, otherwise
 * NULL. The tunes end on the solid green the app shows while connected. */
const NotificationSequence* board_screen_arrival_sequence(const BoardFrame* frame);
