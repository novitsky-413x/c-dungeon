# Dungeon (C, terminal roguelite)

A tiny cross-platform terminal game written in C. Runs in PowerShell, bash, and zsh with ANSI colors. Features a 9x9 world composed of map files, shooting, destructible walls, score/lives, a scoreboard and minimap, and an optional lightweight multiplayer server with server-authoritative simulation.

## Features
- 9x9 world grid loaded from `maps/` at startup (`x0-y0.txt` … `x8-y8.txt`, 40x18 each)
- Tiles: `#` wall, `.` floor, `@` optional start marker, `X` restore lives (consumed), `W` goal, `S` global spawn
- Colors: player cyan, enemies red, walls bright white, floor dim, goal purple, life pickup yellow
- Shooting and destructible walls
  - Space fires in facing direction
  - Enemies: 2 hits to kill; Walls: 5 hits to break
- Lives/score and invincibility (singleplayer)
  - 3 lives; collide with enemy → -1 life and 3s invincibility (flicker)
  - Score +1 per enemy kill
  - On 0 lives: lives reset to 3, score reset to 0, respawn and keep playing
- World navigation
  - Move off edges to change maps; transition only if the exact entry cell in the next map is open
- HUD and UI
  - HP line directly under the main map
  - Scoreboard (left) and minimap (right) on the same row under HP
    - Scoreboard shows up to 16 connected players as colored `@` with 4-digit scores (e.g., 9999)
    - Minimap shows a 9x9 world overview with current map marked `X` and players as colored `@`
    - Ping shown in HUD during Multiplayer
  - Hints (controls) below scoreboard/minimap
  - Multiplayer loading screen: animated sparkles with centered “LOADING” shown until the client receives its first authoritative snapshot (with a brief minimum display)
- Multiplayer (optional)
  - Menu lets you choose Singleplayer or Multiplayer
  - In Multiplayer, enter `host[:port]` (default port 5555) to connect
  - Server-authoritative movement and combat; client renders authoritative state
  - Other players rendered as colored `@`; enemies and bullets from server rendered as overlays
  - Scoring (server-side):
    - +1 per enemy kill (to the shooter)
    - +10 per player kill (PvP, to the shooter)

## Cross-platform terminal robustness
- POSIX (Linux/macOS): renderer uses absolute cursor positioning with per-row clearing, resets scroll region per frame, disables autowrap in the alternate screen, and uses a robust `write(2)` loop with `EINTR/EAGAIN` handling plus `tcdrain()` to ensure complete frames.
- Windows: uses stdio (`fwrite` + `fflush`) with Virtual Terminal sequences enabled.
- Output is unbuffered (`setvbuf(stdout, NULL, _IONBF, 0)`) and initial frames are force-redrawn to fully paint the screen.

## Layout
```
C:\Projects\c\c-dungeon
├─ maps\                  # map files (40 cols x 18 rows)
│  ├─ x0-y0.txt … x8-y8.txt
├─ src\
│  ├─ game.c/.h           # core game/game loop helpers + rendering (map/HUD/scoreboard/minimap/loading)
│  ├─ input.c/.h          # cross-platform input
│  ├─ term.c/.h           # ANSI, cursor and raw mode helpers
│  ├─ timeutil.c/.h       # timing helpers
│  ├─ types.h             # shared types/consts
│  ├─ main.c              # entry; menu; client runtime (SP/MP loop)
│  ├─ mp.c/.h             # multiplayer shared state (client-side overlay/flags)
│  ├─ net.c/.h            # minimal socket helpers (cross-platform)
│  ├─ client_net.c/.h     # client networking (connect/send/poll, message parsing)
│  └─ server\
│     └─ server.c         # lightweight C server (multi-client state broadcast, scoring)
```

## Quickstart
- Build client and server (Linux/macOS):
  ```bash
  gcc src/*.c -o dungeon
  gcc src/server/server.c -o server
  ```
- Build client and server (Windows, MSYS2/MinGW):
  ```bash
  gcc src\*.c -o dungeon.exe -lws2_32
  gcc src\server\server.c -o server.exe -lws2_32
  ```
- Run singleplayer:
  ```bash
  ./dungeon
  ```
- Run multiplayer (two terminals):
  ```bash
  ./server 5555
  ./dungeon  # choose Multiplayer, then enter 127.0.0.1:5555
  ```

## Build
You need a C compiler (GCC/Clang) and a terminal that supports ANSI (PowerShell 5+ or Windows Terminal; most Unix shells do).

### Windows (MSYS2/MinGW or similar)
- Client:
  ```bash
  gcc src\*.c -o dungeon.exe -lws2_32
  ```
- Server:
  ```bash
  gcc src\server\server.c -o server.exe -lws2_32
  ```

If you see a warning about including `winsock2.h` before `windows.h`, the project already handles the order in `main.c` and `net.h`.

### Linux/macOS
- Client:
  ```bash
  gcc src/*.c -o dungeon
  ```
- Server:
  ```bash
  gcc src/server/server.c -o server
  ```

### Compatibility and terminal notes
- Apple Terminal and zsh are supported. The game enters the alternate screen, disables autowrap, clears and redraws from the absolute origin each frame.
- If you ever see a partially drawn frame:
  - Make sure your window is at least 40 columns × 18 rows (HP/HUD needs a bit more; larger is fine).
  - Avoid running inside tmux/multiplexers while debugging terminal issues.
  - In Apple Terminal Preferences → Profiles → Advanced, keep “Allow VT100 application keypad mode” enabled.
  - Resize the window slightly (forces the terminal to refresh its state) and rerun.

## Run
### Singleplayer
- Windows:
  ```bash
  .\dungeon.exe
  ```
- Linux/macOS:
  ```bash
  ./dungeon
  ```

Choose “Singleplayer” at the menu.

### Multiplayer
1) Start the server (from repo root or `src/server/`):
- Windows:
  ```bash
  .\server.exe 5555
  ```
- Linux/macOS:
  ```bash
  ./server 5555 5556   # second arg enables native WebSocket on 5556 (ws)
  ```

The server searches for `maps/` relative to its working directory (`./maps/`, then `../maps/`, then `../../maps/`). Running from the repo root is simplest.

2) Web client: open `webclient.html` (defaults to `wss://runcode.at/ws`; change to `ws://127.0.0.1:5556/ws` when running the local server).
   Native client: choose “Multiplayer”, enter `host[:port]` (default 5555), e.g. `127.0.0.1:5555`.

While connecting and awaiting the first authoritative snapshot, the client displays an animated loading screen. The console client now ensures a brief minimum display so the animation is visible even on fast servers.

## Controls
- Move: WASD or Arrow keys
- Shoot: Space
- Build: B (place a wall in the facing direction)
- Quit: Q

Mobile web:
- On phones/tablets the web client shows on-screen controls automatically (D-pad + Shoot).
- The canvas scales to the viewport on small screens; desktop layout remains unchanged.

## Map format
- Each file is 18 lines x 40 characters wide.
- Valid chars: `# . @ X W S`
  - `S` global spawn: preferred singleplayer spawn anywhere in the world; spawn maps grant brief contact immunity
  - `@` start is only considered in `x0-y0.txt` (fallback if no `S` exists in singleplayer)
  - `X` restores lives to 3 and is consumed on pickup
  - `W` is the goal (ends game in singleplayer)

## Multiplayer protocol (text, line-based)
- Client → Server:
  - `HELLO` (sent once on connect; informational)
  - `INPUT dx dy shoot` where `dx`/`dy` in {-1,0,1}, `shoot` in {0,1}
  - `BYE` (disconnect request)
  - `PING token`
- Server → Client (snapshot each tick; lines may be interleaved):
  - `TICK n` (monotonic server tick counter to help clients align snapshots)
  - `YOU id` (assigned once upon connect)
  - `PLAYER id wx wy x y color active hp invincibleTicks superTicks score`
    - `active` is 0/1
    - `hp` is current lives (server-side in MP)
    - `score` is server-tracked; +1 per enemy kill, +10 per player kill
  - `BULLET wx wy x y active ownerId` for active remote bullets, includes shooter id
  - `ENEMY wx wy x y hp` for visible enemies (hp>0 means alive)
  - `TILE wx wy x y ch` to mutate a map tile (e.g., breaking a wall `#`→'.')
  - `ENTR wx wy bl br bu bd` entrance-block flags (0=open, 1=blocked) at central edges
  - `READY` after initial snapshot, signaling the client may start rendering gameplay
- Server → Client (refusal):
  - `FULL` when server is at capacity

Authoritative rules in MP:
- Movement and position are set by the server (client input is advisory).
- Enemies and bullets are simulated on the server; client only renders them.
- Wall destruction in MP is immediate on bullet impact and is propagated via `TILE` updates.

## Notes
- ANSI on Windows: enabled via Virtual Terminal Processing; PowerShell or Windows Terminal recommended.
- Performance: simple fixed timestep loop; CPU usage is low.
- Web client: input cadence ~100 ms; server-authoritative rendering (no client-side smoothing yet). Default WS endpoint is `wss://runcode.at/ws` and can be edited.
- Cross-platform: no external deps.
- Prebuilt binaries (`dungeon`, `server`, `.exe`) may be present in the repo root for convenience.

### Changelog (recent)
- Stabilized terminal rendering across macOS/Linux/Windows: absolute addressing, autowrap disabled in alt screen, robust POSIX write loop with drain, unbuffered stdout, per-frame scroll-region reset, warmup redraw frames.

## Roadmap
See `ROADMAP.md` for up-to-date status of completed items and future plans.

Enjoy exploring the dungeon!
