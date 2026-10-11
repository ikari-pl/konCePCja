#!/usr/bin/env python3
"""Client for the KCPC binary frontend protocol (docs/protocol/kcpc-binary-v1.md).

Standard library only (socket, struct, threading), so every CI job can run it
with nothing installed. It is the reference client for the test suite next to
it and grows with each implementation slice of the protocol.

The protocol is at major version 0: client and server must come from the same
checkout, and nothing outside this repository should be built on this file.

    from kcpc_client import KcpcClient
    with KcpcClient(port) as c:          # port: `ports.binary` of the manifest
        hello = c.hello('my-tool')
        print(hello.server_version, hello.machine.model)
        c.ping()

Never assume port 6545: read it from the startup manifest the core prints.
"""

import socket
import struct
from dataclasses import dataclass
from typing import List, Optional

MAGIC = b'KCPC'
VERSION_MAJOR = 0
HEADER = struct.Struct('<4sBBHII')  # magic, version, kind, opcode, id, length
HEADER_SIZE = 16

MAX_REQUEST_PAYLOAD = 1 << 20   # what a client may send
MAX_SERVER_FRAME = 16 << 20     # what the server may send
MIN_CLIENT_MAX_FRAME = 128 << 10
DEFAULT_PORT = 6545             # a default only; read the manifest
PORT_PROBE_COUNT = 10
MAX_CLIENTS = 8

KIND_REQUEST = 1
KIND_RESPONSE = 2
KIND_EVENT = 3

# Opcodes implemented so far (section 13 has the whole table).
OP_ERROR = 0x0000
OP_HELLO = 0x0001
OP_PING = 0x0002
OP_SUBSCRIBE = 0x0003
OP_CONTROL = 0x0004
OP_MACHINE_INFO = 0x0005
OP_MEM_READ = 0x0211  # not implemented yet; used to test UNKNOWN_OPCODE

# Status codes (section 5).
OK = 0
BAD_FRAME = 1
UNSUPPORTED_VERSION = 2
HELLO_REQUIRED = 3
TOO_MANY_CLIENTS = 4
UNKNOWN_OPCODE = 5
BAD_PAYLOAD = 6
NOT_CONTROLLER = 7
BUSY = 8
OUT_OF_RANGE = 9
TOO_LARGE = 10
NOT_PRESENT = 11
NOT_FOUND = 12
LIMIT_REACHED = 13
SHUTTING_DOWN = 14
INTERNAL = 15

STATUS_NAMES = {
    OK: 'OK', BAD_FRAME: 'BAD_FRAME',
    UNSUPPORTED_VERSION: 'UNSUPPORTED_VERSION',
    HELLO_REQUIRED: 'HELLO_REQUIRED', TOO_MANY_CLIENTS: 'TOO_MANY_CLIENTS',
    UNKNOWN_OPCODE: 'UNKNOWN_OPCODE', BAD_PAYLOAD: 'BAD_PAYLOAD',
    NOT_CONTROLLER: 'NOT_CONTROLLER', BUSY: 'BUSY',
    OUT_OF_RANGE: 'OUT_OF_RANGE', TOO_LARGE: 'TOO_LARGE',
    NOT_PRESENT: 'NOT_PRESENT', NOT_FOUND: 'NOT_FOUND',
    LIMIT_REACHED: 'LIMIT_REACHED', SHUTTING_DOWN: 'SHUTTING_DOWN',
    INTERNAL: 'INTERNAL',
}

ROLE_OBSERVER = 0
ROLE_CONTROLLER = 1

MACHINE_FLAG_HEADLESS = 1 << 0
MACHINE_FLAG_ASIC = 1 << 1

TIER_NAMES = ('fast', 'wake', 'soldered', 'faithful')


def status_name(status: int) -> str:
    return STATUS_NAMES.get(status, f'status {status}')


class KcpcError(Exception):
    """A request the server refused; `status` is what a client acts on."""

    def __init__(self, status: int, message: str, opcode: int = 0):
        super().__init__(f'{status_name(status)}: {message}')
        self.status = status
        self.message = message
        self.opcode = opcode


class KcpcClosed(Exception):
    """The connection ended. `error` is the ERROR frame the server sent
    first, if it sent one."""

    def __init__(self, error: Optional[KcpcError] = None):
        super().__init__(str(error) if error else 'connection closed')
        self.error = error


class Fields:
    """Reads the field types of section 4 out of a payload, in wire order.

    Bytes beyond the fields a caller reads are left alone: a later minor
    version may append fields to any payload.
    """

    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0

    def _take(self, size: int) -> bytes:
        if self.pos + size > len(self.data):
            raise ValueError(f'payload too short: wanted {size} bytes at '
                             f'{self.pos}, have {len(self.data)}')
        chunk = self.data[self.pos:self.pos + size]
        self.pos += size
        return chunk

    def u8(self) -> int:
        return self._take(1)[0]

    def u16(self) -> int:
        return struct.unpack('<H', self._take(2))[0]

    def u32(self) -> int:
        return struct.unpack('<I', self._take(4))[0]

    def u64(self) -> int:
        return struct.unpack('<Q', self._take(8))[0]

    def string(self) -> str:
        return self._take(self.u16()).decode('utf-8', errors='replace')

    def skip(self, size: int) -> None:
        self._take(size)

    def rest(self) -> bytes:
        return self._take(len(self.data) - self.pos)


def pack_str(text: str) -> bytes:
    raw = text.encode('utf-8')
    if len(raw) > 0xFFFF:
        raise ValueError('a str field holds at most 65535 bytes')
    return struct.pack('<H', len(raw)) + raw


def encode_frame(kind: int, opcode: int, frame_id: int,
                 payload: bytes = b'', version: int = VERSION_MAJOR,
                 magic: bytes = MAGIC, length: Optional[int] = None) -> bytes:
    """One frame. `version`, `magic` and `length` can be overridden to build
    the malformed frames the tests send."""
    if length is None:
        length = len(payload)
    return HEADER.pack(magic, version, kind, opcode, frame_id, length) + payload


@dataclass
class Frame:
    version: int
    kind: int
    opcode: int
    id: int
    payload: bytes

    @property
    def status(self) -> int:
        return struct.unpack_from('<H', self.payload)[0]

    @property
    def message(self) -> str:
        """The message of a failed response."""
        fields = Fields(self.payload)
        fields.skip(4)
        return fields.string()


@dataclass
class Stamp:
    frame: int      # completed frames since the core started
    paused: bool

    @staticmethod
    def read(fields: Fields) -> 'Stamp':
        frame = fields.u64()
        run_state = fields.u8()
        fields.skip(3)
        return Stamp(frame, run_state != 0)


@dataclass
class MachineInfo:
    model: int            # 0 = 464, 1 = 664, 2 = 6128, 3 = 6128+
    crtc_type: int
    effective_tier: int   # index into TIER_NAMES
    flags: int
    ram_bytes: int
    rom_slots: int        # bit N = a ROM is fitted in slot N
    stamp: Stamp

    @property
    def headless(self) -> bool:
        return bool(self.flags & MACHINE_FLAG_HEADLESS)

    @property
    def has_asic(self) -> bool:
        return bool(self.flags & MACHINE_FLAG_ASIC)

    @property
    def tier_name(self) -> str:
        return TIER_NAMES[self.effective_tier]

    @staticmethod
    def read(fields: Fields) -> 'MachineInfo':
        model = fields.u8()
        crtc_type = fields.u8()
        tier = fields.u8()
        flags = fields.u8()
        ram_bytes = fields.u32()
        rom_slots = fields.u32()
        return MachineInfo(model, crtc_type, tier, flags, ram_bytes,
                           rom_slots, Stamp.read(fields))


@dataclass
class Hello:
    server_minor: int
    role: int
    max_frame: int
    caps: int
    client_id: int
    pid: int
    server_version: str
    machine: MachineInfo

    @staticmethod
    def read(fields: Fields) -> 'Hello':
        server_minor = fields.u16()
        role = fields.u8()
        fields.skip(1)
        max_frame = fields.u32()
        caps = fields.u64()
        client_id = fields.u32()
        pid = fields.u32()
        server_version = fields.string()
        return Hello(server_minor, role, max_frame, caps, client_id, pid,
                     server_version, MachineInfo.read(fields))


def hello_payload(name: str = 'kcpc_client.py', role: int = ROLE_OBSERVER,
                  max_frame: int = 0, want_caps: int = 0,
                  client_minor: int = 0) -> bytes:
    return (struct.pack('<HBBIQ', client_minor, role, 0, max_frame, want_caps)
            + pack_str(name))


class KcpcClient:
    """One connection. Not thread-safe: use one client per thread."""

    def __init__(self, port: int, timeout: float = 10.0,
                 host: str = '127.0.0.1'):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.next_id = 1
        self.events: List[Frame] = []
        self.hello_reply: Optional[Hello] = None

    def __enter__(self) -> 'KcpcClient':
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass

    # -- frames ------------------------------------------------------------

    def send_raw(self, data: bytes) -> None:
        self.sock.sendall(data)

    def send_request(self, opcode: int, payload: bytes = b'',
                     request_id: Optional[int] = None) -> int:
        """Send without waiting (pipelining). Returns the request id."""
        if request_id is None:
            request_id = self.next_id
            self.next_id += 1
        self.send_raw(encode_frame(KIND_REQUEST, opcode, request_id, payload))
        return request_id

    def _recv_exact(self, size: int) -> bytes:
        chunks = []
        while size > 0:
            chunk = self.sock.recv(size)
            if not chunk:
                raise KcpcClosed()
            chunks.append(chunk)
            size -= len(chunk)
        return b''.join(chunks)

    def read_frame(self) -> Frame:
        """The next frame, whatever it is. Raises KcpcClosed at end of
        stream."""
        magic, version, kind, opcode, frame_id, length = HEADER.unpack(
            self._recv_exact(HEADER_SIZE))
        if magic != MAGIC:
            raise ValueError(f'bad magic from server: {magic!r}')
        if length > MAX_SERVER_FRAME:
            raise ValueError(f'server frame of {length} bytes is over the '
                             'limit')
        return Frame(version, kind, opcode, frame_id,
                     self._recv_exact(length))

    def read_response(self, request_id: int) -> Frame:
        """The response to `request_id`. Events that arrive first are kept in
        `self.events`: responses are matched by id, never by position."""
        while True:
            frame = self.read_frame()
            if frame.kind == KIND_EVENT:
                self.events.append(frame)
                continue
            if frame.opcode == OP_ERROR and frame.id == 0:
                # The server's last word before it closes the connection.
                raise KcpcClosed(KcpcError(frame.status, frame.message))
            if frame.id != request_id:
                raise ValueError(f'response id {frame.id}, expected '
                                 f'{request_id}')
            return frame

    def request(self, opcode: int, payload: bytes = b'') -> Fields:
        """Send one request and return its reply fields (after the status
        header). Raises KcpcError if the server refused it."""
        request_id = self.send_request(opcode, payload)
        frame = self.read_response(request_id)
        if frame.opcode != opcode:
            raise ValueError(f'response opcode {frame.opcode:#06x}, expected '
                             f'{opcode:#06x}')
        if frame.status != OK:
            raise KcpcError(frame.status, frame.message, opcode)
        fields = Fields(frame.payload)
        fields.skip(4)
        return fields

    # -- opcodes -----------------------------------------------------------

    def hello(self, name: str = 'kcpc_client.py', role: int = ROLE_OBSERVER,
              max_frame: int = 0, want_caps: int = 0) -> Hello:
        """Must be the first request on a connection."""
        self.hello_reply = Hello.read(self.request(
            OP_HELLO, hello_payload(name, role, max_frame, want_caps)))
        return self.hello_reply

    def ping(self) -> None:
        self.request(OP_PING)

    def machine_info(self) -> MachineInfo:
        return MachineInfo.read(self.request(OP_MACHINE_INFO))
