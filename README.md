# steamvr-capture

Bootstrap workspace for recording HTC Vive Tracker 3.0 motion from SteamVR and replaying it back into SteamVR through a virtual tracker driver.

## Components

- `steamvr_capture_recorder`
  - CLI tool.
  - Lists real SteamVR trackers.
  - Records pose samples into a session file for a fixed duration.

- `driver_steamvr_capture_replay`
  - SteamVR OpenVR driver.
  - Loads a recorded session file from SteamVR settings.
  - Exposes virtual `GenericTracker` devices and feeds replay poses into SteamVR.

- `steamvr_capture_session`
  - Shared file format and parser/writer library.

## Current Session Format

The current implementation uses a text bootstrap format so the recorder and driver can be connected quickly.

- Header line: `SVRCAP<TAB>1`
- Tracker lines: `TRACKER<TAB>index<TAB>serial<TAB>tracking_system<TAB>model<TAB>role`
- Sample lines:
  `SAMPLE<TAB>timestamp_ns<TAB>tracker_index<TAB>px<TAB>py<TAB>pz<TAB>qw<TAB>qx<TAB>qy<TAB>qz<TAB>vx<TAB>vy<TAB>vz<TAB>avx<TAB>avy<TAB>avz<TAB>pose_valid<TAB>device_connected<TAB>tracking_result`

This is a temporary bootstrap. The architecture document in `AGENTS.md` already reserves a later migration to a binary format.

## Build

```powershell
cmake -S . -B build -G Ninja
cmake --build build
```

Outputs:

- Recorder: `build/runtime/tools/steamvr_capture_recorder.exe`
- Driver package root: `build/runtime/steamvr_capture_replay`

## Recorder Usage

List trackers:

```powershell
build/runtime/tools/steamvr_capture_recorder.exe --list
```

Record 30 seconds at 100 Hz:

```powershell
build/runtime/tools/steamvr_capture_recorder.exe --record sessions\walk.svrcap --duration 30 --interval-ms 10
```

Pretty print a recorded session file:

```powershell
build/runtime/tools/steamvr_capture_recorder.exe --pretty-print sessions\walk.svrcap
```

Export the pretty print view as CSV:

```powershell
build/runtime/tools/steamvr_capture_recorder.exe --pretty-print-csv sessions\walk.svrcap --output sessions\walk.csv
```

## Driver Registration

Register the replay driver package with SteamVR:

```powershell
& "$env:ProgramFiles(x86)\Steam\steamapps\common\SteamVR\bin\win64\vrpathreg.exe" adddriver "$PWD\build\runtime\steamvr_capture_replay"
```

Then set the session path in SteamVR settings under the `driver_steamvr_capture_replay` section. The default settings file that ships with the driver contains:

- `enable`
- `session_path`
- `loop`
- `playback_speed`

## Known Gaps

- No desktop UI yet.
- Replay currently uses nearest-sample playback, not interpolation.
- Recorder currently snapshots trackers that are present at capture start and does not remap reconnections by serial mid-session.
- No automatic startup orchestration yet.
