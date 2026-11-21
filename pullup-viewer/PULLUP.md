# Pull-up Anatomy

A Windows 11 desktop viewer for a single-sensor pull-up demo. An ESP32-S3 hosts its own Wi-Fi network and sends roll readings over TCP. The viewer renders a stylized 3D athlete with fixed grips and an estimated position in a symmetric, overhand pull-up.

## Start on Windows 11

Install 64-bit CPython 3.12 with the Windows Python launcher. Open PowerShell in this folder, create the environment, and start the demo.

```powershell
py -3.12 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe pullup_viewer.py --legacy-pullup --demo
```

If your Python installation provides `python` instead of `py`, verify `python --version` and use `python -m venv .venv`. Download the graphics dependency while connected to the internet. The viewer works offline after installation. [Panda3D 1.10.16](https://www.panda3d.org/download/sdk-1-10-16/)

**Drag** with the left mouse button to orbit, **scroll** to zoom, **Home** to reset the camera, and **Esc** to close. The local demo repeats automatically with no ESP32. A teal marker shows the example upper-arm sensor position.

## Start the ESP32-S3 stream

1. In Arduino IDE, install **esp32 by Espressif Systems**, version **3.x**, through Boards Manager. Add `https://espressif.github.io/arduino-esp32/package_esp32_index.json` under **Preferences → Additional Boards Manager URLs** if the package is missing. Select your board and COM port. For a generic board, select **ESP32S3 Dev Module** and match the flash and PSRAM settings to the module. [Espressif installation guide](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html) · [Board settings](https://docs.espressif.com/projects/arduino-esp32/en/latest/guides/tools_menu.html)
2. Open [PullupRoll.ino](firmware/PullupRoll/PullupRoll.ino) and upload it with `kUseSimulatedRoll = true` for the first test. The sketch starts a password-protected AP and serves one TCP client at a time. The serial monitor at **115200 baud** reports the address and mode.
3. On Windows, join **PullUp-S3**, password **pullup-demo**. “No internet” is expected. Stay connected to this Wi-Fi network.
4. Run the viewer in live mode. The hardware simulator already matches the viewer’s default calibration at −60° and +60°. If you have saved a different calibration, pass an unused file path with `--calibration` for this test.

```powershell
.\.venv\Scripts\python.exe pullup_viewer.py --legacy-pullup
# Same command with the network defaults made explicit
.\.venv\Scripts\python.exe pullup_viewer.py --legacy-pullup --host 192.168.4.1 --port 8765
```

The upper-right corner shows connection status and the incoming roll. When readings stop, the viewer holds the last pose, keeps the camera responsive, and tries to reconnect. If the window stays at **Connecting** or **Reconnecting**, check the Windows Wi-Fi network, close any other client connected to the board, and verify the IP and port against the serial monitor. If needed, run `Test-NetConnection 192.168.4.1 -Port 8765` with the viewer closed, then reopen it.

## Add your IMU

The sketch includes simulated roll and two hooks for your sensor. In `setupImu()`, initialize your breakout, bus and orientation filter. In `readImuRollDegrees(float &degrees)`, assign a fresh roll in **degrees** to `degrees` and return `true`. Return `false` if a sample is unavailable. Set `kUseSimulatedRoll = false` and reflash. The sketch polls that hook at 25 Hz even while disconnected. If your sensor filter needs a faster update rate, run its update or read its FIFO separately and publish its latest result at 25 Hz.

For an IMU that exposes fused Euler angles, use its X-axis roll after any required sensor-axis remap. With acceleration samples in the remapped frame, `atan2f(ay, az) * 180.0f / PI` gives a quick tilt check while held still, assuming `az` reads positive in the zero-roll pose. Dynamic acceleration during the rep distorts that estimate. For the motion itself, use gyro/accelerometer fusion and avoid Euler orientations where the sensed gravity projects weakly into the YZ plane.

### Mount, verify, calibrate

1. Strap the IMU snugly to the **upper arm**, around mid-humerus, so the board follows the arm with minimal slip. Either arm works. Use a consistent overhand grip and a smooth repetition. For a motion close to the frontal plane, start with sensor **+X** pointing roughly forward, approximately perpendicular to the upper arm’s main swing plane. Aim **+Y** along the arm from elbow toward shoulder and set **+Z** to **+X × +Y**. Read the breakout’s axis markings and remap in firmware if needed. Positive roll follows the right-hand rule about +X in this convention. A roll axis along the humerus mainly reports twist.
2. With the viewer in live mode, slowly move from the bottom hang to the intended top and back. Watch the roll number. It should change mostly in one direction on ascent and reverse on descent. Crossing ±180° is supported. If the reading stays flat, jumps unpredictably or reverses well before the top, adjust the X-axis alignment or recheck the IMU’s orientation convention.
3. Hold the bottom for about one second and press **B**. Hold the top for about one second and press **T**. The app uses a circular median of recent samples and rejects moving or stale readings. Either roll sign works because the capture order defines which pose is bottom and which is top. For example, a bottom of +140° and a top of −95° map through ±180° on a +125° arc.
4. The viewer applies and saves the pair to `calibration.json` beside the script. Supply `--calibration .\my-athlete.json` to use a different file. Repeat both captures whenever the strap position, pull-up style or subject changes. The app needs at least **12°** and at most **175°** of separation and assumes the real motion follows the shorter arc. If the reading does not cover a consistent arc in that range, choose a better sensor axis or mounting orientation.

**Model mapping.** The viewer unwraps roll around the midpoint of the two endpoints, maps the signed arc linearly from 0% to 100%, and clamps readings just outside the endpoints. It eases the displayed pose toward that target with a 95 ms smoothing time constant. In model coordinates X is side-to-side, +Y points behind the athlete, and +Z is up. The estimate varies upper-arm inclination from 17° to 160° from vertical up and solves the elbow/shoulder geometry against fixed wrists. Torso lean and leg bend follow preset curves. The displayed percentage describes position in the modeled range, so the same reading produces the same pose on ascent and descent.

**Limit.** One upper-arm angle cannot uniquely determine body pose, elbow bend, scapular motion, grip or asymmetry. Treat the scene as an approximate visualization for a strict pull-up. Kipping, twisting or switching grips can produce a misleading pose.

## TCP protocol

The ESP32-S3 is the TCP server on **192.168.4.1:8765**. Windows initiates the connection and sends no application data. The server sends one ASCII sample per line at 25 Hz.

```text
ROLL,-60.00
ROLL,+12.34
ROLL,+60.00
```

Each sample ends in LF (`\n`, byte 10). CRLF also works. Use uppercase `ROLL,`, an optional sign, decimal digits with an optional fractional part, and no spaces or units. Values must be in the inclusive range **−180° to +180°**. Lines may contain up to 64 bytes before LF, including an optional CR. Other records are discarded. TCP reads may split or combine lines. The viewer uses its local receive time and the latest valid sample. It treats the stream as paused after 0.6 seconds without a valid sample and reconnects after roughly 2.5 seconds without one.

## Verification

Run the Python checks from this folder. They exercise the decoder, TCP loopback reconnect, calibration and limb geometry. The sketch builds for the generic ESP32-S3 target with Arduino-ESP32 **3.3.10**.

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s tests -v
```

On your Windows 11 and ESP32-S3 setup:

- In local demo mode, confirm a full rep, an orbit to the rear, scroll zoom and Home reset.
- With the ESP32 running simulated roll, confirm Windows can join the AP, the live viewer reports roll from approximately −60° to +60°, and the figure cycles from hang to top. Power-cycle the ESP32 and verify that the pose holds and resumes once Windows rejoins the AP.
- With a real IMU, perform stationary bottom/top captures, then compare several slow reps with the live figure. Check for strap shift, non-monotonic readings, and lag. Relaunch the app and confirm the saved calibration loads and still matches the same mounting.
