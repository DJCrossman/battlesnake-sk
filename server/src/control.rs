//! The rules for turning the player's key presses into moves, and the timing they run against.

use std::collections::VecDeque;
use std::time::Duration;

use crate::game::{Direction, GameState};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Mode {
    /// The player steers; presses that are certain death are ignored.
    Assist,
    /// The player steers; every press except a reversal is obeyed.
    Manual,
    /// The autopilot steers.
    Auto,
}

/// Who chose a move, as reported to the pad.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Source {
    Pad,
    Straight,
    Auto,
}

/// Everything about the player's game that changes between requests.
#[derive(Default)]
pub struct Control {
    pub queue: InputQueue,
    /// The game the pad steers: the latest `/start`, or the first game seen on `/move`.
    pub attached: Option<String>,
    pub overhead: Overhead,
    /// How long we took to answer the attached game's previous `/move`.
    pub last_spent_ms: Option<f64>,
}

/// Presses waiting for a turn. Two are kept so a quick double turn works; more are dropped.
#[derive(Debug, Default)]
pub struct InputQueue {
    inputs: VecDeque<Direction>,
}

/// The engine-side cost of a move that we do not see: network, TLS, the engine itself.
#[derive(Debug, Clone, Copy)]
pub struct Overhead {
    ms: f64,
}

impl Mode {
    pub fn as_str(self) -> &'static str {
        match self {
            Mode::Assist => "assist",
            Mode::Manual => "manual",
            Mode::Auto => "auto",
        }
    }

    /// The pad's mode button: assist → manual → auto → assist.
    pub fn next(self) -> Mode {
        match self {
            Mode::Assist => Mode::Manual,
            Mode::Manual => Mode::Auto,
            Mode::Auto => Mode::Assist,
        }
    }
}

impl From<u8> for Mode {
    fn from(value: u8) -> Self {
        match value {
            1 => Mode::Manual,
            2 => Mode::Auto,
            _ => Mode::Assist,
        }
    }
}

impl From<Mode> for u8 {
    fn from(mode: Mode) -> Self {
        match mode {
            Mode::Assist => 0,
            Mode::Manual => 1,
            Mode::Auto => 2,
        }
    }
}

impl Source {
    pub fn as_str(self) -> &'static str {
        match self {
            Source::Pad => "pad",
            Source::Straight => "straight",
            Source::Auto => "auto",
        }
    }
}

impl InputQueue {
    /// Queues a press; returns false when the queue is full and the press was dropped.
    pub fn push(&mut self, dir: Direction) -> bool {
        if self.inputs.len() >= 2 {
            return false;
        }
        self.inputs.push_back(dir);
        true
    }

    pub fn clear(&mut self) {
        self.inputs.clear();
    }

    pub fn len(&self) -> usize {
        self.inputs.len()
    }

    pub fn is_empty(&self) -> bool {
        self.inputs.is_empty()
    }

    /// The oldest press that may be played this turn. Presses that may not are discarded.
    pub fn take_usable(&mut self, state: &GameState, mode: Mode) -> Option<Direction> {
        while let Some(dir) = self.inputs.pop_front() {
            if is_usable_input(state, mode, dir) {
                return Some(dir);
            }
        }
        None
    }
}

/// A reversal into the neck is never played; in assist a press that is certain death is not either.
pub fn is_usable_input(state: &GameState, mode: Mode, dir: Direction) -> bool {
    !state.is_reversal(dir) && (mode != Mode::Assist || !state.is_certain_death(dir))
}

/// What to play when the player has not pressed anything usable in time: keep going straight
/// (in assist only when that is not certain death). `None` means "ask the autopilot", which is
/// also the answer on turn 0, when the snake has no heading yet.
pub fn move_at_deadline(state: &GameState, mode: Mode) -> Option<Direction> {
    let straight = state.heading()?;
    match mode {
        Mode::Manual => Some(straight),
        Mode::Assist if !state.is_certain_death(straight) => Some(straight),
        Mode::Assist | Mode::Auto => None,
    }
}

impl Overhead {
    pub fn new() -> Self {
        Overhead { ms: 100.0 }
    }

    pub fn ms(self) -> f64 {
        self.ms
    }

    /// Learns from one turn: the engine measured `latency_ms` for our previous answer, of which
    /// we spent `spent_ms` ourselves. A higher sample is taken at once, a lower one slowly, and a
    /// timeout (latency at or past the game's timeout) adds 100 ms on top.
    pub fn observe(&mut self, latency_ms: f64, spent_ms: f64, timeout_ms: f64) {
        if latency_ms >= timeout_ms {
            self.ms += 100.0;
            return;
        }
        let sample = (latency_ms - spent_ms).max(0.0);
        self.ms = if sample > self.ms {
            sample
        } else {
            self.ms * 0.9 + sample * 0.1
        };
    }

    /// How long the server may wait for the player: the game's timeout minus the overhead and a
    /// 50 ms safety margin.
    pub fn hold(self, timeout_ms: u32) -> Duration {
        Duration::from_secs_f64((f64::from(timeout_ms) - self.ms - 50.0).max(0.0) / 1000.0)
    }
}

impl Default for Overhead {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Heading right along the bottom row: (2,0) head, (1,0) neck, (0,0) tail. A longer snake
    // lies across row 1 from x=2 to x=4, its tail stacked at (4,1) because it just ate.
    const BOTTOM_ROW: &str = r#"{
        "game": {"id": "g", "timeout": 500}, "turn": 7,
        "board": {"height": 5, "width": 5, "food": [], "hazards": [], "snakes": [
            {"id": "me", "body": [{"x":2,"y":0},{"x":1,"y":0},{"x":0,"y":0}], "head": {"x":2,"y":0}},
            {"id": "other", "body": [{"x":2,"y":1},{"x":3,"y":1},{"x":4,"y":1},{"x":4,"y":1}], "head": {"x":2,"y":1}}]},
        "you": {"id": "me", "health": 80, "body": [{"x":2,"y":0},{"x":1,"y":0},{"x":0,"y":0}], "head": {"x":2,"y":0}, "length": 3}
    }"#;

    fn bottom_row() -> GameState<'static> {
        serde_json::from_str(BOTTOM_ROW).unwrap()
    }

    #[test]
    fn queue_keeps_at_most_two_presses() {
        let mut queue = InputQueue::default();
        assert!(queue.push(Direction::Up));
        assert!(queue.push(Direction::Left));
        assert!(!queue.push(Direction::Right));
        assert_eq!(queue.len(), 2);
        queue.clear();
        assert!(queue.is_empty());
    }

    #[test]
    fn reversal_is_dropped_in_every_mode() {
        let state = bottom_row();
        for mode in [Mode::Assist, Mode::Manual, Mode::Auto] {
            assert!(!is_usable_input(&state, mode, Direction::Left), "{mode:?}");
        }
    }

    #[test]
    fn assist_drops_certain_death_and_takes_the_next_press() {
        let state = bottom_row();
        let mut queue = InputQueue::default();
        // Up runs into the other snake's head cell (its neck next turn); right is free.
        queue.push(Direction::Up);
        queue.push(Direction::Right);
        assert_eq!(queue.take_usable(&state, Mode::Assist), Some(Direction::Right));
        assert!(queue.is_empty());

        queue.push(Direction::Down);
        assert_eq!(queue.take_usable(&state, Mode::Assist), None);
        assert!(queue.is_empty());
    }

    #[test]
    fn manual_obeys_fatal_presses() {
        let state = bottom_row();
        let mut queue = InputQueue::default();
        queue.push(Direction::Left);
        queue.push(Direction::Down);
        assert_eq!(queue.take_usable(&state, Mode::Manual), Some(Direction::Down));
    }

    #[test]
    fn deadline_goes_straight_when_it_is_safe() {
        let state = bottom_row();
        assert_eq!(move_at_deadline(&state, Mode::Assist), Some(Direction::Right));
        assert_eq!(move_at_deadline(&state, Mode::Manual), Some(Direction::Right));
    }

    #[test]
    fn deadline_in_assist_hands_a_fatal_straight_to_the_autopilot() {
        // Heading up into the top wall.
        let state: GameState = serde_json::from_str(
            r#"{"board": {"height": 5, "width": 5},
                "you": {"body": [{"x":2,"y":4},{"x":2,"y":3}], "head": {"x":2,"y":4}}}"#,
        )
        .unwrap();
        assert_eq!(move_at_deadline(&state, Mode::Assist), None);
        assert_eq!(move_at_deadline(&state, Mode::Manual), Some(Direction::Up));
    }

    #[test]
    fn deadline_on_turn_zero_asks_the_autopilot() {
        let state: GameState = serde_json::from_str(
            r#"{"board": {"height": 5, "width": 5},
                "you": {"body": [{"x":2,"y":2},{"x":2,"y":2},{"x":2,"y":2}], "head": {"x":2,"y":2}}}"#,
        )
        .unwrap();
        assert_eq!(move_at_deadline(&state, Mode::Assist), None);
        assert_eq!(move_at_deadline(&state, Mode::Manual), None);
    }

    #[test]
    fn mode_button_cycles_assist_manual_auto() {
        assert_eq!(Mode::Assist.next(), Mode::Manual);
        assert_eq!(Mode::Manual.next(), Mode::Auto);
        assert_eq!(Mode::Auto.next(), Mode::Assist);
        for mode in [Mode::Assist, Mode::Manual, Mode::Auto] {
            assert_eq!(Mode::from(u8::from(mode)), mode);
        }
    }

    #[test]
    fn overhead_starts_at_100_ms() {
        let overhead = Overhead::new();
        assert_eq!(overhead.ms(), 100.0);
        assert_eq!(overhead.hold(500), Duration::from_millis(350));
    }

    #[test]
    fn overhead_rises_at_once_and_decays_slowly() {
        let mut overhead = Overhead::new();
        overhead.observe(400.0, 250.0, 500.0);
        assert_eq!(overhead.ms(), 150.0);
        overhead.observe(300.0, 250.0, 500.0);
        assert!((overhead.ms() - 140.0).abs() < 1e-9);
    }

    #[test]
    fn overhead_grows_by_100_ms_after_a_timeout() {
        let mut overhead = Overhead::new();
        overhead.observe(500.0, 300.0, 500.0);
        assert_eq!(overhead.ms(), 200.0);
    }

    #[test]
    fn hold_never_goes_negative() {
        let mut overhead = Overhead::new();
        for _ in 0..10 {
            overhead.observe(500.0, 0.0, 500.0);
        }
        assert_eq!(overhead.hold(500), Duration::ZERO);
    }
}
