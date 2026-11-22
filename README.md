# battlesnake-sk

A [Battlesnake](https://play.battlesnake.com) you steer in real time from a Flipper Zero, with an
autopilot behind you.

```
Flipper Zero ──UART──▶ Wi-Fi Devboard ──phone hotspot──▶ server ◀──/move── Battlesnake engine
 Battlesnake app        wifi/ firmware    WebSocket
```

- **`flipper/`** – the Battlesnake app. A Wi-Fi Devboard on the Flipper joins the phone's hotspot and
  holds the WebSocket itself, so the phone is only the path to the internet. The Flipper's buttons
  steer and its screen shows the connection, not the board.
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

**2. Write the settings file.** With qFlipper's file manager or the Flipper mobile app, create
`SD Card/apps_data/snake_pad/wifi.txt`:

```
Hotspot: <the hotspot's name>
Password: <the hotspot's password>
Server: wss://<the pad address from the deploy log, without https://>
Token: <CONTROL_TOKEN>
```

For more than one network, such as home and work, list a `Hotspot` line and its `Password` line
for each, each password right after its hotspot. The app tries them in the order listed, moving
to the next whenever the board reports the current one not found and back to the first after the
last, so a network further down the list takes about 8 seconds longer per network above it.

For a server on your own computer, use `Server: ws://<its address on the hotspot>:8000`. The app
reads the file each time the board asks for it and the board keeps the settings only in memory, so
neither the app nor the firmware contains them.

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
| `No settings file`, `Settings file incomplete` | check `apps_data/snake_pad/wifi.txt` and its four keys |
| `Joining hotspot` | the line below names it |
| `Hotspot not found` | the hotspot named below is off, out of range or on 5 GHz only; the next one in the file gets its turn |
| `Hotspot refused password` | for the hotspot named below |
| `Reaching server` | on the hotspot, but the server has not answered: wrong address, no mobile data or the server is down |
| `Server refused token` | |
| `Connected` | with the ping: the round trip Flipper → server → Flipper, measured every 2 s |

Start a game and steer. Holding Back exits and tells the board to drop the hotspot.

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

The board sends its state when it changes and in answer to each heartbeat, and is silent
otherwise. Any other byte from it is the ESP32's boot text, which the Flipper ignores. A board
that gets no heartbeat from the Flipper for 6 seconds closes its socket and restarts, so a Flipper
that goes away cannot leave a pad connected and the autopilot takes over again. A board that gets
nothing from the server for 6 seconds drops the socket and connects again.

On the WebSocket the board sends the token, then `[6]` to ask the server not to send it the board
frames, then the heartbeats and the presses. A server from before that request ignores it; the
board then reads the frames and throws them away, and drops its socket if one is over 15 kB (a
turn of a four-snake 11×11 game is about 2 kB).

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
  can start a game against the snake, and the pad follows the most recently started game.

## Limits

- The autopilot is a port of the original TypeScript heuristics. Like the original it treats
  hazards as walls and does not know about wrapped boards. It looks five moves ahead where the
  original effectively managed two, so it can choose differently.
- Boards larger than 50×50 are refused.
- The WiFi link trusts one certificate authority for `wss://`, Amazon Root CA 1, which is what
  CloudFront uses. A server behind another authority needs its root added in `wifi/wifi.ino`.
- `wifi.txt` holds the hotspot password and the pad token as plain text on the Flipper's SD card.
