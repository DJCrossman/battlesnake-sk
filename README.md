# battlesnake-sk

A [Battlesnake](https://play.battlesnake.com) you steer in real time from a Flipper Zero, with an
autopilot behind you.

```
Flipper Zero ──Bluetooth keyboard──▶ phone browser (/pad) ──WebSocket──▶ server ◀──/move── Battlesnake engine
 Snake Pad app                        live board + key relay              Rust, in memory
```

- **`flipper/`** – the Snake Pad app. The Flipper pairs with your phone as a Bluetooth keyboard; the
  D-pad sends arrow keys.
- **`server/`** – the Battlesnake server (Rust, axum). It also serves `/pad`, the page you keep open
  on the phone: it draws the live board and relays each key press to the server as one byte over a
  WebSocket.

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

| Flipper button | Sends | Effect |
| --- | --- | --- |
| D-pad | Arrow keys (or W/A/S/D) | steer |
| OK | Space | next mode: assist → manual → auto |
| Back | – | switch between Arrow keys and W/A/S/D |
| Hold Back | – | exit and restore the Flipper's normal Bluetooth |

Each press sends a single key tap, so holding a button does not repeat. The pad page takes the same
keys from any keyboard, which is handy for testing without the Flipper.

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

To play:

1. Start Snake Pad on the Flipper. It advertises as `Snake <your Flipper's name>`.
2. Pair with it in the phone's Bluetooth settings. This pairing is separate from the Flipper mobile
   app's; that one is untouched.
3. Open `http://<your server>/pad#<CONTROL_TOKEN>` on the phone and tap the page once. A banner
   shows whenever the page needs that tap.
4. Start a game. The board appears as soon as the game is created.

If the page does not react to the D-pad on an iPhone or iPad, press Back on the Flipper to switch to
W/A/S/D. Some iOS versions do not pass key events from this kind of Bluetooth keyboard to web pages,
but typed letters still arrive; the page's "Last key via" tile shows which path delivered a press.

## Deploy

Pushing to `main` deploys to AWS (ECS, via SST) from `.github/workflows/deploy.yml`; the image is
built from `Dockerfile`.

- The server keeps the game and pad state in memory, so it must run as exactly one task.
- Set `CONTROL_TOKEN` on the service. Without it a new token is generated at every start and written
  to the log.
- The load balancer listens on plain HTTP, so the token is not encrypted in transit and the page
  cannot keep the phone's screen awake (that needs HTTPS).
- `/start`, `/move` and `/end` are open, as the Battlesnake API requires. Someone who knows the URL
  can start a game against the snake, and the pad follows the most recently started game.

## Limits

- The autopilot is a port of the original TypeScript heuristics. Like the original it treats
  hazards as walls and does not know about wrapped boards. It looks five moves ahead where the
  original effectively managed two, so it can choose differently.
- Boards larger than 50×50 are refused.
