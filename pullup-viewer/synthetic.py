"""Deterministic six-axis examples, with analytically matched gyro and tilt."""

from math import cos, pi, radians, sin

from imu import TiltFilter
from motion import TiltRange
from recordings import tilt_sample


ARC_DEGREES = {'squat': 81, 'bench': 76, 'deadlift': 77, 'hammer_curl': 130, 'pullup': 143}


def gravity_at(theta, heading):
    return (-sin(heading) * sin(theta), cos(heading) * sin(theta), cos(theta))


def raw_session(key):
    base = radians(ARC_DEGREES[key])
    heading = .38
    calibration = TiltRange(gravity_at(0, heading), gravity_at(base, heading))
    readings = []
    for frame in range(661):
        t = frame / 50
        rep = int(t / 4.4)
        duration = 3.32 + .18 * (rep % 2)
        phase = max(0, min(1, (t % 4.4 - .40) / duration))
        peak = (1 - .025 * rep) * base
        theta = peak * sin(pi * phase) ** 2
        rate = (1 - .025 * rep) * ARC_DEGREES[key] * pi / duration * sin(2 * pi * phase)
        acceleration = gravity_at(theta, heading)
        readings.append((frame * 20, *acceleration, rate * cos(heading), rate * sin(heading), 0))
    return readings, calibration


def synthetic_session(key):
    rows, calibration = raw_session(key)
    fusion = TiltFilter()
    return [tilt_sample(ms / 1000, fusion.update(ms / 1000, raw)) for ms, *raw in rows], calibration
