using System;
using System.Collections.Generic;
using UnityEngine;

namespace SteamVRCapture.UnityExample
{
    public sealed class SteamVrCapturePlayback : MonoBehaviour
    {
        [Serializable]
        public sealed class DeviceBinding
        {
            public string serial = string.Empty;
            public string role = string.Empty;
            public SteamVrCaptureDeviceClass deviceClass = SteamVrCaptureDeviceClass.Invalid;
            public Transform target;
        }

        [SerializeField] private SteamVrCaptureSessionAsset session;
        [SerializeField] private bool playOnEnable = true;
        [SerializeField] private bool loop = true;
        [SerializeField] private float playbackSpeed = 1.0f;
        [SerializeField] private bool createDebugTargets = true;
        [SerializeField] private Transform debugTargetRoot;
        [SerializeField] private List<DeviceBinding> bindings = new();
        [SerializeField, HideInInspector] private SteamVrCaptureSessionAsset generatedForSession;

        private double playbackTimeSeconds;
        private bool isPlaying;

        private void OnEnable()
        {
            playbackTimeSeconds = 0.0;
            isPlaying = playOnEnable;
            ApplyCurrentFrame();
        }

        private void OnValidate()
        {
            RecreateTrackerGameObjectsIfSessionChanged();
        }

        private void Update()
        {
            if (session == null)
            {
                return;
            }

            if (isPlaying)
            {
                playbackTimeSeconds += Time.deltaTime * Mathf.Max(0.0f, playbackSpeed);
                double durationSeconds = session.DurationNs / 1_000_000_000.0;
                if (durationSeconds > 0.0 && playbackTimeSeconds > durationSeconds)
                {
                    playbackTimeSeconds = loop ? playbackTimeSeconds % durationSeconds : durationSeconds;
                    isPlaying = loop;
                }
            }

            ApplyCurrentFrame();
        }

        [ContextMenu("Update Tracker GameObjects")]
        public void UpdateTrackerGameObjects()
        {
            if (!createDebugTargets || session == null)
            {
                generatedForSession = session;
                return;
            }

            Transform root = GetTargetRoot();
            foreach (SteamVrCaptureDeviceTrack track in session.Tracks)
            {
                if (FindExistingTarget(root, track) == null)
                {
                    CreateTrackerGameObject(root, track);
                }
            }

            generatedForSession = session;
            ApplyCurrentFrame();
        }

        [ContextMenu("Force Recreate Tracker GameObjects")]
        public void ForceRecreateTrackerGameObjects()
        {
            Transform root = GetTargetRoot();
            DestroyTrackerTargetChildren(root);

            if (createDebugTargets && session != null)
            {
                foreach (SteamVrCaptureDeviceTrack track in session.Tracks)
                {
                    CreateTrackerGameObject(root, track);
                }
            }

            generatedForSession = session;
            ApplyCurrentFrame();
        }

        [ContextMenu("Play")]
        public void Play()
        {
            isPlaying = true;
        }

        [ContextMenu("Pause")]
        public void Pause()
        {
            isPlaying = false;
        }

        [ContextMenu("Restart")]
        public void Restart()
        {
            playbackTimeSeconds = 0.0;
            isPlaying = true;
            ApplyCurrentFrame();
        }

        private void ApplyCurrentFrame()
        {
            if (session == null)
            {
                return;
            }

            Dictionary<SteamVrCaptureDeviceTrack, Transform> targetsByTrack = BuildTargetsByTrack();
            ulong currentTimestampNs = (ulong)Math.Max(0.0, playbackTimeSeconds * 1_000_000_000.0);
            foreach (SteamVrCaptureDeviceTrack track in session.Tracks)
            {
                SteamVrCapturePoseSample sample = SampleAtOrBefore(track.samples, currentTimestampNs);
                if (sample == null)
                {
                    continue;
                }

                if (!SteamVrCaptureSessionParser.TryResolveScenePose(session, sample, out Vector3 scenePosition, out Quaternion sceneRotation))
                {
                    continue;
                }

                Transform target = ResolveTarget(track, targetsByTrack);
                if (target == null)
                {
                    continue;
                }

                target.SetPositionAndRotation(scenePosition, sceneRotation);
            }
        }

        private Transform ResolveTarget(
            SteamVrCaptureDeviceTrack track,
            IReadOnlyDictionary<SteamVrCaptureDeviceTrack, Transform> targetsByTrack)
        {
            foreach (DeviceBinding binding in bindings)
            {
                if (binding.target == null)
                {
                    continue;
                }

                if (!string.IsNullOrEmpty(binding.serial) &&
                    string.Equals(binding.serial, track.serial, StringComparison.Ordinal))
                {
                    return binding.target;
                }

                if (!string.IsNullOrEmpty(binding.role) &&
                    string.Equals(binding.role, track.role, StringComparison.OrdinalIgnoreCase) &&
                    binding.deviceClass == track.deviceClass)
                {
                    return binding.target;
                }
            }

            targetsByTrack.TryGetValue(track, out Transform generatedTarget);
            return generatedTarget;
        }

        private Dictionary<SteamVrCaptureDeviceTrack, Transform> BuildTargetsByTrack()
        {
            Dictionary<SteamVrCaptureDeviceTrack, Transform> targetsByTrack = new();
            if (session == null)
            {
                return targetsByTrack;
            }

            Transform root = GetTargetRoot();
            SteamVrCaptureTrackedDeviceTarget[] targets =
                root.GetComponentsInChildren<SteamVrCaptureTrackedDeviceTarget>(true);

            foreach (SteamVrCaptureTrackedDeviceTarget target in targets)
            {
                foreach (SteamVrCaptureDeviceTrack track in session.Tracks)
                {
                    if (targetsByTrack.ContainsKey(track))
                    {
                        continue;
                    }

                    if (target.Matches(track))
                    {
                        targetsByTrack.Add(track, target.transform);
                        break;
                    }
                }
            }

            return targetsByTrack;
        }

        private void RecreateTrackerGameObjectsIfSessionChanged()
        {
            if (Application.isPlaying || session == generatedForSession)
            {
                return;
            }

            RecreateAllChildrenForSessionChange();
        }

        private Transform GetTargetRoot()
        {
            return debugTargetRoot != null ? debugTargetRoot : transform;
        }

        private static SteamVrCaptureTrackedDeviceTarget FindExistingTarget(Transform root, SteamVrCaptureDeviceTrack track)
        {
            SteamVrCaptureTrackedDeviceTarget[] targets =
                root.GetComponentsInChildren<SteamVrCaptureTrackedDeviceTarget>(true);
            foreach (SteamVrCaptureTrackedDeviceTarget target in targets)
            {
                if (target.Matches(track))
                {
                    return target;
                }
            }

            return null;
        }

        private static GameObject CreateTrackerGameObject(Transform root, SteamVrCaptureDeviceTrack track)
        {
            GameObject marker = GameObject.CreatePrimitive(PrimitiveType.Cube);
            string label = string.IsNullOrWhiteSpace(track.serial) ? $"track_{track.index}" : track.serial;
            marker.name = $"SVRCAP Target {track.index} {label}";
            marker.transform.SetParent(root, false);
            marker.transform.localScale = GetMarkerScale(track.deviceClass);

            SteamVrCaptureTrackedDeviceTarget target = marker.AddComponent<SteamVrCaptureTrackedDeviceTarget>();
            target.ConfigureFromTrack(track);

            Renderer renderer = marker.GetComponent<Renderer>();
            if (renderer != null)
            {
                renderer.sharedMaterial = CreateDebugMaterial(track.deviceClass);
            }

            return marker;
        }

        private void RecreateAllChildrenForSessionChange()
        {
            Transform root = GetTargetRoot();
            DestroyAllChildren(root);

            if (createDebugTargets && session != null)
            {
                foreach (SteamVrCaptureDeviceTrack track in session.Tracks)
                {
                    CreateTrackerGameObject(root, track);
                }
            }

            generatedForSession = session;
            ApplyCurrentFrame();
        }

        private static void DestroyTrackerTargetChildren(Transform root)
        {
            SteamVrCaptureTrackedDeviceTarget[] targets =
                root.GetComponentsInChildren<SteamVrCaptureTrackedDeviceTarget>(true);
            foreach (SteamVrCaptureTrackedDeviceTarget target in targets)
            {
                if (target.transform == root)
                {
                    continue;
                }

                if (Application.isPlaying)
                {
                    Destroy(target.gameObject);
                }
                else
                {
                    DestroyImmediate(target.gameObject);
                }
            }
        }

        private static void DestroyAllChildren(Transform root)
        {
            List<Transform> children = new();
            for (int index = 0; index < root.childCount; index++)
            {
                children.Add(root.GetChild(index));
            }

            foreach (Transform child in children)
            {
                if (Application.isPlaying)
                {
                    Destroy(child.gameObject);
                }
                else
                {
                    DestroyImmediate(child.gameObject);
                }
            }
        }

        private static SteamVrCapturePoseSample SampleAtOrBefore(IReadOnlyList<SteamVrCapturePoseSample> samples, ulong timestampNs)
        {
            if (samples == null || samples.Count == 0)
            {
                return null;
            }

            int low = 0;
            int high = samples.Count - 1;
            int resultIndex = 0;

            while (low <= high)
            {
                int mid = low + ((high - low) / 2);
                ulong sampleTimestamp = samples[mid].timestampNs;
                if (sampleTimestamp == timestampNs)
                {
                    return samples[mid];
                }

                if (sampleTimestamp < timestampNs)
                {
                    resultIndex = mid;
                    low = mid + 1;
                }
                else
                {
                    high = mid - 1;
                }
            }

            return samples[resultIndex];
        }

        private static Vector3 GetMarkerScale(SteamVrCaptureDeviceClass deviceClass)
        {
            return deviceClass switch
            {
                SteamVrCaptureDeviceClass.Hmd => new Vector3(0.18f, 0.12f, 0.12f),
                SteamVrCaptureDeviceClass.Controller => new Vector3(0.05f, 0.18f, 0.05f),
                SteamVrCaptureDeviceClass.GenericTracker => new Vector3(0.08f, 0.08f, 0.08f),
                _ => Vector3.one * 0.06f,
            };
        }

        private static Material CreateDebugMaterial(SteamVrCaptureDeviceClass deviceClass)
        {
            Material material = new(Shader.Find("Universal Render Pipeline/Lit") ?? Shader.Find("Standard"));
            material.color = deviceClass switch
            {
                SteamVrCaptureDeviceClass.Hmd => new Color(0.2f, 0.6f, 1.0f),
                SteamVrCaptureDeviceClass.Controller => new Color(1.0f, 0.85f, 0.2f),
                SteamVrCaptureDeviceClass.GenericTracker => new Color(0.2f, 1.0f, 0.45f),
                _ => new Color(0.85f, 0.85f, 0.85f),
            };
            return material;
        }
    }
}
