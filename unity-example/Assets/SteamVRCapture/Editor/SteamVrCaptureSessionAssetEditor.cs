using UnityEditor;
using UnityEngine;

namespace SteamVRCapture.UnityExample.Editor
{
    [CustomEditor(typeof(SteamVrCaptureSessionAsset))]
    public sealed class SteamVrCaptureSessionAssetEditor : UnityEditor.Editor
    {
        public override void OnInspectorGUI()
        {
            serializedObject.Update();

            SerializedProperty formatVersion = serializedObject.FindProperty("formatVersion");
            SerializedProperty posesAreDriverSpace = serializedObject.FindProperty("posesAreDriverSpace");
            SerializedProperty durationNs = serializedObject.FindProperty("durationNs");
            SerializedProperty sourceAssetPath = serializedObject.FindProperty("sourceAssetPath");
            SerializedProperty importError = serializedObject.FindProperty("importError");
            SerializedProperty tracks = serializedObject.FindProperty("tracks");

            EditorGUILayout.LabelField("Format Version", formatVersion.intValue.ToString());
            EditorGUILayout.LabelField("Driver Space", posesAreDriverSpace.boolValue ? "Yes" : "No");
            EditorGUILayout.LabelField("Duration (ns)", durationNs.ulongValue.ToString());
            EditorGUILayout.LabelField("Source Asset", sourceAssetPath.stringValue);
            EditorGUILayout.LabelField("Track Count", tracks.arraySize.ToString());

            if (!string.IsNullOrWhiteSpace(importError.stringValue))
            {
                EditorGUILayout.HelpBox(importError.stringValue, MessageType.Error);
            }

            EditorGUILayout.Space(8f);
            EditorGUILayout.LabelField("Tracks", EditorStyles.boldLabel);
            EditorGUI.indentLevel++;
            for (int i = 0; i < tracks.arraySize; i++)
            {
                SerializedProperty track = tracks.GetArrayElementAtIndex(i);
                string serial = track.FindPropertyRelative("serial").stringValue;
                string role = track.FindPropertyRelative("role").stringValue;
                int deviceClass = track.FindPropertyRelative("deviceClass").enumValueIndex;
                int sampleCount = track.FindPropertyRelative("samples").arraySize;
                EditorGUILayout.LabelField($"{i}: {serial}", $"class={(SteamVrCaptureDeviceClass)deviceClass}, role={role}, samples={sampleCount}");
            }
            EditorGUI.indentLevel--;

            serializedObject.ApplyModifiedProperties();
        }
    }
}
