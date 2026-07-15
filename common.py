import hashlib

FRAME_MS = 20
PAYLOAD_BYTES = 160
HEADER_FMT = "!I"

SOURCE_TO_SENDER = ("127.0.0.1", 47010)
SEND_TO_RELAY = ("127.0.0.1", 47001)
RELAY_TO_RECV = ("127.0.0.1", 47002)
RECV_TO_RELAY = ("127.0.0.1", 47003)
RELAY_TO_SEND = ("127.0.0.1", 47004)
RECV_TO_PLAYER = ("127.0.0.1", 47020)

def frame_payload(seed: str, i: int) -> bytes:
    out = b""
    j = 0
    while len(out) < PAYLOAD_BYTES:
        out += hashlib.sha256(f"{seed}:{i}:{j}".encode()).digest()
        j += 1
    return out[:PAYLOAD_BYTES]
