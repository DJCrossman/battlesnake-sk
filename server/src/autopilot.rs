//! Deterministic port of the TypeScript `MovementService`/`BoundaryService` heuristics.
//!
//! As in the original, the look-ahead moves only our own snake: every other snake, the food and
//! our health stay as they were on the turn being decided, and our original body stays an
//! obstacle for the whole search.

use crate::game::{Direction, GameState, Point};

/// Expansions below the root before a branch is scored by its weight alone. The original had no
/// depth limit and was cut off by a 250 ms clock instead; this keeps the worst case (an open board
/// with three safe moves at every step) to a few thousand nodes.
const MAX_DEPTH: usize = 6;
/// The flood fill stops counting at this many cells; it is also the space bonus's denominator.
const SPACE_CAP: usize = 30;

#[derive(Clone, Copy, Default)]
struct Cell {
    /// A snake body segment (any snake, us included, every segment) or a hazard.
    blocked: bool,
    food: bool,
    /// Length of the longest other snake whose head can reach this cell next turn (0 = none).
    threat: usize,
    /// First index of this cell in our original body, `usize::MAX` when it is not part of it.
    own_index: usize,
    /// Flood-fill generation that last reached this cell.
    seen: u32,
}

/// The autopilot's move: the highest-scoring direction, earliest in `Direction::ALL` on ties.
/// When every direction scores 0 it takes the first one that is not certain death, and only
/// answers `right` (the original's constant answer) when all of them are.
pub fn choose_move(state: &GameState) -> Direction {
    let scores = score_moves(state);
    let mut best = None;
    let mut best_score = 0.0;
    for (dir, score) in Direction::ALL.into_iter().zip(scores) {
        if score > best_score {
            best = Some(dir);
            best_score = score;
        }
    }
    best.or_else(|| {
        Direction::ALL
            .into_iter()
            .find(|&d| !state.is_certain_death(d))
    })
    .unwrap_or(Direction::Right)
}

/// Each direction's look-ahead score, in `Direction::ALL` order. 0 means the move is ruled out.
pub fn score_moves(state: &GameState) -> [f64; 4] {
    if state.you.body.is_empty() || state.board.width <= 0 || state.board.height <= 0 {
        return [0.0; 4];
    }
    let mut search = Search::new(state);
    let body_len = state.you.body.len();
    Direction::ALL.map(|dir| search.score_move(state.you.head, dir, 1.0, 0, body_len))
}

struct Search<'s, 'a> {
    state: &'s GameState<'a>,
    cells: Vec<Cell>,
    /// `path[k]` is our head after move `k + 1` of the branch being explored.
    path: [Point; MAX_DEPTH],
    /// Our snake only grows while its body is exactly this long, as in the original.
    length: usize,
    stop_weight: f64,
    generation: u32,
    queue: [Point; 4 * SPACE_CAP + 8],
}

impl<'s, 'a> Search<'s, 'a> {
    fn new(state: &'s GameState<'a>) -> Self {
        let board = &state.board;
        let mut search = Search {
            state,
            cells: vec![
                Cell {
                    own_index: usize::MAX,
                    ..Cell::default()
                };
                (board.width * board.height) as usize
            ],
            path: [Point { x: 0, y: 0 }; MAX_DEPTH],
            length: state.you.length.unwrap_or(state.you.body.len()),
            stop_weight: 1.0 / (f64::from(board.height) * 20.0),
            generation: 0,
            queue: [Point { x: 0, y: 0 }; 4 * SPACE_CAP + 8],
        };
        for snake in &board.snakes {
            for &p in &snake.body {
                if let Some(i) = search.index(p) {
                    search.cells[i].blocked = true;
                }
            }
            if snake.id == state.you.id {
                continue;
            }
            for dir in Direction::ALL {
                if let Some(i) = search.index(dir.step(snake.head)) {
                    search.cells[i].threat = search.cells[i].threat.max(snake.body.len());
                }
            }
        }
        for &p in &board.hazards {
            if let Some(i) = search.index(p) {
                search.cells[i].blocked = true;
            }
        }
        for &p in &board.food {
            if let Some(i) = search.index(p) {
                search.cells[i].food = true;
            }
        }
        for (n, &p) in state.you.body.iter().enumerate() {
            if let Some(i) = search.index(p) {
                search.cells[i].own_index = search.cells[i].own_index.min(n);
            }
        }
        search
    }

    /// `calculateMove` + `calculateWeight`: 0 for a ruled-out move, otherwise the decayed
    /// look-ahead weight plus the space bonus.
    fn score_move(
        &mut self,
        from: Point,
        dir: Direction,
        weight: f64,
        depth: usize,
        body_len: usize,
    ) -> f64 {
        let to = dir.step(from);
        let Some(i) = self.index(to) else {
            return 0.0;
        };
        let cell = self.cells[i];
        let new_len = if cell.food && body_len == self.length {
            body_len + 1
        } else {
            body_len
        };
        if cell.blocked || cell.threat >= new_len || self.runs_into_own_body(to, depth, new_len) {
            return 0.0;
        }

        let look_ahead = if weight < self.stop_weight || depth == MAX_DEPTH {
            weight
        } else {
            let health = self.state.you.health;
            let toward_food = self.is_moving_toward_food(from, to);
            // The original's 0.3 "conflict" factor is not here: its condition is the same test
            // that already scored this move 0 above, so it could never apply.
            let decay = if cell.food || (health < 30 && toward_food) {
                0.85
            } else if health < 60 && toward_food {
                0.7
            } else {
                0.55
            };
            self.path[depth] = to;
            let total: f64 = Direction::ALL
                .into_iter()
                .map(|next| self.score_move(to, next, weight * decay, depth + 1, new_len))
                .sum();
            total / 4.0 + weight * 0.55
        };
        let space = self.open_space(to).min(SPACE_CAP) as f64 / SPACE_CAP as f64;
        look_ahead + space * 0.2
    }

    fn index(&self, p: Point) -> Option<usize> {
        self.state
            .is_on_board(p)
            .then(|| (p.y * self.state.board.width + p.x) as usize)
    }

    /// Whether `to` lands on our body as it is after the move: the newest `new_len - 1` cells of
    /// this branch's path followed by our original body.
    fn runs_into_own_body(&self, to: Point, depth: usize, new_len: usize) -> bool {
        let behind_head = new_len - 1;
        let recent = depth.min(behind_head);
        if self.path[depth - recent..depth].contains(&to) {
            return true;
        }
        let i = (to.y * self.state.board.width + to.x) as usize;
        self.cells[i].own_index < behind_head - recent
    }

    /// The original's rule: take the first strictly closest food from `from` and ask whether `to`
    /// is nearer to it.
    fn is_moving_toward_food(&self, from: Point, to: Point) -> bool {
        let distance = |a: Point, b: Point| (a.x - b.x).abs() + (a.y - b.y).abs();
        let mut food = self.state.board.food.iter();
        let Some(&first) = food.next() else {
            return false;
        };
        let closest = food.fold(first, |closest, &f| {
            if distance(from, f) < distance(from, closest) {
                f
            } else {
                closest
            }
        });
        distance(to, closest) < distance(from, closest)
    }

    /// Cells reachable from `start` without crossing a body or hazard, counted up to `SPACE_CAP`.
    fn open_space(&mut self, start: Point) -> usize {
        self.generation += 1;
        let generation = self.generation;
        let Some(i) = self.index(start) else {
            return 0;
        };
        self.cells[i].seen = generation;
        self.queue[0] = start;
        let (mut next, mut end, mut count) = (0, 1, 0);
        while next < end && count < SPACE_CAP {
            let current = self.queue[next];
            next += 1;
            count += 1;
            for dir in Direction::ALL {
                let neighbour = dir.step(current);
                if let Some(j) = self.index(neighbour) {
                    let cell = &mut self.cells[j];
                    if !cell.blocked && cell.seen != generation {
                        cell.seen = generation;
                        self.queue[end] = neighbour;
                        end += 1;
                    }
                }
            }
        }
        count
    }
}

#[cfg(test)]
mod tests {
    use std::time::Instant;

    use super::*;

    fn parse(json: &str) -> GameState<'_> {
        serde_json::from_str(json).unwrap()
    }

    fn score_of(state: &GameState, dir: Direction) -> f64 {
        let i = Direction::ALL.iter().position(|&d| d == dir).unwrap();
        score_moves(state)[i]
    }

    // movement.service.spec.ts: "should avoid the wall"
    #[test]
    fn avoids_the_wall() {
        let state = parse(
            r#"{"turn": 10,
            "board": {"height": 11, "width": 11, "snakes": [], "food": [{"x":5,"y":5}], "hazards": []},
            "you": {"id": "gs_yW6S78yVFjGjF9qKd4yFqCkP", "latency": "115", "health": 92,
                "body": [{"x":10,"y":10},{"x":9,"y":10},{"x":9,"y":9},{"x":10,"y":9}],
                "head": {"x":10,"y":10}, "length": 4}}"#,
        );
        let mv = choose_move(&state);
        assert_eq!(mv, Direction::Down);
        assert!(score_of(&state, mv) > 0.0);
    }

    // movement.service.spec.ts: "should avoid the wall after directly heading for it"
    #[test]
    fn avoids_the_wall_after_heading_for_it() {
        let body = r#"[{"x":6,"y":8},{"x":5,"y":8},{"x":4,"y":8},{"x":3,"y":8},{"x":2,"y":8},
            {"x":2,"y":7},{"x":2,"y":6},{"x":2,"y":5},{"x":2,"y":4}]"#;
        let json = format!(
            r#"{{"game": {{"id": "4b7c6bd9-caff-4e15-92d9-e50ae8139d48", "timeout": 500}}, "turn": 108,
            "board": {{"height": 11, "width": 11,
                "snakes": [{{"id": "gs_FDyy3dc6SpXrB8DRPyjJqHGT", "latency": "104", "health": 88,
                    "body": {body}, "head": {{"x":6,"y":8}}, "length": 9}}],
                "food": [{{"x":0,"y":10}},{{"x":9,"y":10}},{{"x":0,"y":7}},{{"x":8,"y":10}},{{"x":0,"y":0}}],
                "hazards": []}},
            "you": {{"id": "gs_FDyy3dc6SpXrB8DRPyjJqHGT", "latency": "104", "health": 88,
                "body": {body}, "head": {{"x":6,"y":8}}, "length": 9}}}}"#
        );
        let state = parse(&json);
        let mv = choose_move(&state);
        assert!(
            [Direction::Right, Direction::Down].contains(&mv),
            "{mv:?}"
        );
        assert!(score_of(&state, mv) > 0.0);
    }

    // movement.service.spec.ts: "should avoid yourself"
    #[test]
    fn avoids_itself() {
        let state = parse(
            r#"{"turn": 1,
            "board": {"height": 11, "width": 11, "snakes": [], "food": [{"x":2,"y":8},{"x":5,"y":5}], "hazards": []},
            "you": {"id": "gs_34HpMxdhkp9g6P8YJr67HJPY", "latency": "261", "health": 99,
                "body": [{"x":0,"y":9},{"x":1,"y":9},{"x":1,"y":9}], "head": {"x":0,"y":9}, "length": 3}}"#,
        );
        let mv = choose_move(&state);
        assert_eq!(mv, Direction::Down);
        assert!(score_of(&state, mv) > 0.0);
    }

    const CONFLICT_SNAKES: &str = r#"[
        {"id": "gs_3w6HTQYFxv79KwkpFF48K93X", "latency": "11", "health": 76,
            "body": [{"x":6,"y":8},{"x":6,"y":7},{"x":6,"y":6},{"x":6,"y":5},{"x":7,"y":5},{"x":8,"y":5},{"x":9,"y":5}],
            "head": {"x":6,"y":8}, "length": 7},
        {"id": "gs_YtvcJvcJyPR8PTGG3dPgjddW", "latency": "7", "health": 95,
            "body": [{"x":4,"y":8},{"x":4,"y":7},{"x":4,"y":6},{"x":4,"y":5},{"x":4,"y":4}],
            "head": {"x":4,"y":8}, "length": 5}]"#;
    const CONFLICT_FOOD: &str =
        r#"[{"x":2,"y":0},{"x":3,"y":0},{"x":4,"y":10},{"x":1,"y":0},{"x":8,"y":2}]"#;

    fn conflict_state(you: &str) -> String {
        format!(
            r#"{{"game": {{"id": "64a4df54-6631-46e6-8010-88a65f5c467f", "timeout": 500}}, "turn": 60,
            "board": {{"height": 11, "width": 11, "snakes": {CONFLICT_SNAKES}, "food": {CONFLICT_FOOD}, "hazards": []}},
            "you": {you}}}"#
        )
    }

    // movement.service.spec.ts: "should avoid conflict"
    #[test]
    fn avoids_conflict() {
        let json = conflict_state(
            r#"{"id": "gs_YtvcJvcJyPR8PTGG3dPgjddW", "latency": "7", "health": 95,
            "body": [{"x":4,"y":8},{"x":4,"y":7},{"x":4,"y":6},{"x":4,"y":5},{"x":4,"y":4}],
            "head": {"x":4,"y":8}, "length": 5}"#,
        );
        let state = parse(&json);
        let mv = choose_move(&state);
        assert_eq!(mv, Direction::Left);
        assert!(score_of(&state, mv) > 0.0);
    }

    // movement.service.spec.ts: "should avoid conflict unless longer"
    #[test]
    fn takes_conflict_when_longer() {
        let json = conflict_state(
            r#"{"id": "gs_3w6HTQYFxv79KwkpFF48K93X", "latency": "11", "health": 76,
            "body": [{"x":6,"y":8},{"x":6,"y":7},{"x":6,"y":6},{"x":6,"y":5},{"x":7,"y":5},{"x":8,"y":5},{"x":9,"y":5}],
            "head": {"x":6,"y":8}, "length": 7}"#,
        );
        let state = parse(&json);
        let mv = choose_move(&state);
        assert_eq!(mv, Direction::Right);
        assert!(score_of(&state, mv) > 0.0);
    }

    const GROWTH_STATE: &str = r#"{"turn": 4,
        "board": {"height": 11, "width": 11, "snakes": [], "food": [{"x":9,"y":6}], "hazards": []},
        "you": {"id": "gs_4fyGxT7TkcJ9Fcmh6k7bgxGH", "latency": "107", "health": 96,
            "body": [{"x":9,"y":5},{"x":10,"y":5},{"x":10,"y":6}], "head": {"x":9,"y":5}, "length": 3}}"#;

    // boundary.service.spec.ts: moveAsBody "should be increase length when on food". The search
    // keeps the body implicitly: moving up onto the food at (9,6) makes the body one longer, so
    // behind the new head lies the whole old body, old tail (10,6) included.
    #[test]
    fn grows_when_on_food() {
        let state = parse(GROWTH_STATE);
        let search = Search::new(&state);
        let up = Direction::Up.step(state.you.head);
        assert_eq!(up, Point { x: 9, y: 6 });
        let cell = search.cells[search.index(up).unwrap()];
        assert!(cell.food && state.you.body.len() == search.length);
        assert!(search.runs_into_own_body(Point { x: 10, y: 6 }, 0, 4));
    }

    // boundary.service.spec.ts: moveAsBody "should be move length when not on food". Moving down
    // to (9,4) keeps the length at 3, so the old tail (10,6) is no longer body but (10,5) is.
    #[test]
    fn keeps_length_when_not_on_food() {
        let state = parse(GROWTH_STATE);
        let search = Search::new(&state);
        let down = Direction::Down.step(state.you.head);
        assert_eq!(down, Point { x: 9, y: 4 });
        assert!(!search.cells[search.index(down).unwrap()].food);
        assert!(!search.runs_into_own_body(Point { x: 10, y: 6 }, 0, 3));
        assert!(search.runs_into_own_body(Point { x: 10, y: 5 }, 0, 3));
    }

    #[test]
    fn trapped_snake_prefers_a_move_that_is_not_certain_death() {
        // Every move scores 0. Up is our own neck (we just ate, so the tail stays), left is the
        // wall, right is the long snake's body; down is only next to its head, so it may survive.
        let state = parse(
            r#"{"board": {"height": 3, "width": 3, "food": [], "hazards": [], "snakes": [
                {"id": "me", "body": [{"x":0,"y":1},{"x":0,"y":2},{"x":0,"y":2}], "head": {"x":0,"y":1}},
                {"id": "big", "body": [{"x":1,"y":0},{"x":2,"y":0},{"x":2,"y":1},{"x":1,"y":1},{"x":1,"y":2},{"x":2,"y":2},{"x":2,"y":2}],
                    "head": {"x":1,"y":0}}]},
            "you": {"id": "me", "body": [{"x":0,"y":1},{"x":0,"y":2},{"x":0,"y":2}], "head": {"x":0,"y":1}, "length": 3}}"#,
        );
        assert_eq!(score_moves(&state), [0.0; 4]);
        assert_eq!(choose_move(&state), Direction::Down);
    }

    #[test]
    fn worst_case_open_board_stays_well_under_a_millisecond() {
        // A very hungry snake heading for food on an empty 19x19 board keeps the 0.85 decay,
        // so every branch runs to the depth cap.
        let state = parse(
            r#"{"board": {"height": 19, "width": 19, "food": [{"x":0,"y":0}], "hazards": [], "snakes": []},
            "you": {"id": "me", "health": 10, "body": [{"x":18,"y":18},{"x":18,"y":17},{"x":18,"y":16}],
                "head": {"x":18,"y":18}, "length": 3}}"#,
        );
        choose_move(&state);
        let runs = 50;
        let started = Instant::now();
        for _ in 0..runs {
            std::hint::black_box(choose_move(std::hint::black_box(&state)));
        }
        let per_move = started.elapsed() / runs;
        println!("worst-case autopilot move: {per_move:?}");
        // Debug builds are ~20x slower than release; the bound is for release.
        let bound = if cfg!(debug_assertions) { 20_000 } else { 1_000 };
        assert!(per_move.as_micros() < bound, "{per_move:?}");
    }
}
