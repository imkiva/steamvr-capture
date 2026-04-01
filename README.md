# steamvr-capture

Bootstrap workspace for recording HTC Vive Tracker 3.0 motion from SteamVR and replaying it back into SteamVR through a virtual tracker driver.

## Components

- `steamvr_capture_recorder`
  - CLI tool.
  - Lists real SteamVR trackers.
  - Records pose samples into a session file for a fixed duration.

- `driver_steamvr_capture_replay`
  - SteamVR OpenVR driver.
  - Polls replay control state from SteamVR settings.
  - Exposes virtual `GenericTracker` devices and feeds replay poses into SteamVR.

- `steamvr_capture_session`
  - Shared file format and parser/writer library.

- `steamvr_capture_overlay`
  - SteamVR dashboard overlay application.
  - Lets you browse `.svrcap` files in VR, set a session root, paste a copied Explorer path, or enter a direct path.
  - Writes replay selection into SteamVR settings so the replay driver hot-reloads the chosen session.
  - Mirrors the same UI into a desktop window with mouse input, `Ctrl+V`, and Explorer drag-drop.
  - Exposes explicit replay transport controls: `Play`, `Pause`, `Stop`, and `Loop`.

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
- Overlay: `build/runtime/tools/steamvr_capture_overlay.exe`
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

Install the dashboard overlay manifest once:

```powershell
build/runtime/tools/steamvr_capture_overlay.exe --install-manifest
```

After that, restart SteamVR. You should see a `SteamVR Capture Replay` dashboard tab. Use it to choose a session file in VR. The overlay updates these SteamVR settings for the driver:

- `enable`
- `session_path`
- `loop`
- `playback_speed`
- `session_generation`

## Known Gaps

- Overlay uses a simple in-process GDI UI; there is no desktop companion app yet.
- Replay currently uses nearest-sample playback, not interpolation.
- Recorder currently snapshots trackers that are present at capture start and does not remap reconnections by serial mid-session.
- No automatic startup orchestration yet.
