import math
from pathlib import Path
import socket
import tempfile
import time
import unittest

from pose import FOREARM, UPPER_ARM, pullup_pose
from telemetry import Calibration, RollDecoder, RollReceiver, Snapshot, demo_roll


class ProtocolTests(unittest.TestCase):
    def test_fragmentation_coalescing_and_recovery(self):
        decoder = RollDecoder()
        self.assertEqual(list(decoder.feed(b"RO")), [])
        self.assertEqual(list(decoder.feed(b"LL,-60.00\nROLL,+60.00\r\nROLL,18")), [-60, 60])
        self.assertEqual(list(decoder.feed(b"0\nROLL,180.01\nROLL,nan\n\xff\n")), [180])
        self.assertEqual(list(decoder.feed(b"x" * 200 + b"ROLL,10\nROLL,-180\n")), [-180])

    def test_tcp_receiver_reconnects_to_same_server(self):
        with socket.socket() as server:
            server.bind(("127.0.0.1", 0))
            server.listen()
            server.settimeout(4)
            receiver = RollReceiver(*server.getsockname())
            self.addCleanup(receiver.close)

            def wait_for_roll(expected):
                deadline = time.monotonic() + 2
                while time.monotonic() < deadline:
                    if receiver.snapshot().latest(time.monotonic()) == expected:
                        return
                    time.sleep(.01)
                self.fail(f"Receiver did not report {expected}")

            first, _ = server.accept()
            with first:
                first.sendall(b"ROLL,42\nROLL,9")  # leave a partial line behind
                wait_for_roll(42)
            wait_for_roll(None)
            second, _ = server.accept()
            with second:
                second.sendall(b"ROLL,-35.25\n")
                wait_for_roll(-35.25)


class CalibrationTests(unittest.TestCase):
    def test_sign_wrap_and_overshoot(self):
        for bottom, top, middle, before, after in (
            (-60, 60, 0, -80, 80),
            (60, -60, 0, 80, -80),
            (140, -95, -157.5, 130, -85),
            (-95, 140, -157.5, -85, 130),
        ):
            with self.subTest(bottom=bottom, top=top):
                calibration = Calibration(bottom, top)
                self.assertEqual(calibration.phase(bottom), 0)
                self.assertEqual(calibration.phase(top), 1)
                self.assertAlmostEqual(calibration.phase(middle), .5)
                self.assertEqual(calibration.phase(before), 0)
                self.assertEqual(calibration.phase(after), 1)

    def test_rejects_invalid_endpoints(self):
        for endpoints in ((20, 26), (0, 180), (200, 30), (float("nan"), 60)):
            with self.subTest(endpoints=endpoints), self.assertRaises(ValueError):
                Calibration(*endpoints)

    def test_stable_capture_across_wrap(self):
        samples = tuple((10 + i * .1, value) for i, value in enumerate(
            (179, -179, 179, -180, 180, -180, 179)))
        stable = Snapshot("Connected", samples)
        self.assertAlmostEqual(abs(stable.stable_roll(10.65)), 180)
        self.assertIsNone(stable.latest(11.3))
        with self.assertRaises(ValueError):
            stable.stable_roll(11.3)
        moving = Snapshot("Connected", (*samples[:-1], (10.6, 150)))
        with self.assertRaises(ValueError):
            moving.stable_roll(10.65)

    def test_save_reload_and_replace(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "settings.json"
            Calibration().save(path)
            replacement = Calibration(130, -100)
            replacement.save(path)
            self.assertEqual(Calibration.load(path), replacement)


class PoseTests(unittest.TestCase):
    def test_fixed_wrist_and_limb_lengths_throughout_range(self):
        bottom = pullup_pose(0)
        previous_height = bottom.shoulder[2]
        for index in range(101):
            pose = pullup_pose(index / 100)
            self.assertEqual(pose.wrist, bottom.wrist)
            self.assertAlmostEqual(math.dist(pose.shoulder, pose.elbow), UPPER_ARM)
            self.assertAlmostEqual(math.dist(pose.elbow, pose.wrist), FOREARM)
            self.assertAlmostEqual(math.dist(pose.hip, pose.knee), .405)
            self.assertAlmostEqual(math.dist(pose.knee, pose.ankle), .425)
            self.assertGreaterEqual(pose.shoulder[2], previous_height)
            previous_height = pose.shoulder[2]
        self.assertEqual(pullup_pose(-1), bottom)
        self.assertEqual(pullup_pose(2), pullup_pose(1))

    def test_demo_endpoints(self):
        self.assertEqual([demo_roll(t) for t in (0, 0.25, 3, 6)], [-60, -60, 60, -60])


if __name__ == "__main__":
    unittest.main()
