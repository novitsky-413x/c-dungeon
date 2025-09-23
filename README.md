# Dungeon (C, terminal roguelite)

A tiny cross-platform terminal game written in C. Runs in PowerShell, bash, and zsh with ANSI colors. Features a 3x3 world composed of map files, shooting, destructible walls, score/lives, and an optional lightweight multiplayer server.

## Features
- 3x3 world grid loaded from `maps/` at startup (`x0-y0.txt` … `x2-y2.txt`, 40x18 each)
- Tiles: `#` wall, `.` floor, `@` optional start marker, `X` restore lives (consumed), `W` goal
- Colors: player cyan, enemies red, walls bright white, floor dim, goal purple, life pickup yellow
- Shooting and destructible walls
  - Space fires in facing direction
  - Enemies: 2 hits to kill; Walls: 5 hits to break
- Lives/score and invincibility
  - 3 lives; collide with enemy → -1 life and 3s invincibility (flicker)
  - Score +1 per enemy kill
  - On 0 lives: lives reset to 3, score reset to 0, teleport to a random open tile on a different map, keep playing
- World navigation
  - Move off edges to change maps
  - You can only cross if the exact entry cell in the next map is open (no auto-adjust)
  - HUD shows global coordinates across the 3x3 world
- Multiplayer (optional)
  - Menu lets you choose Singleplayer or Multiplayer
  - In Multiplayer, enter `host[:port]` (default port 5555) to connect
  - Other players are rendered as colored `@` on your current map
  - Server-authoritative movement; client disables local movement/enemy/bullet simulation when connected
  - Server sends TILE updates for map changes (e.g., walls broken by bullets)
  - Server broadcasts enemies and bullets each tick; client renders them as an overlay for the current map

## Layout
```
C:\Projects\c\dungeon
├─ maps\                  # map files (40 cols x 18 rows)
│  ├─ x0-y0.txt … x2-y2.txt
├─ src\
│  ├─ game.c/.h           # core game/game loop helpers
│  ├─ input.c/.h          # cross-platform input
│  ├─ term.c/.h           # ANSI, cursor and raw mode helpers
│  ├─ timeutil.c/.h       # timing helpers
│  ├─ types.h             # shared types/consts
│  ├─ main.c              # entry; menu; client runtime
│  ├─ mp.c/.h             # multiplayer shared state (client-side overlay)
│  ├─ net.c/.h            # minimal socket helpers (cross-platform)
│  ├─ client_net.c/.h     # client networking (connect/send/poll)
│  └─ server\
│     └─ server.c         # lightweight C server (multi-client state broadcast)
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
  ./server 5555
  ```

The server searches for `maps/` relative to its working directory (`./maps/`, then `../maps/`, then `../../maps/`). Running from the repo root is simplest.

2) Start the client and choose “Multiplayer”, then enter the server `host[:port]` (default port 5555). Example: `127.0.0.1:5555`.

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
  - `PLAYER id wx wy x y color active` for up to 16 players
  - `BULLET wx wy x y active` for active remote bullets
  - `ENEMY wx wy x y hp` for visible enemies (hp>0 means alive)
  - `TILE wx wy x y ch` to mutate a map tile (e.g., breaking a wall `#`→`.`)
- Server → Client (refusal):
  - `FULL` when server is at capacity

Authoritative rules in MP:
- Movement and position are set by the server (client input is advisory).
- Enemies and bullets are simulated on the server; client only renders them.
- Wall destruction in MP is immediate on bullet impact and is propagated via `TILE` updates.

## Notes
- ANSI on Windows: enabled via Virtual Terminal Processing; PowerShell or Windows Terminal recommended.
- Performance: simple fixed timestep loop; CPU usage is low.
- Cross-platform: no external deps.
- Prebuilt binaries (`dungeon`, `server`, `.exe`) may be present in the repo root for convenience.

## Roadmap
- Short-term
  - Server-enforced player health/damage and respawn rules in MP
  - Make wall durability consistent in MP (track damage counts; not immediate break)
  - Connection quality: basic rate limiting, idle pings, and disconnect reasons
- Mid-term
  - Latency smoothing: simple client-side interpolation/extrapolation of remote players/bullets
  - Configurable world size and dynamic map loading
  - Quality-of-life: Makefile/CMake, Dockerized server, CI build
- Long-term
  - Persistence (high scores, per-user stats)
  - Optional chat and emotes
  - Cross-platform packaging (static builds where feasible)

Enjoy exploring the dungeon!
