using System;
using System.Collections.Generic;
using UnityEngine;

namespace SteamVRCapture.UnityExample
{
    public enum SteamVrCaptureDeviceClass
    {
        Invalid = 0,
        Hmd = 1,
        Controller = 2,
        GenericTracker = 3,
        TrackingReference = 4,
        DisplayRedirect = 5,
        Max = 6,
    }

    [Serializable]
    public sealed class SteamVrCaptureTrackingSpaceSnapshot
    {
        public bool hasRawToStanding;
        public float[] rawToStanding = new float[12];
        public bool hasSeatedToStanding;
        public float[] seatedToStanding = new float[12];
    }

    [Serializable]
    public sealed class SteamVrCapturePoseSample
    {
        public ulong timestampNs;
        public double poseTimeOffsetSeconds;
        public Vector3 positionMeters;
        public Quaternion rotationWxyz = Quaternion.identity;
        public Vector3 linearVelocityMetersPerSecond;
        public Vector3 angularVelocityRadiansPerSecond;
        public Quaternion worldFromDriverRotationWxyz = Quaternion.identity;
        public Vector3 worldFromDriverTranslationMeters;
        public Quaternion driverFromHeadRotationWxyz = Quaternion.identity;
        public Vector3 driverFromHeadTranslationMeters;
        public bool poseValid;
        public bool deviceConnected;
        public int trackingResult;
        public bool willDriftInYaw;
        public bool shouldApplyHeadModel;
    }

    [Serializable]
    public sealed class SteamVrCaptureDeviceTrack
    {
        public int index;
        public SteamVrCaptureDeviceClass deviceClass;
        public string serial = string.Empty;
        public string trackingSystem = string.Empty;
        public string modelNumber = string.Empty;
        public string manufacturerName = string.Empty;
        public string controllerType = string.Empty;
        public string role = string.Empty;
        public List<SteamVrCapturePoseSample> samples = new();
    }

    public sealed class SteamVrCaptureSessionAsset : ScriptableObject
    {
        [SerializeField] private int formatVersion;
        [SerializeField] private bool posesAreDriverSpace;
        [SerializeField] private ulong durationNs;
        [SerializeField] private string sourceAssetPath = string.Empty;
        [SerializeField] private string importError = string.Empty;
        [SerializeField] private SteamVrCaptureTrackingSpaceSnapshot trackingSpace = new();
        [SerializeField] private List<SteamVrCaptureDeviceTrack> tracks = new();

        public int FormatVersion => formatVersion;
        public bool PosesAreDriverSpace => posesAreDriverSpace;
        public ulong DurationNs => durationNs;
        public string SourceAssetPath => sourceAssetPath;
        public string ImportError => importError;
        public SteamVrCaptureTrackingSpaceSnapshot TrackingSpace => trackingSpace;
        public IReadOnlyList<SteamVrCaptureDeviceTrack> Tracks => tracks;

        public void SetImportedData(
            int importedFormatVersion,
            bool importedPosesAreDriverSpace,
            ulong importedDurationNs,
            string importedSourceAssetPath,
            SteamVrCaptureTrackingSpaceSnapshot importedTrackingSpace,
            List<SteamVrCaptureDeviceTrack> importedTracks)
        {
            formatVersion = importedFormatVersion;
            posesAreDriverSpace = importedPosesAreDriverSpace;
            durationNs = importedDurationNs;
            sourceAssetPath = importedSourceAssetPath ?? string.Empty;
            importError = string.Empty;
            trackingSpace = importedTrackingSpace ?? new SteamVrCaptureTrackingSpaceSnapshot();
            tracks = importedTracks ?? new List<SteamVrCaptureDeviceTrack>();
        }

        public void SetImportError(string importedSourceAssetPath, string message)
        {
            formatVersion = 0;
            posesAreDriverSpace = false;
            durationNs = 0;
            sourceAssetPath = importedSourceAssetPath ?? string.Empty;
            importError = message ?? "Unknown import error.";
            trackingSpace = new SteamVrCaptureTrackingSpaceSnapshot();
            tracks = new List<SteamVrCaptureDeviceTrack>();
        }
    }
}
