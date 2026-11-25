# battlesnake-sk

A [Battlesnake](https://play.battlesnake.com) you steer in real time from a Flipper Zero, with an
autopilot behind you.

```
Flipper Zero ◀─UART─▶ Wi-Fi Devboard ◀─phone hotspot─▶ server ◀──/move── Battlesnake engine
 Battlesnake app       wifi/ firmware    WebSocket
 presses out, board back
```

- **`flipper/`** – the Battlesnake app. A Wi-Fi Devboard on the Flipper joins the phone's hotspot and
  holds the WebSocket itself, so the phone is only the path to the internet. The Flipper's buttons
  steer and its screen shows the live board.
- **`server/`** – the Battlesnake server (Rust, axum). It also serves `/pad`, the page you watch a
  game on: it draws the live board and relays each key press to the server as one byte over a
  WebSocket.
- **`wifi/`** – the firmware for the Wi-Fi Devboard. See [Play over WiFi](#play-over-wifi-wifi-link).

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
pad steers one snake in one game at a time: the first game to start while no other is in
progress. That game keeps the pad until it ends or goes a minute without a request from the
engine, and any other game running at the same time stays on autopilot. A phone that drops off
the network is noticed within 6 seconds, after which the autopilot takes over again.

In `assist` the snake only keeps going straight for you; it does not look for food. Switch to
`auto` (or close the page) when you are not steering.

The Flipper and the page draw the board straight from the server: it appears when the game is
created, updates the moment the engine asks for each move, and moves your own snake as soon as
your move is committed, so what you see is the position your next press applies to.

## Controls

| Flipper button | Effect |
| --- | --- |
| D-pad | steer |
| OK | next mode: assist → manual → auto |
| Hold Back | exit |

The pad page takes Arrow keys (or W/A/S/D) and Space from any keyboard, which is handy for testing
without the Flipper.

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

That installs the app under Apps → Games → Battlesnake and starts it.

### Play over WiFi (WiFi link)

You need the official
[Flipper Zero Wi-Fi Devboard](https://developer.flipper.net/flipperzero/doxygen/dev_board.html)
(ESP32-S2). It plugs into the Flipper's GPIO header and takes its power from it. Its radio is
2.4 GHz only.

**1. Flash the board, once.** The board ships with the Black Magic debugger firmware, which this
replaces. Install [arduino-cli](https://arduino.github.io/arduino-cli/) and build:

```bash
brew install arduino-cli
```

```bash
cd wifi && arduino-cli compile
```

The first build downloads the ESP32 core and the WebSockets library at the versions pinned in
`wifi/sketch.yaml`, about 5 GB, into `~/Library/Arduino15`.

Connect the board's own USB-C port to the computer, with the board off the Flipper. Put it in
download mode: hold **BOOT**, press and release **RESET**, then release **BOOT**. Find the port
that appeared:

```bash
arduino-cli board list
```

Then, from `wifi/`, with that port:

```bash
arduino-cli upload -p /dev/cu.usbmodem01
```

The upload is done at `Hash of data verified`. It can then report `Could not configure port` and
`Failed uploading`: that is the board restarting into the new firmware, which does not show up on
USB at all.

Press **RESET** when it finishes. Flashing again later needs download mode again. To put Black
Magic back, run `ufbt devboard_flash` with the board in download mode.

**2. Write the settings file.** Copy `flipper/wifi.example.json` to `flipper/wifi.json` and fill
it in. `flipper/wifi.json` is git-ignored, so the passwords and the token stay out of the repo.

```json
{
  "server": "wss://<the pad address from the deploy log, without https://>",
  "token": "<CONTROL_TOKEN>",
  "networks": [
    { "name": "<the first hotspot's name>", "password": "<its password>" },
    { "name": "<the next hotspot's name>", "password": "<its password>" }
  ]
}
```

List one network, or several, such as home and work. The app tries them in the order listed,
moving to the next whenever the board reports the current one not found and back to the first
after the last, so a network further down the list takes about 8 seconds longer per network above
it. The values are JSON strings, so a `"` or `\` in a password is written `\"` or `\\`.

For a server on your own computer, use `"server": "ws://<its address on the hotspot>:8000"`. The
app reads the file each time the board asks for it and the board keeps the settings only in
memory, so neither the app nor the firmware contains them.

Put the file on the Flipper as `SD Card/apps_data/snake_pad/wifi.json` with qFlipper's file
manager or the Flipper mobile app, or with this command from the repo root, with the Flipper
connected over USB:

```bash
~/.ufbt/toolchain/arm64-darwin/bin/python3 ~/.ufbt/current/scripts/storage.py -p auto send flipper/wifi.json /ext/apps_data/snake_pad/wifi.json
```

The hotspot's name has to match exactly. An iPhone's default name has a curly apostrophe
(`David’s iPhone`), which is easy to get wrong; renaming the phone in Settings → General → About
avoids it.

**3. Turn the hotspot on, on 2.4 GHz.** On an iPhone that is Settings → Personal Hotspot →
Maximize Compatibility. If the Flipper says the hotspot is not found, keep that settings screen
open while the board joins. On Android, choose the 2.4 GHz band in the hotspot's settings.

**4. Plug the board into the Flipper and start Battlesnake.** The LED blinks blue until
the board is connected and is green after that. The screen says where the connection stands:

| Screen | Meaning |
| --- | --- |
| `Board not answering` | the board is not plugged in or not flashed. It also shows for a few seconds at a time while the board is tied up reaching a server that does not answer |
| `No settings file`, `Settings file incomplete` | check `apps_data/snake_pad/wifi.json` for the server, the token and each network's name and password |
| `Settings file invalid` | `wifi.json` is not JSON, is over 4 KB or 24 networks, or has a value that is not in quotes or has a line break in it |
| `Joining hotspot` | the line below names it |
| `Hotspot not found` | the hotspot named below is off, out of range or on 5 GHz only; the next one in the file gets its turn |
| `Hotspot refused password` | for the hotspot named below |
| `Reaching server` | on the hotspot, but the server has not answered: wrong address, no mobile data or the server is down |
| `Server refused token` | |
| `Connected` | with the ping: the round trip Flipper → server → Flipper, measured every 2 s |

The status screen also shows the mode once the server has said which it is.

Start a game and steer. While a game is on, the Flipper shows the board on the left: your snake
solid with a hollow head, the others outlined, food as dots. On the right it shows the turn, your
health and length, the mode and a bar for the time left to press. A game opens with rings around
your head and a buzz. It ends with fireworks and a tune if you won, or your snake falling off the
board if you lost. The final board then stays up for 15 seconds, and then the status screen
returns. Holding Back exits and tells the board to drop the hotspot.

### WiFi link protocol

The Flipper and the board talk over the UART on the GPIO header at 115200 baud: Flipper pin 13
(TX) to the ESP32-S2's GPIO44, and GPIO43 to Flipper pin 14 (RX).

| Flipper → board | |
| --- | --- |
| `0`–`4` | a press: up, down, left, right, mode. The board sends it on as a one-byte WebSocket frame |
| `5` | heartbeat, every 2 s. The board answers with its state and, when connected, sends the server `[5, n]` |
| `6` | stop: the app is leaving, so the board restarts and waits for settings |
| `0x10` | settings, followed by four lines: hotspot, password, server, token. Sent when the board says it is waiting |

| Board → Flipper | |
| --- | --- |
| `0xA0` | waiting for settings |
| `0xA1` | joining the hotspot |
| `0xA2` | hotspot not found |
| `0xA3` | hotspot refused the password |
| `0xA4` | on WiFi, reaching the server |
| `0xA5` | the server refused the token |
| `0xA6` | connected: the server answered the token |
| `0xA7` | the server answered the last heartbeat |

| `0xB0` … | a board frame from the server, passed on unchanged (below) |
| `0xB1`, `0xB2`, `0xB3` | the mode is now assist, manual, auto: once on connecting, then on every change |

Every byte from `0x80` up starts one of these, and every byte inside a board frame is below
`0x80`. So none can be taken for another, and a frame cut short is dropped when the next one
starts. The board sends its state when it changes and in answer to each heartbeat. Any byte below
`0x80` outside a frame is the ESP32's boot text, which the Flipper ignores. A board
that gets no heartbeat from the Flipper for 6 seconds closes its socket and restarts, so a Flipper
that goes away cannot leave a pad connected and the autopilot takes over again. A board that gets
nothing from the server for 6 seconds drops the socket and connects again.

On the WebSocket the board sends the token, then `[7]` to ask for the board frames in place of the
JSON ones the pad page gets, then the heartbeats and the presses. The server answers `[7]` with the
mode byte. It then sends a board frame when a game starts, when it is asked for each move, when our
move is decided (our snake one step on, so the next press aims from where the head really is) and
when the game ends. Each frame is a binary message that the board writes to the UART as it is.

A server from before that request ignores it and sends JSON. The board throws that away, so the
Flipper shows only the connection. The board drops its socket if a message is over 15 kB (a turn
of a four-snake 11×11 game is about 2 kB of JSON). `[6]` asks for nothing at all.

```
0xB0           start
[0]     flags  bit0 game just started · bit1 game over · bit2 board over 25×25 (width and height 0)
               bit3 we won (ours is the only snake left) · bit4 we lost (ours is gone)
[1]     width  [2] height
[3, 4]  turn   14 bits, low 7 first
[5]     health
[6, 7]  length 14 bits
[8, 9]  hold   time left to press in 10 ms, 14 bits
[10 …]  cells  one nibble each, top row first, first cell in the high nibble:
               0 empty · 1 food · 2 hazard · 3 my body · 4 my head · 5 other body · 6 other head
then    s1 s2  over [0 …]: s1 = (s1 + byte) mod 128, s2 = (s2 + s1) mod 128
```

## Deploy

Pushing to `main` deploys to AWS (one EC2 instance, via SST) from `.github/workflows/deploy.yml`;
the arm64 image is built from `Dockerfile` and pushed to ECR by the deploy itself. The deploy log
ends with two addresses:

- `engine` – the instance's fixed IP, `http://<elastic ip>`. Register this one with Battlesnake;
  the engine calls it directly.
- `pad` – CloudFront, `https://….cloudfront.net`. Open `/pad#<token>` on this one. HTTPS is what
  lets the page keep the phone's screen awake.

Set the pad token once before the first deploy, with AWS credentials for the account, in the region
the deploy runs in:

```bash
AWS_REGION=us-west-2 npx sst secret set ControlToken <token> --stage prod
```

The deploy fails until the secret is set. To change the token, set it again and redeploy.

- The server keeps the game and pad state in memory, so it runs as exactly one container on one
  instance.
- A deploy that changes the server or the token replaces the instance, so the snake is offline for
  a minute or two and any game in progress is lost. The IP stays the same.
- CloudFront caches nothing and forwards every request to the instance over plain HTTP, so that
  last hop is not encrypted.
- The token is readable in the instance's user data by anyone with access to the AWS account.
- `/start`, `/move` and `/end` are open, as the Battlesnake API requires. Someone who knows the URL
  can start a game against the snake. While no game is in progress the pad follows it, and one
  that never asks for a move holds the pad for a minute.

## Limits

- The autopilot is a port of the original TypeScript heuristics. Like the original it treats
  hazards as walls and does not know about wrapped boards. It looks five moves ahead where the
  original effectively managed two, so it can choose differently.
- Boards larger than 50×50 are refused. The Flipper draws boards up to 25×25; on a larger one it
  says the board is too large to show, and the buttons still steer.
- The WiFi link trusts one certificate authority for `wss://`, Amazon Root CA 1, which is what
  CloudFront uses. A server behind another authority needs its root added in `wifi/wifi.ino`.
- `wifi.json` holds the hotspot passwords and the pad token as plain text on the Flipper's SD card,
  and in `flipper/wifi.json` on the computer it was written on.
- Port 80 on the Elastic IP serves `/pad` and `/ws` as well, over plain HTTP. Use the CloudFront
  address for anything that carries the token.
- The token should be random, such as one from `openssl rand -hex 16`.
