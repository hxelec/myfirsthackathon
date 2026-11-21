# Lift Studio

Lift Studio animates a hammer curl, squat, deadlift, bench press, or pull-up from one six-axis IMU. It reads RepWatch microSD recordings and streams over a local ESP32 Wi-Fi access point. It detects reps locally and overlays two time-normalized traces. The original roll-only view remains available with `--legacy-pullup`. [Pull-up Anatomy setup](PULLUP.md)

To see the viewer before installing it, [watch the five-exercise demo](demo/lift-studio-demo.mp4). To collect your own sets, use the [RepWatch firmware and calibration guide](../RepWatch/README.md).

## Start on Windows 11

Use 64-bit CPython 3.12 or 3.13 with Tk support and the Windows Python launcher. Open PowerShell in this folder and start the app. The example uses Python 3.13.

```powershell
py -3.13 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe pullup_viewer.py
```

**Demo** opens by default. Choose an exercise from the dropdown to see three simulated reps. Drag the 3D scene to orbit, scroll to zoom, or press **Home** to reset the camera. Use **Pause** or **Space** and the timeline to inspect a pose. The right panel starts with the last two complete reps. **Prev pair** and **Next pair** step through consecutive comparisons.

For subsequent sessions, double-click [Launch-Lift-Studio.cmd](../Launch-Lift-Studio.cmd) in the repository root. It uses the Python environment created above.

| Source | How to use it |
| --- | --- |
| **Demo** | Explore the model and charts without a watch or internet connection. |
| **Live** | Join the watch Wi-Fi, pick the exercise, then click **Live**. Capture both endpoints. **Record CSV** saves the fused readings, timestamps, and calibration until you stop recording. |
| **Open CSV** | Select a RepWatch `sets/setNNNNNN.csv`, a sample from `demo`, or a desktop recording. Choose the exercise if needed, then press **Play** or scrub the timeline. |

RepWatch set files use the watch's recorded rep times, which cover the timed lifting portion. The viewer estimates pose endpoints from the watch's armed and completed-rep events when explicit endpoints are absent. If the events are missing or the estimated pose looks off, scrub to the bottom and top and capture them manually. The exercise dropdown supplies the movement for watch logs without an exercise name.

Command-line shortcuts include `--exercise bench`, `--live`, `--host 192.168.4.1 --port 8765`, and `--file "demo/pullup.csv"`. The exercise IDs are `hammer_curl`, `squat`, `deadlift`, `bench`, and `pullup`. The [demo video](demo/lift-studio-demo.mp4) shows one rep of each exercise from the five synthetic six-axis CSVs.

## Mount and calibrate

Fasten the watch so it follows the measured segment and keep the mount consistent across comparable sets. For live calibration, hold still at the first pose after connecting, capture both endpoints, then perform a slow test rep. Aim for a clear change in tilt across the exercise.

| Exercise | Mount | Set start | Set end |
| --- | --- | --- | --- |
| Squat | Front or outer thigh over the femur | Standing | Bottom at your usual depth |
| Bench press | Outer upper arm over the humerus | Press lockout | Normal chest-touch position |
| Deadlift | Firmly fixed to the torso over the spine, for example mid-back | Floor setup | Standing at lockout |
| Hammer curl | Wrist on the forearm side, with the wrist held neutral | Arm down | Top of the curl |
| Pull-up | Outer upper arm over the humerus | Dead hang | Top of the pull-up |

In **Live**, hold each pose for about one second and click **Set start** or **Set end**. In **Open CSV**, scrub to each recorded pose and use the same buttons. **Edit angles** accepts four values in degrees in the order start pitch, start roll, end pitch, end roll. Recalibrate when you change the mount, grip, stance, or movement range. To preserve calibration for playback, capture before starting a live CSV recording or include endpoint metadata in the SD log. Changing calibration closes an active desktop recording so its header still matches the data. The synthetic files already include calibration.

## What the model and score measure

- The app fuses raw gyro and accelerometer readings into gravity-relative tilt. For RepWatch card files, it uses the tilt RepWatch already fused. The signed angle along the calibrated arc drives a symmetric 3D model with preset joint motion and constraints. One sensor supplies one segment's tilt. Yaw about gravity, absolute position, and the remaining joints are underdetermined.
- The detector looks for departure from the start pose, an excursion through at least 72% of the calibrated range, then a return. It accepts reps lasting 0.6 to 18 seconds and excludes attempts interrupted by a telemetry gap over 0.35 seconds. Start and end times reflect these thresholds. A deadlift rep includes the lift and the return to the floor.
- The app resamples each rep at 101 points over 0–100% of its duration. The six-axis plots show degrees along the calibrated arc and degrees away from its plane. Legacy pitch/roll files retain their original charts. The score is `100 × max(0, 1 − mean trace gap / calibration span)`. Matching traces score 100 even if the reps take different amounts of time. The panel shows durations separately.
- A high score means the measured segment moved similarly across the two reps. Similar reps can score highly even if both use poor technique. Compare sets only when the watch mount and calibration are comparable.

## RepWatch Wi-Fi and microSD

In the [RepWatch sketch](../RepWatch/README.md), open **Settings** and turn on **Wireless**. Join **LiftWatch-S3** with password `lift-demo`, then click **Live**. The viewer connects to `192.168.4.1:8765`. The watch can record a set to microSD while streaming. For SD playback, wait for **Saved** on the watch before removing the microSD card, then open the set file. The existing [LiftWatch.ino](firmware/LiftWatch/LiftWatch.ino) is a separate simulated pitch/roll example for generic ESP32-S3 hardware.

### CSV and TCP formats

Use increasing integer milliseconds with acceleration in g and gyro in degrees per second. Optional metadata goes before the header. Playback rebases timestamps to the first usable sample. A raw-only log begins with a near-stationary reading so the viewer can initialize from gravity.

```csv
# exercise=pullup
# start_tilt=0,0,1
# end_tilt=0,0.6,-0.8
ms,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps
0,0,0,1,0,0,0
20,0,0.004,0.99999,12,0,0
```

TCP sends newline-delimited `IMU6,ms,ax,ay,az,gx,gy,gz` records. The viewer also accepts legacy `IMU,ms,pitch,roll` records and CSV columns `ms,pitch_deg,roll_deg` with optional `# start_deg` and `# end_deg` metadata. Desktop six-axis recordings store the fused `tilt_x,tilt_y,tilt_z` vector. RepWatch set files can also include fused pitch/roll, raw axes, rep fields, and a summary comment.

## Optional OpenAI notes

For CSV playback, set `OPENAI_API_KEY` before starting the viewer, open a file and turn on **AI form**. The current file and later CSVs get a review while the toggle is on for that app session. If an older log has no exercise name, confirm the selected exercise. Current RepWatch headers use `# exercise_name=HAMMER CURL`, `# velocity_loss_pct=80`, and `# cutoff_basis=rep_duration_increase`. With that basis, the cutoff is 80% longer than the first completed rep. The footer can report an interrupted attempt separately from completed reps. The viewer also accepts `# exercise=hammer_curl` and `# velocity_loss_threshold_pct=40%`.

When AI form is enabled, the viewer sends a summary of each CSV with completed reps to OpenAI for one short observation. The summary includes still periods near the calibrated top and bottom when the data supports measuring them. The observation can describe what was consistent or flag a change worth noting. CSV rows and filenames stay on the PC. Any AI score out of 10 describes consistency of the measured segment.

Set `OPENAI_API_KEY` in the environment before launching. `LIFT_LLM_MODEL` optionally overrides the default `gpt-4o-mini`. Enable **LLM notes** in **Live** and confirm the upload for that session. At most once every eight seconds, when new reps are available, the app sends the exercise ID, up to four completed rep time ranges, durations and peak excursions, and the latest pair's area and score. The API returns a short observation. It requires internet access alongside the watch AP, for example via Ethernet. API usage is billed separately. Local segmentation, playback, charts, and scoring work without it. [OpenAI Chat Completions API](https://developers.openai.com/api/reference/resources/chat/subresources/completions/methods/create)

## Verify on your hardware

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s tests -v
```

1. Open a CSV from `demo` and play or scrub through three reps.
2. Join the watch access point, calibrate each mount, and record a live set. Reopen the desktop CSV and compare the motion and rep traces.
3. Open a saved set from the watch's microSD card, select its exercise, and capture both endpoints. Power-cycle the watch during a live view to check reconnection.
