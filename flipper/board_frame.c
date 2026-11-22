#include "board_frame.h"

#include <string.h>

static bool board_frame_received_intact(const BoardFrameParser* parser, uint16_t cells_len);

bool board_frame_parser_feed(BoardFrameParser* parser, uint8_t byte, BoardFrame* frame) {
    if(byte >= 0x80) {
        parser->in_frame = byte == BOARD_FRAME_START;
        parser->received = 0;
        return false;
    }
    // Outside a frame this is the ESP32's boot text
    if(!parser->in_frame) return false;

    const uint8_t* p = parser->payload;
    parser->payload[parser->received++] = byte;
    if(parser->received < 3) return false;
    // Width and height are in, and the length follows from them
    if(p[1] > BOARD_FRAME_SIDE_MAX || p[2] > BOARD_FRAME_SIDE_MAX) {
        parser->in_frame = false;
        return false;
    }
    const uint16_t cells_len = (p[1] * p[2] + 1) / 2;
    if(parser->received < BOARD_FRAME_HEADER_LEN + cells_len + 2) return false;

    parser->in_frame = false;
    if(!board_frame_received_intact(parser, cells_len)) return false;
    // Two-byte values come 7 bits at a time, low bits first
    frame->flags = p[0];
    frame->width = p[1];
    frame->height = p[2];
    frame->turn = p[3] | (uint16_t)p[4] << 7;
    frame->health = p[5];
    frame->length = p[6] | (uint16_t)p[7] << 7;
    frame->hold_10ms = p[8] | (uint16_t)p[9] << 7;
    memcpy(frame->cells, p + BOARD_FRAME_HEADER_LEN, cells_len);
    return true;
}

BoardCell board_frame_cell_at(const BoardFrame* frame, uint8_t x, uint8_t y) {
    const uint16_t index = (uint16_t)(frame->height - 1 - y) * frame->width + x;
    return (BoardCell)(index % 2 ? frame->cells[index / 2] & 0x0F : frame->cells[index / 2] >> 4);
}

// The check bytes match and every cell is one the server sends; anything else means bytes were
// lost or changed on the way. The low nibble after an odd number of cells is not a cell.
static bool board_frame_received_intact(const BoardFrameParser* parser, uint16_t cells_len) {
    const uint8_t* p = parser->payload;
    const uint16_t checked_len = BOARD_FRAME_HEADER_LEN + cells_len;
    uint8_t s1 = 0;
    uint8_t s2 = 0;
    for(uint16_t i = 0; i < checked_len; i++) {
        s1 = (s1 + p[i]) % 128;
        s2 = (s2 + s1) % 128;
    }
    if(s1 != p[checked_len] || s2 != p[checked_len + 1]) return false;

    for(uint16_t i = 0; i < p[1] * p[2]; i++) {
        const uint8_t pair = p[BOARD_FRAME_HEADER_LEN + i / 2];
        if((i % 2 ? pair & 0x0F : pair >> 4) > BoardCellOtherHead) return false;
    }
    return true;
}
