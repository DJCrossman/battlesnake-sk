//! The `/ws` socket the pad page talks to.

use std::sync::Arc;
use std::sync::atomic::Ordering;
use std::time::Duration;

use axum::extract::State;
use axum::extract::ws::{CloseFrame, Message, WebSocket, WebSocketUpgrade, close_code};
use axum::response::Response;
use subtle::ConstantTimeEq;
use tokio::sync::broadcast::error::RecvError;
use tracing::info;

use crate::App;
use crate::control::Mode;
use crate::game::Direction;

pub(crate) async fn connect(State(app): State<Arc<App>>, ws: WebSocketUpgrade) -> Response {
    ws.on_upgrade(move |socket| serve_pad(app, socket))
}

/// Protocol: the first frame is the control token as text. After that the pad sends binary
/// frames — `[0..=3]` a direction (up, down, left, right), `[4]` the mode button, `[5, ..]` an
/// echo request — and receives the JSON frames broadcast for the attached game.
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
    let hello = format!(r#"{{"t":"hello","mode":"{}"}}"#, app.mode().as_str());
    let mut outgoing = Some(Message::Text(hello.into()));
    // Each turn of the loop sends at most one frame: the hello, an echo or a broadcast event.
    while let Some(frame) = outgoing.take() {
        if socket.send(frame).await.is_err() {
            break;
        }
        while outgoing.is_none() {
            tokio::select! {
                incoming = socket.recv() => match incoming {
                    Some(Ok(Message::Binary(bytes))) => match bytes.first() {
                        Some(&b @ 0..=3) => {
                            let queued = app.control.lock().unwrap().queue.push(Direction::ALL[usize::from(b)]);
                            if queued {
                                app.wake.notify_waiters();
                            }
                        }
                        Some(4) => press_mode_button(&app),
                        Some(5) => outgoing = Some(Message::Binary(bytes)),
                        _ => {}
                    },
                    Some(Ok(Message::Close(_))) | Some(Err(_)) | None => break,
                    Some(Ok(_)) => {}
                },
                event = events.recv() => match event {
                    Ok(frame) => outgoing = Some(Message::Text(frame)),
                    Err(RecvError::Lagged(_)) => {}
                    Err(RecvError::Closed) => break,
                },
            }
        }
    }

    let pads = app.pads.fetch_sub(1, Ordering::AcqRel) - 1;
    info!(pads, "pad disconnected");
    app.wake.notify_waiters();
}

/// Assist → manual → auto → assist, announced to every pad.
fn press_mode_button(app: &App) {
    let previous = app.mode.update(Ordering::AcqRel, Ordering::Acquire, |m| {
        Mode::from(m).next().into()
    });
    let mode = Mode::from(previous).next();
    info!(mode = mode.as_str(), "mode changed");
    app.broadcast(format!(r#"{{"t":"mode","mode":"{}"}}"#, mode.as_str()));
    app.wake.notify_waiters();
}
