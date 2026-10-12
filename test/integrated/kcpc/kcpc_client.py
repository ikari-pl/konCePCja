#!/usr/bin/env python3
"""Client for the KCPC binary frontend protocol (docs/protocol/kcpc-binary-v1.md).

Standard library only (socket, struct, threading), so every CI job can run it
with nothing installed. It is the reference client for the test suite next to
it and grows with each implementation slice of the protocol.

The protocol is at major version 0: client and server must come from the same
checkout, and nothing outside this repository should be built on this file.

    from kcpc_client import KcpcClient, SPACE_CPU
    with KcpcClient(port) as c:          # port: `ports.binary` of the manifest
        hello = c.hello('my-tool')
        print(hello.server_version, hello.machine.model)
        c.ping()
        regs = c.regs_get()              # works paused or running
        ram = c.mem_read(SPACE_CPU, 0, 0x10000)   # 64 KiB, one reply
        if c.acquire_control().is_controller:     # changes need control
            c.regs_set(hl=0x1234)                 # exactly that register
            c.mem_write(SPACE_CPU_RAM, 0x8000, b'\x18\xfe')
            c.subscribe(CLASS_RUN)                # events are pushed
            c.pause()                             # reply, then EVT_STOPPED
            stopped = c.wait_event(EVT_STOPPED)   # blocks; no polling
            c.step(count=3)                       # ends with EVT_STOPPED
            c.key(*KEY_A, down=True)              # a matrix position
            c.text('print 1+1~RETURN~')           # the AutoTypeQueue

Never assume port 6545: read it from the startup manifest the core prints.
"""

import socket
import struct
from dataclasses import dataclass
from typing import List, Optional, Tuple

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
OP_REGS_GET = 0x0200
OP_REGS_SET = 0x0201
OP_MEM_INFO = 0x0210
OP_MEM_READ = 0x0211
OP_MEM_WRITE = 0x0212
OP_PAUSE = 0x0100
OP_RUN = 0x0101
OP_STEP = 0x0102
OP_RESET = 0x0103
OP_RUN_STATE = 0x0104
OP_BP_LIST = 0x0300
OP_BP_ADD = 0x0301
OP_BP_DEL = 0x0302
OP_BP_CLEAR = 0x0303
OP_KEY = 0x0400
OP_KEYS_RELEASE = 0x0401
OP_TEXT = 0x0402
OP_MOUSE = 0x0403
OP_INPUT_STATE = 0x0404
OP_CHIP_GET = 0x0500  # not implemented yet; used to test UNKNOWN_OPCODE

# Events (section 9): frames of kind 3 the server sends unasked.
EVT_STOPPED = 0x8001
EVT_RESUMED = 0x8002
EVT_RESET = 0x8003
EVT_FRAME = 0x8004
EVT_BREAKPOINTS_CHANGED = 0x8006
EVT_CONTROL_CHANGED = 0x8009
EVT_OVERRUN = 0x800A
EVT_SHUTDOWN = 0x800B

EVENT_NAMES = {
    EVT_STOPPED: 'EVT_STOPPED', EVT_RESUMED: 'EVT_RESUMED',
    EVT_RESET: 'EVT_RESET', EVT_FRAME: 'EVT_FRAME',
    EVT_BREAKPOINTS_CHANGED: 'EVT_BREAKPOINTS_CHANGED',
    EVT_CONTROL_CHANGED: 'EVT_CONTROL_CHANGED', EVT_OVERRUN: 'EVT_OVERRUN',
    EVT_SHUTDOWN: 'EVT_SHUTDOWN',
}

# Event classes of SUBSCRIBE.
CLASS_RUN = 1 << 0       # EVT_STOPPED, EVT_RESUMED, EVT_RESET
CLASS_FRAME = 1 << 1     # EVT_FRAME
CLASS_MEDIA = 1 << 2
CLASS_DEBUG = 1 << 3
CLASS_MACHINE = 1 << 4
CLASS_ALL = (1 << 5) - 1

# EVT_STOPPED reasons.
STOP_PAUSED = 1
STOP_BREAKPOINT = 2
STOP_WATCHPOINT = 3
STOP_IO_BREAKPOINT = 4
STOP_STEP_DONE = 5
STOP_STEP_TIMEOUT = 6
STOP_STEP_STALLED = 7

# EVT_CONTROL_CHANGED reasons.
CONTROL_ACQUIRED = 1   # while it was free
CONTROL_TAKEN = 2      # by force
CONTROL_RELEASED = 3
CONTROL_LEFT = 4       # the controller disconnected or timed out

# STEP modes.
STEP_IN = 0
STEP_OVER = 1
STEP_OUT = 2
STEP_TO = 3
STEP_FRAMES = 4
MAX_STEP_IN_COUNT = 4096
DEFAULT_STEP_TIMEOUT_MS = 5000   # what timeout_ms 0 means for over, out, to

# Breakpoints: `kind` of an entry, `access` of a watch or I/O entry (and of
# EVT_STOPPED), and how many the probe arms of each kind.
BP_EXEC = 0
BP_WATCH = 1
BP_IO = 2
BP_ALL = 0xFF        # BP_CLEAR only
ACCESS_READ = 1      # a read, or IN
ACCESS_WRITE = 2     # a write, or OUT
ACCESS_BOTH = 3
MAX_EXEC_BREAKPOINTS = 32
MAX_WATCHPOINTS = 16
MAX_IO_BREAKPOINTS = 8

RESET_FLAG_STAY_PAUSED = 1 << 0

# Capability bits (section 6) of the groups implemented so far.
CAP_MACHINE_READ = 1 << 0
CAP_MACHINE_WRITE = 1 << 1
CAP_RUN_CONTROL = 1 << 2
CAP_EVENTS = 1 << 3
CAP_BREAKPOINTS = 1 << 4
CAP_INPUT = 1 << 5
CAP_MEM_ROM = 1 << 10

# Input (section 12). A key is a place in the CPC keyboard matrix: (row, bit).
# Row 9 is joystick 0; a pressed key reads as a clear bit in INPUT_STATE.
KEY_ROWS = 10
KEY_A = (8, 5)
KEY_SHIFT = (2, 5)
KEY_RETURN = (2, 2)
JOY0_UP = (9, 0)
JOY0_DOWN = (9, 1)
JOY0_LEFT = (9, 2)
JOY0_RIGHT = (9, 3)
JOY0_FIRE2 = (9, 4)
JOY0_FIRE1 = (9, 5)
MOUSE_LEFT = 1 << 0
MOUSE_MIDDLE = 1 << 1
MOUSE_RIGHT = 1 << 2
MOUSE_FLAG_BUTTONS = 1 << 0   # MOUSE: the `buttons` field is to be applied

# Memory spaces (section 11).
SPACE_CPU = 0        # what the Z80 would read now
SPACE_CPU_RAM = 1    # the RAM behind that address, ROMs ignored
SPACE_PHYS_RAM = 2   # physical RAM, flat
SPACE_ROM_LOWER = 3  # the lower ROM, paged in or not
SPACE_ROM_UPPER = 4  # upper ROM `bank`, paged in or not
SPACE_ASIC_PAGE = 5  # 6128+ register page; comes with CAP_CHIPS

CPU_SPACE_SIZE = 0x10000
ROM_SPACE_SIZE = 0x4000
ROM_SLOT_COUNT = 32
MEM_READ_REPLY_OVERHEAD = 16  # status header and stamp, before the bytes

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
IDLE_TIMEOUT = 16

STATUS_NAMES = {
    OK: 'OK', BAD_FRAME: 'BAD_FRAME',
    UNSUPPORTED_VERSION: 'UNSUPPORTED_VERSION',
    HELLO_REQUIRED: 'HELLO_REQUIRED', TOO_MANY_CLIENTS: 'TOO_MANY_CLIENTS',
    UNKNOWN_OPCODE: 'UNKNOWN_OPCODE', BAD_PAYLOAD: 'BAD_PAYLOAD',
    NOT_CONTROLLER: 'NOT_CONTROLLER', BUSY: 'BUSY',
    OUT_OF_RANGE: 'OUT_OF_RANGE', TOO_LARGE: 'TOO_LARGE',
    NOT_PRESENT: 'NOT_PRESENT', NOT_FOUND: 'NOT_FOUND',
    LIMIT_REACHED: 'LIMIT_REACHED', SHUTTING_DOWN: 'SHUTTING_DOWN',
    INTERNAL: 'INTERNAL', IDLE_TIMEOUT: 'IDLE_TIMEOUT',
}

ROLE_OBSERVER = 0
ROLE_CONTROLLER = 1

# CONTROL (section 12, Session).
CONTROL_RELEASE = 0
CONTROL_ACQUIRE = 1
CONTROL_FLAG_FORCE = 1 << 0

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


# The register block of section 12, in wire order.
REG16_NAMES = ('af', 'bc', 'de', 'hl', 'ix', 'iy', 'sp', 'pc',
               'af_alt', 'bc_alt', 'de_alt', 'hl_alt')
REG8_NAMES = ('i', 'r', 'iff1', 'iff2', 'im', 'halt')
REGS = struct.Struct('<12H6B2x')
# REGS_SET: bit N of the mask selects the Nth field of the block.
REG_BITS = {name: 1 << bit
            for bit, name in enumerate(REG16_NAMES + REG8_NAMES)}
REGS_SETTABLE = REG16_NAMES + REG8_NAMES[:-1]   # HALT can only be read


@dataclass
class Regs:
    stamp: Stamp
    af: int
    bc: int
    de: int
    hl: int
    ix: int
    iy: int
    sp: int
    pc: int
    af_alt: int
    bc_alt: int
    de_alt: int
    hl_alt: int
    i: int
    r: int
    iff1: int
    iff2: int
    im: int
    halt: int

    @staticmethod
    def read(fields: Fields) -> 'Regs':
        stamp = Stamp.read(fields)
        return Regs(stamp, *REGS.unpack(fields._take(REGS.size)))

    def values(self) -> dict:
        """Every register by name, without the stamp."""
        return {name: getattr(self, name)
                for name in REG16_NAMES + REG8_NAMES}


@dataclass
class MemInfo:
    ram_bytes: int    # the size of space PHYS_RAM
    ram_config: int   # RAM banking register
    rom_config: int   # bit 2: lower ROM off; bit 3: upper ROM off
    upper_rom: int    # selected upper-ROM slot
    rom_slots: int
    stamp: Stamp

    @property
    def lower_rom_on(self) -> bool:
        return not self.rom_config & 0x04

    @property
    def upper_rom_on(self) -> bool:
        return not self.rom_config & 0x08

    @staticmethod
    def read(fields: Fields) -> 'MemInfo':
        ram_bytes = fields.u32()
        ram_config = fields.u8()
        rom_config = fields.u8()
        upper_rom = fields.u8()
        fields.skip(1)
        rom_slots = fields.u32()
        return MemInfo(ram_bytes, ram_config, rom_config, upper_rom,
                       rom_slots, Stamp.read(fields))


@dataclass
class Control:
    role: int         # what this client holds now
    controller: int   # the controller's client_id; 0 = nobody

    @property
    def is_controller(self) -> bool:
        return self.role == ROLE_CONTROLLER

    @staticmethod
    def read(fields: Fields) -> 'Control':
        role = fields.u8()
        fields.skip(3)
        return Control(role, fields.u32())


@dataclass
class RunState:
    stamp: Stamp
    walk_active: bool   # a step is in progress
    controller: int     # the controller's client_id; 0 = nobody

    @staticmethod
    def read(fields: Fields) -> 'RunState':
        stamp = Stamp.read(fields)
        walk = fields.u8()
        fields.skip(3)
        return RunState(stamp, walk != 0, fields.u32())


@dataclass
class Event:
    """One pushed event, decoded. `seq` is its number on this connection:
    1 for the first event and one more for each one after it. Fields an
    event does not carry are 0."""
    opcode: int
    seq: int
    frame: int = 0
    reason: int = 0          # EVT_STOPPED, EVT_CONTROL_CHANGED
    access: int = 0
    pc: int = 0
    addr: int = 0
    value: int = 0
    old_value: int = 0
    cause_client: int = 0
    cause_request: int = 0
    fb_seq: int = 0          # EVT_FRAME
    controller: int = 0      # EVT_CONTROL_CHANGED
    previous: int = 0
    dropped: int = 0         # EVT_OVERRUN
    generation: int = 0      # EVT_BREAKPOINTS_CHANGED

    @property
    def name(self) -> str:
        return EVENT_NAMES.get(self.opcode, f'event {self.opcode:#06x}')

    @staticmethod
    def decode(frame: Frame) -> 'Event':
        event = Event(frame.opcode, frame.id)
        fields = Fields(frame.payload)
        if frame.opcode == EVT_STOPPED:
            event.frame = fields.u64()
            event.reason = fields.u8()
            event.access = fields.u8()
            event.pc = fields.u16()
            event.addr = fields.u16()
            event.value = fields.u8()
            event.old_value = fields.u8()
            event.cause_client = fields.u32()
            event.cause_request = fields.u32()
        elif frame.opcode in (EVT_RESUMED, EVT_RESET):
            event.frame = fields.u64()
            event.cause_client = fields.u32()
            event.cause_request = fields.u32()
        elif frame.opcode == EVT_FRAME:
            event.frame = fields.u64()
            event.fb_seq = fields.u64()
        elif frame.opcode == EVT_CONTROL_CHANGED:
            event.controller = fields.u32()
            event.previous = fields.u32()
            event.reason = fields.u8()
        elif frame.opcode == EVT_OVERRUN:
            event.dropped = fields.u32()
        elif frame.opcode == EVT_BREAKPOINTS_CHANGED:
            event.generation = fields.u64()
        return event


@dataclass
class BpEntry:
    """One breakpoint, watchpoint or I/O breakpoint. An entry is named by
    its kind and address (and mask, for I/O): adding one that exists
    replaces it."""
    kind: int = BP_EXEC
    addr: int = 0            # address, or port value for I/O
    access: int = 0          # watch and I/O: ACCESS_*; execution: 0
    len_or_mask: int = 0     # watch: length of the range; I/O: port mask
    pass_count: int = 0      # break only after this many hits
    hit_count: int = 0       # ignored in requests
    condition: str = ''      # the text IPC's expression syntax; '' = always

    @property
    def key(self) -> Tuple[int, int, int]:
        """What names the entry."""
        return (self.kind, self.addr,
                self.len_or_mask if self.kind == BP_IO else 0)

    def pack(self) -> bytes:
        return (struct.pack('<BBHH2xII', self.kind, self.access, self.addr,
                            self.len_or_mask, self.pass_count,
                            self.hit_count)
                + pack_str(self.condition))

    @staticmethod
    def read(fields: Fields) -> 'BpEntry':
        kind = fields.u8()
        access = fields.u8()
        addr = fields.u16()
        len_or_mask = fields.u16()
        fields.skip(2)
        pass_count = fields.u32()
        hit_count = fields.u32()
        return BpEntry(kind, addr, access, len_or_mask, pass_count,
                       hit_count, fields.string())


def bp_del_payload(kind: int, addr: int, mask: int = 0) -> bytes:
    return struct.pack('<BBHH2x', kind, 0, addr, mask)


def subscribe_payload(classes: int, frame_interval: int = 0) -> bytes:
    return struct.pack('<II', classes, frame_interval)


def step_payload(mode: int, count: int = 1, addr: int = 0,
                 timeout_ms: int = 0) -> bytes:
    return struct.pack('<BBHII', mode, 0, addr, count, timeout_ms)


def control_payload(action: int, flags: int = 0) -> bytes:
    return struct.pack('<BBH', action, flags, 0)


def regs_set_payload(mask: int, values: Optional[dict] = None) -> bytes:
    """`mask`, then a register block holding `values` (by name) and zeros
    elsewhere."""
    values = values or {}
    block = [values.get(name, 0) for name in REG16_NAMES + REG8_NAMES]
    return struct.pack('<I', mask) + REGS.pack(*block)


def mem_write_payload(space: int, addr: int, data: bytes,
                      bank: int = 0) -> bytes:
    return struct.pack('<BBHI', space, 0, bank, addr) + bytes(data)


def key_payload(row: int, bit: int, down: int) -> bytes:
    return struct.pack('<BBBx', row, bit, down)


def mouse_payload(dx: int, dy: int, buttons: Optional[int] = None) -> bytes:
    """`buttons` None leaves the buttons as they are."""
    flags = 0 if buttons is None else MOUSE_FLAG_BUTTONS
    return struct.pack('<hhBB', dx, dy, buttons or 0, flags)


def mem_read_payload(space: int, addr: int, length: int,
                     bank: int = 0) -> bytes:
    return struct.pack('<BBHII', space, 0, bank, addr, length)


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
        # Events that arrived while a response was being waited for, oldest
        # first. next_event() and wait_event() take them from here before
        # they read the socket.
        self.events: List[Frame] = []
        self.hello_reply: Optional[Hello] = None
        self.last_request_id = 0

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
        self.last_request_id = request_id
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

    # -- events ------------------------------------------------------------

    def next_event(self, timeout: Optional[float] = None) -> Event:
        """The next event on this connection, in the order the server sent
        them. Blocks in the socket read until one arrives: no polling.
        Raises socket.timeout after `timeout` seconds (default: the
        connection's), KcpcClosed when the server closes."""
        if self.events:
            return Event.decode(self.events.pop(0))
        previous = self.sock.gettimeout()
        if timeout is not None:
            self.sock.settimeout(timeout)
        try:
            while True:
                frame = self.read_frame()
                if frame.kind == KIND_EVENT:
                    return Event.decode(frame)
                if frame.opcode == OP_ERROR and frame.id == 0:
                    raise KcpcClosed(KcpcError(frame.status, frame.message))
                raise ValueError(f'response {frame.opcode:#06x} id '
                                 f'{frame.id} with no request waiting')
        finally:
            self.sock.settimeout(previous)

    def wait_event(self, opcode: int,
                   timeout: Optional[float] = None) -> Event:
        """The next event with this opcode; events of other kinds that come
        first are dropped."""
        while True:
            event = self.next_event(timeout)
            if event.opcode == opcode:
                return event

    def subscribe(self, classes: int, frame_interval: int = 0
                  ) -> Tuple[int, int]:
        """Replace this client's subscription. Returns (classes,
        frame_interval) as the server accepted them."""
        fields = self.request(OP_SUBSCRIBE,
                              subscribe_payload(classes, frame_interval))
        return fields.u32(), fields.u32()

    # -- run control ---------------------------------------------------------

    def run_state(self) -> RunState:
        return RunState.read(self.request(OP_RUN_STATE))

    def pause(self) -> Stamp:
        """Controller only. The stop is pushed as EVT_STOPPED, after this
        reply."""
        return Stamp.read(self.request(OP_PAUSE))

    def run(self) -> Stamp:
        return Stamp.read(self.request(OP_RUN))

    def reset(self, stay_paused: bool = False) -> Stamp:
        flags = RESET_FLAG_STAY_PAUSED if stay_paused else 0
        return Stamp.read(self.request(OP_RESET,
                                       struct.pack('<B3x', flags)))

    def step(self, count: int = 1, mode: int = STEP_IN, addr: int = 0,
             timeout_ms: int = 0) -> int:
        """Arm a step and return the request id. How it ended arrives as an
        EVT_STOPPED whose cause_request is that id. `addr` is where STEP_TO
        runs to; `timeout_ms` bounds STEP_OVER, STEP_OUT and STEP_TO (0 =
        DEFAULT_STEP_TIMEOUT_MS)."""
        self.request(OP_STEP, step_payload(mode, count, addr, timeout_ms))
        return self.last_request_id

    # -- breakpoints ---------------------------------------------------------

    def bp_list(self) -> Tuple[int, List[BpEntry]]:
        """(generation, entries): every execution breakpoint, watchpoint
        and I/O breakpoint, whoever set it."""
        fields = self.request(OP_BP_LIST)
        generation = fields.u64()
        return generation, [BpEntry.read(fields)
                            for _ in range(fields.u32())]

    def bp_add(self, entry: BpEntry) -> int:
        """Controller only. Adds the entry, or replaces the one with the
        same kind and address (and mask). Returns the new generation."""
        return self.request(OP_BP_ADD, entry.pack()).u64()

    def bp_del(self, kind: int, addr: int, mask: int = 0) -> int:
        return self.request(OP_BP_DEL, bp_del_payload(kind, addr, mask)).u64()

    def bp_clear(self, kind: int = BP_ALL) -> int:
        return self.request(OP_BP_CLEAR, struct.pack('<B', kind)).u64()

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

    def regs_get(self) -> Regs:
        return Regs.read(self.request(OP_REGS_GET))

    def mem_info(self) -> MemInfo:
        return MemInfo.read(self.request(OP_MEM_INFO))

    def mem_read_stamped(self, space: int, addr: int, length: int,
                         bank: int = 0) -> Tuple[Stamp, bytes]:
        """`length` bytes of `space` from `addr`, all from the one instant the
        stamp names. One request, one reply frame, whatever the length."""
        fields = self.request(OP_MEM_READ,
                              mem_read_payload(space, addr, length, bank))
        stamp = Stamp.read(fields)
        data = fields.rest()
        if len(data) != length:
            raise ValueError(f'MEM_READ returned {len(data)} bytes, asked '
                             f'for {length}')
        return stamp, data

    def mem_read(self, space: int, addr: int, length: int,
                 bank: int = 0) -> bytes:
        return self.mem_read_stamped(space, addr, length, bank)[1]

    # -- control and writes (controller only) -------------------------------

    def control(self, action: int, flags: int = 0) -> Control:
        return Control.read(self.request(OP_CONTROL,
                                         control_payload(action, flags)))

    def acquire_control(self, force: bool = False) -> Control:
        """Ask for control. Without `force` it is granted only when nobody
        has it; the reply says which role this client holds now. With
        `force` it is always granted and the previous controller becomes an
        observer: use it only when the user asked for it."""
        return self.control(CONTROL_ACQUIRE,
                            CONTROL_FLAG_FORCE if force else 0)

    def release_control(self) -> Control:
        return self.control(CONTROL_RELEASE)

    def regs_set(self, **values: int) -> Regs:
        """Set the registers named, and only those: `regs_set(pc=0x4000)`.
        Returns the register block after the write."""
        mask = 0
        for name in values:
            mask |= REG_BITS[name]
        return Regs.read(self.request(OP_REGS_SET,
                                      regs_set_payload(mask, values)))

    def mem_write(self, space: int, addr: int, data: bytes,
                  bank: int = 0) -> Stamp:
        """Write `data` to SPACE_CPU_RAM or SPACE_PHYS_RAM, in one request
        (at most MAX_REQUEST_PAYLOAD - 8 bytes)."""
        return Stamp.read(self.request(
            OP_MEM_WRITE, mem_write_payload(space, addr, data, bank)))

    # -- input (KEY, KEYS_RELEASE, TEXT and MOUSE: controller only) ----------

    def key(self, row: int, bit: int, down: bool = True) -> None:
        """Press or release the key at matrix position (row, bit). It stays
        down until it is released, KEYS_RELEASE is sent, or this client loses
        control. Modifiers are keys like any other: press KEY_SHIFT too."""
        self.request(OP_KEY, key_payload(row, bit, 1 if down else 0))

    def keys_release(self) -> None:
        """Release every key of the matrix, whoever pressed it."""
        self.request(OP_KEYS_RELEASE)

    def text(self, text: str) -> int:
        """Type `text` through the AutoTypeQueue (`~RETURN~` and the other
        tokens of the text IPC's `input type`). Returns how many key actions
        are now queued; the typing itself takes emulated time."""
        return self.request(OP_TEXT, pack_str(text)).u32()

    def mouse(self, dx: int = 0, dy: int = 0,
              buttons: Optional[int] = None) -> None:
        """Move the AMX or Symbiface mouse by (dx, dy) and, when `buttons`
        is given, set its buttons (MOUSE_LEFT | MOUSE_MIDDLE | MOUSE_RIGHT).
        NOT_PRESENT when no mouse is enabled."""
        self.request(OP_MOUSE, mouse_payload(dx, dy, buttons))

    def input_state(self) -> bytes:
        """The keyboard matrix, one byte per row (KEY_ROWS of them); a clear
        bit is a key that is down. Anyone may ask."""
        rows = self.request(OP_INPUT_STATE).rest()[:KEY_ROWS]
        if len(rows) != KEY_ROWS:
            raise ValueError(f'INPUT_STATE returned {len(rows)} rows')
        return rows
