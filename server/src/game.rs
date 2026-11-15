//! The engine's payload and the board questions both the human path and the autopilot ask.

use std::borrow::Cow;

use serde::Deserialize;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Deserialize)]
pub struct Point {
    pub x: i32,
    pub y: i32,
}

#[derive(Debug, Deserialize)]
pub struct GameState<'a> {
    #[serde(borrow, default)]
    pub game: Game<'a>,
    #[serde(default)]
    pub turn: u32,
    pub board: Board<'a>,
    #[serde(borrow)]
    pub you: Snake<'a>,
}

#[derive(Debug, Deserialize)]
pub struct Game<'a> {
    #[serde(borrow, default)]
    pub id: Cow<'a, str>,
    #[serde(default = "default_timeout")]
    pub timeout: u32,
}

/// The largest board side accepted. The official maps top out at 25; anything past this is refused
/// when the payload is read, so the grids built from it stay small.
pub const MAX_BOARD_SIDE: i32 = 50;

#[derive(Debug, Deserialize)]
pub struct Board<'a> {
    /// 1..=`MAX_BOARD_SIDE`, enforced while deserializing.
    #[serde(deserialize_with = "playable_side")]
    pub height: i32,
    #[serde(deserialize_with = "playable_side")]
    pub width: i32,
    #[serde(default)]
    pub food: Vec<Point>,
    #[serde(default)]
    pub hazards: Vec<Point>,
    #[serde(borrow, default)]
    pub snakes: Vec<Snake<'a>>,
}

#[derive(Debug, Deserialize)]
pub struct Snake<'a> {
    #[serde(borrow, default)]
    pub id: Cow<'a, str>,
    #[serde(default)]
    pub health: i32,
    pub body: Vec<Point>,
    pub head: Point,
    #[serde(default)]
    pub length: Option<usize>,
    #[serde(borrow, default)]
    pub latency: Cow<'a, str>,
}

/// In Battlesnake `up` is y+1; y=0 is the bottom row.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Direction {
    Up,
    Down,
    Left,
    Right,
}

impl Direction {
    /// Fixed order: the autopilot breaks ties in favour of the earlier entry.
    pub const ALL: [Direction; 4] = [
        Direction::Up,
        Direction::Down,
        Direction::Left,
        Direction::Right,
    ];

    pub fn as_str(self) -> &'static str {
        match self {
            Direction::Up => "up",
            Direction::Down => "down",
            Direction::Left => "left",
            Direction::Right => "right",
        }
    }

    pub fn step(self, p: Point) -> Point {
        match self {
            Direction::Up => Point { x: p.x, y: p.y + 1 },
            Direction::Down => Point { x: p.x, y: p.y - 1 },
            Direction::Left => Point { x: p.x - 1, y: p.y },
            Direction::Right => Point { x: p.x + 1, y: p.y },
        }
    }
}

impl GameState<'_> {
    pub fn is_on_board(&self, p: Point) -> bool {
        p.x >= 0 && p.y >= 0 && p.x < self.board.width && p.y < self.board.height
    }

    /// The direction the snake is travelling, or `None` on turn 0 when its body is stacked.
    pub fn heading(&self) -> Option<Direction> {
        let neck = *self.you.body.get(1)?;
        Direction::ALL
            .into_iter()
            .find(|d| d.step(neck) == self.you.head)
    }

    /// Classic snake: turning back into your own neck is never allowed.
    pub fn is_reversal(&self, dir: Direction) -> bool {
        self.you
            .body
            .get(1)
            .is_some_and(|&neck| neck != self.you.head && dir.step(self.you.head) == neck)
    }

    /// Off the board, or into a body cell that will still be occupied after this turn.
    /// A tail moves away unless the snake just ate (its last two segments are equal).
    pub fn is_certain_death(&self, dir: Direction) -> bool {
        let next = dir.step(self.you.head);
        if !self.is_on_board(next) {
            return true;
        }
        self.board.snakes.iter().any(|snake| {
            let body = &snake.body;
            let just_ate = body.len() >= 2 && body[body.len() - 1] == body[body.len() - 2];
            let still_occupied = if just_ate {
                body.len()
            } else {
                body.len().saturating_sub(1)
            };
            body[..still_occupied].contains(&next)
        })
    }
}

fn playable_side<'de, D: serde::Deserializer<'de>>(deserializer: D) -> Result<i32, D::Error> {
    let side = i64::deserialize(deserializer)?;
    if (1..=i64::from(MAX_BOARD_SIDE)).contains(&side) {
        Ok(side as i32)
    } else {
        Err(serde::de::Error::custom(format!(
            "board side {side} is outside 1..={MAX_BOARD_SIDE}"
        )))
    }
}

fn default_timeout() -> u32 {
    500
}

impl Default for Game<'_> {
    fn default() -> Self {
        Game {
            id: Cow::Borrowed(""),
            timeout: default_timeout(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const HEADING_UP: &str = r#"{
        "game": {"id": "g", "timeout": 500}, "turn": 5,
        "board": {"height": 11, "width": 11, "food": [], "hazards": [],
            "snakes": [
                {"id": "me", "health": 90, "body": [{"x":5,"y":10},{"x":5,"y":9},{"x":5,"y":8}], "head": {"x":5,"y":10}},
                {"id": "fed", "health": 90, "body": [{"x":7,"y":10},{"x":6,"y":10},{"x":6,"y":10}], "head": {"x":7,"y":10}},
                {"id": "hungry", "health": 90, "body": [{"x":3,"y":8},{"x":3,"y":9},{"x":4,"y":9},{"x":4,"y":10}], "head": {"x":3,"y":8}}
            ]},
        "you": {"id": "me", "health": 90, "body": [{"x":5,"y":10},{"x":5,"y":9},{"x":5,"y":8}], "head": {"x":5,"y":10}, "length": 3}
    }"#;

    #[test]
    fn heading_follows_the_neck() {
        let state: GameState = serde_json::from_str(HEADING_UP).unwrap();
        assert_eq!(state.heading(), Some(Direction::Up));
        assert!(state.is_reversal(Direction::Down));
        assert!(!state.is_reversal(Direction::Left));
    }

    #[test]
    fn stacked_body_has_no_heading_and_no_reversal() {
        let state: GameState = serde_json::from_str(
            r#"{"board": {"height": 11, "width": 11},
                "you": {"body": [{"x":1,"y":1},{"x":1,"y":1},{"x":1,"y":1}], "head": {"x":1,"y":1}}}"#,
        )
        .unwrap();
        assert_eq!(state.heading(), None);
        assert!(Direction::ALL.iter().all(|&d| !state.is_reversal(d)));
    }

    #[test]
    fn certain_death_covers_walls_bodies_and_fed_tails_only() {
        let state: GameState = serde_json::from_str(HEADING_UP).unwrap();
        // Up leaves the board.
        assert!(state.is_certain_death(Direction::Up));
        // Right is (6,10): the tail of a snake that just ate, so it stays.
        assert!(state.is_certain_death(Direction::Right));
        // Left is (4,10): the tail of a snake that did not eat, so it moves away.
        assert!(!state.is_certain_death(Direction::Left));
        // Down is our own neck.
        assert!(state.is_certain_death(Direction::Down));
    }

    #[test]
    fn boards_outside_1_to_50_are_refused() {
        let parse = |width: i64, height: i64| {
            serde_json::from_str::<GameState>(&format!(
                r#"{{"board": {{"height": {height}, "width": {width}}},
                    "you": {{"body": [{{"x":0,"y":0}}], "head": {{"x":0,"y":0}}}}}}"#
            ))
            .is_ok()
        };
        assert!(parse(1, 1));
        assert!(parse(50, 50));
        assert!(!parse(0, 11));
        assert!(!parse(11, 51));
        assert!(!parse(65536, 65536));
        assert!(!parse(-11, 11));
    }

    #[test]
    fn missing_game_fields_take_engine_defaults() {
        let state: GameState = serde_json::from_str(
            r#"{"game": {}, "board": {"height": 11, "width": 11},
                "you": {"body": [{"x":1,"y":1}], "head": {"x":1,"y":1}, "latency": ""}}"#,
        )
        .unwrap();
        assert_eq!(state.game.timeout, 500);
        assert_eq!(state.game.id, "");
        assert!(state.is_on_board(Point { x: 10, y: 0 }));
        assert!(!state.is_on_board(Point { x: 11, y: 0 }));
    }
}
