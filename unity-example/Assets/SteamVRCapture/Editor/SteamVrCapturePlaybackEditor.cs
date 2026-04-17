using UnityEditor;
using UnityEngine;

namespace SteamVRCapture.UnityExample.Editor
{
    [CustomEditor(typeof(SteamVrCapturePlayback))]
    public sealed class SteamVrCapturePlaybackEditor : UnityEditor.Editor
    {
        public override void OnInspectorGUI()
        {
            DrawDefaultInspector();

            EditorGUILayout.Space(8f);

            SteamVrCapturePlayback playback = (SteamVrCapturePlayback)target;
            if (GUILayout.Button("Update Tracker GameObjects"))
            {
                Undo.RegisterFullObjectHierarchyUndo(playback.gameObject, "Update SteamVR Capture Tracker GameObjects");
                playback.UpdateTrackerGameObjects();
                EditorUtility.SetDirty(playback);
            }

            if (GUILayout.Button("Force Recreate Tracker GameObject"))
            {
                Undo.RegisterFullObjectHierarchyUndo(playback.gameObject, "Force Recreate SteamVR Capture Tracker GameObjects");
                playback.ForceRecreateTrackerGameObjects();
                EditorUtility.SetDirty(playback);
            }
        }
    }
}
