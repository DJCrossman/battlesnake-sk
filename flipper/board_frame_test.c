// Host-side test of the board frame parser. Not part of the app.
// cc -std=c11 -Wall -Wextra -Werror -o /tmp/board_frame_test board_frame.c board_frame_test.c

#include "board_frame.h"

#include <stdio.h>
#include <string.h>

static const uint8_t shared_vector[] =
    {0x53, 0x0B, 0x00, 0x11, 0x03, 0x02, 0x02, 0x01, 0x5A, 0x03, 0x23, 0x43, 0x01, 0x65};

static int failures = 0;

static void check(bool ok, const char* what);
static int feed(
    BoardFrameParser* parser,
    const uint8_t* bytes,
    size_t len,
    uint32_t start_ms,
    uint32_t step_ms,
    BoardFrame* frames);
static bool is_shared_vector(const BoardFrame* frame);

int main(void) {
    BoardFrameParser parser = {0};
    BoardFrame frames[4];

    // The shared vector in one chunk
    check(feed(&parser, shared_vector, sizeof(shared_vector), 0, 0, frames) == 1, "one chunk: one frame");
    check(is_shared_vector(&frames[0]), "one chunk: decodes to the shared vector");
    check((frames[0].flags & BOARD_FLAG_GAME_STARTED) != 0, "game-start flag set");
    check(BOARD_FLAG_MODE(frames[0].flags) == BoardModeManual, "mode is manual");

    // The same bytes one at a time, 10 ms apart
    board_frame_parser_reset(&parser);
    check(feed(&parser, shared_vector, sizeof(shared_vector), 1000, 10, frames) == 1, "byte by byte: one frame");
    check(is_shared_vector(&frames[0]), "byte by byte: decodes identically");

    // Two frames glued into one chunk, with noise in front
    uint8_t glued[3 + 2 * sizeof(shared_vector)] = {0x00, 0xFF, 0x12};
    memcpy(glued + 3, shared_vector, sizeof(shared_vector));
    memcpy(glued + 3 + sizeof(shared_vector), shared_vector, sizeof(shared_vector));
    board_frame_parser_reset(&parser);
    check(feed(&parser, glued, sizeof(glued), 2000, 0, frames) == 2, "glued: two frames");
    check(is_shared_vector(&frames[0]) && is_shared_vector(&frames[1]), "glued: both decode identically");

    // An over-long length (401) is dropped, and the next frame still decodes
    uint8_t over_long[3 + sizeof(shared_vector)] = {0x53, 0x91, 0x01};
    memcpy(over_long + 3, shared_vector, sizeof(shared_vector));
    board_frame_parser_reset(&parser);
    check(feed(&parser, over_long, sizeof(over_long), 3000, 0, frames) == 1, "over-long: resynchronises");
    check(is_shared_vector(&frames[0]), "over-long: following frame decodes");

    // A frame that stalls for more than 500 ms is dropped, and the next frame still decodes
    board_frame_parser_reset(&parser);
    check(feed(&parser, shared_vector, 6, 4000, 0, frames) == 0, "stall: partial frame yields nothing");
    check(
        feed(&parser, shared_vector + 6, sizeof(shared_vector) - 6, 4600, 0, frames) == 0,
        "stall: rest after 600 ms is not completed into a frame");
    check(feed(&parser, shared_vector, sizeof(shared_vector), 4700, 0, frames) == 1, "stall: resynchronises");
    check(is_shared_vector(&frames[0]), "stall: following frame decodes");

    printf(failures ? "FAILED: %d check(s)\n" : "All checks passed\n", failures);
    return failures ? 1 : 0;
}

static void check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if(!ok) failures++;
}

static int feed(
    BoardFrameParser* parser,
    const uint8_t* bytes,
    size_t len,
    uint32_t start_ms,
    uint32_t step_ms,
    BoardFrame* frames) {
    int count = 0;
    for(size_t i = 0; i < len; i++) {
        if(board_frame_parser_feed(parser, bytes[i], start_ms + i * step_ms, &frames[count])) {
            count++;
        }
    }
    return count;
}

static bool is_shared_vector(const BoardFrame* frame) {
    // Top row: my head, my body, empty. Bottom row: food, other head, other body.
    return frame->flags == 0x11 && frame->width == 3 && frame->height == 2 &&
           frame->turn == 258 && frame->health == 90 && frame->length == 3 &&
           frame->hold_10ms == 35 && board_frame_cell_at(frame, 0, 1) == BoardCellMyHead &&
           board_frame_cell_at(frame, 1, 1) == BoardCellMyBody &&
           board_frame_cell_at(frame, 2, 1) == BoardCellEmpty &&
           board_frame_cell_at(frame, 0, 0) == BoardCellFood &&
           board_frame_cell_at(frame, 1, 0) == BoardCellOtherHead &&
           board_frame_cell_at(frame, 2, 0) == BoardCellOtherBody;
}
