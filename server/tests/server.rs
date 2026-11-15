//! Starts the real router on an ephemeral port and drives it like the engine and the pad do.

use std::time::{Duration, Instant};

use battlesnake_sk::{Config, router};
use futures_util::{SinkExt, StreamExt};
use tokio::net::{TcpListener, TcpStream};
use tokio_tungstenite::tungstenite::Message;
use tokio_tungstenite::{MaybeTlsStream, WebSocketStream, connect_async};

type Pad = WebSocketStream<MaybeTlsStream<TcpStream>>;

const TOKEN: &str = "test-token";
/// With the default 100 ms overhead and a 500 ms timeout the server holds for 350 ms.
const HOLD: Duration = Duration::from_millis(350);

#[tokio::test]
async fn wrong_token_is_rejected() {
    let addr = spawn_server().await;
    let (mut pad, _) = connect_async(format!("ws://{addr}/ws")).await.unwrap();
    pad.send(Message::text("not-the-token")).await.unwrap();
    match tokio::time::timeout(Duration::from_secs(2), pad.next())
        .await
        .unwrap()
    {
        Some(Ok(Message::Close(_))) | None | Some(Err(_)) => {}
        other => panic!("expected the socket to close, got {other:?}"),
    }
}

#[tokio::test]
async fn queued_input_answers_well_before_the_hold() {
    let addr = spawn_server().await;
    let mut pad = connect_pad(&addr).await;
    post(&addr, "/start", &game_state("g1")).await;
    pad.send(Message::binary(vec![2u8])).await.unwrap();
    // The echo comes back after the server has handled the press sent before it.
    echo(&mut pad, vec![5, 1]).await;

    let (answer, took) = post(&addr, "/move", &game_state("g1")).await;
    assert_eq!(answer, r#"{"move":"left"}"#);
    assert!(took < Duration::from_millis(100), "{took:?}");
}

#[tokio::test]
async fn input_during_the_hold_answers_at_once() {
    let addr = spawn_server().await;
    let mut pad = connect_pad(&addr).await;
    post(&addr, "/start", &game_state("g1")).await;
    let request = tokio::spawn({
        let addr = addr.clone();
        async move { post(&addr, "/move", &game_state("g1")).await }
    });
    // Wait for the turn frame, then press right.
    next_text_containing(&mut pad, r#""t":"turn""#).await;
    pad.send(Message::binary(vec![3u8])).await.unwrap();
    let (answer, took) = request.await.unwrap();
    assert_eq!(answer, r#"{"move":"right"}"#);
    assert!(took < HOLD - Duration::from_millis(100), "{took:?}");
    let moved = next_text_containing(&mut pad, r#""t":"moved""#).await;
    assert!(moved.contains(r#""source":"pad""#), "{moved}");
}

#[tokio::test]
async fn no_input_goes_straight_after_the_hold() {
    let addr = spawn_server().await;
    let mut pad = connect_pad(&addr).await;
    post(&addr, "/start", &game_state("g1")).await;

    let (answer, took) = post(&addr, "/move", &game_state("g1")).await;
    assert_eq!(answer, r#"{"move":"up"}"#);
    assert!(took >= HOLD - Duration::from_millis(10), "{took:?}");
    assert!(took < HOLD + Duration::from_millis(100), "{took:?}");
    let moved = next_text_containing(&mut pad, r#""t":"moved""#).await;
    assert!(moved.contains(r#""source":"straight""#), "{moved}");
}

#[tokio::test]
async fn without_a_pad_the_autopilot_answers_immediately() {
    let addr = spawn_server().await;
    post(&addr, "/start", &game_state("g1")).await;
    let (answer, took) = post(&addr, "/move", &game_state("g1")).await;
    assert!(answer.starts_with(r#"{"move":""#), "{answer}");
    assert!(took < Duration::from_millis(50), "{took:?}");
}

#[tokio::test]
async fn auto_mode_answers_immediately() {
    let addr = spawn_server().await;
    let mut pad = connect_pad(&addr).await;
    post(&addr, "/start", &game_state("g1")).await;
    pad.send(Message::binary(vec![4u8])).await.unwrap();
    next_text_containing(&mut pad, r#""mode":"manual""#).await;
    pad.send(Message::binary(vec![4u8])).await.unwrap();
    next_text_containing(&mut pad, r#""mode":"auto""#).await;

    let (_, took) = post(&addr, "/move", &game_state("g1")).await;
    assert!(took < Duration::from_millis(50), "{took:?}");
    let moved = next_text_containing(&mut pad, r#""t":"moved""#).await;
    assert!(moved.contains(r#""source":"auto""#), "{moved}");
}

#[tokio::test]
async fn another_game_is_played_by_the_autopilot_without_waiting() {
    let addr = spawn_server().await;
    let _pad = connect_pad(&addr).await;
    post(&addr, "/start", &game_state("mine")).await;
    let (_, took) = post(&addr, "/move", &game_state("someone-elses")).await;
    assert!(took < Duration::from_millis(50), "{took:?}");
}

#[tokio::test]
async fn echo_frames_come_back_unchanged() {
    let addr = spawn_server().await;
    let mut pad = connect_pad(&addr).await;
    echo(&mut pad, vec![5, 0, 1, 2, 3, 250]).await;
}

#[tokio::test]
async fn describes_the_snake() {
    let addr = spawn_server().await;
    let body = reqwest::get(format!("http://{addr}/"))
        .await
        .unwrap()
        .text()
        .await
        .unwrap();
    let value: serde_json::Value = serde_json::from_str(&body).unwrap();
    assert_eq!(value["apiversion"], "1");
    assert_eq!(value["author"], "David Crossman");
    assert_eq!(value["color"], "#DD69b4");
    assert_eq!(value["head"], "bendr");
    assert_eq!(value["tail"], "freckled");
}

async fn spawn_server() -> String {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = listener.local_addr().unwrap().to_string();
    let app = router(Config {
        token: TOKEN.to_owned(),
        colour: "#DD69b4".to_owned(),
    });
    tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });
    addr
}

/// A pad that has authenticated and received its hello.
async fn connect_pad(addr: &str) -> Pad {
    let (mut pad, _) = connect_async(format!("ws://{addr}/ws")).await.unwrap();
    pad.send(Message::text(TOKEN)).await.unwrap();
    let hello = next_text_containing(&mut pad, r#""t":"hello""#).await;
    assert_eq!(hello, r#"{"t":"hello","mode":"assist"}"#);
    pad
}

/// Heading up from (5,5) on an empty 11x11 board, with a 500 ms timeout.
fn game_state(game: &str) -> String {
    let body = r#"[{"x":5,"y":5},{"x":5,"y":4},{"x":5,"y":3}]"#;
    format!(
        r#"{{"game": {{"id": "{game}", "ruleset": {{"name": "standard", "version": "v1"}}, "timeout": 500}},
        "turn": 3,
        "board": {{"height": 11, "width": 11, "food": [], "hazards": [],
            "snakes": [{{"id": "me", "name": "me", "health": 97, "body": {body}, "head": {{"x":5,"y":5}}, "latency": "", "length": 3}}]}},
        "you": {{"id": "me", "name": "me", "health": 97, "body": {body}, "head": {{"x":5,"y":5}}, "latency": "", "length": 3}}}}"#
    )
}

async fn post(addr: &str, path: &str, body: &str) -> (String, Duration) {
    let started = Instant::now();
    let response = reqwest::Client::new()
        .post(format!("http://{addr}{path}"))
        .header("content-type", "application/json")
        .body(body.to_owned())
        .send()
        .await
        .unwrap();
    assert!(
        response.status().is_success(),
        "{path}: {}",
        response.status()
    );
    let text = response.text().await.unwrap();
    (text, started.elapsed())
}

async fn echo(pad: &mut Pad, frame: Vec<u8>) {
    pad.send(Message::binary(frame.clone())).await.unwrap();
    loop {
        match next_frame(pad).await {
            Message::Binary(bytes) => {
                assert_eq!(bytes.as_ref(), frame.as_slice());
                return;
            }
            Message::Text(_) => continue,
            other => panic!("expected the echo, got {other:?}"),
        }
    }
}

async fn next_text_containing(pad: &mut Pad, needle: &str) -> String {
    loop {
        if let Message::Text(text) = next_frame(pad).await
            && text.contains(needle)
        {
            return text.to_string();
        }
    }
}

async fn next_frame(pad: &mut Pad) -> Message {
    tokio::time::timeout(Duration::from_secs(2), pad.next())
        .await
        .expect("a frame within 2 s")
        .expect("the socket stays open")
        .unwrap()
}
