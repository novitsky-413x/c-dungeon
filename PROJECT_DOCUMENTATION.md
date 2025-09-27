## Project Documentation: Dungeon (C, terminal roguelite) with Multiplayer Server

This document explains the entire project, module by module, with an in-depth walkthrough of the server. For complex concepts, links to official documentation are provided.

### Repository Layout

```
/maps/                Map files (40x18 each): x0-y0.txt … x8-y8.txt
/src/
  client_net.c/.h    Client networking: connect, send input, parse server messages
  game.c/.h          Game world, rendering, HUD, SP logic, MP overlays
  input.c/.h         Cross-platform non-blocking keyboard input
  main.c             Entry point: menu, game loop, SP/MP modes
  mp.c/.h            Multiplayer shared state (client-side, for rendering overlays)
  net.c/.h           Minimal cross-platform socket utilities
  term.c/.h          Terminal utilities: ANSI, alt screen, raw mode
  timeutil.c/.h      Timing utility
  types.h            Shared constants and types
  server/server.c    Standalone multiplayer server (authoritative state, TCP + WebSocket)
README.md            Quickstart and feature overview
ROADMAP.md           Future work and status
webclient.html       Browser client using the same text protocol over WebSocket
```

---

## Types and Global Configuration (`src/types.h`)

- Defines map dimensions, world entity limits, and shared structs used by client and server.
- Key constants: `MAP_WIDTH=40`, `MAP_HEIGHT=18`, `WORLD_W=9`, `WORLD_H=9` (on server and client game).
- Important structs:
  - `Vec2 { int x, y; }`
  - `Enemy { int isAlive, hp; Vec2 pos; }` (client-side singleplayer)
  - `Projectile { active, pos, dir }` (client-side singleplayer)
  - `RemotePlayer`, `RemoteBullet`, `RemoteEnemy`: client-side mirrors for server snapshots, including last-known position for interpolation/extrapolation.

References:
- C enums and structs: `https://en.cppreference.com/w/c/language` (select C topics for structs/enums)

---

## Terminal Utilities (`src/term.h`, `src/term.c`)

- Enables ANSI sequences, toggles cursor visibility, clears screen, enters/exits alternate buffer, and configures raw input mode (POSIX only).
- Alt screen: autowrap is disabled on entry and restored on exit; scroll region is reset each frame by the renderer to avoid residual terminal state.
- A helper `term_get_size(int* rows, int* cols)` queries the current terminal size (Windows console or POSIX via `ioctl(TIOCGWINSZ)`).
- Windows: uses `SetConsoleMode` with `ENABLE_VIRTUAL_TERMINAL_PROCESSING`.
- POSIX: uses `termios` to disable canonical mode and echo, and `fcntl` to set `O_NONBLOCK`.

References:
- termios: `https://man7.org/linux/man-pages/man3/termios.3.html`
- Windows console virtual terminal: `https://learn.microsoft.com/windows/console/console-virtual-terminal-sequences`

---

## Input Utilities (`src/input.h`, `src/input.c`)

- Cross-platform, non-blocking key read mapping arrow keys to WASD and returning single char codes.
- Windows: `_kbhit()`/`_getch()`; POSIX: reads from `STDIN`, decodes escape sequences for arrows.

References:
- `_kbhit`, `_getch`: `https://learn.microsoft.com/cpp/c-runtime-library/reference/kbhit` and `getch`

---

## Time Utilities (`src/timeutil.h`, `src/timeutil.c`)

- `now_ms()`: returns a monotonic time in milliseconds.
- Windows: `QueryPerformanceCounter`; POSIX: `clock_gettime(CLOCK_MONOTONIC, ...)`.

References:
- `clock_gettime`: `https://man7.org/linux/man-pages/man2/clock_gettime.2.html`

---

## Networking Utilities (`src/net.h`, `src/net.c`)

- Cross-platform wrappers for sockets: initialization, TCP options, connect, send-all loop, non-blocking recv.
- `net_connect_hostport` uses `getaddrinfo` and tries each address until `connect` succeeds.
- `net_set_tcp_nodelay_keepalive` configures `TCP_NODELAY` and `SO_KEEPALIVE`.

References:
- Berkeley sockets: `https://beej.us/guide/bgnet/`
- `getaddrinfo`: `https://man7.org/linux/man-pages/man3/getaddrinfo.3.html`
- Nagle’s algorithm / TCP_NODELAY: `https://en.wikipedia.org/wiki/Nagle%27s_algorithm`

---

## Multiplayer State (Client-side) (`src/mp.h`, `src/mp.c`)

- Globals that the client uses to render remote players, bullets, enemies, and track MP status.
- `g_mp_active`: whether multiplayer is active.
- `g_mp_joined`: set after first authoritative snapshot for self.
- `g_net_ping_ms`: smoothed ping reported in HUD.

---

## Client Networking (`src/client_net.h`, `src/client_net.c`)

Purpose: Connect to server, send input, poll and parse line-based protocol messages, update `mp` state and `game` tiles.

Key functions:
- `client_connect(addr_input)`: parses `host[:port]`, normalizes `localhost` to IPv4, connects, sets non-blocking and TCP options, sends `HELLO`.
- `client_send_input(dx,dy,shoot)`: sends `INPUT dx dy shoot`.
- `client_poll_messages()`: periodic ping, non-blocking recv, maintain a rolling line buffer, parse lines and update remote players/bullets/enemies, apply `TILE` updates via `game_mp_set_tile` and set self position via `game_mp_set_self`. Returns 1 if a redraw is warranted.
- `client_send_bye()`: send `BYE` before disconnect.

Protocol lines handled:
- `YOU id`, `PLAYER ...`, `BULLET ...`, `ENEMY ...`, `TILE ...`, `PONG token`, `FULL`.

References:
- Text protocols and line parsing tips: `https://www.rfc-editor.org/rfc/rfc5234` (ABNF basics)

Detailed function explanations:
- parse_host_port(const char* in, char* host, size_t hostcap, char* port, size_t portcap)
  - Splits `host[:port]`; defaults port to `5555`; normalizes `localhost` to `127.0.0.1` so it matches the server’s default IPv4 bind.

- client_connect(const char* addr_input) → int
  - Initializes sockets (`net_init`), parses host/port, connects (`net_connect_hostport`), sets non-blocking and TCP options, sends `HELLO`, returns 0 on success.

- client_disconnect(void)
  - Closes socket and cleans up networking state.

- client_send_input(int dx, int dy, int shoot)
  - Sends one line `INPUT dx dy shoot` to the server.

- client_poll_messages(void) → int
  - Sends `PING` at 1 Hz with current `now_ms()`; non-blocking `recv` into temp buffer; appends into rolling `g_recv_buf` with overflow handling.
  - Processes complete lines; for each line:
    - `YOU`: set `g_my_player_id`; marks changed.
    - `PLAYER`: updates `g_remote_players[id]`, saving last position for smoothing; if it’s self, calls `game_mp_set_self` and sets `g_mp_joined=1`.
    - `TILE`: updates map via `game_mp_set_tile`.
    - `BULLET`: finds or allocates a slot in `g_remote_bullets`, preserves last position to support smoothing.
    - `ENEMY`: stores in `g_remote_enemies` with hp and position.
    - `PONG`: computes RTT and applies exponential smoothing to `g_net_ping_ms`.
  - Returns 1 if any snapshot content applied (should redraw).

- client_send_bye(void)
  - Sends `BYE` prior to disconnect.

---

## Game Core (`src/game.c`, `src/game.h`)

Responsibilities:
- World and map loading from `/maps` files with enforced inter-map door connectivity and global spawn `S`.
- Singleplayer: player lives/score, enemies, projectiles, wall damage, win condition `W`.
- Multiplayer: disable local AI/projectiles; render server state via overlays; smoothing for remote players/bullets.
- Rendering: map grid, players/enemies/bullets, HUD (HP, Ping), scoreboard, minimap, loading animation for MP.
  - Rendering details (terminal stability):
    - Absolute cursor addressing for each row (`ESC[y;1H`) with per-row clear (`ESC[K`), avoiding reliance on newlines.
    - Scroll region reset (`ESC[r]`) and full clear at frame start.
    - POSIX: robust `write()` loop with `EINTR/EAGAIN` handling and `tcdrain()` to ensure frames fully reach the terminal; Windows: `fwrite`+`fflush`.
    - Stdout is unbuffered and a few initial warmup frames are forced to paint the screen completely.
    - HUD/minimap adapts to terminal width; minimap hides if there isn’t enough space.

Important functions:
- `game_init`, `world_init`, `load_map_file`.
- `game_attempt_move_player`, `try_enter_map`: preserve non-crossing axis when changing maps.
- `game_spawn_enemies`, `game_move_enemies`, `game_update_projectiles`, `game_tick_status`.
- MP helpers: `game_mp_set_tile`, `game_mp_set_self`, and getters for current world tile.

Detailed function explanations (selected):
- load_map_file(int mx, int my)
  - Loads `maps/x{mx}-y{my}.txt`; if missing, constructs a bordered map; sanitizes characters; enforces inter-map doors and ensures a central spawn `S` when at the world center and none exists.

- world_init(void)
  - Loads all maps; in singleplayer, finds global `S` if present and uses it for initial position; otherwise uses `@` in the starting map or defaults to (1,1). In MP, position is placeholder until server snapshot arrives.

- game_attempt_move_player(int dx, int dy) → int
  - In SP only (no local movement in MP): sets facing, attempts move if open; if stepping beyond bounds, tries entering the neighboring map while preserving the non-crossing axis.

- game_player_shoot(void)
  - In SP only: spawns a projectile in facing direction, enforcing a cooldown unless `super_frames` is active.

- game_update_projectiles(void) → int
  - Steps local projectiles; resolves enemy hits (hp decrement; +score), wall damage with break after 5 hits, and deactivation if blocked.

- game_draw(void)
  - Renders the map grid with ANSI colors, overlays local or remote entities depending on SP/MP, draws HUD with HP and Ping, a 4x4 scoreboard (up to 16 players), and a 9x9 minimap. Performs simple interpolation/extrapolation for MP smoothing.

- game_draw_loading(int tick)
  - Renders a dim dot background, sparkles based on a deterministic per-tick RNG, and centered "LOADING" text while waiting for the first map/snapshot in MP.

---

## Program Entry and Loop (`src/main.c`)

Flow:
1) Initialize terminal (ANSI, alt screen, hide cursor, raw mode on POSIX).
2) Menu: choose Singleplayer or Multiplayer.
3) If MP: prompt for server `host[:port]`; connect via `client_connect`; set `g_mp_active` on success.
4) Initialize game; in loop: read input, if MP then `client_poll_messages`, show loading until joined, update/render at ~60 FPS with sleep to cap frame time.
5) On quit: restore terminal; print outcome message.

Input mapping:
- WASD/Arrows move; Space shoots; Q quits. In MP, inputs are sent to server; local movement/projectiles are disabled.

---

## Web Client (`webclient.html`)

- Canvas-based renderer mirroring console visuals and the same text protocol over WebSocket.
- Sends `INPUT` on a fixed cadence (~100 ms), pings every second with tokens for RTT, displays HUD with HP and Ping, shows a loading overlay until the first full map is received.
- Mobile support: detects coarse-pointer devices and shows a touch D-pad and Shoot button; inputs are merged with keyboard state. Canvas scales responsively on small screens without affecting desktop layout.

References:
- WebSocket API: `https://developer.mozilla.org/docs/Web/API/WebSocket`
- Canvas 2D API: `https://developer.mozilla.org/docs/Web/API/CanvasRenderingContext2D`

---

## Server: Authoritative Multiplayer (`src/server/server.c`)

The server simulates a 9x9 world, accepts TCP and WebSocket clients, broadcasts snapshots every tick (~50 ms), and applies game rules authoritatively: movement, bullets, enemies, scoring, wall destruction, pickups, timers.

High-level architecture:
- Sockets: two listening sockets — TCP on `port` (default 5555) and WebSocket on `wsport` (default 5556).
- Event loop: `select()` with 50 ms timeout drives the server tick. Each tick:
  1) Accept new TCP and WS clients.
  2) Read data from client sockets.
  3) Parse `HELLO` (ignored), `PING`, `INPUT dx dy shoot`, `BYE`, and perform WS handshake if needed.
  4) Step bullets/enemies at lower frequencies, apply enemy contact damage, handle pickups, tick timers/refill tokens.
  5) Broadcast state (`TICK`, `PLAYER`, `BULLET`, `ENEMY`) and on tile changes send `TILE` lines.

Key data structures:
- `Map world[WORLD_H][WORLD_W]`: `tiles[18][41]` (+1 for NUL) and `wallDmg[18][40]` per map.
- `Client clients[MAX_CLIENTS]`: connection info, position (`worldX/Y` + `pos`), color, facing, hp, status timers (invincible/super/shootCooldown), score, address/port, connection id, and a simple leaky-bucket rate limiter for inputs.
- `SrvBullet bullets[MAX_REMOTE_BULLETS]`: active bullets with world, position, direction, and owner id for scoring.
- `SrvEnemy enemies[WORLD_H][WORLD_W][MAX_ENEMIES]`: per-map enemies with hp and position; only simulated when the map has active players.

Line-by-line walkthrough of major functions and logic:

Initialization and Helpers:
- Socket typedefs and includes are guarded for Windows vs POSIX; `sock_t` is either `SOCKET` or `int`.
- `Map`, `SrvBullet`, `SrvEnemy`, `Client` are defined with fields used throughout the loop.
- `ws_count_active_for_ip` and `ws_rate_allow` enforce basic per-IP concurrent connection and rate limits for WebSocket upgrades.
- Minimal `base64_encode` and `sha1` support WebSocket handshake per RFC 6455.
  - WS Accept: `Sec-WebSocket-Accept = base64( SHA1( key + GUID ) )`.
  - References: RFC 6455 Handshake `https://datatracker.ietf.org/doc/html/rfc6455#section-4.2.2`, SHA-1 `https://www.rfc-editor.org/rfc/rfc3174`.
- Map loading via `load_map_file(mx,my)` searches `./maps/`, then `../`, then `../../`. If not found, creates an all-`.` map, ensuring door connectivity and a central `S` at world center.
- `spawn_enemies_for_map`: spawns up to `MAX_ENEMIES` on open tiles, skipping maps that contain `S`.
- `place_near_spawn`: finds a nearest open tile near a global spawn `S` and avoids already-occupied cells by connected players.

WebSocket helpers:
- `ws_handshake(Client *c)`: Parses HTTP headers in `c->wsBuf`, extracts `Sec-WebSocket-Key` (case-insensitive parsing), computes `Sec-WebSocket-Accept`, sends 101 Switching Protocols, and marks `wsHandshakeDone`.
- `ws_send_text_frame(sock, data, len)`: Sends a server->client unmasked text frame per RFC 6455. Lengths <126, 16-bit, or 64-bit are handled.

Broadcast and snapshots:
- `send_text_to_client(idx,data,len)`: abstracts TCP vs WS framing.
- `send_full_map_to(clientIdx)`: sends every `TILE wx wy x y ch` for all maps — used for TCP clients on connect and for WS clients after handshake in some paths.
- `send_map_to(clientIdx, wx, wy)`: sends `TILE` lines for a single map — used after a player transitions to a new map and for WS clients immediately after joining.
- `broadcast_state()`: Builds a single buffer per tick including `TICK n`, a `PLAYER` line for each slot, `BULLET` lines for active bullets, and `ENEMY` lines for active enemies only on maps with players. Sends to all connected clients (WS uses a single framed message per tick).

Simulation steps:
- `step_bullets()`: Moves bullets one tile along their direction on a subrate (~10 steps/sec).
  - If a bullet hits an enemy, decrements hp; when hp <= 0, deactivates the enemy and awards +1 score to bullet owner.
  - PvP: if a bullet hits a player (and the map is not a spawn map), applies damage with invincibility frames; on death, awards +10 score to shooter, respawns victim near spawn with reset timers.
  - If a bullet hits a wall (`#`), increments `wallDmg`; after 5th hit (0..4 then break), changes tile to `.` and broadcasts a `TILE` update.
- `step_enemies()`: For maps with active players only, randomly moves enemies one step if the target tile is open and unoccupied by another enemy. Runs ~6–7 steps/sec.
- `apply_enemy_contact_damage()`: For each connected player not on a spawn map, if standing on an enemy, applies damage with invincibility frames; on death, respawns near spawn and resets status.

Main entry `main(argc, argv)`:
1) Seed RNG, initialize Windows Sockets if needed.
2) Ports: `port` (TCP, default "5555") and `wsport` (WebSocket, default "5556").
3) Load all maps and spawn enemies.
4) Create, bind, and listen on two sockets (TCP and WS). Set `SO_REUSEADDR` and for accepted sockets set `TCP_NODELAY` and `SO_KEEPALIVE`.
   - References: `bind`, `listen`, `accept`, `setsockopt`: Beej’s Guide `https://beej.us/guide/bgnet/`.
5) Event loop (forever):
   - Build `fd_set` with listening sockets and all connected client sockets; `select` with 50 ms timeout.
   - Accept TCP connections: allocate a `Client` slot, initialize state, record peer address via `getnameinfo`, send `YOU id`, send an immediate state frame, and send a full map snapshot. If full: reply `FULL` and close.
   - Accept WS connections: enforce per-IP and per-window limits; allocate a slot; synchronously read request headers with a short timeout; perform WS handshake; initialize player state; send `YOU id`, an immediate state frame, and then a current-map snapshot (`send_map_to`). If the handshake fails, close the socket.
   - Read from client sockets:
     - For WS clients with pending handshake: accumulate headers and attempt handshake.
     - For WS framed data: deframe masked text payloads (FIN+TEXT only, single-frame) and store into `buf` as plain text.
     - Iterate over newline-delimited commands:
       - `BYE`: disconnect the client.
       - `PING t`: reply `PONG t` (client uses RTT).
       - `INPUT dx dy shoot`: rate-limited by a leaky bucket; update facing, attempt movement across maps preserving axis, prevent stepping into other players; after world transition, send them a state frame and `send_map_to` for the new map; if `shoot` is 1 and allowed by cooldown or super, spawn a bullet in facing or inferred direction.
   - Inactivity timeout (3 minutes): disconnect idle clients.
   - Periodic steps: `step_bullets`, `step_enemies`, `apply_enemy_contact_damage`, handle pickups (`X` → restore hp=3, set super and invincibility, clear tile and broadcast), tick down timers and refill input tokens.
   - `broadcast_state()` and increment `g_tick_counter`.

Security and resilience notes:
- Input is line-based and simple; a small leaky-bucket per client avoids spamming `INPUT`.
- WebSocket code is minimal and should be used behind trusted frontends in production; it assumes well-behaved clients and simple frames.
- The server runs single-threaded; CPU usage is low due to small world and tick rate.

### Server: Function-by-function reference

This section enumerates each notable function and explains its responsibility and key logic. For compact helpers with obvious behavior, we summarize; for complex routines, we describe steps in sequence.

- ws_count_active_for_ip(const char* ip) → int
  - Counts currently connected WebSocket clients that match `ip` in `clients[]`.
  - Used to enforce `MAX_WS_PER_IP`.

- ws_rate_allow(const char* ip) → int
  - Implements a simple per-IP connection attempt window using a small fixed array `g_wsIpRates` of slots.
  - For matching IP: if the current time window expired, reset counter; if attempts exceed `WS_CONN_MAX_PER_WINDOW`, deny; else increment and allow.
  - If no existing slot and there is a free slot, initialize it and allow; if no slot is available, defaults to allow.
  - Reference: Token/leaky-bucket rate limiting concept `https://en.wikipedia.org/wiki/Leaky_bucket`.

- base64_encode(const uint8_t* in, int inlen, char* out, int outcap) → int
  - Minimal base64 encoder for the 20-byte SHA1 digest needed by the WS handshake.
  - Handles full 3-byte groups and tail lengths 1 or 2 with `=` padding.
  - Reference: Base64 `https://datatracker.ietf.org/doc/html/rfc4648`.

- sha1(const uint8_t* data, size_t len, uint8_t out[20])
  - Minimal SHA-1 implementation used exclusively for the WS handshake.
  - Pads the message as per SHA-1: append 0x80, pad zeros to 56 mod 64, append 64-bit bit-length, then process 512-bit blocks.
  - Reference: SHA-1 `https://www.rfc-editor.org/rfc/rfc3174`.

- strcasestr_local(const char* haystack, const char* needle) → const char*
  - Simple ASCII case-insensitive substring search; used during WS header parsing.

- ws_send_text_frame(sock_t s, const char* data, int len) → int
  - Builds a server-to-client unmasked WebSocket text frame (FIN=1, opcode=1).
  - Encodes payload length in 7-bit, 16-bit, or 64-bit forms and sends header, then data.
  - Reference: RFC 6455 framing `https://datatracker.ietf.org/doc/html/rfc6455#section-5.2`.

- ws_handshake(Client* c) → int
  - Parses accumulated HTTP headers in `c->wsBuf` until a blank line; extracts `Sec-WebSocket-Key` case-insensitively, trims whitespace.
  - Concatenates key with GUID `258EAFA5-E914-47DA-95CA-C5AB0DC85B11`, computes SHA1, base64-encodes it into `Sec-WebSocket-Accept`.
  - Sends the `101 Switching Protocols` response and marks `wsHandshakeDone`.
  - Returns: 1 success; 0 need more data; -1 failure.
  - References: RFC 6455 handshake `https://datatracker.ietf.org/doc/html/rfc6455#section-4.2.2`.

- send_text_to_client(int idx, const char* data, int len)
  - Abstraction that chooses plain `send` for TCP or `ws_send_text_frame` for WS after handshake.

- send_full_map_to(int clientIdx)
  - Sends a full snapshot of every map tile as `TILE wx wy x y ch` lines.
  - Used on new TCP connections and some WS paths to ensure the client has all tiles.

- send_map_to(int clientIdx, int wx, int wy)
  - Sends a snapshot of a single map’s tiles for `wx,wy` in an efficient buffered manner.
  - Used when a player transitions to a new map and on WS after initial join.

- try_open_map(const char* prefix, int mx, int my) → FILE*
  - Attempts to `fopen` `"%smaps/x%d-y%d.txt"` for different prefixes: `""`, `"../"`, `"../../"`.
  - Allows running the server from repo root or from inside `src/server/`.

- load_map_file(int mx, int my)
  - Tries to open the map file; if not found, generates an all-floor map `'.'` and enforces connectivity and center spawn at world center.
  - On read success, it sanitizes each character to the allowed set and ensures connectivity across interior edges and presence of `S` at world center.

- is_open(Map* m, int x, int y) → int
  - Returns whether a tile is within bounds and not a wall `#`.

- map_has_spawn(int mx, int my) / find_spawn_in_map(int mx, int my, int* sx, int* sy)
  - Helpers to detect and locate `S` in a map.

- spawn_enemies_for_map(int mx, int my, int count)
  - If the map contains a spawn `S`, clears enemies and returns.
  - Otherwise collects all open tiles into `candidates[]`, shuffles with Fisher–Yates, and activates up to `count` enemies with hp=2 at the first `count` shuffled positions.
  - Reference: Fisher–Yates shuffle `https://en.wikipedia.org/wiki/Fisher%E2%80%93Yates_shuffle`.

- place_near_spawn(Client* c)
  - Finds the first map containing `S`, then searches in expanding Manhattan rings around `S` for an open, unoccupied tile; places the client there and records `worldX/worldY/pos`.

- broadcast_state(void)
  - Builds a single string buffer for this tick: `TICK`, all `PLAYER` lines, active `BULLET` lines, and visible `ENEMY` lines (for maps with players only).
  - Sends to all connected clients with the appropriate framing.

- broadcast_tile(int wx, int wy, int x, int y, char ch)
  - Sends a single `TILE` line to all clients, used when walls are destroyed or pickups consumed.

- step_bullets(void)
  - For each active bullet: compute next cell by direction; if out of bounds, deactivate.
  - Enemy hit: decrement hp; on death, deactivate enemy and add +1 score to bullet owner; deactivate bullet.
  - Player hit (same world): if not on spawn map and target player is vulnerable, decrement hp; on death, award +10 to shooter and respawn victim; grant invincibility frames; deactivate bullet.
  - Wall hit: increment `wallDmg` until threshold, then turn `#` into `.` and `broadcast_tile`; deactivate bullet.
  - Otherwise advance bullet to next cell.

- step_enemies(void)
  - For each active map (has players), for each active enemy, choose a random direction and attempt to move if within bounds, open, and not occupied by another enemy.

- apply_enemy_contact_damage(void)
  - For each connected player not on a spawn map, if an enemy occupies the same cell and the player is not invincible, decrement hp and grant invincibility; on death, respawn near spawn and reset status.

- main(int argc, char** argv)
  - Setup: seed RNG; initialize Winsock on Windows; read ports (TCP default 5555, WS default 5556); load maps; spawn enemies; create/bind/listen on two sockets; log listening info.
  - Loop per tick (~50 ms via select timeout):
    - Build fd_set with listeners and connected clients; `select` for readability.
    - Accept TCP: configure `TCP_NODELAY` and `SO_KEEPALIVE`; allocate client slot; initialize state; record address via `getnameinfo`; send `YOU`, an immediate state frame, and `send_full_map_to`. If full, reply `FULL` and close.
    - Accept WS: enforce per-IP concurrency and connection rate; allocate slot; set short receive timeout; read HTTP headers into `wsBuf`; run `ws_handshake`; on success, initialize state, send `YOU`, immediate state frame, and `send_map_to` for current map; otherwise close.
    - Read clients: if WS and not handshaken, accumulate and attempt `ws_handshake`.
    - If WS framed: deframe masked text frames (single-frame FIN+TEXT) and copy payload to `buf`.
    - Parse lines:
      - `BYE`: disconnect.
      - `PING t`: respond with `PONG t`.
      - `INPUT dx dy shoot`: apply rate limiting via token bucket fields (`tokens`, `refillTicks`/`refillAmount`); update facing; handle world transitions preserving the orthogonal axis and check entry cells in neighbor maps; avoid stepping into other players; if world changed, send immediate state + `send_map_to`; if `shoot`, check cooldown or `superTicks` and spawn bullet with owner id.
    - Inactivity timeout: disconnect clients idle for >180s.
    - Step systems: bullets (~10 Hz), enemies (~6–7 Hz), contact damage.
    - Pickups: if standing on `X`, restore hp, grant `superTicks` and `invincibleTicks`, set tile to '.', and `broadcast_tile`.
    - Timers: tick down invincibility/super/cooldown; refill input tokens periodically up to `maxTokens`.
    - `broadcast_state()` and increment global tick.

---

## Multiplayer Text Protocol

Client → Server:
- `HELLO` (optional greeting)
- `INPUT dx dy shoot` where `dx,dy ∈ {-1,0,1}`, `shoot ∈ {0,1}`
- `BYE`
- `PING token`

Server → Client:
- `FULL`
- `YOU id`
- `TICK n`
- `PLAYER id wx wy x y color active hp invincibleTicks superTicks score`
- `BULLET wx wy x y active`
- `ENEMY wx wy x y hp`
- `TILE wx wy x y ch`

---

## Maps (`/maps/*.txt`)

- 18 lines × 40 columns. Valid chars: `# . @ X W S`
- Inter-map connectivity enforced: open door at the center of interior edges. World center guarantees a spawn `S` if absent.

---

## Build and Run

Linux/macOS:
```bash
gcc src/*.c -o dungeon
gcc src/server/server.c -o server
```

Windows (MSYS2/MinGW):
```bash
gcc src\*.c -o dungeon.exe -lws2_32
gcc src\server\server.c -o server.exe -lws2_32
```

Run:
```bash
./server 5555 5556
./dungeon
```
Choose Multiplayer in the menu and connect to `127.0.0.1:5555`.

---

## Notes, Tips, and Links

- ANSI terminals on Windows: PowerShell 5+/Windows Terminal recommended.
- WebSocket RFC 6455: `https://datatracker.ietf.org/doc/html/rfc6455`
- Beej’s Guide to Network Programming: `https://beej.us/guide/bgnet/`
- TCP Keepalive: `https://en.wikipedia.org/wiki/TCP_keepalive`
- Select and fd_set: `https://man7.org/linux/man-pages/man2/select.2.html`

