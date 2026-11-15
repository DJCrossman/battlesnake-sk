//! A Battlesnake a human steers in real time from the `/pad` page, with an autopilot behind them.

pub mod autopilot;
pub mod control;
pub mod game;
mod pad;

use std::sync::atomic::{AtomicU8, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use axum::Router;
use axum::body::Bytes;
use axum::extract::State;
use axum::extract::ws::Utf8Bytes;
use axum::http::StatusCode;
use axum::http::header::CONTENT_TYPE;
use axum::response::{Html, IntoResponse, Response};
use axum::routing::{get, post};
use tokio::sync::{Notify, broadcast};
use tracing::info;

use control::{Control, Mode, Source};
use game::{Direction, GameState};

pub struct Config {
    /// The pad's first WebSocket message must equal this.
    pub token: String,
    pub colour: String,
}

struct App {
    token: String,
    personalisation: String,
    control: Mutex<Control>,
    mode: AtomicU8,
    /// Authenticated pad sockets.
    pads: AtomicUsize,
    /// Wakes a `/move` waiting for the player: a press, a mode change or a pad leaving.
    wake: Notify,
    /// JSON frames for every pad. Slow sockets lag and drop frames rather than hold anyone up.
    events: broadcast::Sender<Utf8Bytes>,
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
    {
        let mut control = app.control.lock().unwrap();
        control.attached = Some(state.game.id.to_string());
        control.queue.clear();
        control.last_spent_ms = None;
    }
    info!(game = %state.game.id, timeout_ms = state.game.timeout, "game started");
    if app.is_watched() {
        app.broadcast(format!(
            r#"{{"t":"start","state":{}}}"#,
            String::from_utf8_lossy(&body)
        ));
    }
    StatusCode::OK
}

async fn play_move(State(app): State<Arc<App>>, body: Bytes) -> Response {
    let received = Instant::now();
    let Ok(state) = serde_json::from_slice::<GameState>(&body) else {
        return StatusCode::BAD_REQUEST.into_response();
    };
    let hold = {
        let mut control = app.control.lock().unwrap();
        if control.attached.is_none() {
            info!(game = %state.game.id, "game attached");
        }
        let attached = control
            .attached
            .get_or_insert_with(|| state.game.id.to_string());
        if *attached == state.game.id {
            let latency = state.you.latency.trim().parse::<f64>().unwrap_or(0.0);
            if let (true, Some(spent)) = (latency > 0.0, control.last_spent_ms) {
                let timeout = f64::from(state.game.timeout);
                control.overhead.observe(latency, spent, timeout);
            }
            Some(control.overhead.hold(state.game.timeout))
        } else {
            None
        }
    };
    let dir = match hold {
        Some(hold) => steer_attached_game(&app, &state, &body, received, hold).await,
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
    let was_attached = {
        let mut control = app.control.lock().unwrap();
        let was_attached = control.attached.as_deref() == Some(&state.game.id);
        if was_attached {
            control.attached = None;
        }
        was_attached
    };
    info!(game = %state.game.id, attached = was_attached, turn = state.turn, "game ended");
    if was_attached && app.is_watched() {
        app.broadcast(format!(
            r#"{{"t":"end","state":{}}}"#,
            String::from_utf8_lossy(&body)
        ));
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

    fn broadcast(&self, frame: String) {
        // An error only means every receiver has gone since `is_watched`.
        let _ = self.events.send(frame.into());
    }
}

/// The attached game's move: the player's if one arrives in time, otherwise the fallback rules.
async fn steer_attached_game(
    app: &App,
    state: &GameState<'_>,
    raw: &[u8],
    received: Instant,
    hold: Duration,
) -> Direction {
    let mode = app.mode();
    let watched = app.is_watched();
    if watched {
        let hold_ms = if mode == Mode::Auto {
            0
        } else {
            hold.as_millis()
        };
        app.broadcast(format!(
            r#"{{"t":"turn","mode":"{}","hold_ms":{hold_ms},"state":{}}}"#,
            mode.as_str(),
            String::from_utf8_lossy(raw)
        ));
    }
    let (dir, source) = if !watched || mode == Mode::Auto {
        (autopilot::choose_move(state), Source::Auto)
    } else {
        wait_for_player(app, state, received + hold).await
    };
    if app.is_watched() {
        app.broadcast(format!(
            r#"{{"t":"moved","turn":{},"move":"{}","source":"{}"}}"#,
            state.turn,
            dir.as_str(),
            source.as_str()
        ));
    }
    app.control.lock().unwrap().last_spent_ms = Some(received.elapsed().as_secs_f64() * 1000.0);
    dir
}

/// Answers the moment a usable press is queued (one already waiting included), and otherwise at
/// `deadline`. A switch to auto or the last pad leaving hands the move to the autopilot at once.
async fn wait_for_player(
    app: &App,
    state: &GameState<'_>,
    deadline: Instant,
) -> (Direction, Source) {
    loop {
        let woken = app.wake.notified();
        tokio::pin!(woken);
        woken.as_mut().enable();

        let mode = app.mode();
        if mode == Mode::Auto || !app.is_watched() {
            return (autopilot::choose_move(state), Source::Auto);
        }
        let input = app.control.lock().unwrap().queue.take_usable(state, mode);
        if let Some(dir) = input {
            return (dir, Source::Pad);
        }
        if tokio::time::timeout_at(deadline.into(), woken)
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
