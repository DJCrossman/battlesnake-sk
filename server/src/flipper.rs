//! The board as the Flipper draws it: one binary frame per broadcast event. Every byte after a
//! frame's start byte is below 0x80, so the Flipper can find where each frame starts on its UART.

use axum::body::Bytes;

use crate::game::{Direction, GameState, Point};

/// Which broadcast event a frame is for.
pub enum Moment {
    /// `/start`.
    Start,
    /// A `/move` for the attached snake; the player has `hold_ms` to press (0 in auto).
    Turn { hold_ms: u128 },
    /// The answer to that `/move`: ours one step on, as the next turn will show it.
    Moved(Direction),
    /// `/end` for the attached snake.
    End,
}

/// What a cell shows, lowest priority first: of two things on one cell, the later one is drawn.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
enum Cell {
    Empty,
    Hazard,
    Food,
    OtherBody,
    MyBody,
    OtherHead,
    MyHead,
}

/// Starts every board frame. 0xB1..=0xB3 are the mode bytes, 0xA0..=0xA7 the WiFi board's own.
const BOARD_FRAME: u8 = 0xB0;
const STARTED: u8 = 0x01;
const GAME_OVER: u8 = 0x02;
/// A side is over 25, more than the Flipper's screen draws, so the frame carries no cells.
const TOO_LARGE: u8 = 0x04;
/// At `/end` ours is the only snake left on the board.
const WON: u8 = 0x08;
/// At `/end` ours is not on the board.
const LOST: u8 = 0x10;

/// The frame for `moment`, drawn from that request's `state`. "Ours" is the snake on the board
/// with `you`'s id.
pub fn board_frame(state: &GameState, moment: Moment) -> Bytes {
    let you = &state.you;
    let mut flags = match moment {
        Moment::Start => STARTED,
        Moment::Turn { .. } | Moment::Moved(_) => 0,
        Moment::End => {
            let ours = state
                .board
                .snakes
                .iter()
                .filter(|snake| snake.id == you.id)
                .count();
            GAME_OVER
                | if ours == 0 {
                    LOST
                } else if ours == state.board.snakes.len() {
                    WON
                } else {
                    0
                }
        }
    };
    let (width, height) = match (
        u8::try_from(state.board.width),
        u8::try_from(state.board.height),
    ) {
        (Ok(width @ 0..=25), Ok(height @ 0..=25)) => (width, height),
        _ => {
            flags |= TOO_LARGE;
            (0, 0)
        }
    };
    // Each is sent as two 7-bit bytes, which carry 0..=16383.
    let turn = match moment {
        Moment::Moved(_) => state.turn.saturating_add(1),
        _ => state.turn,
    }
    .min(16383);
    let length = you.length.unwrap_or(you.body.len()).min(16383);
    let hold = match moment {
        // In units of 10 ms.
        Moment::Turn { hold_ms } => (hold_ms / 10).min(16383),
        _ => 0,
    };
    let mut frame = vec![
        BOARD_FRAME,
        flags,
        width,
        height,
        (turn & 0x7F) as u8,
        (turn >> 7) as u8,
        you.health.clamp(0, 100) as u8,
        (length & 0x7F) as u8,
        (length >> 7) as u8,
        (hold & 0x7F) as u8,
        (hold >> 7) as u8,
    ];

    let mut cells = vec![Cell::Empty; usize::from(width) * usize::from(height)];
    let mut draw = |p: Point, cell: Cell| {
        // Checked on the signed values first: a head can be off the board, a coordinate negative.
        if (0..i32::from(width)).contains(&p.x) && (0..i32::from(height)).contains(&p.y) {
            // The top row comes first.
            let shown =
                &mut cells[((i32::from(height) - 1 - p.y) * i32::from(width) + p.x) as usize];
            *shown = (*shown).max(cell);
        }
    };
    for &p in &state.board.hazards {
        draw(p, Cell::Hazard);
    }
    for &p in &state.board.food {
        draw(p, Cell::Food);
    }
    for snake in &state.board.snakes {
        let mine = snake.id == you.id;
        let (head, body) = match moment {
            // The tail is dropped and the new head drawn as the head, so the body ends one short.
            Moment::Moved(dir) if mine => (
                dir.step(snake.head),
                &snake.body[..snake.body.len().saturating_sub(1)],
            ),
            _ => (snake.head, &snake.body[..]),
        };
        for &p in body {
            draw(p, if mine { Cell::MyBody } else { Cell::OtherBody });
        }
        draw(head, if mine { Cell::MyHead } else { Cell::OtherHead });
    }

    // The Flipper's cell codes, two to a byte, the earlier cell in the high nibble.
    let code = |cell: Option<&Cell>| -> u8 {
        match cell {
            None | Some(Cell::Empty) => 0,
            Some(Cell::Food) => 1,
            Some(Cell::Hazard) => 2,
            Some(Cell::MyBody) => 3,
            Some(Cell::MyHead) => 4,
            Some(Cell::OtherBody) => 5,
            Some(Cell::OtherHead) => 6,
        }
    };
    frame.extend(
        cells
            .chunks(2)
            .map(|pair| (code(pair.first()) << 4) | code(pair.get(1))),
    );
    // Two running sums over everything after the start byte, kept below 0x80 like the rest.
    let (s1, s2) = frame[1..].iter().fold((0, 0), |(s1, s2), &b| {
        let s1 = (s1 + b) % 128;
        (s1, (s2 + s1) % 128)
    });
    frame.extend([s1, s2]);
    Bytes::from(frame)
}

#[cfg(test)]
mod tests {
    use super::*;

    // The protocol's shared vector, which the Flipper's parser is tested against too. Top row:
    // my head, empty, food. Bottom row: my body, my body, the other snake's head.
    const SHARED: &str = r#"{
        "turn": 200,
        "board": {"width": 3, "height": 2, "food": [{"x":2,"y":1}], "hazards": [], "snakes": [
            {"id": "me", "body": [{"x":0,"y":1},{"x":0,"y":0},{"x":1,"y":0}], "head": {"x":0,"y":1}},
            {"id": "other", "body": [{"x":2,"y":0}], "head": {"x":2,"y":0}}]},
        "you": {"id": "me", "health": 85, "body": [{"x":0,"y":1},{"x":0,"y":0},{"x":1,"y":0}], "head": {"x":0,"y":1}, "length": 3}
    }"#;

    /// Food and hazards on every cell of an 11x11 board, two snakes lying across all of it, and
    /// a turn, health and length past what the frame carries.
    fn busy_board() -> String {
        let cells: Vec<String> = (0..11)
            .flat_map(|y| (0..11).map(move |x| format!(r#"{{"x":{x},"y":{y}}}"#)))
            .collect();
        let all = cells.join(",");
        format!(
            r#"{{"turn": 4000000000,
                "board": {{"width": 11, "height": 11, "food": [{all}], "hazards": [{all}], "snakes": [
                    {{"id": "me", "body": [{all}], "head": {{"x":10,"y":10}}}},
                    {{"id": "other", "body": [{all}], "head": {{"x":0,"y":0}}}}]}},
                "you": {{"id": "me", "health": 1000, "body": [{all}], "head": {{"x":10,"y":10}}, "length": 99999}}}}"#
        )
    }

    #[test]
    fn the_shared_vector() {
        let state: GameState = serde_json::from_str(SHARED).unwrap();
        assert_eq!(
            board_frame(&state, Moment::Turn { hold_ms: 350 })[..],
            [
                0xB0, 0x00, 0x03, 0x02, 0x48, 0x01, 0x55, 0x03, 0x00, 0x23, 0x00, 0x40, 0x13, 0x36,
                0x52, 0x1B
            ]
        );
    }

    #[test]
    fn a_start_frame_is_flagged_and_holds_nothing() {
        let state: GameState = serde_json::from_str(SHARED).unwrap();
        assert_eq!(
            board_frame(&state, Moment::Start)[1..11],
            [0x01, 0x03, 0x02, 0x48, 0x01, 0x55, 0x03, 0x00, 0x00, 0x00]
        );
    }

    #[test]
    fn a_shared_cell_shows_what_outranks_the_rest() {
        // x=0 hazard and food; x=1 food and another body; x=2 their body and mine; x=3 my body
        // and their head; x=4 my head, a rival's head and both bodies; x=5 a hazard; x=6 nothing.
        let state: GameState = serde_json::from_str(
            r#"{"board": {"width": 7, "height": 1,
                "hazards": [{"x":0,"y":0},{"x":5,"y":0}], "food": [{"x":0,"y":0},{"x":1,"y":0}],
                "snakes": [
                    {"id": "other", "body": [{"x":3,"y":0},{"x":2,"y":0},{"x":1,"y":0}], "head": {"x":3,"y":0}},
                    {"id": "me", "body": [{"x":4,"y":0},{"x":3,"y":0},{"x":2,"y":0}], "head": {"x":4,"y":0}},
                    {"id": "rival", "body": [{"x":4,"y":0}], "head": {"x":4,"y":0}}]},
                "you": {"id": "me", "body": [{"x":4,"y":0},{"x":3,"y":0},{"x":2,"y":0}], "head": {"x":4,"y":0}}}"#,
        )
        .unwrap();
        let frame = board_frame(&state, Moment::Turn { hold_ms: 0 });
        // Cells food, other body, my body, other head, my head, hazard, empty.
        assert_eq!(frame[11..frame.len() - 2], [0x15, 0x36, 0x42, 0x00]);
    }

    #[test]
    fn moved_shows_ours_one_step_on_at_the_next_turn() {
        // Heading up from (1,1) with its tail at (0,0); another snake's head at (2,0).
        let state: GameState = serde_json::from_str(
            r#"{"turn": 7, "board": {"width": 3, "height": 3, "snakes": [
                    {"id": "me", "body": [{"x":1,"y":1},{"x":1,"y":0},{"x":0,"y":0}], "head": {"x":1,"y":1}},
                    {"id": "other", "body": [{"x":2,"y":0}], "head": {"x":2,"y":0}}]},
                "you": {"id": "me", "health": 50, "body": [{"x":1,"y":1},{"x":1,"y":0},{"x":0,"y":0}], "head": {"x":1,"y":1}, "length": 3}}"#,
        )
        .unwrap();
        let frame = board_frame(&state, Moment::Moved(Direction::Up));
        // Flags 0, 3x3, turn 8, health and length as they were, nothing to hold for. Then the
        // head at (1,2), the body at (1,1) and (1,0), the tail gone and the other head in place.
        assert_eq!(
            frame[1..frame.len() - 2],
            [
                0x00, 0x03, 0x03, 0x08, 0x00, 0x32, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x30, 0x03,
                0x60
            ]
        );
    }

    #[test]
    fn moved_on_turn_zero_leaves_the_stacked_body_behind_the_head() {
        let state: GameState = serde_json::from_str(
            r#"{"board": {"width": 3, "height": 3, "snakes": [
                    {"id": "me", "body": [{"x":1,"y":1},{"x":1,"y":1},{"x":1,"y":1}], "head": {"x":1,"y":1}}]},
                "you": {"id": "me", "body": [{"x":1,"y":1},{"x":1,"y":1},{"x":1,"y":1}], "head": {"x":1,"y":1}}}"#,
        )
        .unwrap();
        let frame = board_frame(&state, Moment::Moved(Direction::Right));
        assert_eq!(frame[4..6], [0x01, 0x00]);
        // My body at (1,1), my head at (2,1).
        assert_eq!(frame[11..16], [0x00, 0x00, 0x34, 0x00, 0x00]);
    }

    #[test]
    fn positions_off_the_board_are_skipped() {
        // Our head ran into the right wall at (3,0); theirs is at (-1,1). Food just past the top
        // row and at the far ends of i32, hazards below the board.
        let state: GameState = serde_json::from_str(
            r#"{"board": {"width": 3, "height": 2,
                "food": [{"x":1,"y":2},{"x":-2147483648,"y":0},{"x":0,"y":2147483647}],
                "hazards": [{"x":0,"y":-1},{"x":-2147483648,"y":-2147483648}],
                "snakes": [
                    {"id": "me", "body": [{"x":3,"y":0},{"x":2,"y":0},{"x":1,"y":0}], "head": {"x":3,"y":0}},
                    {"id": "other", "body": [{"x":-1,"y":1},{"x":0,"y":1}], "head": {"x":-1,"y":1}}]},
                "you": {"id": "me", "health": -5, "body": [{"x":3,"y":0},{"x":2,"y":0},{"x":1,"y":0}], "head": {"x":3,"y":0}}}"#,
        )
        .unwrap();
        let turn = board_frame(&state, Moment::Turn { hold_ms: 350 });
        assert_eq!(turn[6], 0, "health clamps at 0");
        // Their body at (0,1); mine at (1,0) and (2,0).
        assert_eq!(turn[11..14], [0x50, 0x00, 0x33]);
        // One more step right: the head at (4,0) is skipped too, and (1,0) is the dropped tail.
        let moved = board_frame(&state, Moment::Moved(Direction::Right));
        assert_eq!(moved[11..14], [0x50, 0x00, 0x03]);
    }

    #[test]
    fn an_empty_body_draws_just_the_head() {
        let state: GameState = serde_json::from_str(
            r#"{"board": {"width": 3, "height": 3, "snakes": [{"id": "me", "body": [], "head": {"x":1,"y":1}}]},
                "you": {"id": "me", "body": [], "head": {"x":1,"y":1}}}"#,
        )
        .unwrap();
        let turn = board_frame(&state, Moment::Turn { hold_ms: 350 });
        assert_eq!(turn[7..9], [0x00, 0x00], "length is the body's");
        assert_eq!(turn[11..16], [0x00, 0x00, 0x40, 0x00, 0x00]);
        let moved = board_frame(&state, Moment::Moved(Direction::Down));
        assert_eq!(moved[11..16], [0x00, 0x00, 0x00, 0x04, 0x00]);
        assert_eq!(board_frame(&state, Moment::End)[1], 0x0A);
    }

    #[test]
    fn a_board_too_large_to_draw_is_flagged_and_sent_without_cells() {
        for (width, height, flags_and_size, len) in [
            (26, 25, [0x05, 0, 0], 13),
            (25, 26, [0x05, 0, 0], 13),
            (50, 50, [0x05, 0, 0], 13),
            // The largest the Flipper draws.
            (25, 25, [0x01, 25, 25], 1 + 10 + 313 + 2),
        ] {
            let json = format!(
                r#"{{"board": {{"width": {width}, "height": {height}}},
                    "you": {{"body": [{{"x":0,"y":0}}], "head": {{"x":0,"y":0}}}}}}"#
            );
            let state: GameState = serde_json::from_str(&json).unwrap();
            let frame = board_frame(&state, Moment::Start);
            assert_eq!(frame[1..4], flags_and_size, "{width}x{height}");
            assert_eq!(frame.len(), len, "{width}x{height}");
        }
    }

    #[test]
    fn every_byte_after_the_start_is_below_0x80() {
        let json = busy_board();
        let state: GameState = serde_json::from_str(&json).unwrap();
        let moments = [
            Moment::Start,
            Moment::Turn { hold_ms: u128::MAX },
            Moment::End,
        ]
        .into_iter()
        .chain(Direction::ALL.map(Moment::Moved));
        for moment in moments {
            let frame = board_frame(&state, moment);
            assert_eq!(frame[0], 0xB0);
            assert_eq!(frame.len(), 74);
            assert!(frame[1..].iter().all(|&b| b < 0x80), "{frame:?}");
        }
        // Turn, health, length and hold at their largest.
        assert_eq!(
            board_frame(&state, Moment::Turn { hold_ms: u128::MAX })[4..11],
            [0x7F, 0x7F, 100, 0x7F, 0x7F, 0x7F, 0x7F]
        );
    }

    #[test]
    fn the_check_bytes_sum_everything_after_the_start() {
        // Flags 0x05 then nine zeros: s1 is 5 throughout, so s2 is 5 x 10.
        let state: GameState = serde_json::from_str(
            r#"{"board": {"width": 26, "height": 25}, "you": {"body": [], "head": {"x":0,"y":0}}}"#,
        )
        .unwrap();
        assert_eq!(
            board_frame(&state, Moment::Start)[..],
            [0xB0, 0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x05, 0x32]
        );

        // Over a busy board the sums wrap at 128 many times over.
        let json = busy_board();
        let state: GameState = serde_json::from_str(&json).unwrap();
        let frame = board_frame(&state, Moment::Turn { hold_ms: 350 });
        let (body, check) = frame[1..].split_at(frame.len() - 3);
        let (mut s1, mut s2) = (0, 0);
        for &b in body {
            s1 = (s1 + b) % 128;
            s2 = (s2 + s1) % 128;
        }
        assert_eq!(check, [s1, s2]);
    }

    #[test]
    fn game_over_says_whether_we_won_lost_or_neither() {
        let me = r#"{"id": "me", "body": [{"x":0,"y":0}], "head": {"x":0,"y":0}}"#;
        let other = r#"{"id": "other", "body": [{"x":2,"y":2}], "head": {"x":2,"y":2}}"#;
        for (snakes, flags) in [
            // Only ours left: won.
            (me.to_owned(), 0x0A),
            // Ours is gone: lost, also when every snake went at once.
            (other.to_owned(), 0x12),
            (String::new(), 0x12),
            // Several left: the game was stopped, game over only.
            (format!("{me},{other}"), 0x02),
        ] {
            let json = format!(
                r#"{{"board": {{"width": 3, "height": 3, "snakes": [{snakes}]}},
                    "you": {{"id": "me", "body": [{{"x":0,"y":0}}], "head": {{"x":0,"y":0}}}}}}"#
            );
            let state: GameState = serde_json::from_str(&json).unwrap();
            let frame = board_frame(&state, Moment::End);
            assert_eq!(frame[1], flags, "{snakes}");
            assert_eq!(frame[9..11], [0x00, 0x00], "{snakes}");
        }
    }
}
