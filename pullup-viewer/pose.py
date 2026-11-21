"""Constrained symmetric overhand pull-up in metres, with fixed wrists."""

from dataclasses import dataclass
from math import cos, radians, sin, sqrt


Point = tuple[float, float, float]
BAR_Z = 2.38
GRIP_X = 0.37
WRIST_Z = BAR_Z - 0.052
SHOULDER_X = 0.195
UPPER_ARM = 0.315
FOREARM = 0.270


@dataclass(frozen=True)
class Pose:
    shoulder: Point
    elbow: Point
    wrist: Point
    hip: Point
    knee: Point
    ankle: Point


def pullup_pose(phase: float) -> Pose:
    """Solve the positive-X side. Mirror its X coordinate for the other side."""
    phase = min(1.0, max(0.0, phase))
    inclination = radians(17 + 143 * phase)
    reach = UPPER_ARM * sin(inclination)
    upper_dz = UPPER_ARM * cos(inclination)
    shoulder_y = 0.105 - 0.043 * phase
    elbow_x = SHOULDER_X + reach * cos(0.23)
    elbow_y = shoulder_y + reach * sin(0.23)
    forearm_dz = sqrt(FOREARM**2 - (GRIP_X - elbow_x)**2 - elbow_y**2)
    elbow_z = WRIST_Z - forearm_dz
    shoulder_z = elbow_z - upper_dz

    # Prescribe two small sagittal rotations and preserve both leg lengths.
    hip = (0.097, shoulder_y + 0.042 + 0.07 * phase, shoulder_z - 0.54)
    thigh_angle = radians(-3 + 6 * phase)
    shin_angle = radians(8 + 13 * phase)
    knee = (hip[0], hip[1] + 0.405 * sin(thigh_angle), hip[2] - 0.405 * cos(thigh_angle))
    ankle = (knee[0], knee[1] + 0.425 * sin(shin_angle), knee[2] - 0.425 * cos(shin_angle))
    return Pose(
        (SHOULDER_X, shoulder_y, shoulder_z),
        (elbow_x, elbow_y, elbow_z),
        (GRIP_X, 0.0, WRIST_Z),
        hip, knee, ankle,
    )
