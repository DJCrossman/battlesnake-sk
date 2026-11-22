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
const PAD_IDLE: Duration = Duration::from_secs(6);

#[tokio::test]
async fn wrong_token_is_rejected() {
    let addr = spawn_server(PAD_IDLE).await;
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
    let addr = spawn_server(PAD_IDLE).await;
    let mut pad = connect_pad(&addr).await;
    post(&addr, "/start", &game_state("g1", "me", 3)).await;
    pad.send(Message::binary(vec![2u8])).await.unwrap();
    // The echo comes back after the server has handled the press sent before it.
    echo(&mut pad, vec![5, 1]).await;

    let (answer, took) = post(&addr, "/move", &game_state("g1", "me", 3)).await;
    assert_eq!(answer, r#"{"move":"left"}"#);
    assert!(took < Duration::from_millis(100), "{took:?}");
}

#[tokio::test]
async fn input_during_the_hold_answers_at_once() {
    let addr = spawn_server(PAD_IDLE).await;
    let mut pad = connect_pad(&addr).await;
    post(&addr, "/start", &game_state("g1", "me", 3)).await;
    let request = tokio::spawn({
        let addr = addr.clone();
        async move { post(&addr, "/move", &game_state("g1", "me", 3)).await }
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
    let addr = spawn_server(PAD_IDLE).await;
    let mut pad = connect_pad(&addr).await;
    post(&addr, "/start", &game_state("g1", "me", 3)).await;

    let (answer, took) = post(&addr, "/move", &game_state("g1", "me", 3)).await;
    assert_eq!(answer, r#"{"move":"up"}"#);
    assert!(took >= HOLD - Duration::from_millis(10), "{took:?}");
    assert!(took < HOLD + Duration::from_millis(100), "{took:?}");
    let moved = next_text_containing(&mut pad, r#""t":"moved""#).await;
    assert!(moved.contains(r#""source":"straight""#), "{moved}");
}

#[tokio::test]
async fn without_a_pad_the_autopilot_answers_immediately() {
    let addr = spawn_server(PAD_IDLE).await;
    post(&addr, "/start", &game_state("g1", "me", 3)).await;
    let (answer, took) = post(&addr, "/move", &game_state("g1", "me", 3)).await;
    assert!(answer.starts_with(r#"{"move":""#), "{answer}");
    assert!(took < Duration::from_millis(50), "{took:?}");
}

#[tokio::test]
async fn auto_mode_answers_immediately() {
    let addr = spawn_server(PAD_IDLE).await;
    let mut pad = connect_pad(&addr).await;
    post(&addr, "/start", &game_state("g1", "me", 3)).await;
    pad.send(Message::binary(vec![4u8])).await.unwrap();
    next_text_containing(&mut pad, r#""mode":"manual""#).await;
    pad.send(Message::binary(vec![4u8])).await.unwrap();
    next_text_containing(&mut pad, r#""mode":"auto""#).await;

    let (_, took) = post(&addr, "/move", &game_state("g1", "me", 3)).await;
    assert!(took < Duration::from_millis(50), "{took:?}");
    let moved = next_text_containing(&mut pad, r#""t":"moved""#).await;
    assert!(moved.contains(r#""source":"auto""#), "{moved}");
}

#[tokio::test]
async fn another_game_is_played_by_the_autopilot_without_waiting() {
    let addr = spawn_server(PAD_IDLE).await;
    let _pad = connect_pad(&addr).await;
    post(&addr, "/start", &game_state("mine", "me", 3)).await;
    let (_, took) = post(&addr, "/move", &game_state("someone-elses", "me", 3)).await;
    assert!(took < Duration::from_millis(50), "{took:?}");
}

#[tokio::test]
async fn echo_frames_come_back_unchanged() {
    let addr = spawn_server(PAD_IDLE).await;
    let mut pad = connect_pad(&addr).await;
    echo(&mut pad, vec![5, 0, 1, 2, 3, 250]).await;
}

#[tokio::test]
async fn describes_the_snake() {
    let addr = spawn_server(PAD_IDLE).await;
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

#[tokio::test]
async fn oversized_boards_are_refused_and_the_server_keeps_answering() {
    let addr = spawn_server(PAD_IDLE).await;
    let client = reqwest::Client::new();
    for (path, side) in [
        ("/start", 51),
        ("/move", 65536),
        ("/move", 20000),
        ("/end", 0),
    ] {
        let body = game_state("g1", "me", 3).replace(
            r#""height": 11, "width": 11"#,
            &format!(r#""height": {side}, "width": {side}"#),
        );
        let response = client
            .post(format!("http://{addr}{path}"))
            .header("content-type", "application/json")
            .body(body)
            .send()
            .await
            .unwrap();
        assert_eq!(response.status(), 400, "{path} with side {side}");
    }
    let (answer, _) = post(&addr, "/move", &game_state("g1", "me", 3)).await;
    assert!(answer.starts_with(r#"{"move":""#), "{answer}");
}

#[tokio::test]
async fn bodies_past_512_kib_are_refused_and_a_crowded_19x19_game_still_gets_a_move() {
    let addr = spawn_server(PAD_IDLE).await;
    // Food on every cell of a 19x19 board and hazards stacked eight deep on each, as deep as the
    // Sinkholes map stacks its centre: more than any real game sends.
    let cells: Vec<String> = (0..19)
        .flat_map(|y| (0..19).map(move |x| format!(r#"{{"x":{x},"y":{y}}}"#)))
        .collect();
    let crowded = game_state("g1", "me", 3).replace(
        r#""height": 11, "width": 11, "food": [], "hazards": []"#,
        &format!(
            r#""height": 19, "width": 19, "food": [{}], "hazards": [{}]"#,
            cells.join(","),
            vec![cells.join(","); 8].join(",")
        ),
    );
    let (answer, _) = post(&addr, "/move", &crowded).await;
    assert!(answer.starts_with(r#"{"move":""#), "{answer}");

    // Trailing spaces keep the JSON valid: exactly 512 KiB is answered, one byte more is refused.
    // Only one byte over, the server has read the whole body by the time it refuses, so it does
    // not hang up while the client is still sending.
    let mut padded = game_state("g1", "me", 3);
    padded.extend(std::iter::repeat_n(' ', 512 * 1024 - padded.len()));
    let (answer, _) = post(&addr, "/move", &padded).await;
    assert!(answer.starts_with(r#"{"move":""#), "{answer}");
    padded.push(' ');
    let response = reqwest::Client::new()
        .post(format!("http://{addr}/move"))
        .header("content-type", "application/json")
        .body(padded)
        .send()
        .await
        .unwrap();
    assert_eq!(response.status(), 413);
}

#[tokio::test]
async fn presses_made_in_auto_are_not_replayed() {
    let addr = spawn_server(PAD_IDLE).await;
    let mut pad = connect_pad(&addr).await;
    post(&addr, "/start", &game_state("g1", "me", 3)).await;
    for mode in ["manual", "auto"] {
        pad.send(Message::binary(vec![4u8])).await.unwrap();
        next_text_containing(&mut pad, &format!(r#""mode":"{mode}""#)).await;
    }
    pad.send(Message::binary(vec![2u8])).await.unwrap();
    pad.send(Message::binary(vec![4u8])).await.unwrap();
    next_text_containing(&mut pad, r#""mode":"assist""#).await;

    let (answer, took) = post(&addr, "/move", &game_state("g1", "me", 3)).await;
    assert_eq!(answer, r#"{"move":"up"}"#);
    assert!(took >= HOLD - Duration::from_millis(10), "{took:?}");
}

#[tokio::test]
async fn changing_mode_drops_queued_presses() {
    let addr = spawn_server(PAD_IDLE).await;
    let mut pad = connect_pad(&addr).await;
    post(&addr, "/start", &game_state("g1", "me", 3)).await;
    pad.send(Message::binary(vec![2u8])).await.unwrap();
    pad.send(Message::binary(vec![4u8])).await.unwrap();
    next_text_containing(&mut pad, r#""mode":"manual""#).await;

    let (answer, took) = post(&addr, "/move", &game_state("g1", "me", 3)).await;
    assert_eq!(answer, r#"{"move":"up"}"#);
    assert!(took >= HOLD - Duration::from_millis(10), "{took:?}");
}

#[tokio::test]
async fn a_game_attached_by_its_first_move_starts_without_old_presses() {
    let addr = spawn_server(PAD_IDLE).await;
    let mut pad = connect_pad(&addr).await;
    post(&addr, "/start", &game_state("g1", "me", 3)).await;
    pad.send(Message::binary(vec![2u8])).await.unwrap();
    echo(&mut pad, vec![5, 1]).await;
    post(&addr, "/end", &game_state("g1", "me", 3)).await;
    // Between games a press has nothing to steer.
    pad.send(Message::binary(vec![3u8])).await.unwrap();
    echo(&mut pad, vec![5, 2]).await;

    let (answer, took) = post(&addr, "/move", &game_state("g2", "me", 3)).await;
    assert_eq!(answer, r#"{"move":"up"}"#);
    assert!(took >= HOLD - Duration::from_millis(10), "{took:?}");
}

#[tokio::test]
async fn our_second_snake_in_the_same_game_does_not_wait() {
    let addr = spawn_server(PAD_IDLE).await;
    let _pad = connect_pad(&addr).await;
    post(&addr, "/start", &game_state("g1", "me", 3)).await;
    let (_, took) = post(&addr, "/move", &game_state("g1", "twin", 3)).await;
    assert!(took < Duration::from_millis(50), "{took:?}");
}

#[tokio::test]
async fn a_newer_turn_releases_the_older_request_without_taking_the_press() {
    let addr = spawn_server(PAD_IDLE).await;
    let mut pad = connect_pad(&addr).await;
    post(&addr, "/start", &game_state("g1", "me", 3)).await;
    let older = tokio::spawn({
        let addr = addr.clone();
        async move { post(&addr, "/move", &game_state("g1", "me", 3)).await }
    });
    next_text_containing(&mut pad, r#""t":"turn""#).await;
    let newer = tokio::spawn({
        let addr = addr.clone();
        async move { post(&addr, "/move", &game_state("g1", "me", 4)).await }
    });
    let (answer, took) = older.await.unwrap();
    assert_eq!(answer, r#"{"move":"up"}"#);
    assert!(took < HOLD - Duration::from_millis(100), "{took:?}");

    next_text_containing(&mut pad, r#""turn": 4,"#).await;
    pad.send(Message::binary(vec![2u8])).await.unwrap();
    let (answer, took) = newer.await.unwrap();
    assert_eq!(answer, r#"{"move":"left"}"#);
    assert!(took < HOLD - Duration::from_millis(100), "{took:?}");
}

#[tokio::test]
async fn the_flipper_gets_no_board_for_the_move_of_a_turn_the_engine_gave_up_on() {
    let addr = spawn_server(PAD_IDLE).await;
    let mut page = connect_pad(&addr).await;
    let mut flipper = connect_pad(&addr).await;
    flipper.send(Message::binary(vec![7u8])).await.unwrap();
    assert_eq!(
        next_frame(&mut flipper).await,
        Message::binary(vec![0xB1u8])
    );
    post(&addr, "/start", &game_state("g1", "me", 3)).await;
    let Message::Binary(start) = next_frame(&mut flipper).await else {
        panic!("expected the start board");
    };
    assert_eq!(start[..2], [0xB0, 0x01]);

    let older = tokio::spawn({
        let addr = addr.clone();
        async move { post(&addr, "/move", &game_state("g1", "me", 3)).await }
    });
    let Message::Binary(turn) = next_frame(&mut flipper).await else {
        panic!("expected turn 3's board");
    };
    assert_eq!(turn[4..6], [3, 0]);
    let newer = tokio::spawn({
        let addr = addr.clone();
        async move { post(&addr, "/move", &game_state("g1", "me", 4)).await }
    });
    let (answer, _) = older.await.unwrap();
    assert_eq!(answer, r#"{"move":"up"}"#);

    // Turn 3's moved board would show turn 4 with nothing to hold for, before or after this one.
    let Message::Binary(turn) = next_frame(&mut flipper).await else {
        panic!("expected turn 4's board");
    };
    assert_eq!(turn[4..6], [4, 0]);
    assert_eq!(turn[9..11], [35, 0]);
    flipper.send(Message::binary(vec![2u8])).await.unwrap();
    let (answer, _) = newer.await.unwrap();
    assert_eq!(answer, r#"{"move":"left"}"#);
    let Message::Binary(moved) = next_frame(&mut flipper).await else {
        panic!("expected turn 4's moved board");
    };
    assert_eq!(moved[4..6], [5, 0]);
    assert_eq!(moved[9..11], [0, 0]);
    flipper.send(Message::binary(vec![5u8, 1])).await.unwrap();
    assert_eq!(
        next_frame(&mut flipper).await,
        Message::binary(vec![5u8, 1])
    );

    // The pad page is still told how the older request was answered.
    next_text_containing(&mut page, r#""t":"moved","turn":3,"#).await;
}

#[tokio::test]
async fn the_flipper_keeps_the_game_over_board_when_the_last_move_answers_after_the_end() {
    let addr = spawn_server(PAD_IDLE).await;
    let mut flipper = connect_pad(&addr).await;
    flipper.send(Message::binary(vec![7u8])).await.unwrap();
    assert_eq!(
        next_frame(&mut flipper).await,
        Message::binary(vec![0xB1u8])
    );
    post(&addr, "/start", &game_state("g1", "me", 3)).await;
    next_frame(&mut flipper).await;

    let last = tokio::spawn({
        let addr = addr.clone();
        async move { post(&addr, "/move", &game_state("g1", "me", 3)).await }
    });
    let Message::Binary(turn) = next_frame(&mut flipper).await else {
        panic!("expected turn 3's board");
    };
    assert_eq!(turn[4..6], [3, 0]);
    post(&addr, "/end", &game_state("g1", "me", 3)).await;
    let Message::Binary(end) = next_frame(&mut flipper).await else {
        panic!("expected the game-over board");
    };
    assert_eq!(end[1] & 0x02, 0x02);
    last.await.unwrap();

    // The move answered at its deadline, after the end: no board replaces the game-over one.
    flipper.send(Message::binary(vec![5u8, 1])).await.unwrap();
    assert_eq!(
        next_frame(&mut flipper).await,
        Message::binary(vec![5u8, 1])
    );
}

#[tokio::test]
async fn a_silent_pad_is_closed_and_moves_stop_waiting() {
    let addr = spawn_server(Duration::from_millis(300)).await;
    let mut pad = connect_pad(&addr).await;
    post(&addr, "/start", &game_state("g1", "me", 3)).await;
    // Say nothing; the server should hang up shortly after 300 ms.
    let closed = tokio::time::timeout(Duration::from_secs(2), async {
        loop {
            match pad.next().await {
                Some(Ok(Message::Text(_))) => continue,
                _ => return,
            }
        }
    })
    .await;
    assert!(closed.is_ok(), "the silent pad was not closed");
    let (_, took) = post(&addr, "/move", &game_state("g1", "me", 3)).await;
    assert!(took < Duration::from_millis(50), "{took:?}");
}

/// Everything the WiFi board sends: the token, the request for quiet, a `[5, n]` heartbeat on a
/// timer and one byte per press.
#[tokio::test]
async fn heartbeats_alone_keep_a_pad_past_the_idle_limit_and_it_still_steers() {
    let addr = spawn_server(Duration::from_millis(300)).await;
    let (mut board, _) = connect_async(format!("ws://{addr}/ws")).await.unwrap();
    board.send(Message::text(TOKEN)).await.unwrap();
    board.send(Message::binary(vec![6u8])).await.unwrap();
    post(&addr, "/start", &game_state("g1", "me", 3)).await;
    // Three idle limits go by with nothing but heartbeats.
    for n in 0..9u8 {
        echo(&mut board, vec![5, n]).await;
        tokio::time::sleep(Duration::from_millis(100)).await;
    }
    board.send(Message::binary(vec![3u8])).await.unwrap();
    echo(&mut board, vec![5, 9]).await;

    let (answer, took) = post(&addr, "/move", &game_state("g1", "me", 3)).await;
    assert_eq!(answer, r#"{"move":"right"}"#);
    assert!(took < Duration::from_millis(100), "{took:?}");
}

#[tokio::test]
async fn a_pad_that_asks_for_quiet_is_sent_no_state_frames() {
    let addr = spawn_server(PAD_IDLE).await;
    let mut pad = connect_pad(&addr).await;
    pad.send(Message::binary(vec![6u8])).await.unwrap();
    // The echo comes back after the server has handled the request sent before it.
    echo(&mut pad, vec![5, 0]).await;

    post(&addr, "/start", &game_state("g1", "me", 3)).await;
    pad.send(Message::binary(vec![3u8])).await.unwrap();
    pad.send(Message::binary(vec![5u8, 1])).await.unwrap();
    // The start frame went to every other pad; the next frame here is the echo.
    assert_eq!(next_frame(&mut pad).await, Message::binary(vec![5u8, 1]));

    let (answer, _) = post(&addr, "/move", &game_state("g1", "me", 3)).await;
    assert_eq!(answer, r#"{"move":"right"}"#);
    pad.send(Message::binary(vec![5u8, 2])).await.unwrap();
    // Nor did the turn and moved frames come.
    assert_eq!(next_frame(&mut pad).await, Message::binary(vec![5u8, 2]));
}

#[tokio::test]
async fn a_pad_that_asks_for_the_flipper_s_frames_gets_boards_and_no_text() {
    let addr = spawn_server(PAD_IDLE).await;
    let mut pad = connect_pad(&addr).await;
    pad.send(Message::binary(vec![7u8])).await.unwrap();
    // Answered with the mode byte: assist.
    assert_eq!(next_frame(&mut pad).await, Message::binary(vec![0xB1u8]));

    post(&addr, "/start", &game_state("g1", "me", 3)).await;
    let Message::Binary(start) = next_frame(&mut pad).await else {
        panic!("expected the start board");
    };
    // The start byte, then the flags.
    assert_eq!(start[..2], [0xB0, 0x01]);
    assert_eq!(start.len(), 74);

    post(&addr, "/move", &game_state("g1", "me", 3)).await;
    let Message::Binary(turn) = next_frame(&mut pad).await else {
        panic!("expected the turn board");
    };
    assert_eq!(turn[..2], [0xB0, 0x00]);
    // 350 ms to press, in units of 10 ms.
    assert_eq!(turn[9..11], [35, 0]);
    let Message::Binary(moved) = next_frame(&mut pad).await else {
        panic!("expected the moved board");
    };
    assert_eq!(moved[..2], [0xB0, 0x00]);
    assert_eq!(moved[4..6], [4, 0]);
    assert_eq!(moved[9..11], [0, 0]);

    post(&addr, "/end", &game_state("g1", "me", 4)).await;
    let Message::Binary(end) = next_frame(&mut pad).await else {
        panic!("expected the game-over board");
    };
    // Game over, and ours is the only snake on the board.
    assert_eq!(end[..2], [0xB0, 0x02 | 0x08]);

    pad.send(Message::binary(vec![4u8])).await.unwrap();
    assert_eq!(next_frame(&mut pad).await, Message::binary(vec![0xB2u8]));
    // Nothing else was sent: the next frame is the echo.
    pad.send(Message::binary(vec![5u8, 1])).await.unwrap();
    assert_eq!(next_frame(&mut pad).await, Message::binary(vec![5u8, 1]));
}

#[tokio::test]
async fn oversized_pad_frames_close_the_socket() {
    let addr = spawn_server(PAD_IDLE).await;
    let mut pad = connect_pad(&addr).await;
    let mut frame = vec![0u8; 2048];
    frame[0] = 5;
    pad.send(Message::binary(frame)).await.unwrap();
    let reply = tokio::time::timeout(Duration::from_secs(2), async {
        loop {
            match pad.next().await {
                Some(Ok(Message::Text(_))) => continue,
                other => return other,
            }
        }
    })
    .await
    .unwrap();
    assert!(
        !matches!(reply, Some(Ok(Message::Binary(_)))),
        "a 2 KiB frame was echoed"
    );
}

async fn spawn_server(pad_idle: Duration) -> String {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = listener.local_addr().unwrap().to_string();
    let app = router(Config {
        token: TOKEN.to_owned(),
        colour: "#DD69b4".to_owned(),
        pad_idle,
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

/// `snake` heading up from (5,5) on an empty 11x11 board, with a 500 ms timeout.
fn game_state(game: &str, snake: &str, turn: u32) -> String {
    let body = r#"[{"x":5,"y":5},{"x":5,"y":4},{"x":5,"y":3}]"#;
    format!(
        r#"{{"game": {{"id": "{game}", "ruleset": {{"name": "standard", "version": "v1"}}, "timeout": 500}},
        "turn": {turn},
        "board": {{"height": 11, "width": 11, "food": [], "hazards": [],
            "snakes": [{{"id": "me", "name": "me", "health": 97, "body": {body}, "head": {{"x":5,"y":5}}, "latency": "", "length": 3}}]}},
        "you": {{"id": "{snake}", "name": "{snake}", "health": 97, "body": {body}, "head": {{"x":5,"y":5}}, "latency": "", "length": 3}}}}"#
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
