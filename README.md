# steamvr-capture

Bootstrap workspace for recording SteamVR tracked-device motion and replaying it back into SteamVR through a virtual tracker driver and an experimental live hotpatch path.

## Components

- `steamvr_capture_recorder`
  - CLI tool.
  - Lists real SteamVR trackers.
  - Legacy recorder path for fixed-duration standing-space tracker capture.
  - Skips this project's own virtual replay trackers so replay output is never re-recorded by mistake.

- `driver_steamvr_capture_replay`
  - SteamVR OpenVR driver.
  - Polls replay control state from SteamVR settings.
  - Exposes virtual `GenericTracker` devices and feeds replay poses into SteamVR.

- `steamvr_capture_session`
  - Shared file format and parser/writer library.
  - Supports legacy v1 tracker sessions and v2 full `DriverPose_t` pose-channel sessions.

- `steamvr_capture_overlay`
  - SteamVR dashboard overlay application.
  - Lets you browse `.svrcap` files in VR, set a session root, paste a copied Explorer path, or enter a direct path.
  - Writes replay selection into SteamVR settings so the replay driver hot-reloads the chosen session.
  - Mirrors the same UI into a desktop window with mouse input, `Ctrl+V`, and Explorer drag-drop.
  - Exposes explicit replay transport controls: `Play`, `Pause`, `Stop`, and `Loop`.
  - Starts and stops broker-driven recording directly from the overlay.
  - Lets you configure the recording interval.

- `steamvr_capture_broker` and `steamvr_capture_hotpatch.dll`
  - Experimental no-restart live hook path.
  - Injects into `vrserver.exe`, records full live `DriverPose_t` samples, and applies one of three live modes for matching real devices:
    - `Passthrough`
    - `Suppress`
    - `Replace`

- `steamvr_capture_setup_helper`
  - Installer/repair helper.
  - Registers the replay driver with SteamVR's own `vrpathreg.exe`.
  - Generates and registers the overlay application manifest.
  - Enables dashboard overlay auto-launch so the UI follows SteamVR startup.
  - Initializes the default session directory and non-destructive settings.

## Current Session Format

The current implementation uses a text bootstrap format so the recorder and driver can be connected quickly.

- v1 header: `SVRCAP<TAB>1`
- v1 tracker lines: `TRACKER<TAB>index<TAB>serial<TAB>tracking_system<TAB>model<TAB>role`
- v1 sample lines:
  `SAMPLE<TAB>timestamp_ns<TAB>tracker_index<TAB>px<TAB>py<TAB>pz<TAB>qw<TAB>qx<TAB>qy<TAB>qz<TAB>vx<TAB>vy<TAB>vz<TAB>avx<TAB>avy<TAB>avz<TAB>pose_valid<TAB>device_connected<TAB>tracking_result`

- v2 header: `SVRCAP<TAB>2`
- v2 device lines:
  `DEVICE<TAB>index<TAB>device_class<TAB>serial<TAB>tracking_system<TAB>model<TAB>manufacturer<TAB>controller_type<TAB>role`
- v2 space line:
  `SPACE<TAB>has_raw_to_standing<12 floats><TAB>has_seated_to_standing<12 floats>`
- v2 sample lines store full pose-channel data needed to reconstruct `DriverPose_t`, including:
  - `poseTimeOffset`
  - `vecPosition`
  - `qRotation`
  - `vecVelocity`
  - `vecAngularVelocity`
  - `qWorldFromDriverRotation`
  - `vecWorldFromDriverTranslation`
  - `qDriverFromHeadRotation`
  - `vecDriverFromHeadTranslation`
  - `poseIsValid`
  - `deviceIsConnected`
  - `result`
  - `willDriftInYaw`
  - `shouldApplyHeadModel`

- v3 header: `SVRCAP<TAB>3`
- v3 semantic: app-standing calibrated capture for Unity playback after Space Calibrator alignment.
- v3 is written by `steamvr_capture_recorder --record-mode app_standing_calibrated`, not by the broker.

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
- Setup helper: `build/runtime/tools/steamvr_capture_setup_helper.exe`
- Driver package root: `build/runtime/steamvr_capture_replay`

## Installer Packaging

Build the installer end-to-end:

```powershell
cmake -S . -B build -G Ninja
cmake --build build --target steamvr_capture_installer
```

If `ISCC.exe` is not available, CMake downloads the portable `Tools.InnoSetup` NuGet package into `build/_deps/innosetup` and uses its `tools/ISCC.exe`.

The generated installer is:

```text
build\installer\SteamVRCaptureSetup-0.1.0.exe
```

Manual staging is still available with `cmake --install build --prefix dist/SteamVRCapture`.

The installer targets:

```text
%LOCALAPPDATA%\Programs\SteamVRCapture
```

Post-install registration is handled by:

```powershell
%LOCALAPPDATA%\Programs\SteamVRCapture\tools\steamvr_capture_setup_helper.exe --install
```

The helper registers the replay driver, registers the dashboard overlay manifest, enables overlay auto-launch, and creates the default session directory:

```text
Documents\SteamVR Capture\Sessions
```

To inspect an installation:

```powershell
%LOCALAPPDATA%\Programs\SteamVRCapture\tools\steamvr_capture_setup_helper.exe --status
```

Uninstall runs the helper with `--uninstall` to remove SteamVR driver and overlay registrations. User recordings under Documents are left in place.

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

The CLI recorder supports:

- `--record-mode legacy_tracker` for legacy v1 tracker-only standing-pose sessions.
- `--record-mode app_standing_calibrated` for v3 app-standing calibrated HMD/controller/tracker sessions intended for Unity playback.

The overlay's `Record Mode = Driver` path uses the broker to write v2 full driver-pose sessions. The overlay's `Record Mode = Calibrated` path spawns `steamvr_capture_recorder.exe` and writes v3 sessions.

## Manual Registration For Development

Installed users should not need this section. The installer performs these steps through `steamvr_capture_setup_helper`.

For development builds, register the replay driver package with SteamVR:

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
- `Record Mode`: switch between broker-driven driver-pose recording and recorder.exe-driven calibrated app-standing recording.
- `Start Rec / Stop Rec`: start or stop recording directly from the overlay.

`Start Rec` saves into the current session directory. If a direct-path session is active, the new recording is saved next to that file.

## Live Hook Notes

The live hook path is experimental and is not part of the normal OpenVR driver lifecycle. It works by injecting `steamvr_capture_hotpatch.dll` into `vrserver.exe`.

- `Passthrough`: do not alter real tracker updates.
- `Suppress`: force matching real devices to disconnected pose while playback is active.
- `Replace`: overwrite matching real device updates with recorded pose data.

Use this path carefully. It is intended for local testing and iteration, not as a stable public integration point.

Current limitation:

- If a v2 session contains HMD or controller serials, the current live matching path will target those devices too.
- In `Suppress`, this can disconnect the active HMD/controller pose feed.
- In `Replace`, this can overwrite the active HMD/controller pose feed with recorded data.
- In practice this can cause severe headset stutter or an unusable view.
- Until per-class replay targeting is added, treat live `Suppress` / `Replace` as tracker-focused only and avoid using HMD/controller-inclusive sessions for those modes.

## Known Gaps

- Overlay uses a simple in-process GDI UI; there is no richer native desktop companion app yet.
- Replay currently uses nearest-sample playback, not interpolation.
- Recorder currently snapshots trackers that are present at capture start and does not remap reconnections by serial mid-session.
- The live hook path is intentionally experimental and depends on current SteamVR runtime behavior.
- Live `Suppress` / `Replace` currently match every recorded device class in a v2 session, including HMD and controllers.
