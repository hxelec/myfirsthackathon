"""Pitch/roll calibration, repetition detection, and waveform comparison."""

from bisect import bisect_right
from dataclasses import dataclass
import math

from imu import angle, blend, dot, from_angles, tilt_angles, unit
from telemetry import angle_delta


@dataclass(frozen=True)
class Exercise:
    name: str
    mount: str
    start_name: str
    end_name: str
    start: tuple[float, float]
    end: tuple[float, float]


EXERCISES = {
    "squat": Exercise("Squat", "Thigh (femur)", "Standing", "Bottom", (0, 0), (80, 8)),
    "bench": Exercise("Bench press", "Upper arm (humerus)", "Press lockout", "Chest touch", (0, 0), (73, 26)),
    "deadlift": Exercise("Deadlift", "Torso over spine", "Floor setup", "Standing", (48, 4), (0, 0)),
    "hammer_curl": Exercise("Hammer curl", "Wrist (forearm)", "Arm down", "Curl top", (0, 0), (65, 70)),
    "pullup": Exercise("Pull-up", "Upper arm (humerus)", "Dead hang", "Pull-up top", (0, 0), (45, 135)),
}


@dataclass(frozen=True)
class Sample:
    t: float
    pitch: float
    roll: float
    gravity: tuple[float, float, float] | None = None

    def tilt(self):
        return self.gravity if self.gravity is not None else from_angles(self.pitch, self.roll)


@dataclass(frozen=True)
class Range:
    start: tuple[float, float]
    end: tuple[float, float]

    def __post_init__(self):
        for pair in (self.start, self.end):
            if len(pair) != 2 or any(not -180 <= value <= 180 for value in pair):
                raise ValueError("Enter two angles from -180 to +180 for each pose.")
        if any(abs(component) > 175 for component in self.vector) or self.span < 12:
            raise ValueError("Move at least 12° between poses and keep each axis within a 175° arc.")

    @property
    def vector(self):
        return tuple(angle_delta(end, start) for start, end in zip(self.start, self.end))

    @property
    def span(self):
        return math.hypot(*self.vector)

    def progress(self, sample):
        vector = self.vector
        offset = [
            angle_delta(value, start + component / 2)
            for value, start, component in zip((sample.pitch, sample.roll), self.start, vector)
        ]
        return .5 + sum(a * b for a, b in zip(offset, vector)) / sum(x * x for x in vector)


class TiltRange:
    """Project sensed gravity onto the signed great-circle arc between endpoints."""

    def __init__(self, start, end):
        self.start_gravity, self.end_gravity = unit(start), unit(end)
        self.span = angle(self.start_gravity, self.end_gravity)
        if not 12 <= self.span <= 175:
            raise ValueError("Choose tilt endpoints 12° to 175° apart.")
        self.start = tilt_angles(self.start_gravity)
        self.end = tilt_angles(self.end_gravity)
        along = dot(self.start_gravity, self.end_gravity)
        self.tangent = unit(tuple(b - along * a for a, b in zip(self.start_gravity, self.end_gravity)))
        x, y, z = self.start_gravity
        a, b, c = self.tangent
        self.normal = (y * c - z * b, z * a - x * c, x * b - y * a)

    def progress(self, sample):
        measured = sample.tilt()
        arc = math.degrees(math.atan2(dot(measured, self.tangent), dot(measured, self.start_gravity)))
        return .5 + angle_delta(arc, self.span / 2) / self.span

    def coordinates(self, sample):
        across = math.degrees(math.asin(max(-1, min(1, dot(sample.tilt(), self.normal)))))
        return self.progress(sample) * self.span, across


def default_range(key):
    exercise = EXERCISES[key]
    return Range(exercise.start, exercise.end)


def interpolate(a, b, t):
    fraction = (t - a.t) / (b.t - a.t) if b.t > a.t else 0
    if a.gravity is not None and b.gravity is not None:
        gravity = a.gravity if dot(a.gravity, b.gravity) < -.999 else blend(a.gravity, b.gravity, fraction)
        return Sample(t, *tilt_angles(gravity), gravity)
    return Sample(t,
                  angle_delta(a.pitch + fraction * angle_delta(b.pitch, a.pitch), 0),
                  angle_delta(a.roll + fraction * angle_delta(b.roll, a.roll), 0))


def sample_at(samples, t):
    index = bisect_right(samples, t, key=lambda sample: sample.t)
    if index == 0:
        return samples[0]
    if index == len(samples) or samples[index].t - samples[index - 1].t > .35:
        return samples[index - 1]
    return interpolate(samples[index - 1], samples[index], t)


@dataclass(frozen=True)
class Rep:
    samples: tuple[Sample, ...]
    peak: float

    @property
    def start(self):
        return self.samples[0].t

    @property
    def end(self):
        return self.samples[-1].t

    @property
    def duration(self):
        return self.end - self.start

    def trace(self, calibration, count=101):
        if isinstance(calibration, TiltRange):
            return [calibration.coordinates(sample_at(self.samples, self.start + self.duration * i / (count - 1)))
                    for i in range(count)]
        previous = self.samples[0]
        pitch = angle_delta(previous.pitch, calibration.start[0])
        roll = angle_delta(previous.roll, calibration.start[1])
        values = []
        for index in range(count):
            sample = sample_at(self.samples, self.start + self.duration * index / (count - 1))
            pitch += angle_delta(sample.pitch, previous.pitch)
            roll += angle_delta(sample.roll, previous.roll)
            values.append((pitch, roll))
            previous = sample
        return values


class RepDetector:
    """Find a start-zone departure, an excursion, then a return to start."""

    def __init__(self, calibration):
        self.calibration = calibration
        self.previous = None
        self.progress = None
        self.anchor = None
        self.departure = []
        self.active = []
        self.peak = 0.0

    def push(self, sample):
        target = self.calibration.progress(sample)
        dt = sample.t - self.previous.t if self.previous else 0
        if self.previous is None or dt > .35 or dt <= 0:
            self.anchor = None
            self.departure = []
            self.active = []
            self.progress = target
        else:
            self.progress += (target - self.progress) * -math.expm1(-dt / .065)
        self.previous = sample

        if not self.active:
            if self.progress <= .14:
                self.anchor = sample
                self.departure = []
            elif self.anchor is not None:
                if sample.t - self.anchor.t > 18:
                    self.anchor = None
                    self.departure = []
                else:
                    self.departure.append(sample)
                    if self.progress >= .22:
                        self.active = [self.anchor, *self.departure]
                        self.departure = []
                        self.peak = self.progress
            return None

        self.active.append(sample)
        self.peak = max(self.peak, self.progress)
        duration = sample.t - self.active[0].t
        if duration <= 18 and self.progress > .14:
            return None

        rep = None
        if .60 <= duration <= 18 and self.peak >= .72:
            rep = Rep(tuple(self.active), self.peak)
        self.active = []
        self.anchor = sample if self.progress <= .14 else None
        return rep


def find_reps(samples, calibration):
    detector = RepDetector(calibration)
    return [rep for sample in samples if (rep := detector.push(sample)) is not None]


@dataclass(frozen=True)
class Comparison:
    earlier: list[tuple[float, float]]
    later: list[tuple[float, float]]
    area: float
    score: float


def compare_reps(earlier, later, calibration):
    first, second = earlier.trace(calibration), later.trace(calibration)
    differences = [math.dist(a, b) for a, b in zip(first, second)]
    area = (sum(differences[1:-1]) + .5 * (differences[0] + differences[-1])) / (len(differences) - 1)
    score = 100 * max(0, 1 - area / calibration.span)
    return Comparison(first, second, area, score)


def demo_samples(key):
    calibration = default_range(key)
    samples = []
    for index in range(332):
        t = index / 25
        repetition = int(t / 4.4)
        cycle = (t % 4.4 - .38) / (3.42 + .14 * (repetition % 2))
        phase = math.sin(math.pi * min(1, max(0, cycle))) ** 2
        pitch, roll = [
            angle_delta(start + phase * (delta + drift * repetition), 0)
            for start, delta, drift in zip(calibration.start, calibration.vector, (1.8, -3.2))
        ]
        samples.append(Sample(t, pitch, roll))
    return samples
