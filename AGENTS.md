# SteamVR Capture Architecture

## Goal

Build a SteamVR companion that can:

1. Attach to an existing SteamVR runtime that is already receiving real HTC Vive Tracker 3.0 poses from Lighthouse tracking.
2. Record tracked-device motion into a session file.
3. Replay the recorded motion back into SteamVR as virtual `GenericTracker` devices or by hotpatching matching live devices.

## Architecture

The project is intentionally split into two OpenVR layers:

1. `capture-service`
   - OpenVR application layer (`openvr.h`)
   - Currently split in practice into:
     - CLI recorder for OpenVR app-layer capture sources
     - shared OpenVR helpers used by the broker for live driver-pose capture metadata
   - Enumerates real HMD, controller, and tracker devices from SteamVR.
   - Captures tracking-space metadata and device descriptors.
   - The CLI recorder is not a legacy-only binary; it currently owns two app-layer record sources:
     - `legacy_tracker`: sampled standing-space tracker poses
     - `app_standing_calibrated`: sampled `TrackingUniverseStanding` poses for HMD/controller/tracker, intended for Unity-side playback in a calibrated world space
   - Broker-owned `driver_pose` recording is a separate record source and writes v2 full `DriverPose_t` sessions.

2. `replay-driver`
   - OpenVR driver layer (`openvr_driver.h`)
   - Registered with SteamVR through `vrpathreg adddriver`.
   - Creates virtual `TrackedDeviceClass_GenericTracker` devices.
   - Reads a previously recorded session file.
   - Pushes replay poses back into SteamVR through `TrackedDevicePoseUpdated`.

3. `session-format`
   - Shared library with no OpenVR header dependency.
   - Defines device descriptors, pose samples, file parsing, and file writing.
   - Safe to link into both the recorder and the driver because it does not include either `openvr.h` or `openvr_driver.h`.
   - Supports:
     - legacy v1 standing-pose tracker sessions
     - v2 full `DriverPose_t` pose-channel sessions for HMD/controller/tracker recording
     - v3 app-standing calibrated sessions for Unity-side playback

4. `steamvr-dashboard-overlay`
   - OpenVR overlay application layer (`openvr.h`)
   - Creates a SteamVR dashboard tab for replay control.
   - Browses `.svrcap` session files, accepts direct path entry, supports clipboard paste and desktop drag-drop.
   - Mirrors the same UI into a desktop window.
   - Writes replay control state into `vrsettings` so the replay driver can hot-reload the selected session without hard-coded config edits.
   - Starts and stops recording directly, saving new recordings into the current session directory.
   - `driver_pose` recording is broker-driven.
   - `app_standing_calibrated` recording is handled by spawning the sibling `steamvr_capture_recorder.exe`; broker does not own calibrated recording anymore.
   - Calibrated overlay recording is two-stage: first `Start Rec` arms the request, then both controller triggers or a second `Start Rec` click confirms and launches the recorder.
   - A successful calibrated recorder launch plays a system beep so the user has immediate headset-side feedback.
   - Exposes replay transport controls, live mode switching, and configurable recording interval.

5. `hotpatch-broker` and `steamvr hotpatch dll`
   - Experimental no-restart path for live interception inside `vrserver.exe`.
   - `hotpatch-broker` runs as a user-space helper process, watches replay settings, records live driver-pose samples, parses the selected session, and injects the hotpatch DLL into `vrserver.exe`.
   - `steamvr hotpatch dll` runs inside `vrserver.exe`, reads a shared-memory control block, and hooks the live server-driver boundary.
   - Current live modes are:
     - `passthrough`: do nothing to real tracker updates
     - `suppress`: force matching real devices to disconnected pose while replay runs
     - `replace`: overwrite matching real device updates with recorded pose data
   - This path is intentionally separate from the replay driver because it is a runtime hook architecture, not a normal OpenVR driver lifecycle.

6. `setup-helper` and installer
   - OpenVR application/utility layer (`openvr.h`), never `openvr_driver.h`.
   - Built as `steamvr_capture_setup_helper.exe`.
   - Handles installed-user registration and repair:
     - locate SteamVR and its own `vrpathreg.exe`
     - register/unregister the replay driver
     - generate the installed overlay `.vrmanifest`
     - register/unregister the overlay app manifest
     - enable overlay auto-launch with SteamVR
     - initialize non-destructive defaults such as session root and record interval
   - Inno Setup is the installer path. Do not add a zip portable packaging path unless explicitly requested.
   - CMake may download the `Tools.InnoSetup` NuGet package into `build/_deps/innosetup` and use its portable `tools/ISCC.exe` when no system `ISCC.exe` is found.

## Constraints

- Do not mix `openvr.h` and `openvr_driver.h` inside the same binary.
- Recorder and replay driver must remain separate binaries.
- Overlay must remain a separate OpenVR application binary and must not link against `openvr_driver.h`.
- Session files must include serial number, tracking system name, model number, device class, and role metadata.
- Replay driver must expose unique virtual serial numbers and must not impersonate real HTC tracker serial numbers.
- Recorder must skip the project's own virtual replay trackers so a new capture session never re-records replay output.
- Replay mode still prefers real trackers to be powered off unless the hotpatch path is intentionally being used for `suppress` or `replace`.
- The no-restart hotpatch path is experimental and unsupported by Valve. Keep heavy logic out of the injected DLL; session parsing and transport control belong in the broker.
- Current live matching is session-driven. If a v2 session contains HMD or controller serials, `suppress` and `replace` will currently target those devices too. This can disrupt the active headset view and controller tracking and is a known limitation until per-class replay filtering is added.
- Unity's `.svrcap` importer is allowed to ignore a single truncated final `POSE` or `SAMPLE` line. This is only to tolerate files interrupted by `Ctrl-C`; mid-file corruption should still fail import.
- The CLI recorder should treat `Ctrl-C` as a graceful stop request: finish the current sampling pass, flush the writer, and then exit. Do not leave partially written pose rows on normal console interruption.
- Installed runtime layout is rooted at the installer directory with sibling `tools/` and `steamvr_capture_replay/` directories. Overlay-launched helpers must be resolved relative to the overlay executable, not build-tree paths.
- Installer registration should use SteamVR's installed `vrpathreg.exe`; do not bundle or call a copied `vrpathreg.exe`.
- Keep overlay `driver_pose` recording immediate through the broker. Only `app_standing_calibrated` uses the armed waiting state and external recorder spawn.
- The second `Start Rec` click while calibrated recording is armed is an intentional desktop/no-VR debug shortcut and should remain supported.
- Do not describe `steamvr_capture_recorder.exe` as the legacy recorder. `legacy_tracker` is one recorder mode; recorder-vs-broker is a record-source boundary.

## Shell Execution

- When running PowerShell commands for this repo, always start PowerShell in `-NoProfile -NonInteractive` mode to avoid profile side effects and PSReadLine noise.
- In Codex CLI command output, keep the displayed command to the `-Command` body only. Do not wrap commands in an explicit nested `pwsh.exe` prefix unless there is no other workable option.

## Current MVP Scope

- CMake workspace with vendored OpenVR SDK under `third_party/openvr`.
- Text-based session format for bootstrapping end-to-end flow quickly.
- Session v2 stores full `DriverPose_t` pose semantics plus tracking-space metadata.
- Session v3 stores calibrated standing-space poses for Unity-side playback without requiring Space Calibrator to be present at playback time.
- CLI recorder that can list tracked devices, record app-layer sources for a fixed duration, or record until an external stop event is signaled.
- Recorder uses target-time scheduling instead of naive fixed sleeps, and currently defaults to a 10 ms interval.
- Recorder `--list` is expected to show HMD, controller, and tracker devices for calibrated recording diagnostics.
- SteamVR replay driver that hot-reloads the selected session file and exposes virtual trackers.
- SteamVR dashboard overlay for choosing a session root or direct replay file path inside VR, with desktop mirror support.
- Overlay-driven broker recording with session-directory output, replay transport controls, live mode switching, and configurable recording interval.
- Broker-owned `driver_pose` recording and recorder-owned app-layer recording are separate sources exposed through overlay record mode selection.
- Overlay calibrated recording arms first, then starts from a both-trigger chord or a second `Start Rec` click, and launches the sibling CLI recorder with a confirmation beep.
- Installer build via the `steamvr_capture_installer` CMake target. Installed users get driver registration and overlay auto-launch through `steamvr_capture_setup_helper.exe`.
- Unity example project imports `.svrcap` assets directly and provides debug-target playback for v1/v2/v3 sessions.
- Experimental no-restart live hook path with `passthrough / suppress / replace`.
- No interpolation, compression, or editing tools yet.

## Near-Term Roadmap

1. Add per-class replay targeting so HMD and controller pose capture does not automatically imply live HMD/controller suppression or replacement.
2. Replace the bootstrap text session format with a binary or chunked format once the data path is stable.
3. Add interpolation for smoother replay between recorded samples.
4. Harden live-hook observability with explicit logging, failure surfacing, and diagnostics.
5. Add tray integration after the base data path is reliable.
6. Add tests for file parsing, pose conversion helpers, setup-helper registration logic, and live-mode matching logic.
