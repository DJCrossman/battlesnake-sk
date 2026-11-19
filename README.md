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

There is a third link, **WiFi**, that needs no page and no Bluetooth. A Wi-Fi Devboard on the
Flipper joins the phone's hotspot and holds the WebSocket itself, so the phone is only the path to
the internet. The Flipper's buttons steer and its screen shows the connection, not the board.

```
Flipper Zero ──UART──▶ Wi-Fi Devboard ──phone hotspot──▶ server ◀──/move── Battlesnake engine
 Snake Pad app          wifi/ firmware    WebSocket
```

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

For a server on your own computer, use `Server: ws://<its address on the hotspot>:8000`. The app
reads the file each time the board asks for it and the board keeps the settings only in memory, so
neither the app nor the firmware contains them.

The hotspot's name has to match exactly. An iPhone's default name has a curly apostrophe
(`David’s iPhone`), which is easy to get wrong; renaming the phone in Settings → General → About
avoids it.

**3. Turn the hotspot on, on 2.4 GHz.** On an iPhone that is Settings → Personal Hotspot →
Maximize Compatibility. If the Flipper says the hotspot is not found, keep that settings screen
open while the board joins. On Android, choose the 2.4 GHz band in the hotspot's settings.

**4. Plug the board into the Flipper, start Snake Pad and choose WiFi.** The LED blinks blue until
the board is connected and is green after that. The screen says where the connection stands:

| Screen | Meaning |
| --- | --- |
| `Board not answering` | the board is not plugged in or not flashed. It also shows for a few seconds at a time while the board is tied up reaching a server that does not answer |
| `No settings file`, `Settings file incomplete` | check `apps_data/snake_pad/wifi.txt` and its four keys |
| `Joining hotspot` | |
| `Hotspot not found` | the hotspot is off, out of range or on 5 GHz only |
| `Hotspot refused password` | |
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

### Play with the board on the Flipper (Board link)

1. Start Snake Pad on the Flipper and choose **Board**. It advertises as
   `Board <your Flipper's name>`.
2. In Chrome on Android, open `https://<your server>/pad#<CONTROL_TOKEN>` and tap **Connect Flipper
   screen**. Pick the Flipper from the list. There is no pairing: no PIN and no prompt.
3. Start a game. The Flipper buzzes and shows the board; the bar at the bottom right is the time
   left to press.

The button only appears where the browser offers Web Bluetooth, which Chrome does on HTTPS pages and
on `localhost`. The Keyboard link pairs separately from the Flipper mobile app, and the Board link
does not pair at all; the mobile app's pairing is untouched.

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

The page and the Flipper talk over Snake Pad's own Bluetooth service,
`4b514e43-fc33-4934-b8e7-4a26f71991ee`. None of its characteristics needs encryption, so nothing
pairs:

| Characteristic | Direction | |
| --- | --- | --- |
| `324ba1da-cf74-46e4-8654-b49ad6b8f401` | page → Flipper | frame bytes, written in chunks of at most 20 |
| `324ba1da-cf74-46e4-8654-b49ad6b8f402` | Flipper → page | button bytes, indicated |
| `324ba1da-cf74-46e4-8654-b49ad6b8f403` | Flipper → page | flow-control credit, read and notified |

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

The flow-control characteristic is a big-endian uint32, the 1024 bytes the Flipper will take. The
page never writes more than that, and once it has spent all of it waits for the Flipper to notify
the value again, which it does when it has read everything.

The Flipper shows "Connected" from the page's first subscribe or write, because without pairing it
is not told about the connection itself.

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
- The WiFi link trusts one certificate authority for `wss://`, Amazon Root CA 1, which is what
  CloudFront uses. A server behind another authority needs its root added in `wifi/wifi.ino`.
- `wifi.txt` holds the hotspot password and the pad token as plain text on the Flipper's SD card.
- The Board link is unauthenticated. While Board mode is open, anyone in Bluetooth range can connect
  to the Flipper instead of your phone. They could read your button presses and draw on the
  Flipper's screen, and nothing else.
