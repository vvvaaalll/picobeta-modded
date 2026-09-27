# PicoBeta Enhanced v2

Minimal Minecraft Beta 1.7.3 server (protocol 14) – Windows version.

## What’s new vs Tiny

- **Players can see each other** (spawn + movement via EntityTeleport)
- **Command replies are private** (only the player who typed the command sees the result)
- **Infinite flat world**: chunks are generated on the fly as players roam
  (no fixed 16×16×16 box anymore), while staying just as light — only
  blocks a player has actually placed/broken are kept in RAM, everything
  else is derived from a tiny flat-terrain function.
- **World persistence**: edited blocks are saved to `world.pbw` next to
  the executable, and reloaded automatically on the next launch. The
  world autosaves every ~30s (only if something changed), on `/save`,
  and on shutdown (Ctrl+C / window close / `taskkill`).
- **Chunk streaming rate-limit**: the 5×5 view (25 chunks) is no longer
  dumped in a single TCP burst on join — that crashed the stock Beta
  1.7.3 client. Chunks are queued and sent one every few ticks
  (nearest first).
- **Non-blocking I/O + throttled keepalives**: sockets are non-blocking with
  short send/recv timeouts, TCP_NODELAY is on, and KeepAlive is sent at most
  ~every 0.5–2 s. A lagging client can no longer freeze the whole server
  (which used to look like a silent black screen with no “left” message).
- Extra commands: `/list`, `/save`

## Commands

| Command | Description |
|---------|-------------|
| `/h`    | Help |
| `/f`    | Flash message (private) |
| `/list` | List online players (private) |
| `/save` | Force an immediate world save (private confirmation) |

Normal chat remains global. Join/leave messages remain global.

## How the infinite world stays lightweight

There is no per-chunk storage and no real terrain generator. Every chunk
is just the same flat profile (bedrock → stone → dirt → grass → air)
computed from `y` alone, and a single `unordered_map<coords, block>`
holds only the blocks that differ from that profile (i.e. what players
have dug or placed). A chunk is built into a temporary 4 KB buffer only
when it needs to be sent, and it's discarded right after. Each player
only ever has a 5×5 chunk area loaded around them (`VIEW_DISTANCE`),
streamed in/out as they move, so 4 players roaming far apart still costs
next to nothing in RAM. Placing a block that matches the natural terrain
again (e.g. re-placing dirt where dirt already generates) removes the
entry instead of storing it, keeping the save file sparse too.

## Build (from WSL)

```bash
cd enhanced
mkdir -p build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../toolchain-mingw64.cmake ..
cmake --build . -j$(nproc)
```

Binary: `build/picobeta-enhanced.exe`

> Note: this drop doesn't include prebuilt `.exe` files — the sandbox
> this was built in has no internet access and no `mingw-w64`/`cmake`
> installed, only a native Linux g++. The C++17 source was compiled
> and tested there (native Linux build) to confirm it builds cleanly
> and behaves correctly — protocol-level tests covered initial chunk
> streaming, chunk load/unload while moving, block dig/place with
> save-to-disk, reload after restart, and a clean shutdown/`/save`
> path — but you'll need to run the usual `cmake --build` from your
> machine to get fresh Windows binaries.

## Notes

- Still the same tiny 16×16×16 world
- Instant break / place
- Max 4 players
- Movement uses teleport packets (simple & reliable for few players)

## Build Windows XP compatible (32-bit)

A second binary, `picobeta-enhanced-win32.exe`, is provided for old machines:

- **32-bit (i686)**, statically linked (no MinGW DLLs needed on the target machine)
- Only imports `KERNEL32.dll`, `msvcrt.dll`, `WS2_32.dll` (Winsock 1.1-era calls only — `inet_ntoa` instead of the Vista+ `inet_ntop`)
- PE subsystem/OS version set to **5.01**, so it runs on **Windows XP SP2/SP3** and later (32-bit or 64-bit XP), as well as Vista/7/8/10/11.

To rebuild it yourself from WSL/Linux:

```bash
sudo apt install -y g++-mingw-w64-i686
cd picobeta-windows/enhanced
mkdir -p build-win32 && cd build-win32
cmake -DCMAKE_TOOLCHAIN_FILE=../toolchain-mingw32-xp.cmake ..
cmake --build .
```

The regular `picobeta-enhanced.exe` (built with `toolchain-mingw64.cmake`) targets 64-bit Windows 7+ and is not guaranteed to run on XP.
