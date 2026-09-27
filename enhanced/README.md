# PicoBeta Enhanced v1

Minimal Minecraft Beta 1.7.3 server (protocol 14) – Windows version.

## What’s new vs Tiny

- **Players can see each other** (spawn + movement via EntityTeleport)
- **Command replies are private** (only the player who typed the command sees the result)
- Extra command: `/list`

## Commands

| Command | Description |
|---------|-------------|
| `/h`    | Help |
| `/f`    | Flash message (private) |
| `/list` | List online players (private) |

Normal chat remains global. Join/leave messages remain global.

## Build (from WSL)

```bash
cd enhanced
mkdir -p build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../toolchain-mingw64.cmake ..
cmake --build . -j$(nproc)
```

Binary: `build/picobeta-enhanced.exe`

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
