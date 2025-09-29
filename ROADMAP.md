# Roadmap

Status reflects current repository state. Items listed as completed are implemented in the codebase at the time of writing.

## Completed
- Basic roguelite gameplay: 9x9 world grid, destructible walls, enemies, shooting.
- Map I/O from `maps/` with inter-map door alignment and global spawn `S` support.
- Singleplayer: lives, invincibility on hit, score for enemy kills, `X` pickup restores lives and grants brief super/invincibility, `W` goal to win.
- HUD: HP line; scoreboard and minimap rendered under the map; hints.
- Multiplayer TCP server with server-authoritative simulation for players, enemies, and bullets.
- Multiplayer text protocol (`YOU`, `PLAYER`, `BULLET`, `ENEMY`, `TILE`, `TICK`, `FULL`).
- Client console: MP loading screen with sparkles and minimum visible duration.
- Cross-platform terminal stability: absolute cursor addressing with per-row clear, alt-screen autowrap off/on, robust POSIX write loop with drain, unbuffered stdout, per-frame scroll-region reset, warmup redraw frames.
- Native WebSocket support on server (secondary port), with per-IP limits and basic connection rate limiting.
- Web client (`webclient.html`) consuming the same text protocol, with loading overlay and HUD including ping.
- Protocol additions: `READY` signal after initial snapshot; `ENTR` entrance-block flags per map; `BULLET` includes `ownerId`.
- Client HUD: ping displayed in Multiplayer.
- Build action: `B` places a wall ahead in SP and MP (server validates occupancy).

## Short-term
- Health/score UI polish (icons, color tweaks) in console and web clients.
- Config flags for server (enemy count, ports) via environment or args.
- Lightweight Dockerfile for the server and a simple Makefile.
 - Strip zero-length writes and tighten renderer macros.
 - Document `BUILD` semantics in protocol and ensure consistent behavior on web client.

## Mid-term
- Latency smoothing: interpolation/extrapolation for players and bullets in both clients.
- Configurable/dynamic world size and streaming map I/O.
- Continuous Integration (build + basic lint) and release assets for common platforms.
- PvE/PvP toggles and friendly fire control.
 - Terminal integration tests on macOS (Apple Terminal, iTerm2) and Linux (xterm, GNOME Terminal).
 - Server-side map persistence and optional rollback-safe edits for tiles built/destroyed in MP.

## Long-term
- Persistence: high scores and per-user stats (files or SQLite).
- Matchmaking/lobbies and optional spectator mode.
- Chat/emotes and simple cosmetics (color themes).
- Cross-platform packaging (static builds where feasible).

