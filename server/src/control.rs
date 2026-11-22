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
    /// The snake the pad steers: ours in the latest `/start`, or in the first game seen on `/move`.
    attached: Option<Attachment>,
    pub overhead: Overhead,
    /// How long we took to answer the attached snake's previous `/move`.
    pub last_spent_ms: Option<f64>,
    /// Counts the attached snake's `/move` requests. A request still waiting when a newer one
    /// arrives has been given up on by the engine, so it stops without taking a press.
    pub latest_move: u64,
}

/// One snake in one game: the same game can hold two of our snakes.
struct Attachment {
    game: String,
    snake: String,
}

/// Presses waiting for a turn. Two are kept so a quick double turn works; more are dropped.
#[derive(Debug, Default)]
pub struct InputQueue {
    inputs: VecDeque<Direction>,
}

/// 20x the engine's 500 ms default: no real game is clipped, and a forged one counts as 10 s.
const TIMEOUT_CAP_MS: f64 = 10_000.0;

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

impl Control {
    /// Hands the pad to `state`'s snake. Presses and timings from before belong to another game.
    pub fn attach(&mut self, state: &GameState) {
        self.attached = Some(Attachment {
            game: state.game.id.to_string(),
            snake: state.you.id.to_string(),
        });
        self.queue.clear();
        self.last_spent_ms = None;
        self.overhead = Overhead::default();
    }

    /// Releases the pad if `state`'s snake holds it; returns whether it did.
    pub fn detach(&mut self, state: &GameState) -> bool {
        if !self.is_attached(state) {
            return false;
        }
        self.attached = None;
        self.queue.clear();
        true
    }

    pub fn is_attached(&self, state: &GameState) -> bool {
        self.attached
            .as_ref()
            .is_some_and(|a| a.game == state.game.id && a.snake == state.you.id)
    }

    /// Whether any snake is attached, i.e. whether a press can be played at all.
    pub fn has_attachment(&self) -> bool {
        self.attached.is_some()
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
    /// Learns from one turn: the engine measured `latency_ms` for our previous answer, of which
    /// we spent `spent_ms` ourselves. A higher sample is taken at once, a lower one slowly, and a
    /// timeout (latency at or past the game's timeout, capped at 10 s) adds 100 ms on top, so a
    /// sample is never more than 10 s.
    pub fn observe(&mut self, latency_ms: f64, spent_ms: f64, timeout_ms: f64) {
        if latency_ms >= timeout_ms.min(TIMEOUT_CAP_MS) {
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

    /// How long the server may wait for the player: the game's timeout (capped at 10 s) minus the
    /// overhead and a 50 ms safety margin.
    pub fn hold(self, timeout_ms: u32) -> Duration {
        Duration::from_secs_f64(
            (f64::from(timeout_ms).min(TIMEOUT_CAP_MS) - self.ms - 50.0).max(0.0) / 1000.0,
        )
    }
}

/// Until the engine reports a latency, assume 100 ms.
impl Default for Overhead {
    fn default() -> Self {
        Overhead { ms: 100.0 }
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
        assert_eq!(queue.inputs.len(), 2);
        queue.clear();
        assert!(queue.inputs.is_empty());
    }

    #[test]
    fn attaching_starts_from_a_clean_slate() {
        let state = bottom_row();
        let mut control = Control::default();
        control.queue.push(Direction::Up);
        control.last_spent_ms = Some(340.0);
        control.attach(&state);
        assert!(control.is_attached(&state));
        assert!(control.queue.inputs.is_empty());
        assert_eq!(control.last_spent_ms, None);
    }

    #[test]
    fn attaching_forgets_the_last_game_s_overhead() {
        let mut control = Control::default();
        control.overhead.observe(400.0, 0.0, 500.0);
        control.attach(&bottom_row());
        assert_eq!(control.overhead.ms, 100.0);
    }

    #[test]
    fn attachment_is_one_snake_in_one_game() {
        let state = bottom_row();
        let mut control = Control::default();
        control.attach(&state);
        let other_snake = BOTTOM_ROW.replace(r#""you": {"id": "me""#, r#""you": {"id": "other""#);
        let other_snake: GameState = serde_json::from_str(&other_snake).unwrap();
        let other_game = BOTTOM_ROW.replace(r#""id": "g""#, r#""id": "g2""#);
        let other_game: GameState = serde_json::from_str(&other_game).unwrap();
        assert!(!control.is_attached(&other_snake));
        assert!(!control.is_attached(&other_game));
        assert!(!control.detach(&other_snake));

        control.queue.push(Direction::Right);
        assert!(control.detach(&state));
        assert!(!control.has_attachment());
        assert!(control.queue.inputs.is_empty());
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
        assert_eq!(
            queue.take_usable(&state, Mode::Assist),
            Some(Direction::Right)
        );
        assert!(queue.inputs.is_empty());

        queue.push(Direction::Down);
        assert_eq!(queue.take_usable(&state, Mode::Assist), None);
        assert!(queue.inputs.is_empty());
    }

    #[test]
    fn manual_obeys_fatal_presses() {
        let state = bottom_row();
        let mut queue = InputQueue::default();
        queue.push(Direction::Left);
        queue.push(Direction::Down);
        assert_eq!(
            queue.take_usable(&state, Mode::Manual),
            Some(Direction::Down)
        );
    }

    #[test]
    fn deadline_goes_straight_when_it_is_safe() {
        let state = bottom_row();
        assert_eq!(
            move_at_deadline(&state, Mode::Assist),
            Some(Direction::Right)
        );
        assert_eq!(
            move_at_deadline(&state, Mode::Manual),
            Some(Direction::Right)
        );
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
        let overhead = Overhead::default();
        assert_eq!(overhead.ms, 100.0);
        assert_eq!(overhead.hold(500), Duration::from_millis(350));
    }

    #[test]
    fn overhead_rises_at_once_and_decays_slowly() {
        let mut overhead = Overhead::default();
        overhead.observe(400.0, 250.0, 500.0);
        assert_eq!(overhead.ms, 150.0);
        overhead.observe(300.0, 250.0, 500.0);
        assert!((overhead.ms - 140.0).abs() < 1e-9);
    }

    #[test]
    fn overhead_grows_by_100_ms_after_a_timeout() {
        let mut overhead = Overhead::default();
        overhead.observe(500.0, 300.0, 500.0);
        assert_eq!(overhead.ms, 200.0);
    }

    #[test]
    fn hold_never_goes_negative() {
        let mut overhead = Overhead::default();
        for _ in 0..10 {
            overhead.observe(500.0, 0.0, 500.0);
        }
        assert_eq!(overhead.hold(500), Duration::ZERO);
    }

    #[test]
    fn a_forged_timeout_counts_as_10_s_at_most() {
        let mut overhead = Overhead::default();
        assert_eq!(overhead.hold(u32::MAX), Duration::from_millis(9_850));
        // A latency just under a u32::MAX timeout is past the cap, so it is one more timeout.
        overhead.observe(4_294_967_294.0, 0.0, 4_294_967_295.0);
        assert_eq!(overhead.ms, 200.0);
        overhead.observe(9_999.0, 0.0, 4_294_967_295.0);
        assert_eq!(overhead.ms, 9_999.0);
    }
}
