# SteamVR Capture Architecture

## Goal

Build a SteamVR companion that can:

1. Attach to an existing SteamVR runtime that is already receiving real HTC Vive Tracker 3.0 poses from Lighthouse tracking.
2. Record tracker motion into a session file.
3. Replay the recorded motion back into SteamVR as virtual `GenericTracker` devices.

## Architecture

The project is intentionally split into two OpenVR layers:

1. `capture-service`
   - OpenVR application layer (`openvr.h`)
   - Runs as a background app.
   - Enumerates real `TrackedDeviceClass_GenericTracker` devices from SteamVR.
   - Reads standing-space poses with `GetDeviceToAbsoluteTrackingPose`.
   - Saves normalized tracker data into a session file.

2. `replay-driver`
   - OpenVR driver layer (`openvr_driver.h`)
   - Registered with SteamVR through `vrpathreg adddriver`.
   - Creates virtual `TrackedDeviceClass_GenericTracker` devices.
   - Reads a previously recorded session file.
   - Pushes replay poses back into SteamVR through `TrackedDevicePoseUpdated`.

3. `session-format`
   - Shared library with no OpenVR header dependency.
   - Defines tracker descriptors, pose samples, file parsing, and file writing.
   - Safe to link into both the recorder and the driver because it does not include either `openvr.h` or `openvr_driver.h`.

4. `desktop-ui`
   - Not implemented yet.
   - Will later supervise capture/replay, session management, and tracker-role mapping.

## Constraints

- Do not mix `openvr.h` and `openvr_driver.h` inside the same binary.
- Recorder and replay driver must remain separate binaries.
- Recorder uses `TrackingUniverseStanding` and stores sampled, non-predicted poses.
- Session files must include serial number, tracking system name, model number, and tracker role metadata.
- Replay driver must expose unique virtual serial numbers and must not impersonate real HTC tracker serial numbers.
- MVP assumes real trackers are disconnected or powered off during replay to avoid role conflicts.

## Current MVP Scope

- CMake workspace with vendored OpenVR SDK under `third_party/openvr`.
- Text-based session format for bootstrapping end-to-end flow quickly.
- CLI recorder that can list trackers and record for a fixed duration.
- SteamVR replay driver that loads a configured session file and exposes virtual trackers.
- No desktop UI, live passthrough, interpolation, compression, or editing tools yet.

## Near-Term Roadmap

1. Replace the bootstrap text session format with a binary or chunked format once the data path is stable.
2. Add interpolation for smoother replay between recorded samples.
3. Add a control channel between UI and replay driver for transport controls.
4. Add auto-launch behavior and tray integration after the base data path is reliable.
5. Add tests for file parsing and pose conversion helpers.
