#pragma once

// Board frames written by the pad web page over the Bluetooth serial link.
// Plain C with no Furi calls, so the parser also builds and runs on the host (board_frame_test.c).

#include <stdbool.h>
#include <stdint.h>

#define BOARD_FRAME_START       0x53
#define BOARD_FRAME_PAYLOAD_MAX 400
#define BOARD_FRAME_HEADER_LEN  8
#define BOARD_FRAME_SIDE_MAX    25
#define BOARD_FRAME_STALL_MS    500

#define BOARD_FLAG_GAME_STARTED   (1 << 0)
#define BOARD_FLAG_GAME_OVER      (1 << 1)
#define BOARD_FLAG_WAITING        (1 << 2)
#define BOARD_FLAG_TOO_LARGE      (1 << 3)
#define BOARD_FLAG_SERVER_LOST    (1 << 6)
#define BOARD_FLAG_MODE(flags)    (((flags) >> 4) & 0x3)

typedef enum {
    BoardModeAssist = 0,
    BoardModeManual = 1,
    BoardModeAuto = 2,
} BoardMode;

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
    uint8_t length;
    uint8_t hold_10ms;
    // Two cells per byte, first cell in the high nibble, top row first
    uint8_t cells[BOARD_FRAME_PAYLOAD_MAX - BOARD_FRAME_HEADER_LEN];
} BoardFrame;

typedef enum {
    BoardFrameParserSeekStart,
    BoardFrameParserLenLo,
    BoardFrameParserLenHi,
    BoardFrameParserPayload,
} BoardFrameParserStep;

typedef struct {
    BoardFrameParserStep step;
    uint16_t len;
    uint16_t received;
    uint32_t started_ms;
    uint8_t payload[BOARD_FRAME_PAYLOAD_MAX];
} BoardFrameParser;

/** Starts looking for a frame start. A zeroed parser is already reset. */
void board_frame_parser_reset(BoardFrameParser* parser);

/** Feeds one received byte. Returns true when it completed a valid frame, which is written to
 * `frame`. Over-long, malformed and stalled frames are dropped and the parser resynchronises. */
bool board_frame_parser_feed(
    BoardFrameParser* parser,
    uint8_t byte,
    uint32_t now_ms,
    BoardFrame* frame);

/** What is on the board at (x, y), with y = 0 at the bottom. */
BoardCell board_frame_cell_at(const BoardFrame* frame, uint8_t x, uint8_t y);
