# PicoBeta Windows – Minimal Minecraft Beta 1.7.3 Server

Port of the original Pico (Arduino/WiFi) PicoBeta sketch to a native **Windows** executable.

- Protocol: **Minecraft Beta 1.7.3** (protocol version **14**)
- World size: **16×16×16**
- Max players: **4**
- Features: connect, walk, place/break blocks (instant), chat, basic inventory (infinite hotbar items), void protection, `/f` and `/h` commands

## Requirements to build (on WSL / Linux)

- CMake ≥ 3.16
- MinGW-w64 (`g++-mingw-w64-x86-64`)

```bash
sudo apt update
sudo apt install -y g++-mingw-w64-x86-64 cmake
```

## Build (cross-compile from WSL)

```bash
cd picobeta-windows
mkdir -p build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../toolchain-mingw64.cmake ..
cmake --build . -j$(nproc)
```

The executable is produced at:

```
build/picobeta.exe
```

Copy `picobeta.exe` to any Windows machine (x64). It only needs the usual Windows system DLLs (KERNEL32, msvcrt, WS2_32) – no extra redistributable required for most modern Windows.

## Run

```cmd
picobeta.exe
```

It listens on `0.0.0.0:25565`.

In Minecraft Beta 1.7.3 (or a compatible client such as the classic launcher / MultiMC with the correct version):

1. Multiplayer → Direct Connect
2. Address: `127.0.0.1` (or the IP of the machine running the server)
3. Join

## Notes / limitations (same as original Pico version)

- Very small fixed world (16³)
- No lighting engine (everything is full-bright)
- No entity physics / other players visible
- Inventory window clicks are ignored (items are infinite via SetSlot)
- Instant block break
- Only a couple of chat commands (`/f`, `/h`)
- Designed for fun / testing, not a full server

## Original project

This is a faithful port of the Arduino `.ino` that ran on a Raspberry Pi Pico W with WiFi. Networking was rewritten with Winsock2 (and POSIX sockets for possible future Linux builds).

Enjoy!

## Build Windows XP compatible (32-bit)

A second binary, `picobeta-win32.exe`, is provided for old machines:

- **32-bit (i686)**, statically linked (no MinGW DLLs needed on the target machine)
- Only imports `KERNEL32.dll`, `msvcrt.dll`, `WS2_32.dll` (Winsock 1.1-era calls only — `inet_ntoa` instead of the Vista+ `inet_ntop`)
- PE subsystem/OS version set to **5.01**, so it runs on **Windows XP SP2/SP3** and later (32-bit or 64-bit XP), as well as Vista/7/8/10/11.

To rebuild it yourself from WSL/Linux:

```bash
sudo apt install -y g++-mingw-w64-i686
cd picobeta-windows/tiny
mkdir -p build-win32 && cd build-win32
cmake -DCMAKE_TOOLCHAIN_FILE=../toolchain-mingw32-xp.cmake ..
cmake --build .
```

The regular `picobeta.exe` (built with `toolchain-mingw64.cmake`) targets 64-bit Windows 7+ and is not guaranteed to run on XP.
