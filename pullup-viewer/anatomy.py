"""Procedural, lit 3D anatomy and pull-up rig for Panda3D."""

from math import cos, pi, sin, tau

from panda3d.core import (
    AmbientLight, DirectionalLight, Geom, GeomNode, GeomTriangles,
    GeomVertexData, GeomVertexFormat, GeomVertexWriter, LineSegs, Mat3, Quat, Vec3,
)

from pose import BAR_Z, FOREARM, GRIP_X, UPPER_ARM, WRIST_Z, Pose


SKIN = (0.75, 0.51, 0.37, 1)
SKIN_LIGHT = (0.77, 0.54, 0.40, 1)
SKIN_DARK = (0.55, 0.34, 0.26, 1)
SHORTS = (0.19, 0.26, 0.32, 1)
SHOES = (0.24, 0.31, 0.35, 1)
HAIR = (0.21, 0.17, 0.15, 1)
STEEL = (0.49, 0.63, 0.69, 1)
TEAL = (0.30, 0.86, 0.80, 1)
GRIP = (0.20, 0.36, 0.39, 1)


def smooth_profile(rings):
    """Subdivide a sculpted profile with slopes that preserve local extrema."""
    slopes = []
    for index, ring in enumerate(rings):
        previous = rings[index - 1] if index else ring
        following = rings[index + 1] if index + 1 < len(rings) else ring
        tangents = []
        for column in (1, 2, 3):
            left = (ring[column] - previous[column]) / (ring[0] - previous[0]) if index else 0
            right = (following[column] - ring[column]) / (following[0] - ring[0]) if following != ring else 0
            tangents.append(2 * left * right / (left + right) if left * right > 0 else 0)
        slopes.append(tangents)
    for index, (start, end) in enumerate(zip(rings, rings[1:])):
        dz = end[0] - start[0]
        for step in range(4):
            t = step / 4
            h00, h01 = (1 + 2 * t) * (1 - t)**2, t**2 * (3 - 2 * t)
            h10, h11 = t * (1 - t)**2, t**2 * (t - 1)
            yield (
                start[0] + t * dz,
                *(h00 * a + h01 * b + dz * (h10 * m + h11 * n)
                  for a, b, m, n in zip(start[1:], end[1:], slopes[index], slopes[index + 1])),
            )
    yield rings[-1]


def loft(parent, name, rings, color, sides=32, smooth=False):
    """Skin a series of elliptical rings (z, radius_x, radius_y, center_y)."""
    if smooth:
        rings = list(smooth_profile(rings))
    data = GeomVertexData(name, GeomVertexFormat.get_v3n3(), Geom.UH_static)
    data.set_num_rows(len(rings) * sides)
    vertices = GeomVertexWriter(data, "vertex")
    normals = GeomVertexWriter(data, "normal")
    for index, (z, radius_x, radius_y, center_y) in enumerate(rings):
        before = rings[max(0, index - 1)]
        after = rings[min(len(rings) - 1, index + 1)]
        for side in range(sides):
            angle = side * tau / sides
            x, y = cos(angle), sin(angle)
            around = Vec3(-radius_x * y, radius_y * x, 0)
            along = Vec3(
                (after[1] - before[1]) * x,
                (after[2] - before[2]) * y + after[3] - before[3],
                after[0] - before[0],
            )
            vertices.add_data3(radius_x * x, center_y + radius_y * y, z)
            normals.add_data3(around.cross(along).normalized())
    triangles = GeomTriangles(Geom.UH_static)
    for ring in range(len(rings) - 1):
        for side in range(sides):
            first = ring * sides + side
            second = ring * sides + (side + 1) % sides
            triangles.add_vertices(first, second, second + sides)
            triangles.add_vertices(first, second + sides, first + sides)
    for side in range(1, sides - 1):
        triangles.add_vertices(0, side + 1, side)
        end = (len(rings) - 1) * sides
        triangles.add_vertices(end, end + side, end + side + 1)
    geometry = Geom(data)
    geometry.add_primitive(triangles)
    node = GeomNode(name)
    node.add_geom(geometry)
    surface = parent.attach_new_node(node)
    surface.set_color(*color)
    return surface


def oval(parent, name, center, radii, color):
    rx, ry, rz = radii
    rings = []
    for index in range(17):
        angle = pi * (index / 16 - 0.5)
        outline = max(0.001, cos(angle))
        rings.append((rz * sin(angle), rx * outline, ry * outline, 0))
    surface = loft(parent, name, rings, color, sides=24)
    surface.set_pos(*center)
    return surface


def aim(node, origin, endpoint):
    """Point local Z along the segment and keep its Y toward the back."""
    up = (Vec3(*endpoint) - Vec3(*origin)).normalized()
    right = Vec3(0, 1, 0).cross(up)
    if right.length_squared() < 1e-8:
        right = Vec3(0, 0, 1).cross(up)
    right.normalize()
    back = up.cross(right)
    rotation = Quat()
    rotation.set_from_matrix(Mat3(*right, *back, *up))
    node.set_pos(*origin)
    node.set_quat(rotation)


def limb(parent, name, length, sections, color):
    rings = [(length * fraction, width, depth, y) for fraction, width, depth, y in sections]
    return loft(parent, name, rings, color, smooth=True)


def tube(parent, name, start, end, radius, color):
    length = (Vec3(*end) - Vec3(*start)).length()
    tube_node = loft(parent, name, [
        (0, radius, radius, 0),
        (0.001, radius, radius, 0),
        (length - 0.001, radius, radius, 0),
        (length, radius, radius, 0),
    ], color)
    aim(tube_node, start, end)
    return tube_node


def scene_lighting(render):
    ambient = AmbientLight("Soft ambient")
    ambient.set_color((0.46, 0.46, 0.48, 1))
    render.set_light(render.attach_new_node(ambient))
    for name, color, direction in (
        ("Warm key", (0.78, 0.68, 0.58, 1), (0.65, 1, -0.85)),
        ("Cool rim", (0.38, 0.58, 0.68, 1), (-0.5, -1, -0.35)),
    ):
        light = DirectionalLight(name)
        light.set_color(color)
        light.set_direction(Vec3(*direction))
        render.set_light(render.attach_new_node(light))


def backdrop(render):
    floor = render.attach_new_node("Reference grid")
    grid = LineSegs("Grid lines")
    grid.set_thickness(1)
    for index in range(-8, 9):
        offset = index * 0.22
        color = (0.16, 0.24, 0.28, 1) if index == 0 else (0.105, 0.156, 0.185, 1)
        grid.set_color(*color)
        grid.move_to(offset, -1.76, 0)
        grid.draw_to(offset, 1.76, 0)
        grid.move_to(-1.76, offset, 0)
        grid.draw_to(1.76, offset, 0)
    floor.attach_new_node(grid.create())
    floor.set_light_off()


class Arm:
    def __init__(self, parent, side):
        self.upper = limb(parent, "Upper arm", UPPER_ARM, [
            (-0.20, .002, .002, 0), (-0.14, .041, .043, 0), (0, .075, .072, 0),
            (.15, .074, .074, -.005), (.33, .061, .064, -.005),
            (.54, .053, .057, -.006), (.7, .046, .048, 0),
            (.91, .038, .039, 0), (1.06, .038, .035, 0), (1.14, .002, .002, 0),
        ], SKIN)
        self.lower = limb(parent, "Forearm", FOREARM, [
            (-.15, .002, .002, 0), (-.06, .032, .036, 0), (.07, .042, .047, 0),
            (.24, .046, .052, -.004), (.43, .041, .045, -.004),
            (.70, .031, .033, -.003), (1, .024, .027, 0), (1.04, .002, .002, 0),
        ], SKIN_LIGHT)
        oval(self.lower, "Elbow", (0, 0, -.006), (.037, .039, .039), SKIN)
        self.hand = parent.attach_new_node("Grip hand")
        self._hand(self.hand, side)
        self.dumbbell = self.hand.attach_new_node("Dumbbell")
        tube(self.dumbbell, "Handle", (-.078, 0, .052), (.078, 0, .052), .012, STEEL)
        for offset in (-1, 1):
            tube(self.dumbbell, "Dumbbell head", (offset * .056, 0, .052),
                 (offset * .086, 0, .052), .041, SHOES)
        self.dumbbell.hide()

    def _hand(self, parent, side):
        grip_x = 0
        oval(parent, "Palm", (grip_x, .025, .028), (.038, .023, .050), SKIN)
        for index in range(4):
            x = grip_x + (index - 1.5) * .017
            for section in range(3):
                start = 0.35 + section * .71
                end = start + .79
                first = (x, .029 * cos(start), BAR_Z - WRIST_Z + .029 * sin(start))
                second = (x, .029 * cos(end), BAR_Z - WRIST_Z + .029 * sin(end))
                center = tuple((a + b) / 2 for a, b in zip(first, second))
                joint = oval(parent, "Curled finger", (0, 0, 0), (.010, .012, .019), SKIN_LIGHT)
                aim(joint, center, second)
        thumb = oval(parent, "Thumb", (0, 0, 0), (.013, .013, .028), SKIN)
        aim(thumb, (grip_x - side * .037, .003, .030),
            (grip_x - side * .020, -.023, .039))

    def update(self, shoulder, elbow, wrist):
        aim(self.upper, shoulder, elbow)
        aim(self.lower, elbow, wrist)
        self.hand.set_pos(*wrist)


def watch_marker(parent, center, width=.038):
    marker = parent.attach_new_node("Watch")
    marker.set_pos(*center)
    oval(marker, "Watch housing", (0, 0, 0), (width, .015, width * .86), TEAL)
    oval(marker, "Watch face", (0, -.012, 0), (width * .75, .008, width * .62), GRIP)
    indicator = oval(marker, "Indicator", (0, -.020, .007), (.007, .004, .007), TEAL)
    indicator.set_light_off()
    return marker


class Leg:
    def __init__(self, parent):
        self.thigh = limb(parent, "Thigh", .405, [
            (-.13, .001, .001, 0), (-.08, .061, .068, 0), (0, .090, .104, 0),
            (.23, .089, .103, -.005), (.47, .070, .085, -.006),
            (.71, .057, .065, 0), (.97, .048, .050, 0), (1.05, .044, .044, 0),
            (1.12, .001, .001, 0),
        ], SKIN)
        limb(self.thigh, "Shorts leg", .405, [
            (-.14, .002, .002, 0), (-.10, .064, .073, 0), (0, .094, .109, 0),
            (.23, .093, .108, -.005), (.46, .080, .093, -.005),
        ], SHORTS)
        self.shin = limb(parent, "Calf", .425, [
            (-.12, .001, .001, 0), (-.02, .038, .037, 0), (.16, .050, .056, .004),
            (.31, .056, .066, .010), (.53, .047, .051, .006),
            (.77, .029, .032, .003), (1.02, .028, .029, 0), (1.06, .001, .001, 0),
        ], SKIN_LIGHT)
        oval(self.shin, "Knee", (0, -.004, 0), (.044, .047, .040), SKIN)
        self.foot = parent.attach_new_node("Foot")
        shoe = oval(self.foot, "Shoe", (0, -.058, -.060), (.045, .121, .047), SHOES)
        shoe.set_p(17)
        sole = oval(self.foot, "Sole", (0, -.065, -.081), (.046, .120, .022), SHORTS)
        sole.set_p(17)

    def update(self, hip, knee, ankle, phase):
        aim(self.thigh, hip, knee)
        aim(self.shin, knee, ankle)
        self.foot.set_pos(*ankle)
        self.foot.set_p(-10 * phase)


class Anatomy:
    def __init__(self, render):
        parent = render.attach_new_node("Pull-up model")
        self.pullup_bar = parent.attach_new_node("Pull-up bar")
        tube(self.pullup_bar, "Steel pull-up bar", (-.82, 0, BAR_Z), (.82, 0, BAR_Z), .019, STEEL)
        for side in (1, -1):
            center = side * GRIP_X
            tube(self.pullup_bar, "Bar grip", (center - .094, 0, BAR_Z),
                 (center + .094, 0, BAR_Z), .024, GRIP)
            for mark in (center - .095, center + .095):
                tube(self.pullup_bar, "Grip edge", (mark - .004, 0, BAR_Z),
                     (mark + .004, 0, BAR_Z), .025, TEAL)
            tube(self.pullup_bar, "Bar cap", (side * .813, 0, BAR_Z),
                 (side * .833, 0, BAR_Z), .025, GRIP)

        self.torso = parent.attach_new_node("Body")
        loft(self.torso, "Torso", [
            (-.60, .027, .039, 0), (-.56, .135, .084, .001), (-.49, .162, .103, .004),
            (-.415, .144, .090, .007), (-.35, .128, .077, .002),
            (-.275, .137, .093, -.002), (-.20, .158, .107, -.006),
            (-.13, .186, .109, -.009), (-.065, .204, .100, -.007),
            (-.012, .191, .086, -.003), (.034, .145, .062, 0),
            (.071, .061, .057, -.006), (.089, .033, .037, -.007),
        ], SKIN, smooth=True)
        loft(self.torso, "Waistband and pelvis", [
            (-.61, .030, .035, .002), (-.57, .141, .091, .005),
            (-.50, .167, .110, .004), (-.44, .155, .101, .005),
            (-.408, .146, .094, .006),
        ], SHORTS, smooth=True)
        loft(self.torso, "Neck", [
            (.024, .063, .059, -.003), (.091, .053, .052, -.004),
            (.145, .056, .048, -.013), (.174, .042, .046, -.017),
        ], SKIN)
        loft(self.torso, "Head", [
            (.113, .020, .024, -.045), (.131, .049, .055, -.028),
            (.155, .064, .067, -.022), (.188, .074, .076, -.010),
            (.228, .080, .078, -.008), (.273, .077, .074, -.005),
            (.311, .059, .060, 0), (.330, .032, .034, .003),
            (.335, .001, .001, .006),
        ], SKIN_LIGHT, smooth=True)
        hair = oval(self.torso, "Cropped hair", (0, .013, .279), (.080, .074, .061), HAIR)
        hair.set_p(-16)
        nose = oval(self.torso, "Nose", (0, -.087, .207), (.016, .019, .025), SKIN)
        nose.set_p(-12)
        for side in (-1, 1):
            oval(self.torso, "Ear", (.076 * side, -.003, .198), (.012, .017, .026), SKIN)
            eye = oval(self.torso, "Eye", (.033 * side, -.079, .232), (.015, .005, .004), SKIN_DARK)
            eye.set_h(15 * side)
        oval(self.torso, "Mouth", (0, -.088, .169), (.023, .003, .003), SKIN_DARK)
        self.arms = [(side, Arm(parent, side)) for side in (1, -1)]
        self.legs = [(side, Leg(parent)) for side in (1, -1)]
        self.upper_arm_watch = watch_marker(self.arms[0][1].upper, (0, -.060, .150))
        self.markers = {
            "squat": watch_marker(self.legs[0][1].thigh, (0, -.111, .130)),
            "deadlift": watch_marker(self.torso, (0, .106, -.246)),
            "hammer_curl": watch_marker(self.arms[0][1].lower, (0, -.030, .250), .027),
        }
        self.markers["deadlift"].set_h(180)
        self.upper_arm_watch.hide()
        for marker in self.markers.values():
            marker.hide()
        self.weights = parent.attach_new_node("Barbell")
        tube(self.weights, "Steel shaft", (-.83, 0, 0), (.83, 0, 0), .019, STEEL)
        self.plates = []
        for side in (-1, 1):
            self.plates.append(tube(self.weights, "Plate", (side * .66, 0, 0),
                                    (side * .723, 0, 0), .195, SHOES))
            self.plates.append(tube(self.weights, "Plate rim", (side * .68, 0, 0),
                                    (side * .69, 0, 0), .197, SHORTS))
        self.weights.hide()
        self.bench = parent.attach_new_node("Bench")
        cushion = oval(self.bench, "Bench cushion", (0, .126, .491), (.151, .56, .085), GRIP)
        cushion.set_scale(1, 1, .71)
        for y in (-.18, .46):
            tube(self.bench, "Bench support", (0, y, .055), (0, y, .490), .033, STEEL)
            tube(self.bench, "Bench foot", (-.215, y, .047), (.215, y, .047), .025, GRIP)
        self.bench.hide()

    def exercise(self, key):
        self.pullup_bar.show() if key == "pullup" else self.pullup_bar.hide()
        self.weights.show() if key in ("squat", "bench", "deadlift") else self.weights.hide()
        self.upper_arm_watch.show() if key in ("bench", "pullup") else self.upper_arm_watch.hide()
        self.bench.show() if key == "bench" else self.bench.hide()
        for name, marker in self.markers.items():
            marker.show() if name == key else marker.hide()
        for _, arm in self.arms:
            arm.dumbbell.show() if key == "hammer_curl" else arm.dumbbell.hide()
        plate_scale = {"squat": .88, "bench": .69}.get(key, 1)
        for plate in self.plates:
            plate.set_scale(plate_scale, plate_scale, 1)

    def update(self, pose: Pose, phase: float):
        shoulder_center = (0, *pose.shoulder[1:])
        hip_center = (0, *pose.hip[1:])
        torso_axis = Vec3(*shoulder_center) - Vec3(*hip_center)
        aim(self.torso, shoulder_center, Vec3(*shoulder_center) + torso_axis)

        def on_side(point, side):
            return (point[0] * side, *point[1:])

        for side, arm in self.arms:
            arm.update(*(on_side(point, side) for point in (pose.shoulder, pose.elbow, pose.wrist)))
        for side, leg in self.legs:
            leg.update(*(on_side(point, side) for point in (pose.hip, pose.knee, pose.ankle)), phase)

    def update_exercise(self, exercise_pose, phase, key):
        self.update(exercise_pose.body, phase)
        if exercise_pose.bar is not None:
            self.weights.set_pos(0, *exercise_pose.bar)
        if key == "hammer_curl":
            neutral_grip = Quat()
            neutral_grip.set_hpr((90, 0, 0))
            for _, arm in self.arms:
                arm.hand.set_quat(neutral_grip * arm.lower.get_quat())
        else:
            for _, arm in self.arms:
                arm.hand.set_hpr(0, 0, 0)
