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

        private readonly Dictionary<string, Transform> generatedTargetsBySerial = new();
        private double playbackTimeSeconds;
        private bool isPlaying;

        private void OnEnable()
        {
            playbackTimeSeconds = 0.0;
            isPlaying = playOnEnable;
            RebuildDebugTargets();
            ApplyCurrentFrame();
        }

        private void OnDisable()
        {
            generatedTargetsBySerial.Clear();
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

        [ContextMenu("Rebuild Debug Targets")]
        public void RebuildDebugTargets()
        {
            generatedTargetsBySerial.Clear();
            if (!createDebugTargets || session == null)
            {
                return;
            }

            Transform root = debugTargetRoot != null ? debugTargetRoot : transform;
            List<Transform> children = new();
            for (int index = 0; index < root.childCount; index++)
            {
                children.Add(root.GetChild(index));
            }

            foreach (Transform child in children)
            {
                if (child.name.StartsWith("SVRCAP_", StringComparison.Ordinal))
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

            foreach (SteamVrCaptureDeviceTrack track in session.Tracks)
            {
                GameObject marker = GameObject.CreatePrimitive(PrimitiveType.Cube);
                marker.name = $"SVRCAP_{track.index}_{track.serial}";
                marker.transform.SetParent(root, false);
                marker.transform.localScale = GetMarkerScale(track.deviceClass);

                Renderer renderer = marker.GetComponent<Renderer>();
                if (renderer != null)
                {
                    renderer.sharedMaterial = CreateDebugMaterial(track.deviceClass);
                }

                generatedTargetsBySerial[track.serial] = marker.transform;
            }
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

                Transform target = ResolveTarget(track);
                if (target == null)
                {
                    continue;
                }

                target.SetPositionAndRotation(scenePosition, sceneRotation);
            }
        }

        private Transform ResolveTarget(SteamVrCaptureDeviceTrack track)
        {
            foreach (DeviceBinding binding in bindings)
            {
                if (binding.target == null)
                {
                    continue;
                }

                if (!string.IsNullOrEmpty(binding.serial) && string.Equals(binding.serial, track.serial, StringComparison.Ordinal))
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

            generatedTargetsBySerial.TryGetValue(track.serial, out Transform generatedTarget);
            return generatedTarget;
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
