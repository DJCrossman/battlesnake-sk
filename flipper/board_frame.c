#include "board_frame.h"

#include <string.h>

// Bytes after the start marker that are checked before the payload is trusted:
// length low, length high, flags, width, height
#define BOARD_FRAME_CHECKED_LEN 5

static bool board_frame_header_rejected(const BoardFrameParser* parser);
static void board_frame_parser_rescan(BoardFrameParser* parser, uint32_t now_ms, BoardFrame* frame);

bool board_frame_parser_feed(
    BoardFrameParser* parser,
    uint8_t byte,
    uint32_t now_ms,
    BoardFrame* frame) {
    if(parser->step != BoardFrameParserSeekStart &&
       now_ms - parser->started_ms > BOARD_FRAME_STALL_MS) {
        board_frame_parser_reset(parser);
    }

    switch(parser->step) {
    case BoardFrameParserSeekStart:
        if(byte == BOARD_FRAME_START) {
            parser->step = BoardFrameParserLenLo;
            parser->started_ms = now_ms;
        }
        return false;
    case BoardFrameParserLenLo:
        parser->len = byte;
        parser->step = BoardFrameParserLenHi;
        return false;
    case BoardFrameParserLenHi:
        parser->len |= (uint16_t)byte << 8;
        parser->received = 0;
        parser->step = BoardFrameParserPayload;
        if(board_frame_header_rejected(parser)) board_frame_parser_rescan(parser, now_ms, frame);
        return false;
    case BoardFrameParserPayload:
        parser->payload[parser->received++] = byte;
        if(board_frame_header_rejected(parser)) {
            board_frame_parser_rescan(parser, now_ms, frame);
            return false;
        }
        if(parser->received < parser->len) return false;

        parser->step = BoardFrameParserSeekStart;
        frame->flags = parser->payload[0];
        frame->width = parser->payload[1];
        frame->height = parser->payload[2];
        frame->turn = parser->payload[3] | (uint16_t)parser->payload[4] << 8;
        frame->health = parser->payload[5];
        frame->length = parser->payload[6];
        frame->hold_10ms = parser->payload[7];
        memcpy(
            frame->cells,
            parser->payload + BOARD_FRAME_HEADER_LEN,
            parser->len - BOARD_FRAME_HEADER_LEN);
        return true;
    }
    return false;
}

void board_frame_parser_reset(BoardFrameParser* parser) {
    parser->step = BoardFrameParserSeekStart;
    parser->len = 0;
    parser->received = 0;
}

BoardCell board_frame_cell_at(const BoardFrame* frame, uint8_t x, uint8_t y) {
    const uint16_t index = (uint16_t)(frame->height - 1 - y) * frame->width + x;
    return (BoardCell)(index % 2 ? frame->cells[index / 2] & 0x0F : frame->cells[index / 2] >> 4);
}

// The page always sends exactly 8 + ceil(width * height / 2) payload bytes, so anything else
// means the parser locked onto a 0x53 that was not a frame start.
static bool board_frame_header_rejected(const BoardFrameParser* parser) {
    if(parser->len < BOARD_FRAME_HEADER_LEN || parser->len > BOARD_FRAME_PAYLOAD_MAX) return true;
    if(parser->received < 3) return false;

    const uint8_t width = parser->payload[1];
    const uint8_t height = parser->payload[2];
    return width > BOARD_FRAME_SIDE_MAX || height > BOARD_FRAME_SIDE_MAX ||
           parser->len != BOARD_FRAME_HEADER_LEN + (width * height + 1) / 2;
}

// A rejected header may have swallowed the real start marker: look through the bytes it
// consumed (everything after the false 0x53) again.
// Too few bytes to complete a frame, so `frame` is never written here.
static void board_frame_parser_rescan(BoardFrameParser* parser, uint32_t now_ms, BoardFrame* frame) {
    uint8_t consumed[BOARD_FRAME_CHECKED_LEN] = {parser->len & 0xFF, parser->len >> 8};
    const uint8_t consumed_len = 2 + parser->received;
    memcpy(consumed + 2, parser->payload, parser->received);

    board_frame_parser_reset(parser);
    for(uint8_t i = 0; i < consumed_len; i++) {
        board_frame_parser_feed(parser, consumed[i], now_ms, frame);
    }
}
