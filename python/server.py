import socket
import struct
import threading
import zlib
import select
import time


# ============================================================
# CONFIG
# ============================================================

HOST = "0.0.0.0"
PORT = 25565

WORLD_SIZE_X = 16
WORLD_SIZE_Y = 16
WORLD_SIZE_Z = 16

MAX_CONNECTIONS = 4

MAX_WORLD_SIZE = (
    WORLD_SIZE_X *
    WORLD_SIZE_Y *
    WORLD_SIZE_Z
)


# ============================================================
# PACKETS
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
PRE_CHUNK = 0x32
CHUNK = 0x33
BLOCK_UPDATE = 0x35
EFFECT = 0x3D
SET_SLOT = 0x67
DISCONNECT = 0xFF


# ============================================================
# BLOCS
# ============================================================

AIR = 0
STONE = 1
GRASS = 2
DIRT = 3
BEDROCK = 7


# ============================================================
# MONDE
# ============================================================

world = bytearray(MAX_WORLD_SIZE)


def get_block_index(x, y, z):

    if (
        x < 0 or
        x >= WORLD_SIZE_X or
        y < 0 or
        y >= WORLD_SIZE_Y or
        z < 0 or
        z >= WORLD_SIZE_Z
    ):
        return 0

    return (
        x * (WORLD_SIZE_Y * WORLD_SIZE_Z)
        + z * WORLD_SIZE_Y
        + y
    )


def get_block(x, y, z):

    if (
        x < 0 or
        x >= WORLD_SIZE_X or
        y < 0 or
        y >= WORLD_SIZE_Y or
        z < 0 or
        z >= WORLD_SIZE_Z
    ):
        return AIR

    return world[get_block_index(x, y, z)]


def set_block(x, y, z, block):

    if (
        x < 0 or
        x >= WORLD_SIZE_X or
        y < 0 or
        y >= WORLD_SIZE_Z or
        z < 0 or
        z >= WORLD_SIZE_Z
    ):
        return

    world[get_block_index(x, y, z)] = block


def fill_world():

    print(
        "Generating world "
        f"({WORLD_SIZE_X}x{WORLD_SIZE_Y}x{WORLD_SIZE_Z})..."
    )

    for x in range(WORLD_SIZE_X):

        for z in range(WORLD_SIZE_Z):

            for y in range(WORLD_SIZE_Y):

                index = get_block_index(x, y, z)

                if y == 0:
                    world[index] = BEDROCK

                elif y < WORLD_SIZE_Y // 3:
                    world[index] = STONE

                elif y < WORLD_SIZE_Y // 2:
                    world[index] = DIRT

                elif y == WORLD_SIZE_Y // 2:
                    world[index] = GRASS

                else:
                    world[index] = AIR

    print("World generated!")


# ============================================================
# SOCKET
# ============================================================

def recv_exact(sock, length):

    data = bytearray()

    while len(data) < length:

        part = sock.recv(
            length - len(data)
        )

        if not part:
            raise ConnectionError(
                "Connection closed"
            )

        data.extend(part)

    return bytes(data)


def send_all(sock, data):

    total = 0

    while total < len(data):

        sent = sock.send(
            data[total:]
        )

        if sent <= 0:
            raise ConnectionError(
                "Send failed"
            )

        total += sent


# ============================================================
# PRIMITIVES BIG-ENDIAN
# ============================================================

def read_byte(sock):

    return struct.unpack(
        ">b",
        recv_exact(sock, 1)
    )[0]


def read_ubyte(sock):

    return recv_exact(sock, 1)[0]


def write_byte(sock, value):

    send_all(
        sock,
        struct.pack(">B", value & 0xFF)
    )


def write_ubyte(sock, value):

    send_all(
        sock,
        struct.pack(">B", value)
    )


def read_short(sock):

    return struct.unpack(
        ">h",
        recv_exact(sock, 2)
    )[0]


def write_short(sock, value):

    send_all(
        sock,
        struct.pack(">h", value)
    )


def read_int(sock):

    return struct.unpack(
        ">i",
        recv_exact(sock, 4)
    )[0]


def write_int(sock, value):

    send_all(
        sock,
        struct.pack(">i", value)
    )


def read_long(sock):

    return struct.unpack(
        ">q",
        recv_exact(sock, 8)
    )[0]


def write_long(sock, value):

    send_all(
        sock,
        struct.pack(">q", value)
    )


def read_float(sock):

    return struct.unpack(
        ">f",
        recv_exact(sock, 4)
    )[0]


def write_float(sock, value):

    send_all(
        sock,
        struct.pack(">f", value)
    )


def read_double(sock):

    return struct.unpack(
        ">d",
        recv_exact(sock, 8)
    )[0]


def write_double(sock, value):

    send_all(
        sock,
        struct.pack(">d", value)
    )


# ============================================================
# STRING16
#
# Même comportement que PicoBeta :
#
# short length
# puis pour chaque caractère :
#   high byte
#   low byte
#
# ============================================================

def read_string16(sock):

    length = read_short(sock)

    if length < 0 or length > 1024:

        raise ValueError(
            "Invalid String16 length: "
            + str(length)
        )

    chars = bytearray()

    for _ in range(length):

        high = read_byte(sock)
        low = read_byte(sock)

        chars.append(low & 0xFF)

    return chars.decode(
        "latin-1",
        errors="replace"
    )


def write_string16(sock, message):

    encoded = message.encode(
        "latin-1",
        errors="replace"
    )

    write_short(
        sock,
        len(encoded)
    )

    for value in encoded:

        write_byte(
            sock,
            0
        )

        write_byte(
            sock,
            value
        )


# ============================================================
# PACKET
# ============================================================

def write_packet_id(sock, packet_id):

    write_ubyte(
        sock,
        packet_id
    )


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


players = []

global_entity_id = 0

players_lock = threading.Lock()


# ============================================================
# SPAWN
# ============================================================

SPAWN_X = WORLD_SIZE_X // 2
SPAWN_Y = WORLD_SIZE_Y // 2
SPAWN_Z = WORLD_SIZE_Z // 2


def set_position_spawn(player):

    player.x = float(SPAWN_X)
    player.y = float(SPAWN_Y) + 3.5
    player.z = float(SPAWN_Z)


# ============================================================
# PRE CHUNK
# ============================================================

def send_pre_chunk(sock, x, z, mode):

    write_packet_id(
        sock,
        PRE_CHUNK
    )

    write_int(sock, x)
    write_int(sock, z)

    write_byte(
        sock,
        1 if mode else 0
    )


# ============================================================
# CHUNK
#
# Traduction directe de SendChunk() de PicoBeta.
# ============================================================

def send_chunk(sock, x, z):

    write_packet_id(
        sock,
        CHUNK
    )

    # X
    write_int(
        sock,
        x
    )

    # Y
    write_short(
        sock,
        0
    )

    # Z
    write_int(
        sock,
        z
    )

    # Size X/Y/Z
    write_byte(
        sock,
        WORLD_SIZE_X - 1
    )

    write_byte(
        sock,
        WORLD_SIZE_Y - 1
    )

    write_byte(
        sock,
        WORLD_SIZE_Z - 1
    )

    # --------------------------------------------------------
    # PicoBeta utilise :
    #
    # dataSize = 4096
    #
    # blocks       = 4096
    # metadata     = 2048
    # lighting     = 4096
    #
    # total = 10240
    # --------------------------------------------------------

    data_size = MAX_WORLD_SIZE
    true_size = int(data_size * 2.5)

    write_int(
        sock,
        true_size + 11
    )

    # ZLIB header
    write_byte(sock, 0x78)
    write_byte(sock, 0x01)

    # Deflate stored block
    write_byte(sock, 0x01)

    # Length
    write_byte(
        sock,
        true_size & 0xFF
    )

    write_byte(
        sock,
        (true_size >> 8) & 0xFF
    )

    # One's complement
    inverse = (~true_size) & 0xFFFF

    write_byte(
        sock,
        inverse & 0xFF
    )

    write_byte(
        sock,
        (inverse >> 8) & 0xFF
    )

    # --------------------------------------------------------
    # Adler32
    # --------------------------------------------------------

    A = 1
    B = 0

    # Blocks
    for value in world:

        write_byte(
            sock,
            value
        )

        A = (
            A + value
        ) % 65521

        B = (
            B + A
        ) % 65521

    # Metadata
    for _ in range(data_size // 2):

        write_byte(
            sock,
            0
        )

        B = (
            B + A
        ) % 65521

    # Lighting
    for _ in range(data_size):

        write_byte(
            sock,
            0xFF
        )

        A = (
            A + 0xFF
        ) % 65521

        B = (
            B + A
        ) % 65521

    adler = (
        (B << 16) |
        A
    )

    write_int(
        sock,
        adler
    )


# ============================================================
# LOGIN
# ============================================================

def send_login_request(sock, player):

    protocol = read_int(sock)

    if protocol != 14:

        print(
            "Invalid protocol version:",
            protocol
        )

    # username again
    unused = read_string16(sock)

    # seed
    read_long(sock)

    # dimension
    read_byte(sock)

    print(
        "Login request from:",
        player.username
    )

    # --------------------------------------------------------
    # Server Login Request
    # --------------------------------------------------------

    write_packet_id(
        sock,
        LOGIN_REQUEST
    )

    write_int(
        sock,
        player.entity_id
    )

    write_string16(
        sock,
        ""
    )

    write_long(
        sock,
        0
    )

    write_byte(
        sock,
        0
    )

    # --------------------------------------------------------
    # HOTBAR
    # --------------------------------------------------------

    send_set_slot(
        sock,
        0,
        36,
        1,
        1,
        0
    )

    send_set_slot(
        sock,
        0,
        37,
        4,
        1,
        0
    )

    send_set_slot(
        sock,
        0,
        38,
        45,
        1,
        0
    )

    send_set_slot(
        sock,
        0,
        39,
        3,
        1,
        0
    )

    send_set_slot(
        sock,
        0,
        40,
        5,
        1,
        0
    )

    send_set_slot(
        sock,
        0,
        41,
        17,
        1,
        0
    )

    send_set_slot(
        sock,
        0,
        42,
        18,
        1,
        0
    )

    send_set_slot(
        sock,
        0,
        43,
        20,
        1,
        0
    )

    send_set_slot(
        sock,
        0,
        44,
        44,
        1,
        0
    )

    # --------------------------------------------------------
    # CHUNKS
    # --------------------------------------------------------

    for x in range(-1, 2):

        for z in range(-1, 2):

            send_pre_chunk(
                sock,
                x,
                z,
                True
            )

    send_chunk(
        sock,
        0,
        0
    )

    # --------------------------------------------------------
    # SPAWN
    # --------------------------------------------------------

    send_spawn_position(
        sock
    )

    set_position_spawn(
        player
    )

    send_player_position(
        sock,
        player
    )

    send_global_chat(
        player.username,
        "has joined!",
        "e"
    )

    player.stage = "Connected"


# ============================================================
# HANDSHAKE
# ============================================================

def send_handshake(sock, player):

    # PicoBeta lit UNIQUEMENT le pseudo ici.
    player.username = read_string16(
        sock
    )

    global global_entity_id

    player.entity_id = global_entity_id

    global_entity_id += 1

    print(
        player.username,
        "has joined the game!"
    )

    write_packet_id(
        sock,
        HANDSHAKE
    )

    write_string16(
        sock,
        "-"
    )


# ============================================================
# POSITION
# ============================================================

def send_player_position(sock, player):

    write_packet_id(
        sock,
        PLAYER_POSITION_LOOK
    )

    write_double(
        sock,
        player.x
    )

    write_double(
        sock,
        player.y
    )

    write_double(
        sock,
        player.stance
    )

    write_double(
        sock,
        player.z
    )

    write_float(
        sock,
        player.yaw
    )

    write_float(
        sock,
        player.pitch
    )

    write_byte(
        sock,
        1 if player.on_ground else 0
    )


def send_spawn_position(sock):

    write_packet_id(
        sock,
        SPAWN_POSITION
    )

    write_int(
        sock,
        SPAWN_X
    )

    write_int(
        sock,
        SPAWN_Y
    )

    write_int(
        sock,
        SPAWN_Z
    )


# ============================================================
# CHAT
# ============================================================

def send_chat_message(
    sock,
    sender,
    message,
    color="r"
):

    full_message = (
        "\xa7" +
        color +
        "<" +
        sender +
        "> " +
        message
    )

    write_packet_id(
        sock,
        CHAT_MESSAGE
    )

    write_string16(
        sock,
        full_message
    )


def send_global_chat(
    sender,
    message,
    color="r"
):

    with players_lock:

        current = list(players)

    for player in current:

        if player.active:

            try:

                send_chat_message(
                    player.sock,
                    sender,
                    message,
                    color
                )

            except Exception:
                pass


# ============================================================
# SET SLOT
# ============================================================

def send_set_slot(
    sock,
    window,
    slot,
    item_id,
    amount,
    damage
):

    write_packet_id(
        sock,
        SET_SLOT
    )

    write_byte(
        sock,
        window
    )

    write_short(
        sock,
        slot
    )

    write_short(
        sock,
        item_id
    )

    write_byte(
        sock,
        amount
    )

    write_short(
        sock,
        damage
    )


# ============================================================
# BLOCK UPDATE
# ============================================================

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


def send_block_update(
    sock,
    x,
    y,
    z,
    block_type,
    metadata
):

    if (
        x < 0 or
        x >= WORLD_SIZE_X or
        y < 0 or
        y >= WORLD_SIZE_Y or
        z < 0 or
        z >= WORLD_SIZE_Z
    ):

        send_chat_message(
            sock,
            "Server",
            "Out of bounds!",
            "c"
        )

        block_type = AIR
        metadata = 0

    write_packet_id(
        sock,
        BLOCK_UPDATE
    )

    write_int(
        sock,
        x
    )

    write_byte(
        sock,
        y
    )

    write_int(
        sock,
        z
    )

    write_byte(
        sock,
        block_type
    )

    write_byte(
        sock,
        metadata
    )

    set_block(
        x,
        y,
        z,
        block_type
    )


def send_effect(
    sock,
    effect_id,
    x,
    y,
    z,
    data
):

    write_packet_id(
        sock,
        EFFECT
    )

    write_int(
        sock,
        effect_id
    )

    write_int(
        sock,
        x
    )

    write_byte(
        sock,
        y
    )

    write_int(
        sock,
        z
    )

    write_int(
        sock,
        data
    )


def send_global_block_update(
    x,
    y,
    z,
    block_type,
    metadata
):

    with players_lock:

        current = list(players)

    for player in current:

        if not player.active:
            continue

        try:

            send_block_update(
                player.sock,
                x,
                y,
                z,
                block_type,
                metadata
            )

            send_effect(
                player.sock,
                2001,
                x,
                y,
                z,
                block_type
            )

        except Exception:
            pass


# ============================================================
# DIGGING
# ============================================================

def handle_digging(player):

    status = read_byte(
        player.sock
    )

    x = read_int(
        player.sock
    )

    y = read_byte(
        player.sock
    )

    z = read_int(
        player.sock
    )

    face = read_byte(
        player.sock
    )

    block_type = get_block(
        x,
        y,
        z
    )

    # Start digging
    if (
        status == 0 and
        block_type != BEDROCK
    ):

        send_global_block_update(
            x,
            y,
            z,
            AIR,
            0
        )


# ============================================================
# BLOCK PLACEMENT
# ============================================================

def handle_block_placement(player):

    x = read_int(
        player.sock
    )

    y = read_byte(
        player.sock
    )

    z = read_int(
        player.sock
    )

    face = read_byte(
        player.sock
    )

    item_id = read_short(
        player.sock
    )

    if item_id > -1:

        amount = read_byte(
            player.sock
        )

        damage = read_short(
            player.sock
        )

        if item_id < 97:

            # Infinite item
            send_set_slot(
                player.sock,
                0,
                36 + player.hotbar_slot,
                item_id,
                1,
                damage
            )

            x, y, z = face_offset(
                x,
                y,
                z,
                face
            )

            send_global_block_update(
                x,
                y,
                z,
                item_id,
                damage
            )


# ============================================================
# COMMANDS
# ============================================================

def command_processing(
    player,
    message
):

    if not message.startswith("/"):
        return False

    if len(message) < 2:
        return True

    command = message[1]

    if command == "f":

        send_global_chat(
            "Server",
            "Flash! (no LED on Windows)",
            "7"
        )

    elif command == "h":

        send_global_chat(
            "Server",
            "Commands: /f (flash), /h (help)",
            "7"
        )

    return True


# ============================================================
# CLIENT
# ============================================================

def process_packet(player):

    packet_type = read_ubyte(
        player.sock
    )

    # --------------------------------------------------------
    # Login
    # --------------------------------------------------------

    if packet_type == LOGIN_REQUEST:

        player.stage = "Login"

        send_login_request(
            player.sock,
            player
        )

    # --------------------------------------------------------
    # Handshake
    # --------------------------------------------------------

    elif packet_type == HANDSHAKE:

        player.stage = "Shake"

        send_handshake(
            player.sock,
            player
        )

    # --------------------------------------------------------
    # Chat
    # --------------------------------------------------------

    elif packet_type == CHAT_MESSAGE:

        message = read_string16(
            player.sock
        )

        if not command_processing(
            player,
            message
        ):

            send_global_chat(
                player.username or "?",
                message
            )

    # --------------------------------------------------------
    # Player
    # --------------------------------------------------------

    elif packet_type == PLAYER_PACKET:

        player.on_ground = (
            read_byte(player.sock) != 0
        )

    # --------------------------------------------------------
    # Position
    # --------------------------------------------------------

    elif packet_type == PLAYER_POSITION:

        player.x = read_double(
            player.sock
        )

        player.y = read_double(
            player.sock
        )

        player.stance = read_double(
            player.sock
        )

        player.z = read_double(
            player.sock
        )

        player.on_ground = (
            read_byte(player.sock) != 0
        )

    # --------------------------------------------------------
    # Look
    # --------------------------------------------------------

    elif packet_type == PLAYER_LOOK:

        player.yaw = read_float(
            player.sock
        )

        player.pitch = read_float(
            player.sock
        )

        player.on_ground = (
            read_byte(player.sock) != 0
        )

    # --------------------------------------------------------
    # Position + Look
    # --------------------------------------------------------

    elif packet_type == PLAYER_POSITION_LOOK:

        player.x = read_double(
            player.sock
        )

        player.y = read_double(
            player.sock
        )

        player.stance = read_double(
            player.sock
        )

        player.z = read_double(
            player.sock
        )

        player.yaw = read_float(
            player.sock
        )

        player.pitch = read_float(
            player.sock
        )

        player.on_ground = (
            read_byte(player.sock) != 0
        )

    # --------------------------------------------------------
    # Player action
    # --------------------------------------------------------

    elif packet_type == PLAYER_ACTION:

        read_int(player.sock)
        read_byte(player.sock)

    # --------------------------------------------------------
    # Digging
    # --------------------------------------------------------

    elif packet_type == PLAYER_DIGGING:

        handle_digging(
            player
        )

    # --------------------------------------------------------
    # Placement
    # --------------------------------------------------------

    elif packet_type == PLAYER_BLOCK_PLACEMENT:

        handle_block_placement(
            player
        )

    # --------------------------------------------------------
    # Active slot
    # --------------------------------------------------------

    elif packet_type == ACTIVE_SLOT:

        player.hotbar_slot = read_short(
            player.sock
        )

    # --------------------------------------------------------
    # Entity action
    # --------------------------------------------------------

    elif packet_type == ENTITY_ACTION:

        read_int(player.sock)
        read_byte(player.sock)

    # --------------------------------------------------------
    # Keep Alive
    # --------------------------------------------------------

    elif packet_type == KEEP_ALIVE:

        if player.stage == "Connected":

            write_packet_id(
                player.sock,
                KEEP_ALIVE
            )

    # --------------------------------------------------------
    # Disconnect
    # --------------------------------------------------------

    elif packet_type == DISCONNECT:

        message = read_string16(
            player.sock
        )

        print(
            player.username,
            "has disconnected:",
            message
        )

        raise ConnectionError(
            "Client disconnected"
        )

    else:

        print(
            player.username,
            "Unhandled packet: "
            f"0x{packet_type:02X}"
        )


# ============================================================
# PLAYER THREAD
# ============================================================

def player_thread(player):

    address = player.sock.getpeername()

    print()
    print(
        "================================"
    )
    print(
        "New client:",
        address
    )
    print(
        "================================"
    )

    try:

        # ----------------------------------------------------
        # Le client commence par envoyer 0x02.
        # ----------------------------------------------------

        packet = read_ubyte(
            player.sock
        )

        if packet != HANDSHAKE:

            raise ConnectionError(
                "Expected handshake 0x02, "
                f"got 0x{packet:02X}"
            )

        # ----------------------------------------------------
        # IMPORTANT :
        # même ordre que PicoBeta
        # ----------------------------------------------------

        send_handshake(
            player.sock,
            player
        )

        # ----------------------------------------------------
        # Ensuite le client envoie LoginRequest.
        # ----------------------------------------------------

        packet = read_ubyte(
            player.sock
        )

        if packet != LOGIN_REQUEST:

            raise ConnectionError(
                "Expected login 0x01, "
                f"got 0x{packet:02X}"
            )

        send_login_request(
            player.sock,
            player
        )

        print(
            player.username,
            "is now connected!"
        )

        # ----------------------------------------------------
        # Boucle
        # ----------------------------------------------------

        while player.active:

            process_packet(
                player
            )

            # Void protection
            if player.y < 0:

                set_position_spawn(
                    player
                )

                send_player_position(
                    player.sock,
                    player
                )

    except Exception as e:

        print(
            "Client error:",
            address,
            "|",
            repr(e)
        )

    finally:

        player.active = False

        try:
            player.sock.close()
        except Exception:
            pass

        with players_lock:

            if player in players:
                players.remove(
                    player
                )

        if player.username:

            print(
                player.username,
                "left the game."
            )

            send_global_chat(
                player.username,
                "has left.",
                "e"
            )


# ============================================================
# SERVER
# ============================================================

def main():

    fill_world()

    server = socket.socket(
        socket.AF_INET,
        socket.SOCK_STREAM
    )

    server.setsockopt(
        socket.SOL_SOCKET,
        socket.SO_REUSEADDR,
        1
    )

    server.bind(
        (HOST, PORT)
    )

    server.listen(
        MAX_CONNECTIONS
    )

    print()
    print(
        "========================================"
    )
    print(
        " PicoBeta Python"
    )
    print(
        " Minecraft Beta 1.7.3"
    )
    print(
        " Protocol: 14"
    )
    print(
        " Port:",
        PORT
    )
    print(
        " Max players:",
        MAX_CONNECTIONS
    )
    print(
        "========================================"
    )
    print()
    print(
        "Server listening..."
    )

    while True:

        sock, address = server.accept()

        with players_lock:

            if len(players) >= MAX_CONNECTIONS:

                try:

                    write_packet_id(
                        sock,
                        DISCONNECT
                    )

                    write_string16(
                        sock,
                        "Server is full!"
                    )

                    sock.close()

                except Exception:
                    pass

                continue

            player = Player(
                sock
            )

            players.append(
                player
            )

        thread = threading.Thread(
            target=player_thread,
            args=(player,),
            daemon=True
        )

        thread.start()


# ============================================================
# START
# ============================================================

if __name__ == "__main__":

    main()