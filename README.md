# battlesnake-sk

A [Battlesnake](https://play.battlesnake.com) you steer in real time from a Flipper Zero, with an
autopilot behind you.

```
Flipper Zero ◀──Bluetooth──▶ phone browser (/pad) ──WebSocket──▶ server ◀──/move── Battlesnake engine
 Snake Pad app                live board + relay                  Rust, in memory
```

- **`flipper/`** – the Snake Pad app. It talks to the phone over Bluetooth in one of two ways,
  chosen when it starts:
  - **Board** – a two-way link. The Flipper's screen shows the live board, and its buttons steer.
    Needs Chrome on Android (Web Bluetooth) and the pad page served over HTTPS.
  - **Keyboard** – the Flipper is a Bluetooth keyboard and the D-pad sends arrow keys. Works with
    any phone browser, but the board is only on the phone.
- **`server/`** – the Battlesnake server (Rust, axum). It also serves `/pad`, the page you keep open
  on the phone: it draws the live board and relays each press to the server as one byte over a
  WebSocket. In Board mode it also mirrors the board to the Flipper.

## How a move is decided

When the engine asks for a move, the server answers **the instant you press a direction** (a press
that is already waiting counts). If you press nothing it waits as long as it safely can — the game's
timeout minus the engine's measured overhead and a 50 ms margin — and then falls back. Up to two
presses are queued, so a quick double turn works. A press back into your own neck is ignored.

| Mode | A press | No press by the deadline |
| --- | --- | --- |
| `assist` (default) | played, unless it is certain death (wall or body) | straight on, or the autopilot if straight is certain death |
| `manual` | played as-is | straight on, even into a wall |
| `auto` | ignored | the autopilot answers immediately |

With no pad connected the autopilot plays every game and answers in well under a millisecond. The
pad steers one snake in one game at a time — the game that started most recently; any other game
running at the same time stays on autopilot. A phone that drops off the network is noticed within
6 seconds, after which the autopilot takes over again.

In `assist` the snake only keeps going straight for you; it does not look for food. Switch to
`auto` (or close the page) when you are not steering.

The page draws the board straight from the server: it appears when the game is created, updates
the moment the engine asks for each move, and moves your own snake as soon as your move is
committed, so what you see is the position your next press applies to.

## Controls

| Flipper button | Effect |
| --- | --- |
| D-pad | steer |
| OK | next mode: assist → manual → auto |
| Back (Keyboard link only) | switch between Arrow keys and W/A/S/D |
| Hold Back | exit and restore the Flipper's normal Bluetooth |

On the Keyboard link the D-pad sends Arrow keys (or W/A/S/D) and OK sends Space, each as a single
key tap, so holding a button does not repeat. The pad page takes the same keys from any keyboard,
which is handy for testing without the Flipper.

## Run it locally

```bash
cd server
PORT=8000 CONTROL_TOKEN=dev cargo run --release
```

Open <http://localhost:8000/pad#dev>, then start a game with the
[Battlesnake CLI](https://github.com/BattlesnakeOfficial/rules/tree/main/cli):

```bash
battlesnake play -W 11 -H 11 --name me --url http://localhost:8000 -g solo
```

| Variable | Default | |
| --- | --- | --- |
| `PORT` | `80` | port to listen on |
| `CONTROL_TOKEN` | random, logged at boot | the secret after `#` in the pad URL |
| `SNAKE_COLOUR` | `#DD69b4` | snake colour |

`cargo test` runs the unit and integration tests.

## Flipper app

Build with [ufbt](https://github.com/flipperdevices/flipperzero-ufbt). The SDK has to match the
firmware on the Flipper (Settings → About). For Unleashed `unlshd-090`:

```bash
ufbt update --hw-target f7 --url https://github.com/DarkFlippers/unleashed-firmware/releases/download/unlshd-090/flipper-z-f7-sdk-unlshd-090.zip
```

Then, with the Flipper connected over USB:

```bash
cd flipper
ufbt launch
```

That installs the app under Apps → Bluetooth → Snake Pad and starts it.

`board_frame.c` (the parser for what the page sends) has no Flipper dependencies and a host test:

```bash
cc -std=c11 -Wall -Wextra -Werror board_frame.c board_frame_test.c -o /tmp/board_frame_test && /tmp/board_frame_test
```

### Play with the board on the Flipper (Board link)

1. Start Snake Pad on the Flipper and choose **Board**. It advertises as
   `Board <your Flipper's name>`.
2. In Chrome on Android, open `https://<your server>/pad#<CONTROL_TOKEN>` and tap **Connect Flipper
   screen**. Pick the Flipper from the list. The first time, Android asks for the PIN shown on the
   Flipper.
3. Start a game. The Flipper buzzes and shows the board; the bar at the bottom right is the time
   left to press.

The button only appears where the browser offers Web Bluetooth, which Chrome does on HTTPS pages and
on `localhost`. Both links pair separately from the Flipper mobile app; that pairing is untouched.

### Play with the board on the phone (Keyboard link)

1. Start Snake Pad on the Flipper and choose **Keyboard**. It advertises as
   `Snake <your Flipper's name>`.
2. Pair with it in the phone's Bluetooth settings.
3. Open `/pad#<CONTROL_TOKEN>` on the phone and tap the page once. A banner shows whenever the page
   needs that tap.
4. Start a game. The board appears as soon as the game is created.

If the page does not react to the D-pad on an iPhone or iPad, press Back on the Flipper to switch to
W/A/S/D. Some iOS versions do not pass key events from this kind of Bluetooth keyboard to web pages,
but typed letters still arrive; the page's "Last key via" tile shows which path delivered a press.

### Board link protocol

The page and the Flipper talk over the Flipper's Bluetooth serial service.

Flipper → page: one byte per button — `0` up, `1` down, `2` left, `3` right, `4` mode.

Page → Flipper: a stream of frames, one each time the page redraws the board.

```
0x53            start marker
len_lo len_hi   payload length (at most 400)
payload:
 [0] flags      bit0 game just started · bit1 game over · bit2 waiting for a game
                bit3 board too large (over 25 wide or high) · bits 4-5 mode (0 assist, 1 manual, 2 auto)
                bit6 the page lost the server
 [1] width      [2] height
 [3] turn low   [4] turn high
 [5] health     [6] length
 [7] hold       time left to press, in units of 10 ms
 [8..] cells    one nibble each, top row first, first cell in the high nibble:
                0 empty · 1 food · 2 hazard · 3 my body · 4 my head · 5 other body · 6 other head
```

The page never writes more bytes than the service's flow-control characteristic allows, and waits
for the Flipper to announce room again when that reaches zero.

## Deploy

Pushing to `main` deploys to AWS (ECS, via SST) from `.github/workflows/deploy.yml`; the image is
built from `Dockerfile`. The deploy log ends with two addresses:

- `engine` – the load balancer, `http://….elb.amazonaws.com`. Register this one with Battlesnake;
  the engine calls it directly.
- `pad` – CloudFront, `https://….cloudfront.net`. Open `/pad#<token>` on this one. HTTPS is what
  lets the page use Web Bluetooth and keep the phone's screen awake.

Set the pad token once before the first deploy, with AWS credentials for the account, in the region
the deploy runs in:

```bash
AWS_REGION=us-west-2 npx sst secret set ControlToken <token> --stage prod
```

The deploy fails until the secret is set. To change the token, set it again and redeploy.

- The server keeps the game and pad state in memory, so it must run as exactly one task.
- CloudFront caches nothing and forwards every request to the load balancer over plain HTTP, so
  that last hop inside AWS is not encrypted.
- The token is readable in the ECS task definition by anyone with access to the AWS account.
- `/start`, `/move` and `/end` are open, as the Battlesnake API requires. Someone who knows the URL
  can start a game against the snake, and the pad follows the most recently started game.

## Limits

- The autopilot is a port of the original TypeScript heuristics. Like the original it treats
  hazards as walls and does not know about wrapped boards. It looks five moves ahead where the
  original effectively managed two, so it can choose differently.
- Boards larger than 50×50 are refused.
