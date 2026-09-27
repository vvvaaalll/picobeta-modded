import socket
import struct
import threading
import select
import time
import os
import math

# ============================================================
# CONFIG
# ============================================================

HOST = "0.0.0.0"
PORT = 25565

CHUNK_SIZE_X = 16
CHUNK_SIZE_Y = 16
CHUNK_SIZE_Z = 16
CHUNK_VOLUME = CHUNK_SIZE_X * CHUNK_SIZE_Y * CHUNK_SIZE_Z

VIEW_DISTANCE = 2          # 5x5 grid of chunks
MAX_CHUNKS_PER_TICK = 1
CHUNK_SEND_INTERVAL = 0.05  # seconds between draining pending chunks
KEEPALIVE_INTERVAL = 1.0    # seconds
CHUNK_HYSTERESIS = 2.0

WORLD_SAVE_FILE = "world.pbw"
AUTOSAVE_INTERVAL = 30.0    # seconds

MAX_CONNECTIONS = 4

SPAWN_X = CHUNK_SIZE_X // 2
SPAWN_Y = CHUNK_SIZE_Y // 2
SPAWN_Z = CHUNK_SIZE_Z // 2

INVENTORY_HOTBAR = 36

# ============================================================
# PACKETS (Minecraft Beta 1.7.3 / protocol 14)
# ============================================================

KEEP_ALIVE = 0x00
LOGIN_REQUEST = 0x01
HANDSHAKE = 0x02
CHAT_MESSAGE = 0x03
TIME_UPDATE = 0x04
SPAWN_POSITION = 0x06
SET_HEALTH = 0x08
PLAYER_PACKET = 0x0A
PLAYER_POSITION = 0x0B
PLAYER_LOOK = 0x0C
PLAYER_POSITION_LOOK = 0x0D
PLAYER_DIGGING = 0x0E
PLAYER_BLOCK_PLACEMENT = 0x0F
ACTIVE_SLOT = 0x10
PLAYER_ACTION = 0x12
ENTITY_ACTION = 0x13
SPAWN_PLAYER_ENTITY = 0x14
DESTROY_ENTITY = 0x1D
ENTITY_TELEPORT = 0x22
PRE_CHUNK = 0x32
CHUNK = 0x33
BLOCK_UPDATE = 0x35
EFFECT = 0x3D
SET_SLOT = 0x67
DISCONNECT = 0xFF

# ============================================================
# BLOCKS
# ============================================================

AIR = 0
STONE = 1
GRASS = 2
DIRT = 3
BEDROCK = 7

# ============================================================
# WORLD (infinite, flat, sparse overrides)
# ============================================================

# key -> block type ; only blocks that differ from GenerateBlock(y)
block_overrides = {}
world_dirty = False
world_lock = threading.Lock()


def generate_block(y):
    """Pure function of Y: same flat profile for every chunk."""
    if y == 0:
        return BEDROCK
    if y < CHUNK_SIZE_Y // 3:
        return STONE
    if y < CHUNK_SIZE_Y // 2:
        return DIRT
    if y == CHUNK_SIZE_Y // 2:
        return GRASS
    return AIR


def block_key(x, y, z):
    """Pack world coords into a 64-bit key (same layout as C++ Enhanced)."""
    ux = (x + 0x8000000) & 0xFFFFFFF
    uz = (z + 0x8000000) & 0xFFFFFFF
    uy = y & 0xF
    return (ux << 36) | (uz << 4) | uy


def get_block(x, y, z):
    if y < 0 or y >= CHUNK_SIZE_Y:
        return AIR
    key = block_key(x, y, z)
    with world_lock:
        if key in block_overrides:
            return block_overrides[key]
    return generate_block(y)


def set_block(x, y, z, block_type):
    global world_dirty
    if y < 0 or y >= CHUNK_SIZE_Y:
        return
    key = block_key(x, y, z)
    natural = generate_block(y)
    with world_lock:
        if block_type == natural:
            block_overrides.pop(key, None)
        else:
            block_overrides[key] = block_type
        world_dirty = True


def build_chunk_buffer(cx, cz):
    """Fill a CHUNK_VOLUME-byte buffer for one chunk (same index layout as C++)."""
    buf = bytearray(CHUNK_VOLUME)
    for lx in range(CHUNK_SIZE_X):
        for lz in range(CHUNK_SIZE_Z):
            for ly in range(CHUNK_SIZE_Y):
                idx = lx * (CHUNK_SIZE_Y * CHUNK_SIZE_Z) + lz * CHUNK_SIZE_Y + ly
                buf[idx] = get_block(
                    cx * CHUNK_SIZE_X + lx,
                    ly,
                    cz * CHUNK_SIZE_Z + lz,
                )
    return buf


def save_world():
    global world_dirty
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)) or ".", WORLD_SAVE_FILE)
    with world_lock:
        count = len(block_overrides)
        entries = list(block_overrides.items())
    try:
        with open(path, "wb") as f:
            f.write(b"PBW1")
            f.write(struct.pack("<I", count))  # little-endian like typical C++ on x86
            for key, typ in entries:
                x = ((key >> 36) & 0xFFFFFFF) - 0x8000000
                z = ((key >> 4) & 0xFFFFFFF) - 0x8000000
                y = key & 0xF
                # C++ writes int32 x, int32 z, int8 y, uint8 type (native endian)
                f.write(struct.pack("<iiBb", x, z, y, typ))
        with world_lock:
            world_dirty = False
        print(f"World saved ({count} modified blocks) -> {WORLD_SAVE_FILE}")
    except OSError as e:
        print(f"Failed to save world: {e}")


def load_world():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)) or ".", WORLD_SAVE_FILE)
    if not os.path.isfile(path):
        print("No existing world save found, starting fresh.")
        return
    try:
        with open(path, "rb") as f:
            magic = f.read(4)
            if magic != b"PBW1":
                print("Invalid or corrupt world save file, ignoring.")
                return
            count = struct.unpack("<I", f.read(4))[0]
            with world_lock:
                block_overrides.clear()
                for _ in range(count):
                    data = f.read(4 + 4 + 1 + 1)
                    if len(data) < 10:
                        break
                    x, z, y, typ = struct.unpack("<iiBb", data)
                    y = y & 0xF
                    block_overrides[block_key(x, y, z)] = typ
        print(f"World loaded ({count} modified blocks) from {WORLD_SAVE_FILE}")
    except OSError as e:
        print(f"Failed to load world: {e}")


# ============================================================
# NETWORK HELPERS
# ============================================================

def recv_exact(sock, n):
    data = bytearray()
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise ConnectionError("Connection closed")
        data.extend(chunk)
    return bytes(data)


def send_all(sock, data):
    total = 0
    while total < len(data):
        n = sock.send(data[total:])
        if n == 0:
            raise ConnectionError("Connection closed")
        total += n


def read_byte(sock):
    return struct.unpack(">b", recv_exact(sock, 1))[0]


def read_ubyte(sock):
    return struct.unpack(">B", recv_exact(sock, 1))[0]


def write_byte(sock, value):
    send_all(sock, struct.pack(">b", value))


def write_ubyte(sock, value):
    send_all(sock, struct.pack(">B", value & 0xFF))


def read_short(sock):
    return struct.unpack(">h", recv_exact(sock, 2))[0]


def write_short(sock, value):
    send_all(sock, struct.pack(">h", value))


def read_int(sock):
    return struct.unpack(">i", recv_exact(sock, 4))[0]


def write_int(sock, value):
    send_all(sock, struct.pack(">i", value))


def read_long(sock):
    return struct.unpack(">q", recv_exact(sock, 8))[0]


def write_long(sock, value):
    send_all(sock, struct.pack(">q", value))


def read_float(sock):
    return struct.unpack(">f", recv_exact(sock, 4))[0]


def write_float(sock, value):
    send_all(sock, struct.pack(">f", value))


def read_double(sock):
    return struct.unpack(">d", recv_exact(sock, 8))[0]


def write_double(sock, value):
    send_all(sock, struct.pack(">d", value))


def read_string16(sock):
    length = read_short(sock)
    if length < 0 or length > 1024:
        raise ValueError(f"Invalid String16 length: {length}")
    chars = bytearray()
    for _ in range(length):
        _ = read_byte(sock)  # high byte
        low = read_ubyte(sock)
        chars.append(low)
    return chars.decode("latin-1", errors="replace")


def write_string16(sock, message):
    encoded = message.encode("latin-1", errors="replace")
    write_short(sock, len(encoded))
    for value in encoded:
        write_byte(sock, 0)
        write_ubyte(sock, value)


def write_packet_id(sock, packet_id):
    write_ubyte(sock, packet_id)


# ============================================================
# PLAYER
# ============================================================

class Player:
    def __init__(self, sock):
        self.sock = sock
        self.entity_id = 0
        self.username = None
        self.x = 0.0
        self.y = 0.0
        self.z = 0.0
        self.yaw = 0.0
        self.pitch = 0.0
        self.stance = 1.5
        self.on_ground = False
        self.hotbar_slot = 0
        self.stage = "TryingToConnect"
        self.active = True
        # Movement broadcast throttle
        self.last_broadcast_x = 0.0
        self.last_broadcast_y = 0.0
        self.last_broadcast_z = 0.0
        self.last_broadcast_yaw = 0.0
        self.last_broadcast_pitch = 0.0
        # Infinite world chunk tracking
        self.chunk_x = None
        self.chunk_z = None
        self.loaded_chunks = set()   # (cx, cz)
        self.pending_chunks = []     # list of (cx, cz), nearest first
        self.last_chunk_drain = 0.0
        self.last_keepalive = 0.0
        self.lock = threading.Lock()


players = []
players_lock = threading.Lock()
global_entity_id = 1


def set_position_spawn(player):
    player.x = float(SPAWN_X)
    player.y = float(SPAWN_Y) + 3.5
    player.z = float(SPAWN_Z)


def face_offset(x, y, z, face):
    if face == 0:
        y -= 1
    elif face == 1:
        y += 1
    elif face == 2:
        z -= 1
    elif face == 3:
        z += 1
    elif face == 4:
        x -= 1
    elif face == 5:
        x += 1
    return x, y, z


def floor_div_chunk(v, size):
    """Floor division that works correctly for negative coords."""
    return math.floor(v / size)


# ============================================================
# SEND PACKETS
# ============================================================

def send_pre_chunk(sock, cx, cz, mode):
    write_packet_id(sock, PRE_CHUNK)
    write_int(sock, cx)
    write_int(sock, cz)
    write_byte(sock, 1 if mode else 0)


def send_chunk(sock, cx, cz, data):
    """
    Same zlib-stored layout as PicoBeta Enhanced / Tiny:
    blocks (dataSize) + metadata (dataSize/2 zeros) + lighting (dataSize 0xFF)
    """
    data_size = len(data)
    true_size = int(data_size * 2.5)

    write_packet_id(sock, CHUNK)
    write_int(sock, cx * CHUNK_SIZE_X)  # block X origin (Enhanced fixed the freeze)
    write_short(sock, 0)                # Y
    write_int(sock, cz * CHUNK_SIZE_Z)  # block Z origin
    write_byte(sock, CHUNK_SIZE_X - 1)
    write_byte(sock, CHUNK_SIZE_Y - 1)
    write_byte(sock, CHUNK_SIZE_Z - 1)
    write_int(sock, true_size + 11)

    # zlib header + stored deflate block
    write_ubyte(sock, 0x78)
    write_ubyte(sock, 0x01)
    write_ubyte(sock, 0x01)  # BFINAL=1, BTYPE=00
    write_ubyte(sock, true_size & 0xFF)
    write_ubyte(sock, (true_size >> 8) & 0xFF)
    write_ubyte(sock, (~true_size) & 0xFF)
    write_ubyte(sock, ((~true_size) >> 8) & 0xFF)

    A = 1
    B = 0

    for v in data:
        write_ubyte(sock, v)
        A = (A + v) % 65521
        B = (B + A) % 65521

    for _ in range(data_size // 2):
        write_ubyte(sock, 0)
        B = (B + A) % 65521

    for _ in range(data_size):
        write_ubyte(sock, 0xFF)
        A = (A + 0xFF) % 65521
        B = (B + A) % 65521

    write_int(sock, (B << 16) | A)


def send_player_position(sock, player):
    write_packet_id(sock, PLAYER_POSITION_LOOK)
    write_double(sock, player.x)
    write_double(sock, player.y)
    write_double(sock, player.stance)
    write_double(sock, player.z)
    write_float(sock, player.yaw)
    write_float(sock, player.pitch)
    write_byte(sock, 1 if player.on_ground else 0)


def send_spawn_position(sock):
    write_packet_id(sock, SPAWN_POSITION)
    write_int(sock, SPAWN_X)
    write_int(sock, SPAWN_Y)
    write_int(sock, SPAWN_Z)


def send_chat_message(sock, sender, message, color="r"):
    # §c<sender> message
    full = f"\xa7{color}<{sender}> {message}"
    write_packet_id(sock, CHAT_MESSAGE)
    write_string16(sock, full)


def send_global_chat(sender, message, color="r"):
    print(f"<{sender}> {message}")
    with players_lock:
        current = list(players)
    for p in current:
        if not p.active:
            continue
        try:
            send_chat_message(p.sock, sender, message, color)
        except Exception:
            pass


def send_block_update(sock, x, y, z, block_type, meta=0):
    if y < 0 or y >= CHUNK_SIZE_Y:
        send_chat_message(sock, "Server", "Out of bounds!", "c")
        block_type = 0
        meta = 0
    else:
        set_block(x, y, z, block_type)
    write_packet_id(sock, BLOCK_UPDATE)
    write_int(sock, x)
    write_byte(sock, y)
    write_int(sock, z)
    write_byte(sock, block_type)
    write_byte(sock, meta)


def send_effect(sock, effect_id, x, y, z, data):
    write_packet_id(sock, EFFECT)
    write_int(sock, effect_id)
    write_int(sock, x)
    write_byte(sock, y)
    write_int(sock, z)
    write_int(sock, data)


def send_global_block_update(x, y, z, block_type, meta=0):
    with players_lock:
        current = list(players)
    for p in current:
        if not p.active:
            continue
        try:
            send_block_update(p.sock, x, y, z, block_type, meta)
            send_effect(p.sock, 2001, x, y, z, block_type)
        except Exception:
            pass


def send_set_slot(sock, window, slot, item_id, amount, damage):
    write_packet_id(sock, SET_SLOT)
    write_byte(sock, window)
    write_short(sock, slot)
    write_short(sock, item_id)
    write_byte(sock, amount)
    write_short(sock, damage)


def send_disconnect(sock, message):
    try:
        write_packet_id(sock, DISCONNECT)
        write_string16(sock, message)
    except Exception:
        pass


def to_fixed(v):
    return int(v * 32.0)


def send_spawn_player(to_sock, who):
    if not who.username:
        return
    write_packet_id(to_sock, SPAWN_PLAYER_ENTITY)
    write_int(to_sock, who.entity_id)
    write_string16(to_sock, who.username)
    write_int(to_sock, to_fixed(who.x))
    write_int(to_sock, to_fixed(who.y))
    write_int(to_sock, to_fixed(who.z))
    write_byte(to_sock, int(who.yaw * 256.0 / 360.0) & 0xFF)
    write_byte(to_sock, int(who.pitch * 256.0 / 360.0) & 0xFF)
    write_short(to_sock, 0)  # current item


def send_destroy_entity(to_sock, entity_id):
    write_packet_id(to_sock, DESTROY_ENTITY)
    write_int(to_sock, entity_id)


def send_entity_teleport(to_sock, who):
    write_packet_id(to_sock, ENTITY_TELEPORT)
    write_int(to_sock, who.entity_id)
    write_int(to_sock, to_fixed(who.x))
    write_int(to_sock, to_fixed(who.y))
    write_int(to_sock, to_fixed(who.z))
    write_byte(to_sock, int(who.yaw * 256.0 / 360.0) & 0xFF)
    write_byte(to_sock, int(who.pitch * 256.0 / 360.0) & 0xFF)


def broadcast_spawn_player(who):
    with players_lock:
        current = list(players)
    for p in current:
        if not p.active or p.entity_id == who.entity_id:
            continue
        try:
            send_spawn_player(p.sock, who)
        except Exception:
            pass


def broadcast_destroy_player(who):
    with players_lock:
        current = list(players)
    for p in current:
        if not p.active or p.entity_id == who.entity_id:
            continue
        try:
            send_destroy_entity(p.sock, who.entity_id)
        except Exception:
            pass


def broadcast_player_movement(who):
    dx = who.x - who.last_broadcast_x
    dy = who.y - who.last_broadcast_y
    dz = who.z - who.last_broadcast_z
    dyaw = who.yaw - who.last_broadcast_yaw
    dpitch = who.pitch - who.last_broadcast_pitch
    if abs(dx) < 0.05 and abs(dy) < 0.05 and abs(dz) < 0.05 and abs(dyaw) < 1.0 and abs(dpitch) < 1.0:
        return
    who.last_broadcast_x = who.x
    who.last_broadcast_y = who.y
    who.last_broadcast_z = who.z
    who.last_broadcast_yaw = who.yaw
    who.last_broadcast_pitch = who.pitch
    with players_lock:
        current = list(players)
    for p in current:
        if not p.active or p.entity_id == who.entity_id:
            continue
        try:
            send_entity_teleport(p.sock, who)
        except Exception:
            pass


def on_player_joined(player):
    player.last_broadcast_x = player.x
    player.last_broadcast_y = player.y
    player.last_broadcast_z = player.z
    player.last_broadcast_yaw = player.yaw
    player.last_broadcast_pitch = player.pitch
    with players_lock:
        current = list(players)
    # Show existing players to the new one
    for p in current:
        if (
            p.active
            and p.entity_id != player.entity_id
            and p.stage == "Connected"
            and p.username
        ):
            try:
                send_spawn_player(player.sock, p)
            except Exception:
                pass
    # Show the new player to everyone else
    broadcast_spawn_player(player)


# ============================================================
# CHUNK STREAMING
# ============================================================

def drain_pending_chunks(player):
    if not player.pending_chunks or not player.active:
        return
    sent = 0
    while player.pending_chunks and sent < MAX_CHUNKS_PER_TICK:
        cx, cz = player.pending_chunks.pop(0)
        if (cx, cz) in player.loaded_chunks:
            continue
        if player.chunk_x is not None:
            dx = abs(cx - player.chunk_x)
            dz = abs(cz - player.chunk_z)
            if dx > VIEW_DISTANCE or dz > VIEW_DISTANCE:
                continue
        try:
            send_pre_chunk(player.sock, cx, cz, True)
            buf = build_chunk_buffer(cx, cz)
            send_chunk(player.sock, cx, cz, buf)
            player.loaded_chunks.add((cx, cz))
            sent += 1
        except Exception:
            raise


def update_player_chunks(player, force_reload=False):
    new_cx = floor_div_chunk(player.x, CHUNK_SIZE_X)
    new_cz = floor_div_chunk(player.z, CHUNK_SIZE_Z)

    if not force_reload and player.chunk_x == new_cx and player.chunk_z == new_cz:
        return

    # Hysteresis
    if not force_reload and player.chunk_x is not None:
        centre_x = (new_cx + 0.5) * CHUNK_SIZE_X
        centre_z = (new_cz + 0.5) * CHUNK_SIZE_Z
        old_centre_x = (player.chunk_x + 0.5) * CHUNK_SIZE_X
        old_centre_z = (player.chunk_z + 0.5) * CHUNK_SIZE_Z
        dist_new = abs(player.x - centre_x) + abs(player.z - centre_z)
        dist_old = abs(player.x - old_centre_x) + abs(player.z - old_centre_z)
        if dist_new + CHUNK_HYSTERESIS * 2.0 > dist_old:
            return

    player.chunk_x = new_cx
    player.chunk_z = new_cz

    desired = set()
    for dx in range(-VIEW_DISTANCE, VIEW_DISTANCE + 1):
        for dz in range(-VIEW_DISTANCE, VIEW_DISTANCE + 1):
            desired.add((new_cx + dx, new_cz + dz))

    # Unload
    still_loaded = set()
    for old in list(player.loaded_chunks):
        if old not in desired:
            try:
                send_pre_chunk(player.sock, old[0], old[1], False)
            except Exception:
                raise
        else:
            still_loaded.add(old)
    player.loaded_chunks = still_loaded

    # Filter pending
    player.pending_chunks = [
        c for c in player.pending_chunks
        if c in desired and c not in player.loaded_chunks
    ]

    # Queue new
    for want in desired:
        if want not in player.loaded_chunks and want not in player.pending_chunks:
            player.pending_chunks.append(want)

    # Sort by distance to player centre
    def chunk_dist(c):
        return abs(c[0] - new_cx) + abs(c[1] - new_cz)

    player.pending_chunks.sort(key=chunk_dist)


# ============================================================
# LOGIN / HANDSHAKE
# ============================================================

def send_login_request(sock, player):
    protocol = read_int(sock)
    if protocol != 14:
        print(f"Invalid protocol version: {protocol} (expected 14)")
    _ = read_string16(sock)  # username again?
    read_long(sock)
    read_byte(sock)

    write_packet_id(sock, LOGIN_REQUEST)
    write_int(sock, player.entity_id)
    write_string16(sock, "")
    write_long(sock, 0)
    write_byte(sock, 0)  # dimension

    # Hotbar (same as Enhanced)
    send_set_slot(sock, 0, INVENTORY_HOTBAR + 0, 1, 1, 0)   # Stone
    send_set_slot(sock, 0, INVENTORY_HOTBAR + 1, 4, 1, 0)   # Cobblestone
    send_set_slot(sock, 0, INVENTORY_HOTBAR + 2, 45, 1, 0)  # Bricks
    send_set_slot(sock, 0, INVENTORY_HOTBAR + 3, 3, 1, 0)   # Dirt
    send_set_slot(sock, 0, INVENTORY_HOTBAR + 4, 5, 1, 0)   # Planks
    send_set_slot(sock, 0, INVENTORY_HOTBAR + 5, 17, 1, 0)  # Logs
    send_set_slot(sock, 0, INVENTORY_HOTBAR + 6, 18, 1, 0)  # Leaves
    send_set_slot(sock, 0, INVENTORY_HOTBAR + 7, 20, 1, 0)  # Glass
    send_set_slot(sock, 0, INVENTORY_HOTBAR + 8, 44, 1, 0)  # Slab

    set_position_spawn(player)
    player.chunk_x = None
    player.chunk_z = None
    player.loaded_chunks.clear()
    player.pending_chunks.clear()
    update_player_chunks(player, force_reload=True)
    drain_pending_chunks(player)

    send_spawn_position(sock)
    send_player_position(sock, player)

    on_player_joined(player)

    send_global_chat(player.username or "?", "has joined!", "e")
    player.stage = "Connected"


def send_handshake(sock, player):
    global global_entity_id
    player.username = read_string16(sock)
    player.entity_id = global_entity_id
    global_entity_id += 1
    print(f"{player.username} has joined the game!")
    write_packet_id(sock, HANDSHAKE)
    write_string16(sock, "-")


# ============================================================
# ACTIONS
# ============================================================

def handle_digging(player):
    status = read_byte(player.sock)
    x = read_int(player.sock)
    y = read_byte(player.sock)
    z = read_int(player.sock)
    _face = read_byte(player.sock)

    block_type = get_block(x, y, z)
    if status == 0 and block_type != BEDROCK:
        send_global_block_update(x, y, z, AIR, 0)


def handle_block_placement(player):
    x = read_int(player.sock)
    y = read_byte(player.sock)
    z = read_int(player.sock)
    face = read_byte(player.sock)
    item_id = read_short(player.sock)

    if item_id > -1:
        _amount = read_byte(player.sock)
        damage = read_short(player.sock)
        if item_id < 97:
            # Infinite items
            send_set_slot(
                player.sock, 0,
                INVENTORY_HOTBAR + player.hotbar_slot,
                item_id, 1, damage,
            )
            x, y, z = face_offset(x, y, z, face)
            send_global_block_update(x, y, z, item_id, damage & 0xFF)


def command_processing(player, message):
    """Enhanced: replies are PRIVATE (only to the sender)."""
    if not message or message[0] != "/":
        return False
    cmd = message[1].lower() if len(message) > 1 else ""
    if cmd == "f":
        send_chat_message(player.sock, "Server", "Flash! (no LED on Python)", "7")
    elif cmd == "h":
        send_chat_message(player.sock, "Server", "Commands: /f /h /list /save", "7")
    elif cmd == "s":  # /save
        save_world()
        send_chat_message(player.sock, "Server", "World saved.", "a")
    elif cmd == "l":  # /list
        with players_lock:
            names = [
                p.username
                for p in players
                if p.active and p.username and p.stage == "Connected"
            ]
        if names:
            buf = "Players online: " + ", ".join(names)
        else:
            buf = "Players online: (none)"
        send_chat_message(player.sock, "Server", buf, "e")
    else:
        send_chat_message(player.sock, "Server", "Unknown command. Try /h", "c")
    return True


# ============================================================
# PLAYER THREAD
# ============================================================

def player_thread(player):
    sock = player.sock
    sock.settimeout(None)
    try:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    except Exception:
        pass

    try:
        while player.active:
            # Drain pending chunks periodically
            now = time.time()
            if player.stage == "Connected":
                if now - player.last_chunk_drain >= CHUNK_SEND_INTERVAL:
                    player.last_chunk_drain = now
                    try:
                        drain_pending_chunks(player)
                    except Exception:
                        break
                if now - player.last_keepalive >= KEEPALIVE_INTERVAL:
                    player.last_keepalive = now
                    try:
                        write_packet_id(sock, KEEP_ALIVE)
                    except Exception:
                        break

            # Wait for data with short timeout so we can drain chunks / keepalive
            r, _, _ = select.select([sock], [], [], 0.05)
            if not r:
                continue

            try:
                packet_type = read_ubyte(sock)
            except Exception:
                break

            try:
                if packet_type == KEEP_ALIVE:
                    pass

                elif packet_type == LOGIN_REQUEST:
                    player.stage = "Login"
                    send_login_request(sock, player)

                elif packet_type == HANDSHAKE:
                    player.stage = "Shake"
                    send_handshake(sock, player)

                elif packet_type == CHAT_MESSAGE:
                    message = read_string16(sock)
                    if not command_processing(player, message):
                        send_global_chat(player.username or "?", message)

                elif packet_type == PLAYER_PACKET:
                    player.on_ground = read_byte(sock) != 0

                elif packet_type == PLAYER_POSITION:
                    player.x = read_double(sock)
                    player.y = read_double(sock)
                    player.stance = read_double(sock)
                    player.z = read_double(sock)
                    player.on_ground = read_byte(sock) != 0
                    if player.stage == "Connected":
                        update_player_chunks(player)
                        broadcast_player_movement(player)

                elif packet_type == PLAYER_LOOK:
                    player.yaw = read_float(sock)
                    player.pitch = read_float(sock)
                    player.on_ground = read_byte(sock) != 0
                    if player.stage == "Connected":
                        broadcast_player_movement(player)

                elif packet_type == PLAYER_POSITION_LOOK:
                    player.x = read_double(sock)
                    player.y = read_double(sock)
                    player.stance = read_double(sock)
                    player.z = read_double(sock)
                    player.yaw = read_float(sock)
                    player.pitch = read_float(sock)
                    player.on_ground = read_byte(sock) != 0
                    if player.stage == "Connected":
                        update_player_chunks(player)
                        broadcast_player_movement(player)

                elif packet_type == PLAYER_DIGGING:
                    handle_digging(player)

                elif packet_type == PLAYER_BLOCK_PLACEMENT:
                    handle_block_placement(player)

                elif packet_type == ACTIVE_SLOT:
                    player.hotbar_slot = read_short(sock) & 0xFF

                elif packet_type == PLAYER_ACTION:
                    # entityId, action
                    read_int(sock)
                    read_byte(sock)

                elif packet_type == ENTITY_ACTION:
                    read_int(sock)
                    read_byte(sock)

                elif packet_type == DISCONNECT:
                    try:
                        reason = read_string16(sock)
                        print(f"{player.username or '?'} disconnected: {reason}")
                    except Exception:
                        pass
                    break

                else:
                    # Unknown / unsupported — drop to avoid desync
                    print(f"Unknown packet 0x{packet_type:02X} from {player.username or '?'}")
                    break

            except (ConnectionError, OSError, struct.error, ValueError) as e:
                print(f"Protocol error with {player.username or '?'}: {e}")
                break

    except Exception as e:
        print(f"Player thread error ({player.username or '?'}): {e}")
    finally:
        player.active = False
        broadcast_destroy_player(player)
        try:
            sock.close()
        except Exception:
            pass
        with players_lock:
            if player in players:
                players.remove(player)
        if player.username:
            print(f"{player.username} left the game.")
            send_global_chat(player.username, "has left.", "e")


# ============================================================
# AUTOSAVE THREAD
# ============================================================

def autosave_thread():
    while True:
        time.sleep(AUTOSAVE_INTERVAL)
        with world_lock:
            dirty = world_dirty
        if dirty:
            save_world()


# ============================================================
# SERVER
# ============================================================

def main():
    load_world()

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((HOST, PORT))
    server.listen(MAX_CONNECTIONS)

    t = threading.Thread(target=autosave_thread, daemon=True)
    t.start()

    print()
    print("========================================")
    print(" PicoBeta PyEnhanced")
    print(" Minecraft Beta 1.7.3")
    print(" Protocol: 14")
    print(" Port:", PORT)
    print(" Max players:", MAX_CONNECTIONS)
    print(" World: infinite (flat + sparse edits)")
    print(" Features: player visibility, /list /save")
    print("========================================")
    print()
    print("Server listening...")

    try:
        while True:
            sock, address = server.accept()
            with players_lock:
                if len(players) >= MAX_CONNECTIONS:
                    try:
                        send_disconnect(sock, "Server is full!")
                        sock.close()
                    except Exception:
                        pass
                    print("Server full, connection rejected")
                    continue
                player = Player(sock)
                players.append(player)
            print(f"New client connected from {address[0]}")
            thread = threading.Thread(
                target=player_thread,
                args=(player,),
                daemon=True,
            )
            thread.start()
    except KeyboardInterrupt:
        print("\nShutting down, saving world...")
        save_world()
    finally:
        server.close()


if __name__ == "__main__":
    main()
