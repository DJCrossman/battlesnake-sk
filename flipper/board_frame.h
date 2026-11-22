#pragma once

// Board frames the server sends, passed through the Wi-Fi board to the UART unchanged.
// Plain C with no Furi calls, so the parser also builds and runs on the host (board_frame_test.c).
//
// Every byte from 0x80 up starts a message: the board's status bytes (0xA0-0xA7), a mode byte
// (0xB1-0xB3) or a frame (0xB0). Every byte inside a frame is below 0x80.

#include <stdbool.h>
#include <stdint.h>

#define BOARD_FRAME_START 0xB0

// Mode bytes come on their own, never inside a frame: 0xB1 + the mode
#define BOARD_MODE_BYTE_ASSIST 0xB1
#define BOARD_MODE_BYTE_AUTO   0xB3

#define BOARD_FRAME_SIDE_MAX   25
// flags, width, height, turn (2), health, length (2), hold (2)
#define BOARD_FRAME_HEADER_LEN 10
// Two cells per byte
#define BOARD_FRAME_CELLS_MAX  ((BOARD_FRAME_SIDE_MAX * BOARD_FRAME_SIDE_MAX + 1) / 2)

#define BOARD_FLAG_GAME_STARTED (1 << 0)
#define BOARD_FLAG_GAME_OVER    (1 << 1)
#define BOARD_FLAG_TOO_LARGE    (1 << 2) // width = height = 0, no cells
#define BOARD_FLAG_WON          (1 << 3) // at the end ours is the only snake left
#define BOARD_FLAG_LOST         (1 << 4) // at the end ours is not on the board

typedef enum {
    BoardCellEmpty = 0,
    BoardCellFood = 1,
    BoardCellHazard = 2,
    BoardCellMyBody = 3,
    BoardCellMyHead = 4,
    BoardCellOtherBody = 5,
    BoardCellOtherHead = 6,
} BoardCell;

typedef struct {
    uint8_t flags;
    uint8_t width;
    uint8_t height;
    uint16_t turn;
    uint8_t health;
    uint16_t length;
    uint16_t hold_10ms; // time left to press when the server sent it
    // Two cells per byte, first cell in the high nibble, top row first
    uint8_t cells[BOARD_FRAME_CELLS_MAX];
} BoardFrame;

typedef struct {
    bool in_frame;
    uint16_t received;
    // Everything after the start byte: header, cells and the two check bytes
    uint8_t payload[BOARD_FRAME_HEADER_LEN + BOARD_FRAME_CELLS_MAX + 2];
} BoardFrameParser;

/** Feeds one received byte; a zeroed parser is ready. Returns true when it completed a frame whose
 * check bytes match, which is then written to `frame` (and only then). A byte from 0x80 up drops
 * any partial frame, and 0xB0 starts the next. */
bool board_frame_parser_feed(BoardFrameParser* parser, uint8_t byte, BoardFrame* frame);

/** What is on the board at (x, y), with y = 0 at the bottom. */
BoardCell board_frame_cell_at(const BoardFrame* frame, uint8_t x, uint8_t y);
