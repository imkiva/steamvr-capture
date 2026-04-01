# steamvr-capture

Bootstrap workspace for recording HTC Vive Tracker 3.0 motion from SteamVR and replaying it back into SteamVR through a virtual tracker driver.

## Components

- `steamvr_capture_recorder`
  - CLI tool.
  - Lists real SteamVR trackers.
  - Records pose samples into a session file for a fixed duration or until an external stop event is signaled.
  - Skips this project's own virtual replay trackers so replay output is never re-recorded by mistake.

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
  - Can start and stop recording directly from the overlay.
  - Lets you configure the recording interval.

- `steamvr_capture_broker` and `steamvr_capture_hotpatch.dll`
  - Experimental no-restart live hook path.
  - Injects into `vrserver.exe` and applies one of three live modes for matching real trackers:
    - `Passthrough`
    - `Suppress`
    - `Replace`

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
- Broker: `build/runtime/tools/steamvr_capture_broker.exe`
- Hotpatch DLL: `build/runtime/tools/steamvr_capture_hotpatch.dll`
- Driver package root: `build/runtime/steamvr_capture_replay`

## Packaging For Testers

The runtime is intentionally laid out so you can zip the build output without rewriting hard-coded paths:

- `build/runtime/tools`
- `build/runtime/steamvr_capture_replay`

The overlay resolves the recorder and broker relative to its own executable directory, so testers can run the packaged tools directly after extracting the zip.

## Recorder Usage

List trackers:

```powershell
build/runtime/tools/steamvr_capture_recorder.exe --list
```

Record 30 seconds at the default 10 ms interval:

```powershell
build/runtime/tools/steamvr_capture_recorder.exe --record sessions\walk.svrcap --duration 30 --interval-ms 10
```

Record until a named stop event is signaled:

```powershell
build/runtime/tools/steamvr_capture_recorder.exe --record sessions\walk.svrcap --until-event Local\SteamVRCaptureRecorderStop_Test --interval-ms 10
```

Pretty print a recorded session file:

```powershell
build/runtime/tools/steamvr_capture_recorder.exe --pretty-print sessions\walk.svrcap
```

Export the pretty print view as CSV:

```powershell
build/runtime/tools/steamvr_capture_recorder.exe --pretty-print-csv sessions\walk.svrcap --output sessions\walk.csv
```

The recorder currently defaults to `10.0 ms`. That matches the SteamVR Tracking 2.0 rate the project is currently targeting. The overlay exposes this value so you can tune it per session.

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

## Overlay Workflow

The overlay supports both VR and desktop use.

- `Set Session Root`: set the directory scanned for `.svrcap` files.
- `Direct Path`: load a specific session file directly.
- `Paste Clipboard`: accept Explorer-copied files or directories.
- `Play / Pause / Stop`: control replay transport.
- `Loop`: toggle looping.
- `Live Mode`: cycle through `Suppress`, `Replace`, and `Passthrough`.
- `Rec: <ms>`: configure the recording interval.
- `Start Rec / Stop Rec`: launch or stop the recorder directly from the overlay.

`Start Rec` saves into the current session directory. If a direct-path session is active, the new recording is saved next to that file.

## Live Hook Notes

The live hook path is experimental and is not part of the normal OpenVR driver lifecycle. It works by injecting `steamvr_capture_hotpatch.dll` into `vrserver.exe`.

- `Passthrough`: do not alter real tracker updates.
- `Suppress`: force matching real trackers to disconnected pose while playback is active.
- `Replace`: overwrite matching real tracker updates with recorded pose data.

Use this path carefully. It is intended for local testing and iteration, not as a stable public integration point.

## Known Gaps

- Overlay uses a simple in-process GDI UI; there is no richer native desktop companion app yet.
- Replay currently uses nearest-sample playback, not interpolation.
- Recorder currently snapshots trackers that are present at capture start and does not remap reconnections by serial mid-session.
- The live hook path is intentionally experimental and depends on current SteamVR runtime behavior.
- No automatic startup orchestration yet.
