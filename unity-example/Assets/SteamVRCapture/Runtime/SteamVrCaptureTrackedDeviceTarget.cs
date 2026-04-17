using System;
using UnityEngine;

namespace SteamVRCapture.UnityExample
{
    [DisallowMultipleComponent]
    public sealed class SteamVrCaptureTrackedDeviceTarget : MonoBehaviour
    {
        [SerializeField] private int trackIndex = -1;
        [SerializeField] private string serial = string.Empty;
        [SerializeField] private string role = string.Empty;
        [SerializeField] private SteamVrCaptureDeviceClass deviceClass = SteamVrCaptureDeviceClass.Invalid;

        public int TrackIndex => trackIndex;
        public string Serial => serial;
        public string Role => role;
        public SteamVrCaptureDeviceClass DeviceClass => deviceClass;

        public void ConfigureFromTrack(SteamVrCaptureDeviceTrack track)
        {
            if (track == null)
            {
                trackIndex = -1;
                serial = string.Empty;
                role = string.Empty;
                deviceClass = SteamVrCaptureDeviceClass.Invalid;
                return;
            }

            trackIndex = track.index;
            serial = track.serial ?? string.Empty;
            role = track.role ?? string.Empty;
            deviceClass = track.deviceClass;
        }

        public bool Matches(SteamVrCaptureDeviceTrack track)
        {
            if (track == null)
            {
                return false;
            }

            if (!string.IsNullOrEmpty(serial) &&
                string.Equals(serial, track.serial, StringComparison.Ordinal))
            {
                return true;
            }

            if (trackIndex >= 0 &&
                trackIndex == track.index &&
                deviceClass == track.deviceClass)
            {
                return true;
            }

            return !string.IsNullOrEmpty(role) &&
                string.Equals(role, track.role, StringComparison.OrdinalIgnoreCase) &&
                deviceClass == track.deviceClass;
        }
    }
}
