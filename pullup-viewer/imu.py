"""Six-axis IMU tilt fusion in the sensor frame."""

from math import acos, atan2, cos, degrees, expm1, hypot, isfinite, radians, sin


RAW_COLUMNS = ['ax_g', 'ay_g', 'az_g', 'gx_dps', 'gy_dps', 'gz_dps']
GRAVITY_COLUMNS = ['tilt_x', 'tilt_y', 'tilt_z']


def channels(values, count):
    result = tuple(map(float, values))
    if len(result) != count or not all(map(isfinite, result)):
        raise ValueError('Expected finite IMU readings.')
    return result


def unit(vector):
    length = hypot(*vector)
    if length < 1e-8:
        raise ValueError('Tilt vector has no direction.')
    return tuple(value / length for value in vector)


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def angle(a, b):
    return degrees(acos(max(-1, min(1, dot(a, b)))))


def blend(a, b, fraction):
    return unit(tuple(x + fraction * (y - x) for x, y in zip(a, b)))


def tilt_angles(vector):
    x, y, z = vector
    return degrees(atan2(-x, hypot(y, z))), degrees(atan2(y, z))


def from_angles(pitch, roll):
    pitch, roll = radians(pitch), radians(roll)
    return -sin(pitch), cos(pitch) * sin(roll), cos(pitch) * cos(roll)


class TiltFilter:
    """Propagate tilt by gyro, then correct it slowly with near-1g acceleration."""

    def __init__(self):
        self.gravity = None
        self.previous = None

    def update(self, t, raw):
        acc, gyro = raw[:3], raw[3:]
        dt = 0 if self.previous is None else t - self.previous
        self.previous = t
        valid = abs(hypot(*acc) - 1) < .18
        if self.gravity is None or dt > .35:
            self.gravity = unit(acc) if valid else None
            return self.gravity
        x, y, z = self.gravity
        gx, gy, gz = (radians(rate) * dt for rate in gyro)
        predicted = unit((x + y * gz - z * gy, y + z * gx - x * gz, z + x * gy - y * gx))
        self.gravity = blend(predicted, unit(acc), -expm1(-dt / .6)) if valid else predicted
        return self.gravity
