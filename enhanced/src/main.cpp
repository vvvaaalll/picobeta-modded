/*
 * PicoBeta Enhanced - Minimal Minecraft Beta 1.7.3 server
 * Ported to native Windows (cross-compilable from WSL with MinGW-w64)
 * Original: Arduino / Raspberry Pi Pico WiFi version
 *
 * Protocol version 14 (Minecraft Beta 1.7.3)
 * World: 16x16x16
 * Max players: 4
 *
 * Enhanced v1:
 *  - Players can see each other (spawn + movement)
 *  - Command replies are private (only to the sender)
 */

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET SocketType;
#define INVALID_SOCK INVALID_SOCKET
#define CLOSE_SOCKET closesocket
#define SOCK_ERR SOCKET_ERROR
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
typedef int SocketType;
#define INVALID_SOCK (-1)
#define CLOSE_SOCKET close
#define SOCK_ERR (-1)
#endif

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <thread>
#include <cmath>
#include <csignal>
#include <atomic>
#include <algorithm>

// ============================================================
// Config
// ============================================================
// A "chunk" is the 16x16x16 slab of blocks the client receives in one
// Chunk packet (0x33). The world is now infinite in X/Z: chunks are
// generated on the fly (flat terrain) and only edited blocks are kept
// in memory / on disk, so RAM stays low no matter how far players roam.
#define CHUNK_SIZE_X 16
#define CHUNK_SIZE_Y 16
#define CHUNK_SIZE_Z 16
const int32_t CHUNK_VOLUME = CHUNK_SIZE_X * CHUNK_SIZE_Y * CHUNK_SIZE_Z;
// 2 = 5x5 grid (25 chunks). Rate-limited below so the Beta client keeps up.
#define VIEW_DISTANCE 2
// Send at most one full Chunk packet every N main-loop passes (~50–150 ms).
#define MAX_CHUNKS_PER_TICK 1
#define CHUNK_SEND_INTERVAL 3
#define KEEPALIVE_INTERVAL 40   // KeepAlive at most every ~0.5–1.5 s
// Ignore chunk-coord flicker within this many blocks of a boundary (float jitter).
#define CHUNK_HYSTERESIS 2.0
#define WORLD_SAVE_FILE "world.pbw"
#define AUTOSAVE_TICKS 3000 // ~30s at 10ms/loop, only saves if the world changed
#define MAX_CONNECTIONS 4
#define SERVER_PORT 25565

// ============================================================
// Packet IDs (Minecraft Beta 1.7.3 / protocol 14)
// ============================================================
enum Packet : uint8_t {
  KeepAlive = 0x00,
  LoginRequest = 0x01,
  Handshake = 0x02,
  ChatMessage = 0x03,
  TimeUpdate = 0x04,
  EntityEquipment = 0x05,
  SpawnPosition = 0x06,
  ClickEntity = 0x07,
  SetHealth = 0x08,
  Respawn = 0x09,
  PlayerPacket = 0x0A,
  PlayerPosition = 0x0B,
  PlayerLook = 0x0C,
  PlayerPositionLook = 0x0D,
  PlayerDigging = 0x0E,
  PlayerBlockPlacement = 0x0F,
  ActiveSlot = 0x10,
  UseBed = 0x11,
  PlayerAction = 0x12,
  EntityAction = 0x13,
  SpawnPlayerEntity = 0x14,
  SpawnItemEntity = 0x15,
  CollectItem = 0x16,
  SpawnObjectEntity = 0x17,
  SpawnMobEntity = 0x18,
  SpawnPaintingEntity = 0x19,
  PlayerMovement = 0x1B,
  EntityVelocity = 0x1C,
  DestroyEntity = 0x1D,
  Entity = 0x1E,
  EntityRelativeMove = 0x1F,
  EntityLook = 0x20,
  EntityLookRelativeMove = 0x21,
  EntityTeleport = 0x22,
  EntityMetadata = 0x26,
  MountEntity = 0x27,
  EntityMetadata2 = 0x28,
  PreChunk = 0x32,
  Chunk = 0x33,
  MultiBlockUpdate = 0x34,
  BlockUpdate = 0x35,
  BlockAction = 0x36,
  Explosion = 0x3C,
  Effect = 0x3D,
  GameState = 0x46,
  LightningBolt = 0x47,
  OpenWindow = 0x64,
  CloseWindowPacket = 0x65,
  WindowClick = 0x66,
  SetSlot = 0x67,
  WindowItems = 0x68,
  UpdateProgressBar = 0x69,
  Transaction = 0x6A,
  UpdateSign = 0x82,
  MapData = 0x83,
  IncrementStatistic = 0xC8,
  Disconnect = 0xFF
};

// ============================================================
// Helpers
// ============================================================
struct Vec3 {
  double x = 0.0, y = 0.0, z = 0.0;
};

struct Int3 {
  int x = 0, y = 0, z = 0;
};

Int3 spawnPoint = {
  CHUNK_SIZE_X / 2,
  CHUNK_SIZE_Y / 2,
  CHUNK_SIZE_Z / 2
};

void FaceOffset(Int3& pos, int8_t face) {
  switch (face) {
    case 0: pos.y--; break;
    case 1: pos.y++; break;
    case 2: pos.z--; break;
    case 3: pos.z++; break;
    case 4: pos.x--; break;
    case 5: pos.x++; break;
  }
}

#define INVENTORY_HOTBAR 36

enum ConnectionProgress {
  Disconnected = 0,
  TryingToConnect,
  Shake,
  Login,
  Connected
};

struct ChunkCoord {
  int32_t x = 0, z = 0;
};
inline bool operator==(const ChunkCoord& a, const ChunkCoord& b) {
  return a.x == b.x && a.z == b.z;
}

struct Player {
  int32_t entityId = 0;
  char* username = nullptr;
  Vec3 position;
  float yaw = 0.0f;
  float pitch = 0.0f;
  double stance = 1.5;
  bool onGround = false;
  int8_t hotbarSlot = 0;
  int32_t leftToLoad = 1;
  int8_t connectionStage = Disconnected;
  bool hasSentPacket = false;
  SocketType sock = INVALID_SOCK;
  bool active = false;
  // For movement throttling
  Vec3 lastBroadcastPos;
  float lastBroadcastYaw = 0.0f;
  float lastBroadcastPitch = 0.0f;
  // Infinite world: which chunk the player is standing in, and which
  // chunks have already been sent to them (so we only load/unload deltas).
  int32_t chunkX = INT32_MIN;
  int32_t chunkZ = INT32_MIN;
  std::vector<ChunkCoord> loadedChunks;
  // Chunks that still need their PreChunk+Chunk packets. Drained a few
  // at a time so we never flood the Beta 1.7.3 client on join/teleport.
  std::vector<ChunkCoord> pendingChunks;
  // Throttle counters (incremented every ProcessClient call)
  int32_t tickCounter = 0;
  int32_t lastKeepaliveTick = 0;
  int32_t lastChunkDrainTick = 0;
};

Player players[MAX_CONNECTIONS];
int32_t globalEntityId = 1;

// Sparse world storage: only blocks that differ from the flat generated
// terrain are kept here (and saved to disk). Everything else is computed
// on demand by GenerateBlock(), so memory usage stays tiny regardless of
// how far the infinite world is explored.
std::unordered_map<uint64_t, uint8_t> blockOverrides;
bool worldDirty = false;

std::atomic<bool> g_running{true};
void HandleSignal(int) { g_running = false; }
#ifdef _WIN32
BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
  if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_CLOSE_EVENT ||
      ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_SHUTDOWN_EVENT) {
    g_running = false;
    return TRUE;
  }
  return FALSE;
}
#endif

// ============================================================
// Network helpers
// Non-blocking sockets + short select timeouts so a slow/frozen Beta
// client can never stall the whole single-threaded server on send/recv.
// ============================================================
bool SetNonBlocking(SocketType s) {
#ifdef _WIN32
  u_long mode = 1;
  return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
  int flags = fcntl(s, F_GETFL, 0);
  if (flags < 0) return false;
  return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

void SetTcpNoDelay(SocketType s) {
  int opt = 1;
#ifdef _WIN32
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
#endif
}

// Wait until socket is readable (or error/timeout). Returns true if readable.
static bool WaitReadable(SocketType s, int timeoutMs) {
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(s, &fds);
  timeval tv;
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  int r = select(static_cast<int>(s) + 1, &fds, nullptr, nullptr, &tv);
  return r > 0 && FD_ISSET(s, &fds);
}

// Wait until socket is writable.
static bool WaitWritable(SocketType s, int timeoutMs) {
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(s, &fds);
  timeval tv;
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  int r = select(static_cast<int>(s) + 1, nullptr, &fds, nullptr, &tv);
  return r > 0 && FD_ISSET(s, &fds);
}

static bool IsWouldBlock() {
#ifdef _WIN32
  int e = WSAGetLastError();
  return e == WSAEWOULDBLOCK || e == WSAEINTR;
#else
  return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR;
#endif
}

// Read exactly `len` bytes with a per-chunk timeout. Returns len on success,
// <=0 on disconnect/error/timeout (treat as fatal for this client).
int RecvExact(SocketType s, char* buf, int len) {
  int total = 0;
  while (total < len) {
    int n = recv(s, buf + total, len - total, 0);
    if (n > 0) {
      total += n;
      continue;
    }
    if (n == 0) return 0; // peer closed
    if (IsWouldBlock()) {
      // Wait up to 2s for more data; if the client is stuck mid-packet we
      // give up so the server loop never hangs.
      if (!WaitReadable(s, 2000)) return -1;
      continue;
    }
    return -1; // hard error
  }
  return total;
}

// Send all bytes without ever blocking the server for more than a few
// hundred ms total. On persistent would-block we abort the write so a
// lagging client cannot freeze everyone else.
int SendAll(SocketType s, const char* buf, int len) {
  int total = 0;
  int stalls = 0;
  while (total < len) {
    int n = send(s, buf + total, len - total, 0);
    if (n > 0) {
      total += n;
      stalls = 0;
      continue;
    }
    if (n == 0) return 0;
    if (IsWouldBlock()) {
      if (++stalls > 20) return -1; // ~1s of waiting max
      if (!WaitWritable(s, 50)) return -1;
      continue;
    }
    return -1;
  }
  return total;
}

// ============================================================
// Packet read/write (big-endian like original)
// ============================================================
int8_t ReadByte(SocketType s) {
  char b;
  if (RecvExact(s, &b, 1) != 1) return 0;
  return static_cast<int8_t>(b);
}

void WriteByte(SocketType s, int8_t value) {
  char b = static_cast<char>(value);
  SendAll(s, &b, 1);
}

int16_t ReadShort(SocketType s) {
  uint8_t b[2];
  if (RecvExact(s, reinterpret_cast<char*>(b), 2) != 2) return 0;
  return static_cast<int16_t>((b[0] << 8) | b[1]);
}

void WriteShort(SocketType s, int16_t value) {
  uint8_t b[2];
  b[0] = (value >> 8) & 0xFF;
  b[1] = value & 0xFF;
  SendAll(s, reinterpret_cast<char*>(b), 2);
}

int32_t ReadInteger(SocketType s) {
  uint8_t b[4];
  if (RecvExact(s, reinterpret_cast<char*>(b), 4) != 4) return 0;
  return static_cast<int32_t>((b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]);
}

void WriteInteger(SocketType s, int32_t value) {
  uint8_t b[4];
  b[0] = (value >> 24) & 0xFF;
  b[1] = (value >> 16) & 0xFF;
  b[2] = (value >> 8) & 0xFF;
  b[3] = value & 0xFF;
  SendAll(s, reinterpret_cast<char*>(b), 4);
}

int64_t ReadLong(SocketType s) {
  uint8_t b[8];
  if (RecvExact(s, reinterpret_cast<char*>(b), 8) != 8) return 0;
  int64_t r = 0;
  for (int i = 0; i < 8; i++) {
    r = (r << 8) | b[i];
  }
  return r;
}

void WriteLong(SocketType s, int64_t value) {
  uint8_t b[8];
  for (int i = 7; i >= 0; i--) {
    b[i] = value & 0xFF;
    value >>= 8;
  }
  SendAll(s, reinterpret_cast<char*>(b), 8);
}

float ReadFloat(SocketType s) {
  uint32_t bits = static_cast<uint32_t>(ReadInteger(s));
  float f;
  memcpy(&f, &bits, sizeof(f));
  return f;
}

void WriteFloat(SocketType s, float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  WriteInteger(s, static_cast<int32_t>(bits));
}

double ReadDouble(SocketType s) {
  uint64_t bits = static_cast<uint64_t>(ReadLong(s));
  double d;
  memcpy(&d, &bits, sizeof(d));
  return d;
}

void WriteDouble(SocketType s, double value) {
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  WriteLong(s, static_cast<int64_t>(bits));
}

// String16: length (short) + high-byte 0 + char (simplified UTF-16)
char* ReadString16(SocketType s) {
  int16_t length = ReadShort(s);
  if (length < 0 || length > 1024) return nullptr;
  char* buffer = static_cast<char*>(malloc(length + 1));
  if (!buffer) return nullptr;
  for (int i = 0; i < length; i++) {
    ReadByte(s); // high byte (ignored)
    buffer[i] = static_cast<char>(ReadByte(s));
  }
  buffer[length] = '\0';
  return buffer;
}

void WriteString16(SocketType s, const char* message) {
  int16_t length = static_cast<int16_t>(strlen(message));
  WriteShort(s, length);
  for (int16_t i = 0; i < length; i++) {
    WriteByte(s, 0);
    WriteByte(s, message[i]);
  }
}

// ============================================================
// World (infinite, flat, on-demand)
// ============================================================

// Pure function of Y: every chunk in the world uses this same flat
// profile, which is what lets the world be "infinite" without ever
// storing more than the blocks players have actually changed.
uint8_t GenerateBlock(int32_t y) {
  if (y == 0) return 7;                        // bedrock
  if (y < CHUNK_SIZE_Y / 3) return 1;           // stone
  if (y < CHUNK_SIZE_Y / 2) return 3;           // dirt
  if (y == CHUNK_SIZE_Y / 2) return 2;          // grass
  return 0;                                     // air
}

// Packs a block's world coordinates into one 64-bit key for the
// overrides map. X/Z are biased into 28 unsigned bits each (+-~134M
// blocks, far more than any session will reach) and Y needs only 4 bits.
static inline uint64_t BlockKey(int32_t x, int32_t y, int32_t z) {
  uint64_t ux = (static_cast<uint32_t>(x) + 0x8000000u) & 0xFFFFFFFull;
  uint64_t uz = (static_cast<uint32_t>(z) + 0x8000000u) & 0xFFFFFFFull;
  uint64_t uy = static_cast<uint64_t>(y) & 0xF;
  return (ux << 36) | (uz << 4) | uy;
}

uint8_t GetBlock(int32_t x, int32_t y, int32_t z) {
  if (y < 0 || y >= CHUNK_SIZE_Y) return 0;
  auto it = blockOverrides.find(BlockKey(x, y, z));
  if (it != blockOverrides.end()) return it->second;
  return GenerateBlock(y);
}

void SetBlock(int32_t x, int32_t y, int32_t z, uint8_t type) {
  if (y < 0 || y >= CHUNK_SIZE_Y) return;
  uint64_t key = BlockKey(x, y, z);
  if (type == GenerateBlock(y)) {
    // Back to the natural terrain: no need to remember it, keeps the
    // save file (and RAM) as small as possible.
    blockOverrides.erase(key);
  } else {
    blockOverrides[key] = type;
  }
  worldDirty = true;
}

// Fills a caller-provided CHUNK_VOLUME-byte buffer with one chunk's
// blocks, using the same index layout the original fixed world used.
void BuildChunkBuffer(int32_t cx, int32_t cz, uint8_t* buf) {
  for (int32_t lx = 0; lx < CHUNK_SIZE_X; lx++) {
    for (int32_t lz = 0; lz < CHUNK_SIZE_Z; lz++) {
      for (int32_t ly = 0; ly < CHUNK_SIZE_Y; ly++) {
        int32_t idx = lx * (CHUNK_SIZE_Y * CHUNK_SIZE_Z) + lz * CHUNK_SIZE_Y + ly;
        buf[idx] = GetBlock(cx * CHUNK_SIZE_X + lx, ly, cz * CHUNK_SIZE_Z + lz);
      }
    }
  }
}

// ============================================================
// World save / load (only the sparse edits, not the generated terrain)
// ============================================================
void SaveWorld() {
  std::ofstream f(WORLD_SAVE_FILE, std::ios::binary | std::ios::trunc);
  if (!f) {
    std::cerr << "Failed to open " << WORLD_SAVE_FILE << " for writing" << std::endl;
    return;
  }
  const char magic[4] = {'P', 'B', 'W', '1'};
  f.write(magic, 4);
  uint32_t count = static_cast<uint32_t>(blockOverrides.size());
  f.write(reinterpret_cast<const char*>(&count), sizeof(count));
  for (const auto& entry : blockOverrides) {
    uint64_t key = entry.first;
    uint8_t type = entry.second;
    int32_t x = static_cast<int32_t>((key >> 36) & 0xFFFFFFFull) - 0x8000000;
    int32_t z = static_cast<int32_t>((key >> 4) & 0xFFFFFFFull) - 0x8000000;
    int8_t y = static_cast<int8_t>(key & 0xF);
    f.write(reinterpret_cast<const char*>(&x), sizeof(x));
    f.write(reinterpret_cast<const char*>(&z), sizeof(z));
    f.write(reinterpret_cast<const char*>(&y), sizeof(y));
    f.write(reinterpret_cast<const char*>(&type), sizeof(type));
  }
  worldDirty = false;
  std::cout << "World saved (" << count << " modified blocks) -> " << WORLD_SAVE_FILE << std::endl;
}

void LoadWorld() {
  std::ifstream f(WORLD_SAVE_FILE, std::ios::binary);
  if (!f) {
    std::cout << "No existing world save found, starting fresh." << std::endl;
    return;
  }
  char magic[4];
  f.read(magic, 4);
  if (f.gcount() != 4 || memcmp(magic, "PBW1", 4) != 0) {
    std::cerr << "Invalid or corrupt world save file, ignoring." << std::endl;
    return;
  }
  uint32_t count = 0;
  f.read(reinterpret_cast<char*>(&count), sizeof(count));
  blockOverrides.clear();
  blockOverrides.reserve(count);
  for (uint32_t i = 0; i < count; i++) {
    int32_t x = 0, z = 0;
    int8_t y = 0;
    uint8_t type = 0;
    f.read(reinterpret_cast<char*>(&x), sizeof(x));
    f.read(reinterpret_cast<char*>(&z), sizeof(z));
    f.read(reinterpret_cast<char*>(&y), sizeof(y));
    f.read(reinterpret_cast<char*>(&type), sizeof(type));
    if (!f) break;
    blockOverrides[BlockKey(x, y, z)] = type;
  }
  std::cout << "World loaded (" << blockOverrides.size() << " modified blocks) <- " << WORLD_SAVE_FILE << std::endl;
}

// ============================================================
// Send helpers
// Chunk packets are built into a contiguous buffer then sent in ONE
// SendAll call. A partial write mid-packet permanently desyncs the Beta
// client (black screen, no disconnect) — never stream WriteByte-by-WriteByte
// for 10 KB payloads on a non-blocking socket.
// ============================================================
static void BufPushByte(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
static void BufPushShort(std::vector<uint8_t>& b, int16_t v) {
  b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  b.push_back(static_cast<uint8_t>(v & 0xFF));
}
static void BufPushInt(std::vector<uint8_t>& b, int32_t v) {
  b.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
  b.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  b.push_back(static_cast<uint8_t>(v & 0xFF));
}

// Returns true on full send, false on failure (caller must drop the client).
bool SendPreChunk(SocketType s, int32_t x, int32_t z, bool mode) {
  std::vector<uint8_t> pkt;
  pkt.reserve(10);
  BufPushByte(pkt, PreChunk);
  BufPushInt(pkt, x);
  BufPushInt(pkt, z);
  BufPushByte(pkt, mode ? 1 : 0);
  return SendAll(s, reinterpret_cast<const char*>(pkt.data()),
                 static_cast<int>(pkt.size())) == static_cast<int>(pkt.size());
}

// cx/cz are CHUNK indices (same units as PreChunk / SetChunkVisibility).
// The Map Chunk packet requires the BLOCK coordinate of the section corner
// (cx*16, cz*16) — see Betrock++ Packet::ChunkData and wiki.vg protocol 14.
// Sending the chunk index as X/Z made every column land on (0,0) and froze
// the Beta client (instant black screen) as soon as a 2nd chunk arrived.
bool SendChunk(SocketType s, int32_t cx, int32_t cz, int32_t dataSize, uint8_t* data) {
  const int32_t blockX = cx * CHUNK_SIZE_X;
  const int32_t blockZ = cz * CHUNK_SIZE_Z;
  int32_t trueSize = static_cast<int32_t>(dataSize * 2.5);
  std::vector<uint8_t> pkt;
  pkt.reserve(static_cast<size_t>(trueSize) + 32);

  BufPushByte(pkt, Chunk);
  BufPushInt(pkt, blockX);
  BufPushShort(pkt, 0); // Y (bottom of the 16-high section)
  BufPushInt(pkt, blockZ);
  BufPushByte(pkt, static_cast<uint8_t>(CHUNK_SIZE_X - 1));
  BufPushByte(pkt, static_cast<uint8_t>(CHUNK_SIZE_Y - 1));
  BufPushByte(pkt, static_cast<uint8_t>(CHUNK_SIZE_Z - 1));
  BufPushInt(pkt, trueSize + 11);

  // Zlib header + stored (uncompressed) deflate block
  BufPushByte(pkt, 0x78);
  BufPushByte(pkt, 0x01);
  BufPushByte(pkt, 0x01); // BFINAL=1, BTYPE=00
  BufPushByte(pkt, static_cast<uint8_t>(trueSize & 0xFF));
  BufPushByte(pkt, static_cast<uint8_t>((trueSize >> 8) & 0xFF));
  BufPushByte(pkt, static_cast<uint8_t>((~trueSize) & 0xFF));
  BufPushByte(pkt, static_cast<uint8_t>((~trueSize >> 8) & 0xFF));

  uint32_t A = 1, B = 0;

  for (int32_t i = 0; i < dataSize; i++) {
    uint8_t v = data[i];
    pkt.push_back(v);
    A = (A + v) % 65521;
    B = (B + A) % 65521;
  }
  for (int32_t i = 0; i < dataSize / 2; i++) {
    pkt.push_back(0);
    B = (B + A) % 65521; // byte is 0 → A unchanged
  }
  for (int32_t i = 0; i < dataSize; i++) {
    pkt.push_back(0xFF);
    A = (A + 0xFF) % 65521;
    B = (B + A) % 65521;
  }
  BufPushInt(pkt, static_cast<int32_t>((B << 16) | A));

  return SendAll(s, reinterpret_cast<const char*>(pkt.data()),
                 static_cast<int>(pkt.size())) == static_cast<int>(pkt.size());
}

void SendPlayerPosition(SocketType s, Player& p) {
  WriteByte(s, PlayerPositionLook);
  WriteDouble(s, p.position.x);
  WriteDouble(s, p.position.y);
  WriteDouble(s, p.stance);
  WriteDouble(s, p.position.z);
  WriteFloat(s, p.yaw);
  WriteFloat(s, p.pitch);
  WriteByte(s, p.onGround ? 1 : 0);
}

void SendSpawnPosition(SocketType s) {
  WriteByte(s, SpawnPosition);
  WriteInteger(s, spawnPoint.x);
  WriteInteger(s, spawnPoint.y);
  WriteInteger(s, spawnPoint.z);
}

void SendChatMessage(SocketType s, const char* sender, const char* message, char color = 'r', bool log = false) {
  // §c<sender> message
  size_t len = strlen(sender) + strlen(message) + 8;
  char* fullMessage = static_cast<char*>(malloc(len));
  if (fullMessage) {
    fullMessage[0] = static_cast<char>(0xA7); // §
    fullMessage[1] = color;
    fullMessage[2] = '<';
    fullMessage[3] = '\0';
    strcat(fullMessage, sender);
    strcat(fullMessage, "> ");
    strcat(fullMessage, message);
    if (log) std::cout << fullMessage << std::endl;
    WriteByte(s, ChatMessage);
    WriteString16(s, fullMessage);
    free(fullMessage);
  }
}

void SendGlobalChatMessage(const char* sender, const char* message, char color = 'r') {
  // Print once in console
  std::cout << "<" << sender << "> " << message << std::endl;
  for (int i = 0; i < MAX_CONNECTIONS; i++) {
    if (players[i].active && players[i].sock != INVALID_SOCK) {
      SendChatMessage(players[i].sock, sender, message, color, false);
    }
  }
}

void SendBlockUpdate(SocketType s, Int3 pos, int8_t type, int8_t meta) {
  // X/Z are unbounded in the infinite world; only height is limited.
  if (pos.y < 0 || pos.y >= CHUNK_SIZE_Y) {
    SendChatMessage(s, "Server", "Out of bounds!", 'c');
    type = 0;
    meta = 0;
  } else {
    SetBlock(pos.x, pos.y, pos.z, static_cast<uint8_t>(type));
  }
  WriteByte(s, BlockUpdate);
  WriteInteger(s, pos.x);
  WriteByte(s, static_cast<int8_t>(pos.y));
  WriteInteger(s, pos.z);
  WriteByte(s, type);
  WriteByte(s, meta);
}

void SendEffect(SocketType s, int32_t effectId, Int3 pos, int32_t data) {
  WriteByte(s, Effect);
  WriteInteger(s, effectId);
  WriteInteger(s, pos.x);
  WriteByte(s, static_cast<int8_t>(pos.y));
  WriteInteger(s, pos.z);
  WriteInteger(s, data);
}

void SendGlobalBlockUpdate(Int3 pos, int8_t type, int8_t meta) {
  for (int i = 0; i < MAX_CONNECTIONS; i++) {
    if (players[i].active && players[i].sock != INVALID_SOCK) {
      SendBlockUpdate(players[i].sock, pos, type, meta);
      SendEffect(players[i].sock, 2001, pos, type);
    }
  }
}

void SendSetSlot(SocketType s, int8_t window, int16_t slot, int16_t id, int8_t amount, int16_t damage) {
  WriteByte(s, SetSlot);
  WriteByte(s, window);
  WriteShort(s, slot);
  WriteShort(s, id);
  WriteByte(s, amount);
  WriteShort(s, damage);
}

void SetPositionToSpawn(Player& p) {
  p.position.x = static_cast<double>(spawnPoint.x);
  p.position.y = static_cast<double>(spawnPoint.y) + 3.5;
  p.position.z = static_cast<double>(spawnPoint.z);
}

void SendDisconnect(SocketType s, const char* message) {
  WriteByte(s, Disconnect);
  WriteString16(s, message);
}

// ============================================================
// Player visibility (Enhanced)
// ============================================================

// Absolute position is fixed-point * 32
static int32_t ToFixed(double v) {
  return static_cast<int32_t>(v * 32.0);
}

void SendSpawnPlayer(SocketType to, const Player& who) {
  if (!who.username) return;
  WriteByte(to, SpawnPlayerEntity); // 0x14
  WriteInteger(to, who.entityId);
  WriteString16(to, who.username);
  WriteInteger(to, ToFixed(who.position.x));
  WriteInteger(to, ToFixed(who.position.y));
  WriteInteger(to, ToFixed(who.position.z));
  WriteByte(to, static_cast<int8_t>(who.yaw * 256.0f / 360.0f));
  WriteByte(to, static_cast<int8_t>(who.pitch * 256.0f / 360.0f));
  WriteShort(to, 0); // current item
}

void SendDestroyEntity(SocketType to, int32_t entityId) {
  WriteByte(to, DestroyEntity); // 0x1D
  WriteInteger(to, entityId);
}

void SendEntityTeleport(SocketType to, const Player& who) {
  WriteByte(to, EntityTeleport); // 0x22
  WriteInteger(to, who.entityId);
  WriteInteger(to, ToFixed(who.position.x));
  WriteInteger(to, ToFixed(who.position.y));
  WriteInteger(to, ToFixed(who.position.z));
  WriteByte(to, static_cast<int8_t>(who.yaw * 256.0f / 360.0f));
  WriteByte(to, static_cast<int8_t>(who.pitch * 256.0f / 360.0f));
}

// Spawn this player to every other connected player
void BroadcastSpawnPlayer(const Player& who) {
  for (int i = 0; i < MAX_CONNECTIONS; i++) {
    if (players[i].active && players[i].sock != INVALID_SOCK && players[i].entityId != who.entityId) {
      SendSpawnPlayer(players[i].sock, who);
    }
  }
}

// Destroy this player for every other connected player
void BroadcastDestroyPlayer(const Player& who) {
  for (int i = 0; i < MAX_CONNECTIONS; i++) {
    if (players[i].active && players[i].sock != INVALID_SOCK && players[i].entityId != who.entityId) {
      SendDestroyEntity(players[i].sock, who.entityId);
    }
  }
}

// Send this player's current position to every other player (throttled)
void BroadcastPlayerMovement(Player& who) {
  // Only broadcast if moved/rotated enough (avoids flooding)
  double dx = who.position.x - who.lastBroadcastPos.x;
  double dy = who.position.y - who.lastBroadcastPos.y;
  double dz = who.position.z - who.lastBroadcastPos.z;
  float dyaw = who.yaw - who.lastBroadcastYaw;
  float dpitch = who.pitch - who.lastBroadcastPitch;
  if (dx*dx + dy*dy + dz*dz < 0.01 && dyaw*dyaw + dpitch*dpitch < 1.0f) {
    return; // too small, skip
  }
  who.lastBroadcastPos = who.position;
  who.lastBroadcastYaw = who.yaw;
  who.lastBroadcastPitch = who.pitch;

  for (int i = 0; i < MAX_CONNECTIONS; i++) {
    if (players[i].active && players[i].sock != INVALID_SOCK && players[i].entityId != who.entityId) {
      SendEntityTeleport(players[i].sock, who);
    }
  }
}

// When a new player finishes login: show him all existing players, and show him to them
void OnPlayerJoined(Player& p) {
  p.lastBroadcastPos = p.position;
  p.lastBroadcastYaw = p.yaw;
  p.lastBroadcastPitch = p.pitch;
  // Show existing players to the new one
  for (int i = 0; i < MAX_CONNECTIONS; i++) {
    if (players[i].active && players[i].sock != INVALID_SOCK && players[i].entityId != p.entityId
        && players[i].connectionStage == Connected && players[i].username) {
      SendSpawnPlayer(p.sock, players[i]);
    }
  }
  // Show the new player to everyone else
  BroadcastSpawnPlayer(p);
}

// ============================================================
// Chunk streaming (infinite world)
// ============================================================
static inline int32_t FloorDivChunk(double v, int32_t size) {
  return static_cast<int32_t>(std::floor(v / static_cast<double>(size)));
}

static bool ChunkInVector(const std::vector<ChunkCoord>& v, ChunkCoord c) {
  for (const auto& e : v) if (e == c) return true;
  return false;
}

static int ChunkDist(ChunkCoord a, int32_t cx, int32_t cz) {
  int32_t dx = a.x - cx; if (dx < 0) dx = -dx;
  int32_t dz = a.z - cz; if (dz < 0) dz = -dz;
  return static_cast<int>(dx > dz ? dx : dz);
}

// Drop a client cleanly after a failed send (partial/broken stream).
void DropPlayer(Player& p, const char* reason) {
  if (!p.active) return;
  if (p.username) {
    std::cout << p.username << " has disconnected (" << reason << ")" << std::endl;
    SendGlobalChatMessage(p.username, "has left.", 'e');
    BroadcastDestroyPlayer(p);
  }
  if (p.sock != INVALID_SOCK) {
    CLOSE_SOCKET(p.sock);
    p.sock = INVALID_SOCK;
  }
  p.active = false;
  p.connectionStage = Disconnected;
  p.pendingChunks.clear();
  p.loadedChunks.clear();
  if (p.username) { free(p.username); p.username = nullptr; }
}

// Sends at most MAX_CHUNKS_PER_TICK pending chunks. Each PreChunk+Chunk pair is
// written as two complete TCP payloads; any failure drops the client so the
// protocol stream cannot stay half-written (that = permanent black screen).
void DrainPendingChunks(Player& p) {
  if (p.pendingChunks.empty() || p.sock == INVALID_SOCK) return;

  static uint8_t chunkBuf[CHUNK_VOLUME];
  int sent = 0;
  while (!p.pendingChunks.empty() && sent < MAX_CHUNKS_PER_TICK) {
    ChunkCoord c = p.pendingChunks.front();
    p.pendingChunks.erase(p.pendingChunks.begin());

    if (ChunkInVector(p.loadedChunks, c)) continue;
    int32_t dx = c.x - p.chunkX; if (dx < 0) dx = -dx;
    int32_t dz = c.z - p.chunkZ; if (dz < 0) dz = -dz;
    if (dx > VIEW_DISTANCE || dz > VIEW_DISTANCE) continue;

    if (!SendPreChunk(p.sock, c.x, c.z, true)) {
      DropPlayer(p, "send failed");
      return;
    }
    BuildChunkBuffer(c.x, c.z, chunkBuf);
    if (!SendChunk(p.sock, c.x, c.z, CHUNK_VOLUME, chunkBuf)) {
      DropPlayer(p, "send failed");
      return;
    }
    p.loadedChunks.push_back(c);
    sent++;
  }
}

// Loads/unloads chunks around the player. Unloads are immediate (tiny).
// New chunks are queued and drained slowly. Hysteresis avoids thrashing when
// the client's float position jitters across a chunk boundary.
void UpdatePlayerChunks(Player& p, bool forceReload = false) {
  int32_t newCX = FloorDivChunk(p.position.x, CHUNK_SIZE_X);
  int32_t newCZ = FloorDivChunk(p.position.z, CHUNK_SIZE_Z);

  if (!forceReload && newCX == p.chunkX && newCZ == p.chunkZ) return;

  // Hysteresis: only switch the centre chunk once the player is clearly
  // closer to the new chunk's centre than the old one. Stops float jitter
  // at boundaries from unloading/reloading the same columns every tick.
  if (!forceReload && p.chunkX != INT32_MIN) {
    double centreX = (static_cast<double>(newCX) + 0.5) * CHUNK_SIZE_X;
    double centreZ = (static_cast<double>(newCZ) + 0.5) * CHUNK_SIZE_Z;
    double oldCentreX = (static_cast<double>(p.chunkX) + 0.5) * CHUNK_SIZE_X;
    double oldCentreZ = (static_cast<double>(p.chunkZ) + 0.5) * CHUNK_SIZE_Z;
    double distNew = std::abs(p.position.x - centreX) + std::abs(p.position.z - centreZ);
    double distOld = std::abs(p.position.x - oldCentreX) + std::abs(p.position.z - oldCentreZ);
    if (distNew + CHUNK_HYSTERESIS * 2.0 > distOld) return;
  }

  p.chunkX = newCX;
  p.chunkZ = newCZ;

  std::vector<ChunkCoord> desired;
  desired.reserve((2 * VIEW_DISTANCE + 1) * (2 * VIEW_DISTANCE + 1));
  for (int32_t dx = -VIEW_DISTANCE; dx <= VIEW_DISTANCE; dx++) {
    for (int32_t dz = -VIEW_DISTANCE; dz <= VIEW_DISTANCE; dz++) {
      desired.push_back({newCX + dx, newCZ + dz});
    }
  }

  std::vector<ChunkCoord> stillLoaded;
  stillLoaded.reserve(p.loadedChunks.size());
  for (const auto& old : p.loadedChunks) {
    if (!ChunkInVector(desired, old)) {
      if (!SendPreChunk(p.sock, old.x, old.z, false)) {
        DropPlayer(p, "send failed");
        return;
      }
    } else {
      stillLoaded.push_back(old);
    }
  }
  p.loadedChunks = stillLoaded;

  {
    std::vector<ChunkCoord> stillPending;
    stillPending.reserve(p.pendingChunks.size());
    for (const auto& c : p.pendingChunks) {
      if (ChunkInVector(desired, c) && !ChunkInVector(p.loadedChunks, c)) {
        stillPending.push_back(c);
      }
    }
    p.pendingChunks = stillPending;
  }

  for (const auto& want : desired) {
    if (!ChunkInVector(p.loadedChunks, want) && !ChunkInVector(p.pendingChunks, want)) {
      p.pendingChunks.push_back(want);
    }
  }

  std::sort(p.pendingChunks.begin(), p.pendingChunks.end(),
            [newCX, newCZ](const ChunkCoord& a, const ChunkCoord& b) {
              return ChunkDist(a, newCX, newCZ) < ChunkDist(b, newCX, newCZ);
            });
}

// ============================================================
// Login / Handshake
// ============================================================
void SendLoginRequest(SocketType s, Player& p) {
  int32_t protocol = ReadInteger(s);
  if (protocol != 14) {
    std::cout << "Invalid protocol version: " << protocol << " (expected 14)" << std::endl;
  }
  char* unused = ReadString16(s); // username again?
  if (unused) free(unused);
  ReadLong(s);
  ReadByte(s);

  // Reply
  WriteByte(s, LoginRequest);
  WriteInteger(s, p.entityId);
  WriteString16(s, "");
  WriteLong(s, 0);
  WriteByte(s, 0); // dimension

  // Hotbar items
  SendSetSlot(s, 0, INVENTORY_HOTBAR + 0, 1, 1, 0);   // Stone
  SendSetSlot(s, 0, INVENTORY_HOTBAR + 1, 4, 1, 0);   // Cobblestone
  SendSetSlot(s, 0, INVENTORY_HOTBAR + 2, 45, 1, 0);  // Bricks
  SendSetSlot(s, 0, INVENTORY_HOTBAR + 3, 3, 1, 0);   // Dirt
  SendSetSlot(s, 0, INVENTORY_HOTBAR + 4, 5, 1, 0);   // Planks
  SendSetSlot(s, 0, INVENTORY_HOTBAR + 5, 17, 1, 0);  // Logs
  SendSetSlot(s, 0, INVENTORY_HOTBAR + 6, 18, 1, 0);  // Leaves
  SendSetSlot(s, 0, INVENTORY_HOTBAR + 7, 20, 1, 0);  // Glass
  SendSetSlot(s, 0, INVENTORY_HOTBAR + 8, 44, 1, 0);  // Slab

  // Load the chunks around the spawn point (infinite world: generated
  // on demand, streamed as the player moves via UpdatePlayerChunks).
  // They are queued and drained a few per tick — never all 25 at once.
  SetPositionToSpawn(p);
  p.chunkX = INT32_MIN;
  p.chunkZ = INT32_MIN;
  p.loadedChunks.clear();
  p.pendingChunks.clear();
  UpdatePlayerChunks(p, true);
  // Send the nearest chunks immediately so the client has ground under
  // the player before the rest of the view streams in.
  DrainPendingChunks(p);

  SendSpawnPosition(s);
  SendPlayerPosition(s, p);

  // Enhanced: make players visible to each other
  OnPlayerJoined(p);

  SendGlobalChatMessage(p.username ? p.username : "?", "has joined!", 'e');
}

void SendHandshake(SocketType s, Player& p) {
  p.username = ReadString16(s);
  p.entityId = globalEntityId++;
  std::cout << (p.username ? p.username : "?") << " has joined the game!" << std::endl;

  WriteByte(s, Handshake);
  WriteString16(s, "-");
}

// ============================================================
// Player actions
// ============================================================
void SendPlayerBlockPlacement(SocketType s, Player& p) {
  Int3 pos;
  pos.x = ReadInteger(s);
  pos.y = static_cast<int32_t>(ReadByte(s));
  pos.z = ReadInteger(s);
  int8_t face = ReadByte(s);
  int16_t id = ReadShort(s);
  if (id > -1) {
    int8_t amount = ReadByte(s);
    int16_t damage = ReadShort(s);
    if (id < 97) {
      // Give item back (infinite)
      SendSetSlot(s, 0, INVENTORY_HOTBAR + p.hotbarSlot, id, 1, damage);
      FaceOffset(pos, face);
      SendGlobalBlockUpdate(pos, static_cast<int8_t>(id), static_cast<int8_t>(damage));
    }
  }
}

void SendPlayerDigging(SocketType s, Player& p) {
  int8_t status = ReadByte(s);
  Int3 pos;
  pos.x = ReadInteger(s);
  pos.y = static_cast<int32_t>(ReadByte(s));
  pos.z = ReadInteger(s);
  int8_t face = ReadByte(s);

  uint8_t blockType = GetBlock(pos.x, pos.y, pos.z);
  if (status == 0 && blockType != 7) { // start digging, not bedrock
    SendGlobalBlockUpdate(pos, 0, 0);
  }
}

bool CommandProcessing(SocketType s, const char* message) {
  if (!message || message[0] != '/') return false;
  // Enhanced: command replies are PRIVATE (only to the sender)
  switch (message[1]) {
    case 'f':
      SendChatMessage(s, "Server", "Flash! (no LED on Windows)", '7');
      break;
    case 'h':
      SendChatMessage(s, "Server", "Commands: /f /h /list /save", '7');
      break;
    case 's': // /save - manual world save
      SaveWorld();
      SendChatMessage(s, "Server", "World saved.", 'a');
      break;
    case 'l': // /list
      {
        char buf[256] = "Players online: ";
        bool first = true;
        for (int i = 0; i < MAX_CONNECTIONS; i++) {
          if (players[i].active && players[i].username && players[i].connectionStage == Connected) {
            if (!first) strcat(buf, ", ");
            strcat(buf, players[i].username);
            first = false;
          }
        }
        if (first) strcat(buf, "(none)");
        SendChatMessage(s, "Server", buf, 'e');
      }
      break;
    default:
      SendChatMessage(s, "Server", "Unknown command. Try /h", 'c');
      break;
  }
  return true;
}

// ============================================================
// Process one client
// ============================================================
void ProcessClient(Player& p) {
  if (p.sock == INVALID_SOCK || !p.active) return;

  p.tickCounter++;

  // Stream pending chunks slowly even when the client is silent (e.g. right
  // after login). One chunk every few ticks keeps the Beta client alive.
  if (p.connectionStage == Connected) {
    if (p.tickCounter - p.lastChunkDrainTick >= CHUNK_SEND_INTERVAL) {
      p.lastChunkDrainTick = p.tickCounter;
      DrainPendingChunks(p);
    }
  }

  // Check if data available (simple select with 0 timeout)
  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(p.sock, &readfds);
  timeval tv = {0, 0};
  int sel = select(static_cast<int>(p.sock) + 1, &readfds, nullptr, nullptr, &tv);
  if (sel <= 0 || !FD_ISSET(p.sock, &readfds)) {
    // Throttled keepalive — the old code sent one every ~10–50 ms and
    // flooded the fragile Beta client.
    if (p.connectionStage == Connected &&
        p.tickCounter - p.lastKeepaliveTick >= KEEPALIVE_INTERVAL) {
      p.lastKeepaliveTick = p.tickCounter;
      if (SendAll(p.sock, "\0", 1) <= 0) {
        DropPlayer(p, "send failed");
      }
    }
    return;
  }

  // Read packet id
  char packetTypeChar;
  int n = recv(p.sock, &packetTypeChar, 1, 0);
  if (n <= 0) {
    // disconnected
    if (p.username) {
      std::cout << p.username << " has disconnected" << std::endl;
      SendGlobalChatMessage(p.username, "has left.", 'e');
      BroadcastDestroyPlayer(p);
    }
    CLOSE_SOCKET(p.sock);
    p.sock = INVALID_SOCK;
    p.active = false;
    p.connectionStage = Disconnected;
    if (p.username) { free(p.username); p.username = nullptr; }
    return;
  }

  uint8_t packetType = static_cast<uint8_t>(packetTypeChar);
  p.hasSentPacket = true;

  switch (packetType) {
    case LoginRequest:
      if (p.connectionStage == Connected) {
        // Ignore duplicate login packets (prevents desync loops)
        break;
      }
      p.connectionStage = Login;
      SendLoginRequest(p.sock, p);
      p.connectionStage = Connected;
      break;
    case Handshake:
      if (p.connectionStage >= Shake) {
        // Already handshaked, ignore
        break;
      }
      p.connectionStage = Shake;
      SendHandshake(p.sock, p);
      break;
    case ChatMessage: {
      char* message = ReadString16(p.sock);
      if (message) {
        bool isCommand = CommandProcessing(p.sock, message);
        if (!isCommand) {
          SendGlobalChatMessage(p.username ? p.username : "?", message);
        }
        free(message);
      }
      break;
    }
    case PlayerPacket:
      p.onGround = ReadByte(p.sock) != 0;
      break;
    case PlayerPosition:
      p.position.x = ReadDouble(p.sock);
      p.position.y = ReadDouble(p.sock);
      p.stance = ReadDouble(p.sock);
      p.position.z = ReadDouble(p.sock);
      p.onGround = ReadByte(p.sock) != 0;
      if (p.connectionStage == Connected) {
        UpdatePlayerChunks(p);
        DrainPendingChunks(p);
        BroadcastPlayerMovement(p);
      }
      break;
    case PlayerLook:
      p.yaw = ReadFloat(p.sock);
      p.pitch = ReadFloat(p.sock);
      p.onGround = ReadByte(p.sock) != 0;
      if (p.connectionStage == Connected) BroadcastPlayerMovement(p);
      break;
    case PlayerPositionLook:
      p.position.x = ReadDouble(p.sock);
      p.position.y = ReadDouble(p.sock);
      p.stance = ReadDouble(p.sock);
      p.position.z = ReadDouble(p.sock);
      p.yaw = ReadFloat(p.sock);
      p.pitch = ReadFloat(p.sock);
      p.onGround = ReadByte(p.sock) != 0;
      if (p.connectionStage == Connected) {
        UpdatePlayerChunks(p);
        DrainPendingChunks(p);
        BroadcastPlayerMovement(p);
      }
      break;
    case PlayerAction:
      ReadInteger(p.sock);
      ReadByte(p.sock);
      break;
    case ClickEntity: { // 0x07 – left/right click on entity (attack / interact)
      int32_t targetId = ReadInteger(p.sock);
      int8_t leftClick = ReadByte(p.sock); // 1 = left click (attack)
      // For now we just consume the packet so the stream stays in sync.
      // (No damage system yet)
      (void)targetId;
      (void)leftClick;
      break;
    }
    case PlayerBlockPlacement:
      SendPlayerBlockPlacement(p.sock, p);
      break;
    case PlayerDigging:
      SendPlayerDigging(p.sock, p);
      break;
    case ActiveSlot:
      p.hotbarSlot = static_cast<int8_t>(ReadShort(p.sock));
      break;
    case CloseWindowPacket:
      ReadByte(p.sock);
      break;
    case WindowClick:
      ReadByte(p.sock);
      ReadShort(p.sock);
      ReadByte(p.sock);
      ReadShort(p.sock);
      ReadByte(p.sock);
      ReadShort(p.sock);
      ReadByte(p.sock);
      ReadShort(p.sock);
      break;
    case EntityAction: {
      ReadInteger(p.sock);
      ReadByte(p.sock);
      break;
    }
    case Disconnect: {
      char* message = ReadString16(p.sock);
      if (p.username) {
        std::cout << p.username << " has disconnected! ("
                  << (message ? message : "") << ")" << std::endl;
        SendGlobalChatMessage(p.username, "has left.", 'e');
        BroadcastDestroyPlayer(p);
      }
      if (message) free(message);
      CLOSE_SOCKET(p.sock);
      p.sock = INVALID_SOCK;
      p.active = false;
      p.connectionStage = Disconnected;
      if (p.username) { free(p.username); p.username = nullptr; }
      break;
    }
    case KeepAlive:
      // Client echoed keepalive — no need to spam one back every time
      break;
    default:
      std::cout << "Unhandled Packet: 0x" << std::hex << static_cast<int>(packetType)
                << std::dec << " from " << (p.username ? p.username : "?") << " – disconnecting to avoid desync" << std::endl;
      // Unknown packet = stream likely desynced. Disconnect cleanly.
      if (p.username) {
        SendGlobalChatMessage(p.username, "has left (protocol error).", 'e');
        BroadcastDestroyPlayer(p);
      }
      CLOSE_SOCKET(p.sock);
      p.sock = INVALID_SOCK;
      p.active = false;
      p.connectionStage = Disconnected;
      if (p.username) { free(p.username); p.username = nullptr; }
      return;

  }

  if (p.connectionStage == Connected) {
    // Void protection
    if (p.position.y < 0) {
      SetPositionToSpawn(p);
      SendPlayerPosition(p.sock, p);
    }
  }
}

// ============================================================
// Main
// ============================================================
int main() {
#ifdef _WIN32
  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
    std::cerr << "WSAStartup failed" << std::endl;
    return 1;
  }
#endif

  signal(SIGINT, HandleSignal);
  signal(SIGTERM, HandleSignal);
#ifdef _WIN32
  SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#else
  // A player closing their connection uncleanly (crash, alt-F4, network
  // drop) can make a later send() hit a broken pipe; without this the
  // default SIGPIPE action would kill the whole server for everyone.
  signal(SIGPIPE, SIG_IGN);
#endif

  std::cout << "Infinite flat world (chunk " << CHUNK_SIZE_X << "x" << CHUNK_SIZE_Y << "x" << CHUNK_SIZE_Z
            << ", view distance " << VIEW_DISTANCE << ")" << std::endl;
  LoadWorld();

  SocketType serverSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (serverSock == INVALID_SOCK) {
    std::cerr << "socket() failed" << std::endl;
#ifdef _WIN32
    WSACleanup();
#endif
    return 1;
  }

  // Reuse address
  int opt = 1;
#ifdef _WIN32
  setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
  setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(SERVER_PORT);

  if (bind(serverSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCK_ERR) {
    std::cerr << "bind() failed on port " << SERVER_PORT << std::endl;
    CLOSE_SOCKET(serverSock);
#ifdef _WIN32
    WSACleanup();
#endif
    return 1;
  }

  if (listen(serverSock, MAX_CONNECTIONS) == SOCK_ERR) {
    std::cerr << "listen() failed" << std::endl;
    CLOSE_SOCKET(serverSock);
#ifdef _WIN32
    WSACleanup();
#endif
    return 1;
  }

  std::cout << "PicoBeta server listening on 0.0.0.0:" << SERVER_PORT << std::endl;
  std::cout << "Minecraft Beta 1.7.3 (protocol 14) - max " << MAX_CONNECTIONS << " players" << std::endl;
  std::cout << "Connect with: 127.0.0.1:" << SERVER_PORT << std::endl;

  // Main loop
  int autosaveTicks = 0;
  while (g_running) {
    // Accept new connections (non-blocking style with select)
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(serverSock, &readfds);
    SocketType maxfd = serverSock;

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
      if (players[i].active && players[i].sock != INVALID_SOCK) {
        FD_SET(players[i].sock, &readfds);
        if (players[i].sock > maxfd) maxfd = players[i].sock;
      }
    }

    timeval tv = {0, 50000}; // 50ms
    int sel = select(static_cast<int>(maxfd) + 1, &readfds, nullptr, nullptr, &tv);

    if (sel > 0 && FD_ISSET(serverSock, &readfds)) {
      sockaddr_in clientAddr{};
#ifdef _WIN32
      int clientLen = sizeof(clientAddr);
#else
      socklen_t clientLen = sizeof(clientAddr);
#endif
      SocketType clientSock = accept(serverSock, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
      if (clientSock != INVALID_SOCK) {
        // Non-blocking + Nagle off so a lagging Beta client never freezes
        // the whole server on a blocked send(), and small packets leave ASAP.
        SetNonBlocking(clientSock);
        SetTcpNoDelay(clientSock);
        bool added = false;
        for (int i = 0; i < MAX_CONNECTIONS; i++) {
          if (!players[i].active) {
            players[i] = Player{}; // reset
            players[i].sock = clientSock;
            players[i].active = true;
            players[i].connectionStage = TryingToConnect;
            char ip[64];
#ifdef _WIN32
            // inet_ntop() is only exported by ws2_32.dll on Windows Vista and later.
            // Use inet_ntoa() (Winsock 1.1, IPv4-only) so the binary still loads on Windows XP.
            {
                const char* ipStr = inet_ntoa(clientAddr.sin_addr);
                std::snprintf(ip, sizeof(ip), "%s", ipStr ? ipStr : "?");
            }
#else
            inet_ntop(AF_INET, &clientAddr.sin_addr, ip, sizeof(ip));
#endif
            std::cout << "New client connected from " << ip << std::endl;
            added = true;
            break;
          }
        }
        if (!added) {
          SendDisconnect(clientSock, "Server is full!");
          CLOSE_SOCKET(clientSock);
          std::cout << "Server full, connection rejected" << std::endl;
        }
      }
    }

    // Process clients
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
      if (players[i].active) {
        ProcessClient(players[i]);
      }
    }

    // Periodic autosave (only writes to disk if something actually changed)
    if (worldDirty) {
      autosaveTicks++;
      if (autosaveTicks >= AUTOSAVE_TICKS) {
        autosaveTicks = 0;
        SaveWorld();
      }
    } else {
      autosaveTicks = 0;
    }

    // Small sleep to avoid 100% CPU
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  std::cout << std::endl << "Shutting down, saving world..." << std::endl;
  SaveWorld();

  CLOSE_SOCKET(serverSock);
#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}
