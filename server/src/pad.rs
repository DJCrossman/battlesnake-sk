//! The `/ws` socket the pad page talks to.

use std::sync::Arc;
use std::sync::atomic::Ordering;
use std::time::Duration;

use axum::extract::State;
use axum::extract::ws::{CloseFrame, Message, WebSocket, WebSocketUpgrade, close_code};
use axum::response::Response;
use subtle::ConstantTimeEq;
use tokio::sync::broadcast::error::RecvError;
use tokio::time::Instant;
use tracing::info;

use crate::App;
use crate::control::Mode;
use crate::game::Direction;

pub(crate) async fn connect(State(app): State<Arc<App>>, ws: WebSocketUpgrade) -> Response {
    // The pad's frames are a token or a few bytes; nothing larger is read, even before the token.
    ws.max_message_size(1024)
        .max_frame_size(1024)
        .on_upgrade(move |socket| serve_pad(app, socket))
}

/// Protocol: the first frame is the control token as text. After that the pad sends binary
/// frames — `[0..=3]` a direction (up, down, left, right), `[4]` the mode button, `[5, ..]` an
/// echo request, `[6]` a request for quiet — and receives the JSON frames broadcast for the
/// attached snake, unless it asked for quiet: a pad with no screen has no use for them. A pad
/// that sends nothing for `pad_idle` is closed.
async fn serve_pad(app: Arc<App>, mut socket: WebSocket) {
    let first = tokio::time::timeout(Duration::from_secs(5), socket.recv()).await;
    let authenticated = matches!(
        &first,
        Ok(Some(Ok(Message::Text(token)))) if bool::from(token.as_bytes().ct_eq(app.token.as_bytes()))
    );
    if !authenticated {
        let _ = socket
            .send(Message::Close(Some(CloseFrame {
                code: close_code::POLICY,
                reason: "unauthorised".into(),
            })))
            .await;
        return;
    }

    let mut events = app.events.subscribe();
    let pads = app.pads.fetch_add(1, Ordering::AcqRel) + 1;
    info!(pads, "pad connected");
    let mut last_heard = Instant::now();
    let mut quiet = false;
    let mut outgoing = Some(Message::Text(
        format!(r#"{{"t":"hello","mode":"{}"}}"#, app.mode().as_str()).into(),
    ));
    // Each turn of the loop sends at most one frame: the hello, an echo or a broadcast event.
    while let Some(frame) = outgoing.take() {
        if socket.send(frame).await.is_err() {
            break;
        }
        while outgoing.is_none() {
            tokio::select! {
                incoming = socket.recv() => {
                    last_heard = Instant::now();
                    match incoming {
                        Some(Ok(Message::Binary(bytes))) => match bytes.first() {
                            Some(&b @ 0..=3) => queue_press(&app, Direction::ALL[usize::from(b)]),
                            Some(4) => press_mode_button(&app),
                            Some(5) => outgoing = Some(Message::Binary(bytes)),
                            Some(6) => quiet = true,
                            _ => {}
                        },
                        Some(Ok(Message::Close(_))) | Some(Err(_)) | None => break,
                        Some(Ok(_)) => {}
                    }
                },
                event = events.recv() => match event {
                    Ok(frame) if !quiet => outgoing = Some(Message::Text(frame)),
                    Ok(_) | Err(RecvError::Lagged(_)) => {}
                    Err(RecvError::Closed) => break,
                },
                _ = tokio::time::sleep_until(last_heard + app.pad_idle) => {
                    info!(idle_ms = app.pad_idle.as_millis(), "pad went silent");
                    break;
                }
            }
        }
    }

    let pads = app.pads.fetch_sub(1, Ordering::AcqRel) - 1;
    info!(pads, "pad disconnected");
    app.wake.notify_waiters();
}

/// A direction press counts only while a snake is attached and the player is steering; a press
/// made in auto or between games would otherwise be replayed on a later turn.
fn queue_press(app: &App, dir: Direction) {
    let mut control = app.control.lock().unwrap();
    if control.has_attachment() && app.mode() != Mode::Auto && control.queue.push(dir) {
        drop(control);
        app.wake.notify_waiters();
    }
}

/// Assist → manual → auto → assist, announced to every pad. Presses made under the old mode
/// are dropped.
fn press_mode_button(app: &App) {
    let mode = {
        // Held across the switch so a press is queued either before it (and cleared) or after.
        let mut control = app.control.lock().unwrap();
        let previous = app.mode.update(Ordering::AcqRel, Ordering::Acquire, |m| {
            Mode::from(m).next().into()
        });
        control.queue.clear();
        Mode::from(previous).next()
    };
    info!(mode = mode.as_str(), "mode changed");
    let _ = app
        .events
        .send(format!(r#"{{"t":"mode","mode":"{}"}}"#, mode.as_str()).into());
    app.wake.notify_waiters();
}
