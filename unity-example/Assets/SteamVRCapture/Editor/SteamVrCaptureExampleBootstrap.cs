using System.Linq;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace SteamVRCapture.UnityExample.Editor
{
    public static class SteamVrCaptureExampleBootstrap
    {
        private const string SampleSessionPath = "Assets/SteamVRCapture/Samples/example_session_v2.svrcap";
        private const string CalibratedSampleSessionPath = "Assets/SteamVRCapture/Samples/example_session_v3_calibrated.svrcap";
        private const string SampleScenePath = "Assets/SteamVRCapture/Scenes/SteamVrCapturePreview.unity";

        [MenuItem("SteamVR Capture/Create Example Scene")]
        public static void CreateExampleScene()
        {
            SteamVrCaptureSessionAsset session = AssetDatabase.LoadAssetAtPath<SteamVrCaptureSessionAsset>(SampleSessionPath);
            if (session == null)
            {
                throw new System.InvalidOperationException("Failed to load example session asset.");
            }

            Scene scene = EditorSceneManager.NewScene(NewSceneSetup.DefaultGameObjects, NewSceneMode.Single);

            Camera mainCamera = Object.FindObjectsOfType<Camera>().FirstOrDefault(camera => camera.CompareTag("MainCamera"));
            if (mainCamera != null)
            {
                mainCamera.transform.SetPositionAndRotation(new Vector3(0f, 1.6f, -3f), Quaternion.identity);
            }

            GameObject playbackRoot = new("SteamVrCapturePlayback");
            SteamVrCapturePlayback playback = playbackRoot.AddComponent<SteamVrCapturePlayback>();
            SerializedObject serializedPlayback = new(playback);
            serializedPlayback.FindProperty("session").objectReferenceValue = session;
            serializedPlayback.FindProperty("playOnEnable").boolValue = true;
            serializedPlayback.FindProperty("loop").boolValue = true;
            serializedPlayback.FindProperty("playbackSpeed").floatValue = 1.0f;
            serializedPlayback.FindProperty("createDebugTargets").boolValue = true;
            serializedPlayback.ApplyModifiedPropertiesWithoutUndo();

            GameObject floor = GameObject.CreatePrimitive(PrimitiveType.Plane);
            floor.name = "Ground";
            floor.transform.position = Vector3.zero;
            floor.transform.localScale = new Vector3(1.5f, 1f, 1.5f);

            EditorSceneManager.SaveScene(scene, SampleScenePath);
            AssetDatabase.SaveAssets();
            Debug.Log($"SteamVR Capture example scene created at {SampleScenePath}");
        }

        public static void RunImportCheck()
        {
            LogImportedSession(SampleSessionPath);
            LogImportedSession(CalibratedSampleSessionPath);

            CreateExampleScene();
        }

        private static void LogImportedSession(string assetPath)
        {
            AssetDatabase.ImportAsset(assetPath, ImportAssetOptions.ForceSynchronousImport);
            SteamVrCaptureSessionAsset session = AssetDatabase.LoadAssetAtPath<SteamVrCaptureSessionAsset>(assetPath);
            if (session == null)
            {
                throw new System.InvalidOperationException($"Failed to import example session: {assetPath}");
            }

            int sampleCount = 0;
            foreach (SteamVrCaptureDeviceTrack track in session.Tracks)
            {
                sampleCount += track.samples.Count;
            }

            Debug.Log(
                $"SteamVR Capture import check passed. Asset={assetPath}, Format={session.FormatVersion}, DriverSpace={session.PosesAreDriverSpace}, Tracks={session.Tracks.Count}, Samples={sampleCount}, DurationNs={session.DurationNs}");
        }
    }
}
