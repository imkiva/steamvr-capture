SteamVR Capture Unity Example

This Unity 2022.3.22f1 project demonstrates the first import path for .svrcap files.

Included pieces:
- A ScriptedImporter for .svrcap session files.
- A parser for the repository's text v1 and v2 session formats.
- Support for the new v3 calibrated standing-pose session format.
- A ScriptableObject asset that stores imported tracks and pose samples.
- A simple playback MonoBehaviour that spawns debug markers and scrubs imported poses in-scene.

Suggested test flow:
1. Open the project in Unity 2022.3.22f1.
2. Select Assets/SteamVRCapture/Samples/example_session_v2.svrcap or Assets/SteamVRCapture/Samples/example_session_v3_calibrated.svrcap and confirm it imports as a session asset.
3. Open Assets/SteamVRCapture/Scenes/SteamVrCapturePreview.unity.
4. Enter Play Mode to preview the imported device motion as cubes.

Automation already executed once in batchmode:
- The sample .svrcap was imported through the ScriptedImporter.
- The preview scene was created by SteamVRCapture.UnityExample.Editor.SteamVrCaptureExampleBootstrap.

Current scope:
- Direct .svrcap import
- World-space debug playback
- v1 and v2 text session parsing

Not implemented yet:
- XR Origin integration
- Humanoid retargeting
- Animation Rigging setup
- AnimationClip baking
- FBX export
