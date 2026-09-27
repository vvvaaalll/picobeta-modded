/*
 * PicoBeta - Minimal Minecraft Beta 1.7.3 server
 * Ported to native Windows (cross-compilable from WSL with MinGW-w64)
 * Original: Arduino / Raspberry Pi Pico WiFi version
 *
 * Protocol version 14 (Minecraft Beta 1.7.3)
 * World: 16x16x16
 * Max players: 4
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
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
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
#include <string>
#include <vector>
#include <chrono>
#include <thread>

// ============================================================
// Config
// ============================================================
#define WORLD_SIZE_X 16
#define WORLD_SIZE_Y 16
#define WORLD_SIZE_Z 16
const int32_t MAX_WORLD_SIZE = WORLD_SIZE_X * WORLD_SIZE_Y * WORLD_SIZE_Z;
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
  WORLD_SIZE_X / 2,
  WORLD_SIZE_Y / 2,
  WORLD_SIZE_Z / 2
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
};

Player players[MAX_CONNECTIONS];
int32_t globalEntityId = 0;
uint8_t world[MAX_WORLD_SIZE];

// ============================================================
// Network helpers (blocking-ish reads, simple)
// ============================================================
bool SetNonBlocking(SocketType s) {
#ifdef _WIN32
  u_long mode = 1;
  return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
  int flags = fcntl(s, F_GETFL, 0);
  return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

int RecvExact(SocketType s, char* buf, int len) {
  int total = 0;
  while (total < len) {
    int n = recv(s, buf + total, len - total, 0);
    if (n <= 0) return n; // error or closed
    total += n;
  }
  return total;
}

int SendAll(SocketType s, const char* buf, int len) {
  int total = 0;
  while (total < len) {
    int n = send(s, buf + total, len - total, 0);
    if (n <= 0) return n;
    total += n;
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
// World
// ============================================================
int32_t GetBlockIndex(int32_t x, int32_t y, int32_t z) {
  // Protocol 0x33 layout: X planes, each with Z rows, each row Size_Y blocks (Y fastest)
  // index = x * (Y * Z) + z * Y + y
  if (x < 0 || x >= WORLD_SIZE_X || y < 0 || y >= WORLD_SIZE_Y || z < 0 || z >= WORLD_SIZE_Z) {
    return 0;
  }
  int32_t index = x * (WORLD_SIZE_Y * WORLD_SIZE_Z) + z * WORLD_SIZE_Y + y;
  if (index < 0 || index >= MAX_WORLD_SIZE) {
    std::cerr << "Invalid block index " << index << "/" << MAX_WORLD_SIZE << std::endl;
    return 0;
  }
  return index;
}

void FillWorld() {
  for (int32_t x = 0; x < WORLD_SIZE_X; x++) {
    for (int32_t z = 0; z < WORLD_SIZE_Z; z++) {
      for (int16_t y = 0; y < WORLD_SIZE_Y; y++) {
        int32_t index = GetBlockIndex(x, y, z);
        if (y == 0) {
          world[index] = 7; // bedrock
        } else if (y < WORLD_SIZE_Y / 3) {
          world[index] = 1; // stone
        } else if (y < WORLD_SIZE_Y / 2) {
          world[index] = 3; // dirt
        } else if (y == WORLD_SIZE_Y / 2) {
          world[index] = 2; // grass
        } else {
          world[index] = 0; // air
        }
      }
    }
  }
}

// ============================================================
// Send helpers
// ============================================================
void SendPreChunk(SocketType s, int32_t x, int32_t z, bool mode) {
  WriteByte(s, PreChunk);
  WriteInteger(s, x);
  WriteInteger(s, z);
  WriteByte(s, mode ? 1 : 0);
}

void SendChunk(SocketType s, int32_t x, int32_t z, int32_t dataSize, uint8_t* data) {
  WriteByte(s, Chunk);
  WriteInteger(s, x);
  WriteShort(s, 0); // Y
  WriteInteger(s, z);
  WriteByte(s, static_cast<int8_t>(WORLD_SIZE_X - 1));
  WriteByte(s, static_cast<int8_t>(WORLD_SIZE_Y - 1));
  WriteByte(s, static_cast<int8_t>(WORLD_SIZE_Z - 1));

  int32_t trueSize = static_cast<int32_t>(dataSize * 2.5);

  // Size of the chunk + zlib bytes
  WriteInteger(s, trueSize + 11);

  // Zlib header (no compression)
  WriteByte(s, 0x78); // CMF
  WriteByte(s, 0x01); // FLG

  // Raw uncompressed data block
  WriteByte(s, 0x01); // Final + Type 00

  // Length
  WriteByte(s, trueSize & 0xFF);
  WriteByte(s, (trueSize >> 8) & 0xFF);
  // Ones Complement Length
  WriteByte(s, (~trueSize) & 0xFF);
  WriteByte(s, (~trueSize >> 8) & 0xFF);

  // Interleaved Adler32
  uint32_t A = 1;
  uint32_t B = 0;

  // Block data
  for (int32_t i = 0; i < dataSize; i++) {
    WriteByte(s, data[i]);
    A = (A + data[i]) % 65521;
    B = (B + A) % 65521;
  }
  // Metadata (zeros)
  for (int32_t i = 0; i < dataSize / 2; i++) {
    WriteByte(s, 0);
    B = (B + A) % 65521;
  }
  // Lighting (full bright)
  for (int32_t i = 0; i < dataSize; i++) {
    WriteByte(s, 0xFF);
    A = (A + 0xFF) % 65521;
    B = (B + A) % 65521;
  }

  // Adler32
  WriteInteger(s, static_cast<int32_t>((B << 16) | A));
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

void SendChatMessage(SocketType s, const char* sender, const char* message, char color = 'r') {
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
    std::cout << fullMessage << std::endl;
    WriteByte(s, ChatMessage);
    WriteString16(s, fullMessage);
    free(fullMessage);
  }
}

void SendGlobalChatMessage(const char* sender, const char* message, char color = 'r') {
  for (int i = 0; i < MAX_CONNECTIONS; i++) {
    if (players[i].active && players[i].sock != INVALID_SOCK) {
      SendChatMessage(players[i].sock, sender, message, color);
    }
  }
}

void SendBlockUpdate(SocketType s, Int3 pos, int8_t type, int8_t meta) {
  if (pos.x < 0 || pos.x >= WORLD_SIZE_X || pos.y < 0 || pos.y >= WORLD_SIZE_Y || pos.z < 0 || pos.z >= WORLD_SIZE_Z) {
    SendChatMessage(s, "Server", "Out of bounds!", 'c');
    type = 0;
    meta = 0;
  }
  WriteByte(s, BlockUpdate);
  WriteInteger(s, pos.x);
  WriteByte(s, static_cast<int8_t>(pos.y));
  WriteInteger(s, pos.z);
  WriteByte(s, type);
  WriteByte(s, meta);
  int32_t index = GetBlockIndex(pos.x, pos.y, pos.z);
  world[index] = static_cast<uint8_t>(type);
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

  // Pre-chunks around 0,0
  for (int32_t x = -1; x <= 1; x++) {
    for (int32_t z = -1; z <= 1; z++) {
      SendPreChunk(s, x, z, true);
    }
  }
  SendChunk(s, 0, 0, MAX_WORLD_SIZE, world);

  SendSpawnPosition(s);
  SetPositionToSpawn(p);
  SendPlayerPosition(s, p);

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

  uint8_t blockType = world[GetBlockIndex(pos.x, pos.y, pos.z)];
  if (status == 0 && blockType != 7) { // start digging, not bedrock
    SendGlobalBlockUpdate(pos, 0, 0);
  }
}

bool CommandProcessing(SocketType s, const char* message) {
  if (!message || message[0] != '/') return false;
  switch (message[1]) {
    case 'f':
      // Flash LED equivalent: just message
      SendGlobalChatMessage("Server", "Flash! (no LED on Windows)", '7');
      break;
    case 'h':
      SendGlobalChatMessage("Server", "Commands: /f (flash), /h (help)", '7');
      break;
    default:
      break;
  }
  return true;
}

// ============================================================
// Process one client
// ============================================================
void ProcessClient(Player& p) {
  if (p.sock == INVALID_SOCK || !p.active) return;

  // Check if data available (simple select with 0 timeout)
  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(p.sock, &readfds);
  timeval tv = {0, 0};
  int sel = select(static_cast<int>(p.sock) + 1, &readfds, nullptr, nullptr, &tv);
  if (sel <= 0 || !FD_ISSET(p.sock, &readfds)) {
    // Keepalive when connected
    if (p.connectionStage == Connected) {
      WriteByte(p.sock, KeepAlive);
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
      p.connectionStage = Login;
      SendLoginRequest(p.sock, p);
      p.connectionStage = Connected;
      break;
    case Handshake:
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
      break;
    case PlayerLook:
      p.yaw = ReadFloat(p.sock);
      p.pitch = ReadFloat(p.sock);
      p.onGround = ReadByte(p.sock) != 0;
      break;
    case PlayerPositionLook:
      p.position.x = ReadDouble(p.sock);
      p.position.y = ReadDouble(p.sock);
      p.stance = ReadDouble(p.sock);
      p.position.z = ReadDouble(p.sock);
      p.yaw = ReadFloat(p.sock);
      p.pitch = ReadFloat(p.sock);
      p.onGround = ReadByte(p.sock) != 0;
      break;
    case PlayerAction:
      ReadInteger(p.sock);
      ReadByte(p.sock);
      break;
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
      if (p.connectionStage == Connected) {
        WriteByte(p.sock, KeepAlive);
      }
      break;
    default:
      std::cout << "Unhandled Packet: 0x" << std::hex << static_cast<int>(packetType)
                << std::dec << std::endl;
      // Try to keep connection alive
      if (p.connectionStage == Connected) {
        WriteByte(p.sock, KeepAlive);
      }
      break;
  }

  if (p.connectionStage == Connected) {
    WriteByte(p.sock, KeepAlive);
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

  std::cout << "Generating world (" << WORLD_SIZE_X << "x" << WORLD_SIZE_Y << "x" << WORLD_SIZE_Z << ")..." << std::endl;
  FillWorld();
  std::cout << "World generated!" << std::endl;

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
  while (true) {
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

    // Small sleep to avoid 100% CPU
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  CLOSE_SOCKET(serverSock);
#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}
