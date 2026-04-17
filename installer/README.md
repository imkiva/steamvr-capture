# SteamVR Capture Installer

The CMake build can download a portable Inno Setup compiler from the
`Tools.InnoSetup` NuGet package when `ISCC.exe` is not already available.

Build the installer end-to-end:

```powershell
cmake -S . -B build -G Ninja
cmake --build build --target steamvr_capture_installer
```

The installer is written to:

```text
build\installer\SteamVRCaptureSetup-0.1.0.exe
```

You can still stage the runtime manually:

```powershell
cmake --build build --config Release
cmake --install build --config Release --prefix dist/SteamVRCapture
```

And then build with a locally installed Inno Setup compiler:

```powershell
iscc installer/SteamVRCapture.iss
```

The installer writes to:

```text
%LOCALAPPDATA%\Programs\SteamVRCapture
```

Post-install registration is handled by:

```text
tools\steamvr_capture_setup_helper.exe --install
```

The helper registers the replay driver with SteamVR's own `vrpathreg.exe`,
generates and registers the overlay `.vrmanifest`, enables overlay autolaunch,
and initializes the default session directory under Documents.

Uninstall runs:

```text
tools\steamvr_capture_setup_helper.exe --uninstall
```

User recordings under `Documents\SteamVR Capture\Sessions` are intentionally
left behind by the uninstaller.
