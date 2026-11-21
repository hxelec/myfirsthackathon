"""Desktop workspace for viewing, recording, and comparing exercises."""

from collections import deque
from concurrent.futures import Future
import math
import os
from pathlib import Path
from statistics import median
from threading import Thread
import time

from direct.gui import DirectGuiGlobals as DGG
from direct.gui.DirectGui import DirectButton, DirectOptionMenu, DirectSlider
from panda3d.core import LineSegs, TextNode

from exercise_pose import loaded_pose
from imu import angle, from_angles, unit
from llm_notes import request_form_review, request_note, summarize_form, summarize_motion
from motion import EXERCISES, Range, RepDetector, TiltRange, compare_reps, default_range, find_reps, sample_at
from recordings import CsvLog, OrientationReceiver, load_recording, pitch_roll
from synthetic import synthetic_session
from telemetry import angle_delta
from viewer import AMBER, INK, MUTED, TEAL, PullupViewer, card, label


PANEL = (.062, .094, .117, 1)
BUTTON = (.13, .20, .24, 1)
GRID = (.20, .29, .34, 1)
BLUE = (.49, .66, .94, 1)
EXERCISE_KEYS = tuple(EXERCISES)
EXERCISE_NAMES = [exercise.name for exercise in EXERCISES.values()]


def background(function, *args):
    future = Future()

    def run():
        if future.set_running_or_notify_cancel():
            try:
                future.set_result(function(*args))
            except Exception as error:
                future.set_exception(error)

    Thread(target=run, daemon=True).start()
    return future


def dialog(action, **options):
    import tkinter as tk
    from tkinter import filedialog, messagebox, simpledialog

    root = tk.Tk()
    try:
        root.withdraw()
        root.attributes("-topmost", True)
        controls = {"open": filedialog.askopenfilename, "save": filedialog.asksaveasfilename,
                    "angles": simpledialog.askstring, "confirm": messagebox.askyesno}
        return controls[action](parent=root, **options)
    finally:
        root.destroy()


def button(parent, text, x, z, width, command):
    return DirectButton(parent=parent, pos=(x, 0, z), text=text, text_scale=.032,
                        text_align=TextNode.A_left, text_pos=(.017, -.011), text_fg=INK,
                        relief=DGG.FLAT, frameColor=BUTTON,
                        frameSize=(0, width, -.039, .039), command=command,
                        pressEffect=False)


def multiline(parent, x, z, size, color=MUTED, width=.58):
    text = label(parent, "", x, z, size, color, dynamic=True)
    text.textNode.set_wordwrap(width / size)
    return text


class LiftViewer(PullupViewer):
    window_title = "Lift Studio"

    def __init__(self, args):
        self.key = args.exercise
        super().__init__(args, default_range(self.key), False, None, time.monotonic())
        self.address = (args.host, args.port)
        self.watch = None
        self.csv = None
        self.load_job = None
        self.note_job = None
        self.review_job = None
        self.review_enabled = False
        self.file_recording = None
        self.file_exercise_confirmed = False
        self.generation = 0
        self.cloud = False
        self.last_sent = 0
        self.next_note = 0
        self.mode = "demo"
        self.samples = []
        self.history = deque(maxlen=50)
        self.reps = []
        self.detector = RepDetector(self.calibration)
        self.captured = {}
        self.current = None
        self.cursor = 0
        self.playing = False
        self.timeline_sync = 0
        self.pair_index = 0
        self.calibration_source = "Demo angles"
        self._build_dashboard()
        self.accept("space", self._toggle_play)
        self._load_demo()
        if args.file:
            self._request_import(args.file)
        if args.live:
            self._connect(args.host, args.port)

    def _make_overlay(self):
        pass

    def _reset_view(self):
        self.azimuth = {"squat": 1.00, "bench": 1.17, "deadlift": 1.92, "hammer_curl": .80,
                        "pullup": .63}[self.key]
        self.elevation = .23 if self.key == "bench" else .16
        self.distance = 4.60 if self.key == "pullup" else 3.75
        self._position_camera()

    def _position_camera(self):
        target = {"bench": .66, "pullup": 1.47}.get(self.key, .97)
        flat = self.distance * math.cos(self.elevation)
        self.camera.set_pos(flat * math.sin(self.azimuth), -flat * math.cos(self.azimuth),
                            target + self.distance * math.sin(self.elevation))
        self.camera.look_at(0, 0, target)

    def _begin_orbit(self):
        if self.mouseWatcherNode.has_mouse():
            mouse = self.mouseWatcherNode.get_mouse()
            if abs(mouse.x) < .57 and mouse.y > -.78:
                super()._begin_orbit()

    def _build_dashboard(self):
        left, right = self.a2dTopLeft, self.a2dTopRight
        card(left, "Settings panel", .029, -.033, .706, 1.70, PANEL)
        card(right, "Analysis panel", -.750, -.033, .721, 1.70, PANEL)
        label(left, "LIFT STUDIO", .073, -.123, .064, INK)
        label(left, "Exercise", .076, -.229, .038, TEAL)
        self.menu = DirectOptionMenu(parent=left, pos=(.077, 0, -.303), items=EXERCISE_NAMES,
                                     initialitem=EXERCISE_KEYS.index(self.key), text_scale=.044,
                                     text_align=TextNode.A_left, text_pos=(.018, -.016), text_fg=INK,
                                     item_text_fg=INK, item_text_scale=(.041, .041), item_frameColor=BUTTON,
                                     item_pad=(.040, .016), item_relief=DGG.FLAT,
                                     frameSize=(0, .542, -.044, .044), frameColor=BUTTON, relief=DGG.FLAT,
                                     popupMarkerBorder=(0, 0), popupMarker_pos=(.560, 0, 0),
                                     popupMarker_scale=.060, popupMarker_relief=DGG.FLAT, popupMarker_frameColor=GRID,
                                     popupMarker_frameSize=(-.5, .5, -.62, .62), popupMarker_text="v",
                                     popupMarker_text_scale=.54, popupMarker_text_fg=INK,
                                     command=self._choose_exercise)
        self.mount = label(left, "", .077, -.409, .035, INK, dynamic=True)
        label(left, "Source", .076, -.517, .038, TEAL)
        self.sources = {
            "demo": button(left, "Demo", .077, -.586, .158, self._load_demo),
            "live": button(left, "Live", .250, -.586, .141, lambda: self._connect(*self.address)),
            "file": button(left, "Open CSV", .405, -.586, .275, self._browse),
        }
        self.status_line = multiline(left, .077, -.678, .032, INK)
        self.record_btn = button(left, "Record CSV", .077, -.800, .298, self._toggle_recording)
        self.cloud_btn = button(left, "LLM notes: off", .389, -.800, .291, self._toggle_cloud)
        label(left, "Calibration", .076, -.912, .038, TEAL)
        self.cal_text = multiline(left, .077, -.979, .031, INK)
        self.start_btn = button(left, "Set start", .077, -1.106, .191, lambda: self._capture_endpoint("start"))
        self.end_btn = button(left, "Set end", .284, -1.106, .182, lambda: self._capture_endpoint("end"))
        button(left, "Edit angles", .481, -1.106, .199, self._edit_angles)
        self.angles = label(left, "", .077, -1.229, .034, INK, dynamic=True)
        self.tracking_text = label(left, "", .077, -1.286, .029, MUTED, dynamic=True)
        self.feedback = multiline(left, .077, -1.348, .033, AMBER, .60)

        label(right, "REP CONSISTENCY", -.701, -.122, .047, INK)
        self.score = label(right, "—", -.704, -.239, .080, TEAL, dynamic=True)
        self.rep_count = label(right, "0 reps", -.080, -.238, .039, INK, TextNode.A_right, True)
        self.area_line = label(right, "", -.699, -.305, .035, MUTED, dynamic=True)
        self.pair_text = label(right, "", -.699, -.377, .035, INK, dynamic=True)
        label(right, "Earlier", -.698, -.452, .031, BLUE)
        label(right, "Later", -.475, -.452, .031, TEAL)
        self.plot = right.attach_new_node("Comparison plots")
        self.pitch_label = label(right, "PITCH", -.699, -.520, .027, MUTED, dynamic=True)
        self.roll_label = label(right, "ROLL", -.699, -.836, .027, MUTED, dynamic=True)
        label(right, "0", -.700, -1.160, .027)
        label(right, "Normalized rep time", -.395, -1.160, .025, MUTED, TextNode.A_center)
        label(right, "100%", -.078, -1.160, .027, MUTED, TextNode.A_right)
        self.prev_btn = button(right, "< Prev pair", -.700, -1.240, .296, lambda: self._move_pair(-1))
        self.next_btn = button(right, "Next pair >", -.375, -1.240, .296, lambda: self._move_pair(1))
        self.rep_times = multiline(right, -.700, -1.340, .027, MUTED, .61)
        self.note = multiline(right, -.700, -1.462, .030, INK, .61)

        footer = self.a2dBottomCenter
        self.play_btn = button(footer, "Pause", -.848, .118, .198, self._toggle_play)
        self.timeline = DirectSlider(parent=footer, pos=(0, 0, .118), range=(0, 1), value=0,
                                     frameSize=(-.60, .60, -.012, .012), frameColor=GRID,
                                     thumb_frameColor=TEAL, thumb_frameSize=(-.023, .023, -.025, .025),
                                     pageSize=.04, command=self._seek)
        self.time_text = label(footer, "", .848, .107, .034, INK, TextNode.A_right, True)
        label(footer, "Drag to orbit   /   Scroll to zoom   /   Home to reset", 0, .039, .029,
              MUTED, TextNode.A_center)
        label(self.a2dTopCenter, "ESTIMATED POSE", 0, -.075, .030, MUTED, TextNode.A_center)

    def _show_message(self, text):
        self.feedback.setText(text)

    def _stop_source(self):
        self._stop_recording()
        if self.watch is not None:
            self.watch.close()
            self.watch = None
        self.cloud = False
        self.review_job = None
        self.file_recording = None
        self.tracking_text.setText("")
        self.cloud_btn["text"] = "LLM notes: off"
        self.generation += 1
        self.load_job = None
        self.note.setText("")
        self.history.clear()

    def _set_session(self, mode, key, samples, calibration, source, watch_reps=None):
        self.mode, self.key, self.samples = mode, key, samples
        self.watch_reps = watch_reps
        self.calibration_source = source
        self.calibration = calibration
        self.captured.clear()
        self.current = None
        self.cursor, self.phase = 0, 0
        self.playing = mode == "demo"
        self.menu.set(EXERCISE_KEYS.index(key), fCommand=0)
        self.anatomy.exercise(key)
        self._reset_view()
        self._rebuild_reps()
        self._update_controls()

    def _load_demo(self):
        self._stop_source()
        samples, calibration = synthetic_session(self.key)
        self._set_session("demo", self.key, samples, calibration, "Demo endpoints")
        self.status_line.setText("Demo / synthetic 6-axis IMU")
        self._show_message("")

    def _connect(self, host, port):
        self._stop_source()
        self.watch = OrientationReceiver(host, port)
        self._set_session("live", self.key, [], default_range(self.key), "Example endpoints")
        self.status_line.setText("Connecting to watch")
        self._show_message("Hold each endpoint, then capture it before recording.")

    def _browse(self):
        try:
            selected = dialog("open", title="Open watch recording", filetypes=[("Motion CSV", "*.csv")])
        except Exception as error:
            self._show_message(f"Could not open file picker: {error}")
            return
        if selected:
            self._request_import(Path(selected))

    def _request_import(self, path):
        self._show_message(f"Reading {path.name}...")
        self.load_job = path, background(load_recording, path)

    def _check_import(self):
        if self.load_job is None or not self.load_job[1].done():
            return
        path, future = self.load_job
        self.load_job = None
        try:
            recording = future.result()
        except (OSError, ValueError) as error:
            self._show_message(str(error))
            return
        key = recording.exercise or self.key
        self._stop_source()
        fallback = self._example_calibration(key, recording.samples[0].gravity is not None)
        calibration = recording.calibration or recording.estimated_calibration or fallback
        source = ("From CSV" if recording.calibration else
                  "Estimated from watch events" if recording.estimated_calibration else "Example endpoints")
        self._set_session("file", key, recording.samples, calibration, source, recording.watch_reps)
        self.file_recording = recording
        tracking = recording.tracking
        if tracking and tracking.axis == "both":
            self.tracking_text.setText("Watch tracking: both axes")
        elif tracking and tracking.main_deg is not None and tracking.off_deg is not None:
            self.tracking_text.setText(f"{tracking.axis.title()} ±{tracking.main_deg:g}° / off axis ±{tracking.off_deg:g}°")
        elif tracking:
            self.tracking_text.setText(f"Watch tracking: {tracking.axis}")
        self.file_exercise_confirmed = bool(recording.exercise or recording.exercise_name)
        status = [f"CSV playback / {path.name[:32]}"]
        if recording.exercise_name:
            status.append(recording.exercise_name[:31])
        if recording.velocity_loss_pct is not None:
            if recording.cutoff_basis == 'rep_duration_increase':
                status.append(f"Duration cutoff +{recording.velocity_loss_pct:g}%")
            else:
                status.append(f"Velocity loss threshold {recording.velocity_loss_pct:g}%")
        self.status_line.setText("\n".join(status))
        if recording.exercise_name and not recording.exercise:
            self._show_message("Choose the closest 3D model for this exercise.")
        elif calibration is fallback:
            self._show_message("Choose the exercise, then capture both endpoints.")
        elif recording.interrupted_rep_ms is not None:
            self._show_message(f"Interrupted attempt {recording.interrupted_rep_ms / 1000:.2f}s")
        else:
            self._show_message("Select the exercise for this set." if not self.file_exercise_confirmed else "")
        self._queue_review()

    def _choose_exercise(self, name):
        key = EXERCISE_KEYS[EXERCISE_NAMES.index(name)]
        if key == self.key:
            if self.mode == "file" and not self.file_exercise_confirmed:
                self.file_exercise_confirmed = True
                self._queue_review()
            return
        self.key = key
        if self.mode == "demo":
            self._load_demo()
        elif self.mode == "live":
            self._connect(*self.address)
        elif self.watch_reps is not None and self.calibration_source != "Example endpoints":
            self._set_session("file", key, self.samples, self.calibration, self.calibration_source, self.watch_reps)
            self._show_message("")
        else:
            fallback = self._example_calibration(key, self.samples[0].gravity is not None)
            self._set_session("file", key, self.samples, fallback, "Example endpoints", self.watch_reps)
            self._show_message("Exercise changed. Check the endpoint angles.")
        if (self.mode == "file" and self.file_recording is not None
                and not (self.file_recording.exercise or self.file_recording.exercise_name)):
            self.file_exercise_confirmed = True
            self._queue_review()

    @staticmethod
    def _example_calibration(key, with_tilt):
        example = default_range(key)
        return (TiltRange(from_angles(*example.start), from_angles(*example.end))
                if with_tilt else example)

    def _update_controls(self):
        live = self.mode == "live"
        self.record_btn["state"] = DGG.NORMAL if live else DGG.DISABLED
        self.cloud_btn["state"] = DGG.NORMAL if self.mode in ("live", "file") else DGG.DISABLED
        self.cloud_btn["text"] = (f"AI form: {'on' if self.review_enabled else 'off'}" if self.mode == "file"
                                  else f"LLM notes: {'on' if self.cloud else 'off'}")
        for control in (self.play_btn, self.timeline):
            control.hide() if live else control.show()
        for mode, control in self.sources.items():
            control["frameColor"] = (.15, .32, .34, 1) if self.mode == mode else BUTTON
        exercise = EXERCISES[self.key]
        self.mount.setText(exercise.mount)
        self.cal_text.setText(f"{exercise.start_name} to {exercise.end_name}\n{self.calibration_source}")
        self._render_analysis()

    def _capture_endpoint(self, endpoint):
        try:
            with_tilt = isinstance(self.calibration, TiltRange)
            if self.mode == "live":
                if not self.history or time.monotonic() - self.last_received > .35:
                    raise ValueError("Wait for fresh readings from the watch.")
                latest = self.history[-1].t
                recent = [sample for sample in self.history if latest - sample.t <= .7]
                if len(recent) < 5 or recent[-1].t - recent[0].t < .35:
                    raise ValueError("Hold this pose for one second, then capture it.")
                if with_tilt:
                    tilts = [sample.tilt() for sample in recent]
                    value = unit(tuple(sum(component) for component in zip(*tilts)))
                    if any(angle(tilt, value) > 3 for tilt in tilts):
                        raise ValueError("The watch is moving. Hold still and try again.")
                else:
                    values = []
                    for axis in ("pitch", "roll"):
                        reference = getattr(recent[-1], axis)
                        offsets = [angle_delta(getattr(sample, axis), reference) for sample in recent]
                        if max(offsets) - min(offsets) > 5:
                            raise ValueError("The watch is moving. Hold still and try again.")
                        values.append(angle_delta(reference + median(offsets), 0))
                    value = tuple(values)
            else:
                sample = sample_at(self.samples, self.cursor)
                value = sample.tilt() if with_tilt else (sample.pitch, sample.roll)
                self.playing = False
            self.captured[endpoint] = value
            if len(self.captured) == 2:
                calibration = TiltRange if with_tilt else Range
                self._apply_calibration(calibration(**self.captured), "Captured endpoints")
            else:
                self._show_message(f"{endpoint.title()} captured. Set the other endpoint.")
        except ValueError as error:
            self._show_message(str(error))

    def _edit_angles(self):
        try:
            current = ", ".join(f"{angle:.1f}" for angle in (*self.calibration.start, *self.calibration.end))
            text = dialog("angles", title="Pitch and roll endpoints",
                          prompt="Enter start pitch, start roll, end pitch, end roll in degrees.",
                          initialvalue=current)
            if text:
                values = text.split(",")
                if len(values) != 4:
                    raise ValueError("Enter four comma-separated angles.")
                start, end = pitch_roll(values[:2]), pitch_roll(values[2:])
                calibration = (TiltRange(from_angles(*start), from_angles(*end))
                               if isinstance(self.calibration, TiltRange) else Range(start, end))
                self._apply_calibration(calibration, "Manual angles")
        except Exception as error:
            self._show_message(str(error))

    def _apply_calibration(self, calibration, source):
        was_recording = self.csv is not None
        self._stop_recording()
        self.captured.clear()
        self.calibration = calibration
        self.calibration_source = source
        self.generation += 1
        self.last_sent = 0
        if self.cloud:
            self.note.setText("Waiting for an updated comparison")
        self._rebuild_reps()
        self._update_controls()
        self._show_message("Calibration applied. Start a new recording." if was_recording else "Calibration applied.")
        if self.mode == "file":
            self._queue_review()

    def _rebuild_reps(self):
        self.detector = RepDetector(self.calibration)
        if self.watch_reps is not None:
            self.reps = list(self.watch_reps)
        elif self.mode == "live":
            self.reps = [rep for sample in self.samples if (rep := self.detector.push(sample)) is not None]
        else:
            self.reps = find_reps(self.samples, self.calibration)
        self.pair_index = max(0, len(self.reps) - 2)

    def _toggle_recording(self):
        if self.csv is not None:
            if self._stop_recording():
                self._show_message("Recording saved.")
            return
        try:
            stamp = time.strftime("%Y-%m-%d-%H%M%S")
            selected = dialog("save", title="Save live recording", defaultextension=".csv",
                              initialfile=f"{self.key}-{stamp}.csv", filetypes=[("Motion CSV", "*.csv")])
            if selected:
                self.csv = CsvLog(Path(selected), self.key, self.calibration,
                                  with_tilt=self.current is not None and self.current.gravity is not None)
                self.record_btn["text"] = "Stop recording"
                self._show_message(f"Writing {Path(selected).name}")
        except FileExistsError:
            self._show_message("That file exists. Choose a new recording name.")
        except Exception as error:
            self._show_message(str(error))

    def _stop_recording(self):
        if self.csv is not None:
            try:
                self.csv.close()
            except OSError as error:
                self._show_message(f"Could not finish CSV: {error}")
                return False
            finally:
                self.csv = None
                self.record_btn["text"] = "Record CSV"
        return True

    def _toggle_cloud(self):
        if self.mode == "file":
            self._toggle_review()
            return
        if self.cloud:
            self.cloud = False
            self.generation += 1
            self.cloud_btn["text"] = "LLM notes: off"
            self.note.setText("")
            return
        if not os.getenv("OPENAI_API_KEY", "").strip():
            self._show_message("Set OPENAI_API_KEY before launching the app.")
            return
        try:
            allowed = dialog("confirm", title="Enable OpenAI notes?",
                             message="Send the exercise name and completed rep statistics to OpenAI "
                                     "during this live session? Raw readings stay on this PC. API charges may apply.")
        except Exception as error:
            self._show_message(str(error))
            return
        if allowed:
            self.cloud = True
            self.last_sent = 0
            self.next_note = 0
            self.cloud_btn["text"] = "LLM notes: on"
            self.note.setText("Waiting for completed reps")

    def _toggle_review(self):
        if self.review_enabled:
            self.review_enabled = False
            self.review_job = None
            self.note.setText("")
            self.cloud_btn["text"] = "AI form: off"
            return
        if not os.getenv("OPENAI_API_KEY", "").strip():
            self._show_message("Set OPENAI_API_KEY before launching the app.")
            return
        try:
            allowed = dialog("confirm", title="Enable AI form review?",
                             message="Send exercise metadata and measured rep summaries to OpenAI when you open "
                                     "a CSV in this app session? The CSV stays on this PC. API charges may apply.")
        except Exception as error:
            self._show_message(str(error))
            return
        if allowed:
            self.review_enabled = True
            self.cloud_btn["text"] = "AI form: on"
            self._queue_review()

    def _queue_review(self):
        if not self.review_enabled or self.file_recording is None:
            return
        self.review_job = None
        if not self.file_exercise_confirmed:
            try:
                self.file_exercise_confirmed = dialog("confirm", title="Confirm the recorded exercise",
                    message=f"This log has no exercise name. Review it as {EXERCISES[self.key].name}?")
            except Exception as error:
                self.note.setText(str(error))
                return
            if not self.file_exercise_confirmed:
                self.note.setText("Select the recorded exercise to review this set.")
                return
        if not self.reps:
            self.note.setText("No completed reps to review. Check the pose endpoints.")
            return
        recording, key, reps = self.file_recording, self.key, self.reps
        calibration, source = self.calibration, self.calibration_source

        def review():
            return request_form_review(summarize_form(recording, key, reps, calibration, source))

        self.note.setText("Reviewing recorded reps...")
        self.review_job = background(review)

    def _check_review(self):
        if self.review_job is not None and self.review_job.done():
            future, self.review_job = self.review_job, None
            try:
                self.note.setText(future.result())
            except Exception as error:
                self.note.setText(str(error))

    def _check_cloud(self, now):
        if self.note_job is not None:
            generation, future = self.note_job
            if not future.done():
                return
            self.note_job = None
            if self.cloud and generation == self.generation:
                try:
                    self.note.setText(f"LLM note / {future.result()}")
                except Exception as error:
                    self.note.setText(str(error))
        if not self.cloud or now < self.next_note or len(self.reps) <= self.last_sent:
            return
        pair = self.reps[-2:]
        comparison = compare_reps(*pair, self.calibration) if len(pair) == 2 else None
        self.next_note = now + 8
        self.last_sent = len(self.reps)
        summary = summarize_motion(self.key, self.reps[-4:], comparison)
        self.note_job = self.generation, background(request_note, summary)

    def _toggle_play(self):
        if self.mode == "live":
            return
        if self.cursor >= self.samples[-1].t:
            self.cursor = 0
        self.playing = not self.playing

    def _seek(self):
        if self.mode == "live" or not self.samples:
            return
        end = self.samples[-1].t
        value = self.timeline["value"]
        if abs(value - self.timeline_sync) > .0001:
            self.cursor = value * end
            self.playing = False

    def _move_pair(self, step):
        self.pair_index = min(max(0, len(self.reps) - 2), max(0, self.pair_index + step))
        self._render_analysis()

    def _render_analysis(self):
        self.plot.node().remove_all_children()
        source = "watch " if self.watch_reps is not None else ""
        self.rep_count.setText(f"{len(self.reps)} {source}rep{'s' if len(self.reps) != 1 else ''}")
        index = self.pair_index
        self.prev_btn["state"] = DGG.NORMAL if index > 0 else DGG.DISABLED
        self.next_btn["state"] = DGG.NORMAL if index + 2 < len(self.reps) else DGG.DISABLED
        if len(self.reps) < 2:
            self.score.setText("—")
            self.area_line.setText("Needs two completed reps")
            self.pair_text.setText("")
            self.rep_times.setText("")
            self.pitch_label.setText(self._axis_name(0))
            self.roll_label.setText(self._axis_name(1))
            self._draw_plot(None, 0, -.551)
            self._draw_plot(None, 1, -.868)
            return
        first, second = self.reps[index:index + 2]
        comparison = compare_reps(first, second, self.calibration)
        self.score.setText(f"{comparison.score:.0f}%")
        self.area_line.setText(f"{comparison.area:.1f}° mean angle difference")
        self.pair_text.setText(f"Rep {index + 1} vs {index + 2}")
        self.rep_times.setText(
            f"Rep {index + 1}   {first.start:.2f}–{first.end:.2f}s  /  {first.duration:.2f}s\n"
            f"Rep {index + 2}   {second.start:.2f}–{second.end:.2f}s  /  {second.duration:.2f}s")
        self._draw_plot(comparison, 0, -.551)
        self._draw_plot(comparison, 1, -.868)

    def _axis_name(self, axis):
        names = ("ALONG ARC", "OFF-ARC TILT") if isinstance(self.calibration, TiltRange) else ("PITCH", "ROLL")
        return names[axis]

    def _draw_plot(self, comparison, axis, top):
        x0, width, height = -.700, .621, .229
        lines = LineSegs("Normalized traces")
        lines.set_thickness(1)
        lines.set_color(*GRID)
        for fraction in (0, .25, .5, .75, 1):
            x = x0 + width * fraction
            lines.move_to(x, 0, top)
            lines.draw_to(x, 0, top - height)
        for fraction in (0, .5, 1):
            z = top - height * fraction
            lines.move_to(x0, 0, z)
            lines.draw_to(x0 + width, 0, z)
        self.plot.attach_new_node(lines.create())
        if comparison is None:
            return
        traces = comparison.earlier, comparison.later
        values = [row[axis] for trace in traces for row in trace]
        bottom = math.floor(min(0, *values) / 10) * 10
        ceiling = max(bottom + 10, math.ceil(max(values) / 10) * 10)
        if axis == 1 and isinstance(self.calibration, TiltRange):
            limit = max(10, -bottom, ceiling)
            bottom, ceiling = -limit, limit
        name = self._axis_name(axis)
        (self.pitch_label if axis == 0 else self.roll_label).setText(f"{name}  /  {bottom}° to {ceiling}°")
        for trace, color in zip(traces, (BLUE, TEAL)):
            curve = LineSegs(name)
            curve.set_color(*color)
            curve.set_thickness(2)
            for index, row in enumerate(trace):
                x = x0 + width * index / (len(trace) - 1)
                z = top - height * (ceiling - row[axis]) / (ceiling - bottom)
                if index == 0:
                    curve.move_to(x, 0, z)
                else:
                    curve.draw_to(x, 0, z)
            self.plot.attach_new_node(curve.create())

    def _read_live(self, now):
        status, samples = self.watch.drain()
        if not samples and self.history and now - self.last_received > .60 and status.startswith("Live"):
            status = "Waiting for IMU data"
        self.status_line.setText(status)
        if not samples:
            return
        if (samples[0].gravity is not None and self.calibration_source == "Example endpoints"
                and not isinstance(self.calibration, TiltRange)):
            self.captured.clear()
            self.calibration = self._example_calibration(self.key, True)
            self._rebuild_reps()
            self._update_controls()
        self.last_received = now
        self.history.extend(samples)
        previous_count = len(self.reps)
        self.samples.extend(samples)
        for sample in samples:
            if rep := self.detector.push(sample):
                self.reps.append(rep)
        if self.csv is not None:
            try:
                self.csv.add(samples)
            except OSError as error:
                self._stop_recording()
                self._show_message(f"Recording stopped: {error}")
        if len(self.reps) != previous_count:
            if self.pair_index == max(0, previous_count - 2):
                self.pair_index = max(0, len(self.reps) - 2)
            self._render_analysis()
        self.current = samples[-1]

    def _update(self, task):
        now = time.monotonic()
        dt = min(now - self.previous_frame, .12)
        self.previous_frame = now
        self._orbit()
        self._check_import()
        self._check_review()
        self._check_cloud(now)
        if self.mode == "live":
            self._read_live(now)
            self.time_text.setText("")
        else:
            end = self.samples[-1].t
            if self.playing:
                self.cursor += dt
                if self.cursor >= end:
                    self.cursor = 0 if self.mode == "demo" else end
                    self.playing = self.mode == "demo"
            self.current = sample_at(self.samples, self.cursor)
            self.timeline_sync = self.cursor / end
            self.timeline["value"] = self.timeline_sync
            self.play_btn["text"] = "Pause" if self.playing else "Play"
            self.time_text.setText(f"{self.cursor:.1f} / {end:.1f}s")
        if self.current is not None:
            self.angles.setText(f"Pitch {self.current.pitch:+.1f}°    Roll {self.current.roll:+.1f}°")
            target = max(0, min(1, self.calibration.progress(self.current)))
            if self.mode == "live":
                self.phase += (target - self.phase) * -math.expm1(-dt / .065)
            else:
                self.phase = target
        else:
            self.angles.setText("")
        self.anatomy.update_exercise(loaded_pose(self.key, self.phase), self.phase, self.key)
        return task.cont

    def destroy(self):
        if hasattr(self, "watch"):
            self._stop_source()
        super().destroy()
