# Reaction Arcade Prototype Server

This serves the browser-game prototype, keeps the current session score, and relays WebSocket messages between the browser and ESP32.

## Setup

```powershell
cd pc-server
npm install
```

## Run

```powershell
npm start
```

Open the browser game:

```text
http://localhost:8080
```

The ESP32 WebSocket client should connect to:

```text
ws://0.0.0.0:8080
```

Set `REACTION_WEBSOCKET_URI` in `main/network_config.h` to your PC's LAN IP address, for example:

```c
#define REACTION_WEBSOCKET_URI "ws://YOUR_PC_IP:8080"
```

## Manual Commands

Type one of these commands into the server terminal for quick testing:

```text
start
miss
wrong
over
state
clients
quit
```

During normal play:

- The browser sends `GAME_START` when the player starts.
- The browser sends `COLLISION` only when the current target hits the bottom.
- The ESP32 sends `RFID_SCAN` when the RC522 reads a card.
- The server judges success/failure with a 900 ms reaction window using its own clock.
- Successful scans are classified as `PERFECT` from 0-200 ms, `GREAT` from 201-500 ms, or `GOOD` from 501-900 ms.
- The server sends `ATTEMPT_SUCCESS` with the reaction time/rating or `ATTEMPT_FAILED`.
- A success increments `successStreak` and resets `failureStreak`.
- A `MISS` or `WRONG_SCAN` increments `failureStreak` and resets `successStreak`.
- The server sends `GAME_WIN` after 5 successful target hits in a row.
- The server sends `GAME_OVER` after 5 failures in a row or when the session expires.
