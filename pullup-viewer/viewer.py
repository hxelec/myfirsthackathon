"""Panda3D window, camera controls, and live pose display."""

import math
import time

from direct.gui.OnscreenText import OnscreenText
from direct.showbase.ShowBase import ShowBase
from panda3d.core import AntialiasAttrib, CardMaker, TextNode, load_prc_file_data

from anatomy import Anatomy, backdrop, scene_lighting
from pose import pullup_pose
from telemetry import Calibration, demo_roll


INK = (.91, .96, .96, 1)
MUTED = (.62, .73, .76, 1)
TEAL = (.37, .91, .84, 1)
AMBER = (1, .75, .49, 1)
METER_WIDTH = .43


def label(parent, text, x, y, size, color=MUTED, align=TextNode.A_left, dynamic=False):
    return OnscreenText(parent=parent, text=text, pos=(x, y), scale=size, fg=color,
                        align=align, mayChange=dynamic)


def card(parent, name, x, z, width, height, color):
    maker = CardMaker(name)
    maker.set_frame(0, width, -height, 0)
    rectangle = parent.attach_new_node(maker.generate())
    rectangle.set_pos(x, 0, z)
    rectangle.set_color(*color)
    return rectangle


class PullupViewer(ShowBase):
    window_title = "Pull-up Anatomy"

    def __init__(self, args, calibration, calibrated, receiver, started):
        load_prc_file_data("", f"""
            window-title {self.window_title}
            win-size 1240 760
            framebuffer-multisample 1
            multisamples 4
            sync-video true
            audio-library-name null
        """)
        super().__init__()
        self.disable_mouse()
        self.set_background_color(.036, .057, .073)
        self.render.set_antialias(AntialiasAttrib.M_multisample)
        self.camLens.set_min_fov(36)
        self.camLens.set_near_far(.08, 30)
        scene_lighting(self.render)
        backdrop(self.render)
        self.anatomy = Anatomy(self.render)

        self.calibration_path = args.calibration
        self.calibration = calibration
        self.calibrated = calibrated
        self.captured = {}
        self.receiver = receiver
        self.started = started
        self.previous_frame = started
        self.phase = 0.0
        self.has_target = False
        self.last_roll = None
        self.message_until = 0.0
        self.dragging = False
        self.last_mouse = None
        self._make_overlay()
        self._reset_view()
        self.accept("mouse1", self._begin_orbit)
        self.accept("mouse1-up", self._end_orbit)
        self.accept("wheel_up", self._zoom, [0.90])
        self.accept("wheel_down", self._zoom, [1.11])
        self.accept("home", self._reset_view)
        self.accept("escape", self.userExit)
        if self.receiver is not None:
            self.accept("b", self._capture, ["bottom"])
            self.accept("t", self._capture, ["top"])
        self.task_mgr.add(self._update, "Update pose")

    def _make_overlay(self):
        left, right = self.a2dTopLeft, self.a2dTopRight
        label(left, "PULL-UP", .073, -.112, .066, INK)
        label(left, "ANATOMY VIEWER", .077, -.171, .034)
        label(left, "ESTIMATED RANGE", .077, -.288, .029, TEAL)
        card(left, "Range track", .077, -.37, METER_WIDTH, .009, (.19, .27, .32, 1))
        self.meter = card(left, "Range fill", .077, -.37, METER_WIDTH, .009, TEAL)
        self.percent = label(left, "0%", .073, -.347, .049, INK, dynamic=True)

        if self.receiver is None:
            label(right, "DEMO", -.073, -.109, .042, TEAL, TextNode.A_right)
        else:
            label(right, "ESP32  /  TCP", -.073, -.105, .040, TEAL, TextNode.A_right)
        self.status = label(right, "", -.073, -.166, .035, MUTED, TextNode.A_right, True)
        self.roll = label(right, "", -.073, -.223, .035, INK, TextNode.A_right, True)

        bottom = self.a2dBottomLeft
        if self.receiver is not None:
            self.notice = label(bottom, "", .077, .172, .035, AMBER, dynamic=True)
            label(bottom, "B  Bottom capture     T  Top capture", .077, .114, .033, INK)
        label(bottom, "Drag  Orbit     Scroll  Zoom     Home  Reset view", .077, .061, .033)

    def _reset_view(self):
        self.azimuth = .30
        self.elevation = .07
        self.distance = 4.65
        self._position_camera()

    def _position_camera(self):
        flat = self.distance * math.cos(self.elevation)
        self.camera.set_pos(flat * math.sin(self.azimuth), -flat * math.cos(self.azimuth),
                            1.35 + self.distance * math.sin(self.elevation))
        self.camera.look_at(0, 0, 1.35)

    def _begin_orbit(self):
        self.dragging = True
        self.last_mouse = self.win.get_pointer(0)

    def _end_orbit(self):
        self.dragging = False

    def _orbit(self):
        if self.dragging and self.mouseWatcherNode.has_mouse():
            mouse = self.win.get_pointer(0)
            if self.last_mouse.get_in_window():
                dx = mouse.get_x() - self.last_mouse.get_x()
                dy = mouse.get_y() - self.last_mouse.get_y()
                self.azimuth = (self.azimuth - dx * .007) % math.tau
                self.elevation = min(1.38, max(-1.18, self.elevation + dy * .007))
                self._position_camera()
            self.last_mouse = mouse
        elif self.dragging:
            self.last_mouse = self.win.get_pointer(0)
            if not self.last_mouse.get_in_window():
                self._end_orbit()

    def _zoom(self, factor):
        self.distance = min(12.0, max(1.85, self.distance * factor))
        self._position_camera()

    def _capture(self, endpoint):
        now = time.monotonic()
        self.message_until = now + 5
        try:
            roll = self.receiver.snapshot().stable_roll(now)
            self.captured[endpoint] = roll
            if len(self.captured) == 2:
                calibration = Calibration(**self.captured)
                self.calibration = calibration
                self.calibrated = True
                self.captured.clear()
                try:
                    calibration.save(self.calibration_path)
                except OSError as error:
                    print(f"Unable to save {self.calibration_path}: {error}")
                    self.notice.setText("Calibrated for this session. Could not save file.")
                else:
                    self.notice.setText("Calibration saved.")
            else:
                other = "T at the top" if endpoint == "bottom" else "B at the hang"
                self.notice.setText(f"{endpoint.title()} captured at {roll:+.1f} deg. Press {other}.")
        except ValueError as error:
            self.notice.setText(str(error))

    def _update(self, task):
        now = time.monotonic()
        dt = min(now - self.previous_frame, 0.10)
        self.previous_frame = now
        self._orbit()
        if self.receiver is None:
            roll = demo_roll(now - self.started)
            state = "Simulated roll"
        else:
            snapshot = self.receiver.snapshot()
            roll = snapshot.latest(now)
            state = snapshot.status
            if state == "Connected":
                state = "Telemetry paused" if roll is None else "Live telemetry"
            if now >= self.message_until:
                self.notice.setText("" if self.calibrated else "Capture both endpoints to calibrate.")

        if roll is not None:
            target = self.calibration.phase(roll)
            blend = -math.expm1(-dt / .095)
            self.phase = self.phase + (target - self.phase) * blend if self.has_target else target
            self.has_target = True
            self.last_roll = roll

        self.anatomy.update(pullup_pose(self.phase), self.phase)
        self.meter.set_scale(self.phase, 1, 1)
        self.percent.setText(f"{self.phase:.0%}")
        self.status.setText(state)
        if self.last_roll is not None:
            prefix = "Roll" if roll is not None else "Last roll"
            self.roll.setText(f"{prefix}  {self.last_roll:+.1f} deg")
        return task.cont
