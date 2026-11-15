#include "board_frame.h"

#include <string.h>

static bool board_frame_decode(const uint8_t* payload, uint16_t len, BoardFrame* frame);

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
        if(parser->len < BOARD_FRAME_HEADER_LEN || parser->len > BOARD_FRAME_PAYLOAD_MAX) {
            board_frame_parser_reset(parser);
        } else {
            parser->received = 0;
            parser->step = BoardFrameParserPayload;
        }
        return false;
    case BoardFrameParserPayload:
        parser->payload[parser->received++] = byte;
        if(parser->received < parser->len) return false;
        parser->step = BoardFrameParserSeekStart;
        return board_frame_decode(parser->payload, parser->len, frame);
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
    const uint8_t pair = frame->cells[index / 2];
    return (BoardCell)(index % 2 ? pair & 0x0F : pair >> 4);
}

static bool board_frame_decode(const uint8_t* payload, uint16_t len, BoardFrame* frame) {
    const uint8_t width = payload[1];
    const uint8_t height = payload[2];
    if(width > BOARD_FRAME_SIDE_MAX || height > BOARD_FRAME_SIDE_MAX) return false;

    const uint16_t cell_bytes = (width * height + 1) / 2;
    if(len < BOARD_FRAME_HEADER_LEN + cell_bytes) return false;

    frame->flags = payload[0];
    frame->width = width;
    frame->height = height;
    frame->turn = payload[3] | (uint16_t)payload[4] << 8;
    frame->health = payload[5];
    frame->length = payload[6];
    frame->hold_10ms = payload[7];
    memcpy(frame->cells, payload + BOARD_FRAME_HEADER_LEN, cell_bytes);
    return true;
}
