//! A Battlesnake a human steers in real time from the `/pad` page, with an autopilot behind them.

pub mod autopilot;
pub mod control;
mod flipper;
pub mod game;
mod pad;

use std::sync::atomic::{AtomicU8, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use axum::Router;
use axum::body::Bytes;
use axum::extract::ws::Utf8Bytes;
use axum::extract::{DefaultBodyLimit, State};
use axum::http::StatusCode;
use axum::http::header::CONTENT_TYPE;
use axum::response::{Html, IntoResponse, Response};
use axum::routing::{get, post};
use tokio::sync::{Notify, broadcast};
use tracing::info;

use control::{Control, Mode, Source};
use flipper::{Moment, board_frame};
use game::{Direction, GameState};

pub struct Config {
    /// The pad's first WebSocket message must equal this.
    pub token: String,
    pub colour: String,
    /// A pad socket that receives nothing for this long is closed. The page sends an echo
    /// request every 2 s, so silence means the phone went away without closing the connection.
    pub pad_idle: Duration,
}

/// One broadcast event in each form a pad can ask for.
#[derive(Clone)]
struct PadEvent {
    /// For the pad page.
    json: Utf8Bytes,
    /// For the Flipper, see `flipper`; `None` when it has nothing to draw for this event.
    flipper: Option<Bytes>,
}

struct App {
    token: String,
    personalisation: String,
    pad_idle: Duration,
    control: Mutex<Control>,
    mode: AtomicU8,
    /// Authenticated pad sockets.
    pads: AtomicUsize,
    /// Wakes a `/move` waiting for the player: a press, a mode change, a pad leaving or a newer
    /// `/move` for the same snake.
    wake: Notify,
    /// Frames for every pad. Slow sockets lag and drop frames rather than hold anyone up.
    events: broadcast::Sender<PadEvent>,
}

pub fn router(config: Config) -> Router {
    let app = Arc::new(App {
        token: config.token,
        personalisation: serde_json::json!({
            "apiversion": "1",
            "author": "David Crossman",
            "color": config.colour,
            "head": "bendr",
            "tail": "freckled",
        })
        .to_string(),
        pad_idle: config.pad_idle,
        control: Mutex::new(Control::default()),
        mode: AtomicU8::new(Mode::Assist.into()),
        pads: AtomicUsize::new(0),
        wake: Notify::new(),
        events: broadcast::channel(16).0,
    });
    Router::new()
        .route("/", get(describe_snake))
        .route("/start", post(start_game))
        .route("/move", post(play_move))
        .route("/end", post(end_game))
        .route(
            "/pad",
            get(|| async { Html(include_str!("../static/pad.html")) }),
        )
        .route("/ws", get(pad::connect))
        // Only the engine's unauthenticated POSTs read a body, and the autopilot's cost grows with
        // its length. The largest 19x19 game the engine can send (8 snakes filling the board, food
        // on every cell, hazards stacked 8 deep on every cell) is about 92 KB; this is 5.7x that.
        .layer(DefaultBodyLimit::max(512 * 1024))
        .with_state(app)
}

async fn describe_snake(State(app): State<Arc<App>>) -> Response {
    (
        [(CONTENT_TYPE, "application/json")],
        app.personalisation.clone(),
    )
        .into_response()
}

async fn start_game(State(app): State<Arc<App>>, body: Bytes) -> StatusCode {
    let Ok(state) = serde_json::from_slice::<GameState>(&body) else {
        return StatusCode::BAD_REQUEST;
    };
    // A game in progress keeps the pad; this one is then the autopilot's alone.
    let attached = {
        let mut control = app.control.lock().unwrap();
        if control.has_game_in_progress(Instant::now()) {
            false
        } else {
            control.attach(&state);
            true
        }
    };
    info!(game = %state.game.id, snake = %state.you.id, timeout_ms = state.game.timeout, attached, "game started");
    if attached && app.is_watched() {
        let _ = app.events.send(PadEvent {
            json: format!(
                r#"{{"t":"start","state":{}}}"#,
                String::from_utf8_lossy(&body)
            )
            .into(),
            flipper: Some(board_frame(&state, Moment::Start)),
        });
    }
    StatusCode::OK
}

async fn play_move(State(app): State<Arc<App>>, body: Bytes) -> Response {
    let received = Instant::now();
    let Ok(state) = serde_json::from_slice::<GameState>(&body) else {
        return StatusCode::BAD_REQUEST.into_response();
    };
    let attached = {
        let mut control = app.control.lock().unwrap();
        if !control.has_game_in_progress(received) {
            control.attach(&state);
            info!(game = %state.game.id, snake = %state.you.id, "game attached");
        }
        if control.is_attached(&state) {
            control.heard_from_attached(received);
            let latency = state.you.latency.trim().parse::<f64>().unwrap_or(0.0);
            if let (true, Some(spent)) = (latency > 0.0, control.last_spent_ms) {
                control
                    .overhead
                    .observe(latency, spent, f64::from(state.game.timeout));
            }
            control.latest_move += 1;
            Some((
                control.overhead.hold(state.game.timeout),
                control.latest_move,
            ))
        } else {
            None
        }
    };
    let dir = match attached {
        Some((hold, this_move)) => {
            // A request for an earlier turn may still be waiting; it must stop now.
            app.wake.notify_waiters();
            steer_attached_snake(&app, &state, &body, received, hold, this_move).await
        }
        None => autopilot::choose_move(&state),
    };
    (
        [(CONTENT_TYPE, "application/json")],
        format!(r#"{{"move":"{}"}}"#, dir.as_str()),
    )
        .into_response()
}

async fn end_game(State(app): State<Arc<App>>, body: Bytes) -> StatusCode {
    let Ok(state) = serde_json::from_slice::<GameState>(&body) else {
        return StatusCode::BAD_REQUEST;
    };
    let was_attached = app.control.lock().unwrap().detach(&state);
    info!(game = %state.game.id, snake = %state.you.id, attached = was_attached, turn = state.turn, "game ended");
    if was_attached && app.is_watched() {
        let _ = app.events.send(PadEvent {
            json: format!(
                r#"{{"t":"end","state":{}}}"#,
                String::from_utf8_lossy(&body)
            )
            .into(),
            flipper: Some(board_frame(&state, Moment::End)),
        });
    }
    StatusCode::OK
}

impl App {
    fn mode(&self) -> Mode {
        Mode::from(self.mode.load(Ordering::Acquire))
    }

    /// Whether any pad is connected, i.e. whether building frames is worth it.
    fn is_watched(&self) -> bool {
        self.pads.load(Ordering::Acquire) > 0
    }
}

/// The attached snake's move: the player's if one arrives in time, otherwise the fallback rules.
async fn steer_attached_snake(
    app: &App,
    state: &GameState<'_>,
    raw: &[u8],
    received: Instant,
    hold: Duration,
    this_move: u64,
) -> Direction {
    let mode = app.mode();
    let watched = app.is_watched();
    if watched {
        let hold_ms = if mode == Mode::Auto {
            0
        } else {
            hold.as_millis()
        };
        let _ = app.events.send(PadEvent {
            json: format!(
                r#"{{"t":"turn","mode":"{}","hold_ms":{hold_ms},"state":{}}}"#,
                mode.as_str(),
                String::from_utf8_lossy(raw)
            )
            .into(),
            flipper: Some(board_frame(state, Moment::Turn { hold_ms })),
        });
    }
    let (dir, source) = if !watched || mode == Mode::Auto {
        (autopilot::choose_move(state), Source::Auto)
    } else {
        wait_for_player(app, state, received + hold, this_move).await
    };
    let mut control = app.control.lock().unwrap();
    if app.is_watched() {
        let _ = app.events.send(PadEvent {
            json: format!(
                r#"{{"t":"moved","turn":{},"move":"{}","source":"{}"}}"#,
                state.turn,
                dir.as_str(),
                source.as_str()
            )
            .into(),
            // Once a newer `/move` is counted the engine has given up on this one, and this board
            // would replace the newer turn's on the Flipper; once `/end` has detached the snake it
            // would replace the game-over board. Checked and sent under the lock, so neither can
            // go out in between.
            flipper: (control.latest_move == this_move && control.is_attached(state))
                .then(|| board_frame(state, Moment::Moved(dir))),
        });
    }
    if control.is_attached(state) {
        control.last_spent_ms = Some(received.elapsed().as_secs_f64() * 1000.0);
    }
    dir
}

/// Answers the moment a usable press is queued (one already waiting included), and otherwise at
/// `deadline`. A switch to auto or the last pad leaving hands the move to the autopilot at once;
/// a newer `/move` for the same snake ends the wait at once with the deadline's answer, leaving
/// the presses for the newer turn.
async fn wait_for_player(
    app: &App,
    state: &GameState<'_>,
    deadline: Instant,
    this_move: u64,
) -> (Direction, Source) {
    loop {
        let woken = app.wake.notified();
        tokio::pin!(woken);
        woken.as_mut().enable();

        let mode = app.mode();
        if mode == Mode::Auto || !app.is_watched() {
            return (autopilot::choose_move(state), Source::Auto);
        }
        let (superseded, input) = {
            let mut control = app.control.lock().unwrap();
            if control.latest_move != this_move {
                (true, None)
            } else {
                (false, control.queue.take_usable(state, mode))
            }
        };
        if let Some(dir) = input {
            return (dir, Source::Pad);
        }
        if superseded
            || tokio::time::timeout_at(deadline.into(), woken)
                .await
                .is_err()
        {
            return match control::move_at_deadline(state, app.mode()) {
                Some(dir) => (dir, Source::Straight),
                None => (autopilot::choose_move(state), Source::Auto),
            };
        }
    }
}
