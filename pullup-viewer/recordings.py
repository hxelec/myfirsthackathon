"""Shared watch/desktop CSV format and live TCP transport."""

from bisect import bisect_right
from collections import deque
import csv
from dataclasses import dataclass
import socket
import threading
import time

from imu import GRAVITY_COLUMNS, RAW_COLUMNS, TiltFilter, channels, from_angles, tilt_angles, unit
from motion import EXERCISES, Range, Rep, Sample, TiltRange, sample_at


HEADER = ["ms", "pitch_deg", "roll_deg"]
MAX_SAMPLES = 200_000


def pitch_roll(values):
    angles = tuple(map(float, values))
    if len(angles) != 2 or any(not -180 <= value <= 180 for value in angles):
        raise ValueError("Expected pitch and roll degrees from -180 to +180.")
    return angles


def timestamp(raw):
    if not raw.isascii() or not raw.isdigit() or len(raw) > 12:
        raise ValueError("Expected a non-negative integer timestamp in milliseconds.")
    return int(raw)


@dataclass(frozen=True)
class WatchTracking:
    axis: str
    main_deg: float | None
    off_deg: float | None
    bottom_deg: float | None
    concentric_deg: float | None
    finish_deg: float | None

    @classmethod
    def from_metadata(cls, metadata):
        axis = metadata.get("tracking_axis")
        if axis not in ("pitch", "roll", "both"):
            return None

        def degrees(key):
            try:
                value = float(metadata.get(key, ""))
            except ValueError:
                return None
            return value if 0 < value <= 180 else None

        return cls(axis, degrees("main_axis_tolerance_deg"), degrees("off_axis_tolerance_deg"),
                   degrees("bottom_tolerance_deg"), degrees("concentric_tolerance_deg"),
                   degrees("finish_tolerance_deg"))


@dataclass(frozen=True)
class Recording:
    samples: list[Sample]
    exercise: str | None = None
    calibration: Range | TiltRange | None = None
    watch_reps: tuple[Rep, ...] | None = None
    estimated_calibration: TiltRange | None = None
    exercise_name: str | None = None
    velocity_loss_pct: float | None = None
    cutoff_basis: str | None = None
    interrupted_rep_ms: int | None = None
    tracking: WatchTracking | None = None


def tilt_sample(t, gravity):
    return Sample(t, *tilt_angles(gravity), gravity)


def exercise_key(value):
    slug = ''.join(character for character in value.casefold() if character.isalnum())
    return next((key for key, exercise in EXERCISES.items()
                 if slug in (key.replace('_', ''), ''.join(c for c in exercise.name.casefold() if c.isalnum()))), None)


def watch_rep(samples, duration_ms, previous_finish):
    end = samples[-1].t
    start = round(end - duration_ms / 1000, 6)
    if start < max(samples[0].t, previous_finish):
        return None
    index = bisect_right(samples, start, key=lambda sample: sample.t)
    return Rep((sample_at(samples, start), *samples[index:]), 1.0)


def estimate_watch_calibration(bottoms, tops):
    if bottoms and tops:
        try:
            return TiltRange(*(tuple(map(sum, zip(*poses))) for poses in (bottoms, tops)))
        except ValueError:
            pass  # Endpoints may be too close or inconsistent in a partial set.
    return None


def load_recording(path):
    if path.stat().st_size > 24_000_000:
        raise ValueError("Choose a CSV smaller than 24 MB.")
    metadata, samples = {}, []
    footer = {}
    columns, first, previous = None, None, -1
    tilt_filter = TiltFilter()
    watch_reps, bottoms, tops = [], [], []
    previous_phase, previous_count, previous_finish = None, 0, -1
    with path.open(encoding="utf-8-sig", newline="") as stream:
        for index, line in enumerate(stream, 1):
            line_break = line.endswith(("\n", "\r"))
            line = line.strip()
            if not line:
                continue
            if line.startswith("#"):
                comment = line[1:].strip()
                if columns is None:
                    key, separator, value = comment.partition("=")
                    if separator:
                        metadata[key.strip()] = value.strip()
                elif comment.startswith('end='):
                    footer = dict(field.split('=', 1) for field in comment.split(',') if '=' in field)
                continue
            try:
                if len(line) > 256:
                    raise ValueError("CSV line is too long.")
                row = next(csv.reader([line], strict=True))
                if columns is None:
                    field_names = set(row)
                    euler = set(HEADER[1:]) <= field_names
                    raw = set(RAW_COLUMNS) <= field_names
                    tilt = set(GRAVITY_COLUMNS) <= field_names
                    watch_timing = {'phase', 'rep_number', 'rep_ms'} <= field_names
                    if len(row) != len(field_names) or "ms" not in field_names or not (euler or raw or tilt):
                        raise ValueError("Expected ms and pitch/roll, six raw IMU axes, or three tilt axes.")
                    columns = row
                    continue
                if len(row) != len(columns):
                    raise ValueError("Wrong number of columns.")
                values = dict(zip(columns, row))
                ms = timestamp(values['ms'])
                if ms <= previous:
                    raise ValueError("Timestamps must increase.")
                angles = pitch_roll([values[key] for key in HEADER[1:]]) if euler else None
                gravity = None
                if tilt:
                    gravity = unit(channels([values[key] for key in GRAVITY_COLUMNS], 3))
                elif euler and raw:
                    gravity = from_angles(*angles)  # RepWatch already fuses these readings on-device.
                elif raw:
                    gravity = tilt_filter.update(ms / 1000, channels([values[key] for key in RAW_COLUMNS], 6))
                if watch_timing:
                    count = timestamp(values['rep_number'])
                    duration_ms = timestamp(values['rep_ms'])
            except (ValueError, csv.Error) as error:
                if columns is not None and not line_break and len(samples) >= 2:
                    break  # A power loss may leave the last row incomplete.
                raise ValueError(f"Line {index}: {error}") from error
            previous = ms
            if gravity is None and angles is None:
                continue
            if first is None:
                first = ms
            t = (ms - first) / 1000
            sample = tilt_sample(t, gravity) if gravity is not None else Sample(t, *angles)
            samples.append(sample)
            if watch_timing:
                phase = values['phase']
                if phase == 'armed' and previous_phase != 'armed':
                    bottoms.append(sample.tilt())
                if count > previous_count and duration_ms:
                    tops.append(sample.tilt())
                    if rep := watch_rep(samples, duration_ms, previous_finish):
                        watch_reps.append(rep)
                    previous_finish = t
                previous_phase, previous_count = phase, count
            if len(samples) > MAX_SAMPLES:
                raise ValueError("Log exceeds 200,000 samples. Split it into smaller recordings.")
    if len(samples) < 2:
        raise ValueError("CSV needs at least two samples.")
    name = (metadata.get("exercise_name") or metadata.get("exercise") or '')[:96]
    exercise = exercise_key(metadata.get('exercise', '')) or exercise_key(name)
    raw_loss = metadata.get('velocity_loss_pct', metadata.get('velocity_loss_threshold_pct'))
    loss = None
    if raw_loss is not None:
        try:
            percent = float(raw_loss.removesuffix('%').strip())
            if 0 <= percent <= 100:
                loss = percent
        except ValueError:
            pass  # Optional metadata should not prevent motion playback.
    start, end = metadata.get("start_deg"), metadata.get("end_deg")
    if (start is None) != (end is None):
        raise ValueError("Include both start_deg and end_deg, or leave both out.")
    start_tilt, end_tilt = metadata.get('start_tilt'), metadata.get('end_tilt')
    if (start_tilt is None) != (end_tilt is None):
        raise ValueError('Include both start_tilt and end_tilt, or leave both out.')
    calibration = None
    if start_tilt is not None:
        calibration = TiltRange(channels(start_tilt.split(','), 3), channels(end_tilt.split(','), 3))
    elif start is not None:
        calibration = Range(pitch_roll(start.split(',')), pitch_roll(end.split(',')))
    exercise_name = name if 'exercise_name' in metadata or exercise is None else ''
    interrupted = footer.get('interrupted_rep_ms', '')
    interrupted_ms = int(interrupted) if interrupted.isascii() and interrupted.isdigit() and len(interrupted) <= 12 else 0
    return Recording(samples, exercise, calibration, tuple(watch_reps) if watch_timing else None,
                     estimate_watch_calibration(bottoms, tops), exercise_name or None, loss,
                     metadata.get('cutoff_basis'), interrupted_ms or None, WatchTracking.from_metadata(metadata))


class CsvLog:
    def __init__(self, path, key, calibration, with_tilt=False):
        self.stream = path.open("x", encoding="utf-8", newline="")
        self.with_tilt = with_tilt or isinstance(calibration, TiltRange)
        try:
            self.stream.write(f'# exercise={key}\n')
            for name in ('start', 'end'):
                if isinstance(calibration, TiltRange):
                    values = getattr(calibration, f'{name}_gravity')
                    field = f'{name}_tilt'
                else:
                    values = getattr(calibration, name)
                    field = f'{name}_deg'
                self.stream.write(f"# {field}={','.join(f'{value:.8g}' for value in values)}\n")
            self.writer = csv.writer(self.stream, lineterminator="\n")
            self.writer.writerow([*HEADER, *GRAVITY_COLUMNS] if self.with_tilt else HEADER)
            self.stream.flush()
        except OSError:
            self.stream.close()
            raise
        self.origin = None
        self.last_flush = 0

    def add(self, samples):
        for sample in samples:
            if self.origin is None:
                self.origin = sample.t
            gravity = sample.tilt() if self.with_tilt else ()
            self.writer.writerow((round(1000 * (sample.t - self.origin)), sample.pitch, sample.roll, *gravity))
        if samples and samples[-1].t - self.last_flush >= 1:
            self.stream.flush()
            self.last_flush = samples[-1].t

    def close(self):
        self.stream.close()


class OrientationReceiver:
    def __init__(self, host, port):
        self.address = (host, port)
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.queue = deque(maxlen=4096)
        self.status = "Connecting to watch"
        self.origin = time.monotonic()
        self.previous_t = -.36
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def drain(self):
        with self.lock:
            samples = list(self.queue)
            self.queue.clear()
            return self.status, samples

    def close(self):
        self.stop.set()
        self.thread.join(timeout=.08)

    def _set_status(self, status):
        with self.lock:
            self.status = status

    def _run(self):
        while not self.stop.is_set():
            try:
                with socket.create_connection(self.address, timeout=1) as stream:
                    self._receive(stream)
            except OSError:
                pass
            self._set_status("Reconnecting to watch")
            self.stop.wait(1)

    def _receive(self, stream):
        stream.settimeout(.25)
        self._set_status("Waiting for IMU data")
        started, previous_ms = time.monotonic(), None
        pending = bytearray()
        overflow = False
        tilt_filter = TiltFilter()
        previous_wire_t = None
        while not self.stop.is_set() and time.monotonic() - started < 2.5:
            try:
                packet = stream.recv(4096)
            except socket.timeout:
                if time.monotonic() - started > .6:
                    self._set_status("Waiting for IMU data")
                continue
            if not packet:
                break
            for byte in packet:
                if byte != 10:
                    if len(pending) < 256:
                        pending.append(byte)
                    else:
                        overflow = True
                    continue
                line = bytes(pending)
                pending.clear()
                too_long, overflow = overflow, False
                if too_long:
                    continue
                try:
                    fields = line.decode("ascii").rstrip("\r").split(",")
                    raw = fields[0] == 'IMU6' and len(fields) == 8
                    if not raw and (len(fields) not in (4, 5) or fields[0] != 'IMU'):
                        continue
                    ms = timestamp(fields[1])
                    if ms > 0xFFFFFFFF:
                        continue
                    data = channels(fields[2:], 6) if raw else pitch_roll(fields[2:4])
                except ValueError:
                    continue
                received = time.monotonic()
                if previous_ms is None:
                    t = max(received - self.origin, self.previous_t + .36)
                else:
                    interval = (ms - previous_ms) & 0xFFFFFFFF
                    if interval == 0:
                        continue
                    t = previous_wire_t + interval / 1000
                    if interval > 60_000:
                        return
                previous_ms, previous_wire_t, started = ms, t, received
                if raw:
                    gravity = tilt_filter.update(t, data)
                    if gravity is None:
                        continue
                    sample = tilt_sample(t, gravity)
                else:
                    sample = Sample(t, *data)
                self.previous_t = t
                with self.lock:
                    self.queue.append(sample)
                    self.status = 'Live / 6-axis IMU' if raw else 'Live / receiving pitch and roll'
