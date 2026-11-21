"""Start Lift Studio or the original roll-only pull-up viewer."""

import argparse
from pathlib import Path
import time

from telemetry import Calibration, DEFAULT_HOST, DEFAULT_PORT, RollReceiver
from motion import EXERCISES


def options():
    parser = argparse.ArgumentParser(description="Animate exercises and compare ESP32 IMU recordings.")
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--demo", action="store_true", help="animate locally without Wi-Fi (default)")
    source.add_argument("--live", action="store_true", help="connect to the watch at startup")
    source.add_argument("--file", type=Path, help="open an SD or desktop CSV recording at startup")
    parser.add_argument("--exercise", choices=EXERCISES, default="squat", help="initial exercise")
    parser.add_argument("--legacy-pullup", action="store_true", help="use the original roll-only viewer")
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"ESP32 address (default {DEFAULT_HOST})")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"TCP port (default {DEFAULT_PORT})")
    parser.add_argument("--calibration", type=Path, default=Path(__file__).with_name("calibration.json"),
                        help="endpoint calibration file (default calibration.json beside the script)")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    if args.file and args.legacy_pullup:
        parser.error("--file requires Lift Studio.")
    return args


def main():
    args = options()
    try:
        if not args.legacy_pullup:
            from lift_viewer import LiftViewer

            app = LiftViewer(args)
            try:
                app.run()
            finally:
                app.destroy()
            return
        from viewer import PullupViewer
    except ModuleNotFoundError as error:
        if error.name not in {"direct", "panda3d"}:
            raise
        raise SystemExit("Install dependencies with: python -m pip install -r requirements.txt") from error

    legacy_main(args, PullupViewer)


def legacy_main(args, viewer_class):
    calibration = Calibration()
    calibrated = False
    if not args.demo:
        try:
            calibration = Calibration.load(args.calibration)
            calibrated = True
        except FileNotFoundError:
            pass
        except (OSError, ValueError) as error:
            print(f"Unable to load {args.calibration}: {error}")

    receiver = None if args.demo else RollReceiver(args.host, args.port)
    try:
        app = viewer_class(args, calibration, calibrated, receiver, time.monotonic())
        try:
            app.run()
        finally:
            app.destroy()
    finally:
        if receiver is not None:
            receiver.close()


if __name__ == "__main__":
    main()
