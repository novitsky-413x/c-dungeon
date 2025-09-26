# Dungeon (C, terminal roguelite)

A tiny cross-platform terminal game written in C. Runs in PowerShell, bash, and zsh with ANSI colors. Features a 9x9 world composed of map files, shooting, destructible walls, score/lives, a scoreboard and minimap, and an optional lightweight multiplayer server with server-authoritative simulation.

## Features
- 9x9 world grid loaded from `maps/` at startup (`x0-y0.txt` … `x8-y8.txt`, 40x18 each)
- Tiles: `#` wall, `.` floor, `@` optional start marker, `X` restore lives (consumed), `W` goal
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
  - Hints (controls) below scoreboard/minimap
  - Multiplayer loading screen: animated sparkles with centered “LOADING” shown until the client receives its first authoritative snapshot
- Multiplayer (optional)
  - Menu lets you choose Singleplayer or Multiplayer
  - In Multiplayer, enter `host[:port]` (default port 5555) to connect
  - Server-authoritative movement and combat; client renders authoritative state
  - Other players rendered as colored `@`; enemies and bullets from server rendered as overlays
  - Scoring (server-side):
    - +1 per enemy kill (to the shooter)
    - +10 per player kill (PvP, to the shooter)

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

2) Web client: open `webclient.html` (defaults to `ws://127.0.0.1:5556/ws`, editable in the page).
   Native client: choose “Multiplayer”, enter `host[:port]` (default 5555), e.g. `127.0.0.1:5555`.

While connecting and awaiting the first authoritative snapshot, the client displays an animated loading screen. The console client now ensures a brief minimum display so the animation is visible even on fast servers.

## Controls
- Move: WASD or Arrow keys
- Shoot: Space
- Quit: Q

## Map format
- Each file is 18 lines x 40 characters wide.
- Valid chars: `# . @ X W`
  - `@` start is only considered in `x0-y0.txt` (client spawns there; server randomizes spawns for clients)
  - `X` restores lives to 3 and is consumed on pickup
  - `W` is the goal (ends game in singleplayer)

## Multiplayer protocol (text, line-based)
- Client → Server:
  - `HELLO` (sent once on connect; informational)
  - `INPUT dx dy shoot` where `dx`/`dy` in {-1,0,1}, `shoot` in {0,1}
  - `BYE` (disconnect request)
- Server → Client (snapshot each tick; lines may be interleaved):
  - `YOU id` (assigned once upon connect)
  - `PLAYER id wx wy x y color active hp invincibleTicks superTicks score`
    - `active` is 0/1
    - `hp` is current lives (server-side in MP)
    - `score` is server-tracked; +1 per enemy kill, +10 per player kill
  - `BULLET wx wy x y active` for active remote bullets
  - `ENEMY wx wy x y hp` for visible enemies (hp>0 means alive)
  - `TILE wx wy x y ch` to mutate a map tile (e.g., breaking a wall `#`→'.')
- Server → Client (refusal):
  - `FULL` when server is at capacity

Authoritative rules in MP:
- Movement and position are set by the server (client input is advisory).
- Enemies and bullets are simulated on the server; client only renders them.
- Wall destruction in MP is immediate on bullet impact and is propagated via `TILE` updates.

## Notes
- ANSI on Windows: enabled via Virtual Terminal Processing; PowerShell or Windows Terminal recommended.
- Performance: simple fixed timestep loop; CPU usage is low.
- Web client: input cadence reduced (50 ms) and light smoothing/interpolation added for players, bullets, and enemies to match console feel; default WS endpoint is localhost.
- Cross-platform: no external deps.
- Prebuilt binaries (`dungeon`, `server`, `.exe`) may be present in the repo root for convenience.

## Roadmap
### Short-term
- Health/score UI polish (icons, color tweaks)
- Server rate limiting and lightweight anti-spam for INPUT
- Basic config flags (enemy count, world size, port)

### Mid-term
- Latency smoothing: simple interpolation/extrapolation of remote players/bullets
- Configurable/dynamic world size and streaming map I/O
- Makefile/CMake and CI; Dockerized server
- Optional PvE/PvP toggles and friendly fire control

### Long-term
- Persistence: high scores and per-user stats (files or SQLite)
- Matchmaking/lobbies and optional spectator mode
- Chat/emotes and simple cosmetics (color themes)
- Cross-platform packaging (static builds where feasible)

Enjoy exploring the dungeon!
