"""Bounded TCP roll receiver and two-point circular calibration."""

from collections import deque
from dataclasses import dataclass
import json
import math
from pathlib import Path
import re
import socket
from statistics import median
import tempfile
import threading
import time


DEFAULT_HOST = "192.168.4.1"
DEFAULT_PORT = 8765
STALE_AFTER = 0.6
RECONNECT_AFTER = 2.5
MAX_LINE_BYTES = 64
ROLL_LINE = re.compile(rb"ROLL,([+-]?[0-9]+(?:\.[0-9]+)?)\r?")


class RollDecoder:
    """Decode complete lines across TCP reads and discard oversize records."""

    def __init__(self):
        self.pending = bytearray()
        self.discarding = False

    def feed(self, data: bytes):
        for byte in data:
            if byte == 10:
                if not self.discarding:
                    match = ROLL_LINE.fullmatch(self.pending)
                    if match:
                        value = float(match[1])
                        if -180.0 <= value <= 180.0:
                            yield value
                self.pending.clear()
                self.discarding = False
            elif not self.discarding:
                if len(self.pending) == MAX_LINE_BYTES:
                    self.pending.clear()
                    self.discarding = True
                else:
                    self.pending.append(byte)


def angle_delta(angle: float, reference: float) -> float:
    return (angle - reference + 180.0) % 360.0 - 180.0


@dataclass(frozen=True)
class Calibration:
    bottom: float = -60.0
    top: float = 60.0

    def __post_init__(self):
        for value in (self.bottom, self.top):
            if type(value) not in (int, float) or not -180 <= value <= 180:
                raise ValueError("Endpoints must be angles from -180 to +180 degrees.")
        if not 12.0 <= abs(self.span) <= 175.0:
            raise ValueError("Endpoint separation must be 12 to 175 degrees.")

    @property
    def span(self) -> float:
        return angle_delta(self.top, self.bottom)

    def phase(self, roll: float) -> float:
        # Unwrap around the midpoint so small overshoots clamp at the near end.
        middle = self.bottom + self.span / 2
        return min(1.0, max(0.0, 0.5 + angle_delta(roll, middle) / self.span))

    @classmethod
    def load(cls, path: Path):
        data = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(data, dict) or data.get("version") != 1:
            raise ValueError("Unsupported calibration file.")
        return cls(data.get("bottom_deg"), data.get("top_deg"))

    def save(self, path: Path):
        payload = {"version": 1, "bottom_deg": self.bottom, "top_deg": self.top}
        stream = tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent,
                                             prefix=path.name + ".", delete=False)
        temporary = Path(stream.name)
        try:
            with stream:
                stream.write(json.dumps(payload, indent=2) + "\n")
            temporary.replace(path)
        finally:
            temporary.unlink(missing_ok=True)


@dataclass(frozen=True)
class Snapshot:
    status: str
    samples: tuple[tuple[float, float], ...]

    def latest(self, now: float) -> float | None:
        if self.status == "Connected" and self.samples:
            received, value = self.samples[-1]
            if now - received <= STALE_AFTER:
                return value
        return None

    def stable_roll(self, now: float) -> float:
        if self.latest(now) is None or now - self.samples[-1][0] > 0.2:
            raise ValueError("Wait for fresh telemetry.")
        recent = [(stamp, value) for stamp, value in self.samples if now - stamp <= 0.8]
        if len(recent) < 5 or recent[-1][0] - recent[0][0] < 0.4:
            raise ValueError("Hold this pose for one second, then press again.")
        reference = recent[-1][1]
        offsets = [angle_delta(value, reference) for _, value in recent]
        if max(offsets) - min(offsets) > 5.0:
            raise ValueError("Roll is moving. Hold still and press again.")
        return angle_delta(reference + median(offsets), 0.0)


class RollReceiver:
    def __init__(self, host: str, port: int):
        self.address = (host, port)
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.status = "Connecting"
        self.samples = deque(maxlen=128)
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def snapshot(self) -> Snapshot:
        with self.lock:
            return Snapshot(self.status, tuple(self.samples))

    def close(self):
        self.stop.set()
        self.thread.join(timeout=1.2)

    def _run(self):
        while not self.stop.is_set():
            with self.lock:
                self.status = "Connecting"
                self.samples.clear()
            try:
                with socket.create_connection(self.address, timeout=1.0) as stream:
                    self._receive(stream)
            except OSError:
                pass
            with self.lock:
                self.status = "Reconnecting"
                self.samples.clear()
            self.stop.wait(1.0)

    def _receive(self, stream: socket.socket):
        stream.settimeout(0.25)
        decoder = RollDecoder()
        last_valid = time.monotonic()
        with self.lock:
            self.status = "Connected"
        while not self.stop.is_set() and time.monotonic() - last_valid < RECONNECT_AFTER:
            try:
                data = stream.recv(4096)
            except socket.timeout:
                continue
            if not data:
                return
            values = list(decoder.feed(data))
            if values:
                last_valid = time.monotonic()
                with self.lock:
                    self.samples.extend((last_valid, value) for value in values)


def demo_roll(elapsed: float) -> float:
    """Six-second cycle with brief pauses at the bottom and top."""
    cycle = elapsed % 6.0
    if cycle < 0.5:
        phase = 0.0
    elif cycle < 2.75:
        phase = (1.0 - math.cos(math.pi * (cycle - 0.5) / 2.25)) / 2.0
    elif cycle < 3.25:
        phase = 1.0
    else:
        phase = (1.0 + math.cos(math.pi * (cycle - 3.25) / 2.75)) / 2.0
    return -60.0 + 120.0 * phase
