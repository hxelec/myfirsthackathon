from pathlib import Path
import socket
from tempfile import TemporaryDirectory
import time
import unittest
from unittest.mock import patch

from imu import from_angles
from llm_notes import measure_pauses, request_form_review, summarize_form, summarize_motion
from motion import EXERCISES, Range, Rep, Sample, compare_reps, default_range, demo_samples, find_reps, sample_at
from recordings import CsvLog, OrientationReceiver, load_recording
from synthetic import raw_session, synthetic_session


class MotionTests(unittest.TestCase):
    def test_endpoint_pauses_measure_stillness_and_ignore_gaps(self):
        samples = [Sample(i / 50, min(60, max(0, (i - 20) * 3)), 0) for i in range(81)]
        endpoints = {'bottom': from_angles(0, 0), 'top': from_angles(60, 0)}
        pauses = measure_pauses(samples, 0, 1.6, endpoints, 8)
        self.assertAlmostEqual(pauses['bottom_pause_s'], .42, delta=.03)
        self.assertAlmostEqual(pauses['top_pause_s'], .80, delta=.03)
        self.assertEqual(measure_pauses(samples[:10] + samples[20:], 0, 1.6, endpoints, 8), {})

    def test_six_axis_demo_reaches_full_range_without_wrap(self):
        for exercise in EXERCISES:
            with self.subTest(exercise=exercise):
                samples, calibration = synthetic_session(exercise)
                progress = [calibration.progress(sample) for sample in samples[:220]]
                self.assertAlmostEqual(max(progress), 1, delta=.025)
                self.assertLess(max(abs(a - b) for a, b in zip(progress, progress[1:])), .03)
                self.assertEqual(len(find_reps(samples, calibration)), 3)

    def test_each_demo_has_three_complete_reps(self):
        for exercise in EXERCISES:
            with self.subTest(exercise=exercise):
                calibration = default_range(exercise)
                reps = find_reps(demo_samples(exercise), calibration)
                self.assertEqual(len(reps), 3)
                self.assertLess(reps[0].end, reps[1].start)
                comparison = compare_reps(*reps[:2], calibration)
                self.assertGreater(comparison.area, 0)
                self.assertLess(comparison.score, 100)
                self.assertGreater(comparison.score, 85)

    def test_time_normalization_and_known_area(self):
        calibration = Range((0, 0), (60, 20))
        first = Rep((Sample(0, 0, 0), Sample(1, 60, 20), Sample(2, 0, 0)), 1)
        slow = Rep((Sample(3, 0, 0), Sample(5, 60, 20), Sample(7, 0, 0)), 1)
        shifted = Rep((Sample(3, 3, 4), Sample(5, 63, 24), Sample(7, 3, 4)), 1)
        self.assertAlmostEqual(compare_reps(first, slow, calibration).score, 100)
        comparison = compare_reps(first, shifted, calibration)
        self.assertAlmostEqual(comparison.area, 5)
        summary = summarize_motion("squat", [first, shifted], comparison)
        self.assertEqual(summary["mean_angle_difference_deg"], 5)
        self.assertNotIn("samples", summary["reps"][0])

    def test_wrap_interpolation_and_projection(self):
        calibration = Range((170, -145), (-140, 175))
        start = Sample(0, 170, -145)
        end = Sample(2, -140, 175)
        self.assertAlmostEqual(calibration.progress(start), 0)
        self.assertAlmostEqual(calibration.progress(end), 1)
        self.assertAlmostEqual(calibration.progress(sample_at([start, end], .075)), 0)
        end = Sample(.2, end.pitch, end.roll)
        self.assertAlmostEqual(calibration.progress(sample_at([start, end], .1)), .5)

    def test_gap_and_partial_reps_are_excluded(self):
        samples = demo_samples("squat")
        interrupted = [sample for sample in samples if not 1.7 <= sample.t <= 2.5]
        self.assertEqual(len(find_reps(interrupted, default_range("squat"))), 2)
        self.assertEqual(find_reps(samples[:50], default_range("squat")), [])


class WatchTests(unittest.TestCase):
    def test_form_review_uses_exercise_metadata_and_bounded_rep_summary(self):
        with TemporaryDirectory() as directory:
            path = Path(directory) / 'watch.csv'
            template = (Path(__file__).resolve().parents[1] / 'demo' / 'hammer_curl.csv').read_text(encoding='utf-8')
            header = '# exercise_name=Hammer Curl\n# velocity_loss_threshold_pct=37.5%'
            path.write_text(template.replace('# exercise=hammer_curl', header, 1), encoding='utf-8')
            recording = load_recording(path)
            self.assertEqual(recording.exercise, 'hammer_curl')
            reps = find_reps(recording.samples, recording.calibration)
            summary = summarize_form(recording, 'squat', reps, recording.calibration, 'From CSV')
            self.assertEqual(summary['expected_sensor_mount'], 'Wrist (forearm)')
            self.assertEqual(summary['configured_velocity_loss_threshold_pct'], 37.5)
            self.assertEqual(len(summary['sampled_reps']), 3)
            self.assertTrue(summary['score_allowed'])
            with patch('llm_notes._complete', return_value='{"score":8,"feedback":"Tilt stayed consistent."}') as complete:
                self.assertIn('Segment consistency 8/10', request_form_review(summary))
                complete.assert_called_once()
                summary['score_allowed'] = False
                self.assertEqual(request_form_review(summary), 'Tilt stayed consistent.')

    def test_desktop_tilt_log_preserves_calibration_and_reps(self):
        samples, calibration = synthetic_session('hammer_curl')
        with TemporaryDirectory() as directory:
            path = Path(directory) / 'capture.csv'
            log = CsvLog(path, 'hammer_curl', calibration)
            try:
                log.add(samples)
            finally:
                log.close()
            recording = load_recording(path)
            self.assertEqual(recording.exercise, 'hammer_curl')
            self.assertAlmostEqual(recording.calibration.span, calibration.span, places=5)
            self.assertEqual(len(find_reps(recording.samples, recording.calibration)), 3)

    def test_repwatch_sd_header_and_footer(self):
        with TemporaryDirectory() as directory:
            path = Path(directory) / 'set000001.csv'
            path.write_text('ms,roll_deg,pitch_deg,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps,phase,rep_number,rep_ms\n'
                            '100,0,0,0,0,1,0,0,0,armed,0,0\n'
                            '120,90,0,0,1,0,0,0,0,concentric,1,20\n'
                            '# end=Stopped by user,reps=1\n', encoding='utf-8')
            recording = load_recording(path)
            self.assertEqual([sample.t for sample in recording.samples], [0, .02])
            self.assertEqual(recording.samples[1].roll, 90)
            self.assertEqual(recording.samples[1].gravity[1], 1)
            self.assertIsNone(recording.exercise_name)
            self.assertIsNone(recording.velocity_loss_pct)
            self.assertIsNone(recording.tracking)

            old_csv = path.read_text(encoding='utf-8')
            path.write_text('# exercise_name=Single-Arm Row\n# velocity_loss_pct=40\n'
                            '# tracking_axis=pitch\n# main_axis_tolerance_deg=15\n# off_axis_tolerance_deg=10\n'
                            '# bottom_tolerance_deg=8.250\n# concentric_tolerance_deg=7.500\n'
                            '# finish_tolerance_deg=15.000\n' + old_csv,
                            encoding='utf-8')
            recording = load_recording(path)
            self.assertEqual(recording.exercise_name, 'Single-Arm Row')
            self.assertEqual(recording.velocity_loss_pct, 40)
            self.assertEqual(recording.tracking.axis, 'pitch')
            self.assertEqual((recording.tracking.main_deg, recording.tracking.off_deg), (15, 10))
            self.assertEqual((recording.tracking.bottom_deg, recording.tracking.concentric_deg,
                              recording.tracking.finish_deg), (8.25, 7.5, 15))
            self.assertEqual(len(recording.samples), 2)

    def test_watch_timing_keeps_short_reps_and_handles_unfinished_attempt(self):
        with TemporaryDirectory() as directory:
            path = Path(directory) / 'set000001.csv'
            path.write_text('# exercise_name=HAMMER CURL\n# velocity_loss_pct=80\n'
                            '# cutoff_basis=rep_duration_increase\n'
                            'ms,roll_deg,pitch_deg,phase,rep_number,rep_ms\n'
                            '10,-90,10,armed,0,0\n'
                            '210,-90,40,concentric,0,0\n'
                            '520,85,76,find_bottom,1,310\n'
                            '760,-90,10,armed,1,0\n'
                            '970,-85,42,concentric,1,0\n'
                            '1200,-90,10,find_bottom,1,0\n'
                            '# end=Velocity loss cutoff reached,reps=1,interrupted_rep_ms=558\n', encoding='utf-8')
            recording = load_recording(path)
            (rep,) = recording.watch_reps
            self.assertAlmostEqual(rep.start, .20)
            self.assertAlmostEqual(rep.duration, .31)
            self.assertEqual(recording.exercise, 'hammer_curl')
            self.assertEqual(recording.velocity_loss_pct, 80)
            summary = summarize_form(recording, 'hammer_curl', recording.watch_reps,
                                     recording.estimated_calibration, 'Estimated from watch events')
            self.assertEqual(summary['cutoff_basis'], 'rep_duration_increase')
            self.assertEqual(summary['interrupted_attempt_duration_s'], .558)
            self.assertEqual(summary['completed_rep_count'], 1)
            self.assertAlmostEqual(recording.estimated_calibration.progress(recording.samples[0]), 0)
            self.assertAlmostEqual(recording.estimated_calibration.progress(recording.samples[2]), 1)

    def test_raw_wire_samples_follow_synthetic_session(self):
        rows, calibration = raw_session('pullup')
        with socket.socket() as server:
            server.bind(('127.0.0.1', 0))
            server.listen()
            server.settimeout(3)
            receiver = OrientationReceiver(*server.getsockname())
            self.addCleanup(receiver.close)
            peer, _ = server.accept()
            with peer:
                for row in rows[:220]:
                    line = 'IMU6,' + ','.join(map(str, row)) + '\n'
                    peer.sendall(line.encode('ascii'))
                received = []
                deadline = time.monotonic() + 2
                while len(received) < 220 and time.monotonic() < deadline:
                    received.extend(receiver.drain()[1])
                    time.sleep(.01)
                self.assertEqual(len(received), 220)
                self.assertAlmostEqual(max(map(calibration.progress, received)), 1, delta=.025)

    def test_live_log_can_be_reopened(self):
        with TemporaryDirectory() as directory:
            path = Path(directory) / "live.csv"
            calibration = default_range("deadlift")
            log = CsvLog(path, "deadlift", calibration)
            try:
                log.add(demo_samples("deadlift"))
            finally:
                log.close()
            recording = load_recording(path)
            self.assertEqual(recording.exercise, "deadlift")
            self.assertEqual(recording.calibration, calibration)
            self.assertEqual(len(find_reps(recording.samples, calibration)), 3)
            with self.assertRaises(FileExistsError):
                CsvLog(path, "deadlift", calibration)

    def test_csv_ignores_yaw_and_recovers_truncated_tail(self):
        with TemporaryDirectory() as directory:
            path = Path(directory) / "sd.csv"
            path.write_text("ms,pitch_deg,roll_deg,yaw_deg\n100,0,1,27\n140,2,3,30\n180,4", encoding="utf-8")
            recording = load_recording(path)
            self.assertIsNone(recording.exercise)
            self.assertEqual(recording.samples, [Sample(0, 0, 1), Sample(.04, 2, 3)])
            path.write_text("ms,pitch_deg,roll_deg\n100,0,1\n90,2,3\n")
            with self.assertRaisesRegex(ValueError, "Timestamps"):
                load_recording(path)

    def test_tcp_fragmentation_wrap_and_reconnect_gap(self):
        def collect(receiver, expected):
            received = []
            deadline = time.monotonic() + 2
            while len(received) < expected and time.monotonic() < deadline:
                received.extend(receiver.drain()[1])
                time.sleep(.01)
            self.assertEqual(len(received), expected)
            return received

        with socket.socket() as server:
            server.bind(("127.0.0.1", 0))
            server.listen()
            server.settimeout(4)
            receiver = OrientationReceiver(*server.getsockname())
            self.addCleanup(receiver.close)
            peer, _ = server.accept()
            with peer:
                peer.sendall(b"IMU,42949672")
                peer.sendall(b"80,5,8\nIMU,24,7,9,55\nIMU,24,80,90\n")
                samples = collect(receiver, 2)
                self.assertEqual([sample.pitch for sample in samples], [5, 7])
                self.assertAlmostEqual(samples[1].t - samples[0].t, .04)
            peer, _ = server.accept()
            with peer:
                peer.sendall(b"IMU,0,-4,2\n")
                self.assertGreaterEqual(collect(receiver, 1)[0].t - samples[-1].t, .35)
