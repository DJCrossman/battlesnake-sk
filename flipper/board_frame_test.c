// Host-side test of the board frame parser. Not part of the app.
// cc -std=c11 -Wall -Wextra -Werror -o /tmp/board_frame_test board_frame.c board_frame_test.c

#include "board_frame.h"

#include <stdio.h>
#include <string.h>

// The vector the server's Rust test checks too: a 3x2 board on turn 200 with a 350 ms hold
static const uint8_t shared_vector[] =
    {0xB0, 0x00, 0x03, 0x02, 0x48, 0x01, 0x55, 0x03, 0x00, 0x23, 0x00, 0x40, 0x13, 0x36, 0x52, 0x1B};

static const char boot_text[] = "ets Jul 29 2019 12:21:46\r\nrst:0x1 (POWERON_RESET)\r\n";

static int failures = 0;

static void check(bool ok, const char* what);
static int feed(BoardFrameParser* parser, const uint8_t* bytes, size_t len, BoardFrame* frames);
static bool is_shared_vector(const BoardFrame* frame);
static void seal_frame(uint8_t* bytes, size_t len);

int main(void) {
    BoardFrameParser parser = {0};
    BoardFrame frames[4];

    check(
        feed(&parser, shared_vector, sizeof(shared_vector), frames) == 1,
        "shared vector: one frame");
    check(is_shared_vector(&frames[0]), "shared vector: decodes to the spec's values and cells");

    // Completes on the last check byte, not before
    bool early = false;
    for(size_t i = 0; i + 1 < sizeof(shared_vector); i++) {
        early = early || board_frame_parser_feed(&parser, shared_vector[i], &frames[0]);
    }
    check(!early, "byte by byte: no frame before the last byte");
    check(
        board_frame_parser_feed(&parser, shared_vector[sizeof(shared_vector) - 1], &frames[0]),
        "byte by byte: the last byte completes it");
    check(is_shared_vector(&frames[0]), "byte by byte: decodes to the shared vector");

    // Boot text and status bytes before, between and after frames
    uint8_t mixed[512];
    size_t mixed_len = 0;
    memcpy(mixed + mixed_len, boot_text, strlen(boot_text));
    mixed_len += strlen(boot_text);
    mixed[mixed_len++] = 0xA0;
    mixed[mixed_len++] = 0xA6;
    memcpy(mixed + mixed_len, shared_vector, sizeof(shared_vector));
    mixed_len += sizeof(shared_vector);
    mixed[mixed_len++] = 0xA7;
    memcpy(mixed + mixed_len, boot_text, strlen(boot_text));
    mixed_len += strlen(boot_text);
    mixed[mixed_len++] = 0xB2;
    memcpy(mixed + mixed_len, shared_vector, sizeof(shared_vector));
    mixed_len += sizeof(shared_vector);
    memcpy(mixed + mixed_len, boot_text, strlen(boot_text));
    mixed_len += strlen(boot_text);
    mixed[mixed_len++] = 0xA6;
    parser = (BoardFrameParser){0};
    check(
        feed(&parser, mixed, mixed_len, frames) == 2,
        "boot text and status bytes around: two frames");
    check(
        is_shared_vector(&frames[0]) && is_shared_vector(&frames[1]),
        "boot text and status bytes around: both decode to the shared vector");

    // A frame cut off after its header, with boot text after it, then a full one
    uint8_t truncated[sizeof(shared_vector) + sizeof(boot_text) + sizeof(shared_vector)];
    size_t truncated_len = 0;
    memcpy(truncated, shared_vector, 9);
    truncated_len += 9;
    memcpy(truncated + truncated_len, boot_text, strlen(boot_text));
    truncated_len += strlen(boot_text);
    memcpy(truncated + truncated_len, shared_vector, sizeof(shared_vector));
    truncated_len += sizeof(shared_vector);
    parser = (BoardFrameParser){0};
    check(feed(&parser, truncated, truncated_len, frames) == 1, "truncated then full: one frame");
    check(is_shared_vector(&frames[0]), "truncated then full: the full one decodes");

    // A mode byte in the middle of a frame drops it
    uint8_t interrupted[sizeof(shared_vector) + 1];
    memcpy(interrupted, shared_vector, 6);
    interrupted[6] = 0xB2;
    memcpy(interrupted + 7, shared_vector + 6, sizeof(shared_vector) - 6);
    parser = (BoardFrameParser){0};
    check(
        feed(&parser, interrupted, sizeof(interrupted), frames) == 0,
        "mode byte inside a frame: dropped");
    check(
        feed(&parser, shared_vector, sizeof(shared_vector), frames) == 1 &&
            is_shared_vector(&frames[0]),
        "mode byte inside a frame: the next frame decodes");

    // One byte inserted shifts the rest, so the frame ends early and its check bytes are wrong
    uint8_t shifted[sizeof(shared_vector) + 1];
    memcpy(shifted, shared_vector, 7);
    shifted[7] = 0x00;
    memcpy(shifted + 8, shared_vector + 7, sizeof(shared_vector) - 7);
    parser = (BoardFrameParser){0};
    check(feed(&parser, shifted, sizeof(shifted), frames) == 0, "one byte inserted: rejected");
    check(
        feed(&parser, shared_vector, sizeof(shared_vector), frames) == 1 &&
            is_shared_vector(&frames[0]),
        "one byte inserted: the next frame decodes");

    uint8_t bad_check[sizeof(shared_vector)];
    memcpy(bad_check, shared_vector, sizeof(shared_vector));
    bad_check[sizeof(bad_check) - 1] ^= 0x01;
    parser = (BoardFrameParser){0};
    check(
        feed(&parser, bad_check, sizeof(bad_check), frames) == 0,
        "bad second check byte: rejected");
    memcpy(bad_check, shared_vector, sizeof(shared_vector));
    bad_check[sizeof(bad_check) - 2] ^= 0x01;
    check(
        feed(&parser, bad_check, sizeof(bad_check), frames) == 0,
        "bad first check byte: rejected");
    memcpy(bad_check, shared_vector, sizeof(shared_vector));
    bad_check[12] ^= 0x01;
    check(feed(&parser, bad_check, sizeof(bad_check), frames) == 0, "changed cell byte: rejected");

    // Cell 1 becomes 7, with check bytes that match it
    uint8_t bad_cell[sizeof(shared_vector)];
    memcpy(bad_cell, shared_vector, sizeof(shared_vector));
    bad_cell[11] = 0x47;
    seal_frame(bad_cell, sizeof(bad_cell));
    parser = (BoardFrameParser){0};
    check(feed(&parser, bad_cell, sizeof(bad_cell), frames) == 0, "cell code 7: rejected");

    // Width 26 is dropped at the header, and the rest of it is not mistaken for a frame
    uint8_t too_wide[3 + 10 + 7 + 2 + sizeof(shared_vector)] = {0xB0, 0x00, 26, 1};
    memcpy(too_wide + 3 + 10 + 7 + 2, shared_vector, sizeof(shared_vector));
    parser = (BoardFrameParser){0};
    check(
        feed(&parser, too_wide, sizeof(too_wide), frames) == 1 && is_shared_vector(&frames[0]),
        "width 26: rejected, and the next frame decodes");

    // Turn 16, health 50, length 5, no cells
    uint8_t too_large[] = {0xB0, BOARD_FLAG_TOO_LARGE, 0, 0, 16, 0, 50, 5, 0, 0, 0, 0, 0};
    seal_frame(too_large, sizeof(too_large));
    parser = (BoardFrameParser){0};
    check(feed(&parser, too_large, sizeof(too_large), frames) == 1, "too large: one frame");
    check(
        frames[0].flags == BOARD_FLAG_TOO_LARGE && frames[0].width == 0 && frames[0].height == 0 &&
            frames[0].turn == 16 && frames[0].health == 50 && frames[0].length == 5 &&
            frames[0].hold_10ms == 0,
        "too large: decodes with no board");

    // The largest board fills every buffer: turn, length and hold at 16383, and cell i holds i % 7
    uint8_t largest[1 + BOARD_FRAME_HEADER_LEN + BOARD_FRAME_CELLS_MAX + 2] = {
        0xB0, 0x00, 25, 25, 0x7F, 0x7F, 100, 0x7F, 0x7F, 0x7F, 0x7F};
    for(int i = 0; i < BOARD_FRAME_SIDE_MAX * BOARD_FRAME_SIDE_MAX; i++) {
        largest[1 + BOARD_FRAME_HEADER_LEN + i / 2] |= (i % 7) << (i % 2 ? 0 : 4);
    }
    seal_frame(largest, sizeof(largest));
    parser = (BoardFrameParser){0};
    check(feed(&parser, largest, sizeof(largest), frames) == 1, "25x25: one frame");
    // (0, 24) is cell 0, (24, 24) cell 24, (0, 0) cell 600 and (24, 0) cell 624
    check(
        frames[0].turn == 16383 && frames[0].length == 16383 && frames[0].hold_10ms == 16383 &&
            board_frame_cell_at(&frames[0], 0, 24) == 0 &&
            board_frame_cell_at(&frames[0], 24, 24) == 24 % 7 &&
            board_frame_cell_at(&frames[0], 0, 0) == 600 % 7 &&
            board_frame_cell_at(&frames[0], 24, 0) == 624 % 7,
        "25x25: decodes the largest values and the corner cells");

    uint8_t back_to_back[2 * sizeof(shared_vector)];
    memcpy(back_to_back, shared_vector, sizeof(shared_vector));
    memcpy(back_to_back + sizeof(shared_vector), shared_vector, sizeof(shared_vector));
    parser = (BoardFrameParser){0};
    check(
        feed(&parser, back_to_back, sizeof(back_to_back), frames) == 2,
        "back to back: two frames");
    check(
        is_shared_vector(&frames[0]) && is_shared_vector(&frames[1]),
        "back to back: both decode to the shared vector");

    printf(failures ? "FAILED: %d check(s)\n" : "All checks passed\n", failures);
    return failures ? 1 : 0;
}

static void check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if(!ok) failures++;
}

// Feeds the bytes one at a time, as the app does, and returns how many frames they completed
static int feed(BoardFrameParser* parser, const uint8_t* bytes, size_t len, BoardFrame* frames) {
    int count = 0;
    for(size_t i = 0; i < len; i++) {
        if(board_frame_parser_feed(parser, bytes[i], &frames[count])) count++;
    }
    return count;
}

// Top row (y = 1): my head, empty, food. Bottom row (y = 0): my body, my body, other head.
static bool is_shared_vector(const BoardFrame* frame) {
    return frame->flags == 0 && frame->width == 3 && frame->height == 2 && frame->turn == 200 &&
           frame->health == 85 && frame->length == 3 && frame->hold_10ms == 35 &&
           board_frame_cell_at(frame, 0, 1) == BoardCellMyHead &&
           board_frame_cell_at(frame, 1, 1) == BoardCellEmpty &&
           board_frame_cell_at(frame, 2, 1) == BoardCellFood &&
           board_frame_cell_at(frame, 0, 0) == BoardCellMyBody &&
           board_frame_cell_at(frame, 1, 0) == BoardCellMyBody &&
           board_frame_cell_at(frame, 2, 0) == BoardCellOtherHead;
}

// Writes the check bytes the server would send at the end of a frame (start byte first)
static void seal_frame(uint8_t* bytes, size_t len) {
    uint8_t s1 = 0;
    uint8_t s2 = 0;
    for(size_t i = 1; i < len - 2; i++) {
        s1 = (s1 + bytes[i]) % 128;
        s2 = (s2 + s1) % 128;
    }
    bytes[len - 2] = s1;
    bytes[len - 1] = s2;
}
