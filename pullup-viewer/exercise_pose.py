"""Illustrative exercise poses driven by one calibrated motion parameter."""

from dataclasses import dataclass
from math import cos, sin, sqrt

from panda3d.core import Vec3

from pose import FOREARM, UPPER_ARM, Pose, pullup_pose


@dataclass(frozen=True)
class ExercisePose:
    body: Pose
    bar: tuple[float, float] | None  # y, z of bar center


def elbow_between(shoulder, wrist, bend):
    shoulder, wrist = Vec3(*shoulder), Vec3(*wrist)
    direction = (wrist - shoulder).normalized()
    distance = min((wrist - shoulder).length(), UPPER_ARM + FOREARM - .003)
    along = (UPPER_ARM**2 - FOREARM**2 + distance**2) / (2 * distance)
    guide = Vec3(*bend)
    perpendicular = (guide - direction * guide.dot(direction)).normalized()
    height = sqrt(max(0, UPPER_ARM**2 - along**2))
    return tuple(shoulder + direction * along + perpendicular * height)


def loaded_pose(key, phase):
    phase = min(1, max(0, phase))
    if key == "pullup":
        return ExercisePose(pullup_pose(phase), None)
    bar = None
    if key == "bench":
        hip = (.097, -.22, .54)
        shoulder = (.195, .309, .648)
        knee = (.153, -.623, .548)
        ankle = (.153, -.743, .140)
        wrist = (.335, .30 - .085 * phase, 1.205 - .386 * phase)
        elbow = elbow_between(shoulder, wrist, (.92, -.42, -.35))
        bar = (wrist[1], wrist[2] + .052)
    else:
        bend = phase if key == "squat" else (1 - phase if key == "deadlift" else 0)
        ankle = (.112, 0, .125)
        shin = (.50 if key == "squat" else .25) * bend
        thigh = (1.42 if key == "squat" else 1.39) * bend
        torso = (.56 if key == "squat" else 1.34) * bend
        knee = (.112, ankle[1] - .425 * sin(shin), ankle[2] + .425 * cos(shin))
        hip = (.097, knee[1] + .405 * sin(thigh), knee[2] + .405 * cos(thigh))
        shoulder = (.195, hip[1] - .542 * sin(torso), hip[2] + .542 * cos(torso))
        if key == "squat":
            wrist = (.416, shoulder[1] + .063, shoulder[2] + .012)
            elbow = elbow_between(shoulder, wrist, (.25, .55, -.82))
            bar = (wrist[1], wrist[2] + .052)
        elif key == "deadlift":
            wrist_y = -.204 + .03 * phase
            reach = sqrt(.579**2 - (.245 - shoulder[0])**2 - (wrist_y - shoulder[1])**2)
            wrist = (.245, wrist_y, shoulder[2] - reach)
            elbow = elbow_between(shoulder, wrist, (.45, -.65, -.10))
            bar = (wrist[1], wrist[2] + .052)
        else:
            elbow = (.220, shoulder[1] - .033 * phase, shoulder[2] - .313)
            flex = 2.27 * phase
            wrist = (elbow[0], elbow[1] - FOREARM * sin(flex), elbow[2] - FOREARM * cos(flex))
    return ExercisePose(Pose(shoulder, elbow, wrist, hip, knee, ankle), bar)
